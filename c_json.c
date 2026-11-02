#include "c_json.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 容器解析状态机的状态。
 */
typedef enum frame_state {
    FRAME_ARR_OPEN,  /**< 数组刚打开：期待第一个值或 ']' */
    FRAME_ARR_COMMA, /**< 数组读到逗号后：期待值，不允许直接跟 ']' */
    FRAME_ARR_VALUE, /**< 数组刚读完一个值：期待 ',' 或 ']' */
    FRAME_OBJ_OPEN,  /**< 对象刚打开：期待键或 '}' */
    FRAME_OBJ_COMMA, /**< 对象读到逗号后：期待键，不允许直接跟 '}' */
    FRAME_OBJ_KEY,   /**< 刚读完键：期待 ':' */
    FRAME_OBJ_COLON, /**< 刚读完 ':': 期待值 */
    FRAME_OBJ_VALUE  /**< 刚读完值：期待 ',' 或 '}' */
} frame_state;

/**
 * @brief 显式解析栈中的一帧，对应一个尚未闭合的数组或对象。
 */
typedef struct parse_frame {
    size_t      node_index; /**< 该容器在节点数组中的索引 */
    size_t      count;      /**< 已解析并挂入该容器的直接子项数 */
    frame_state state;      /**< 当前状态机状态 */
    cjson_type  type;       /**< CJSON_ARRAY 或 CJSON_OBJECT */
    const char *key;        /**< 对象正在处理的键；统计阶段为 NULL */
    size_t      key_len;    /**< 键的字节数 */
} parse_frame;

/**
 * @brief 两遍扫描共用的解析上下文。
 *
 * 第一遍 build=0，只校验和统计；第二遍 build=1，把结果写入预先分配好的内存池。
 */
typedef struct parser {
    const char  *text; /**< 输入文本首地址 */
    size_t       len;  /**< 输入文本字节数 */
    size_t       pos;  /**< 当前扫描位置 */
    int          build; /**< 0=统计阶段；1=填充阶段 */
    cjson_error *err;  /**< 错误输出；调用者可以为 NULL */

    size_t value_count;  /**< 统计阶段：值节点总数 */
    size_t string_bytes; /**< 统计阶段：字符串区所需字节数（含结尾 '\0'） */
    size_t max_depth;    /**< 统计阶段：记录到的最大嵌套深度 */

    size_t *child_counts;    /**< 每个容器节点的直接子项数，按节点索引排列 */
    size_t  child_counts_cap; /**< child_counts 的容量 */

    cjson_doc   *doc;     /**< 填充阶段：结果文档，同时是池首地址 */
    cjson_value *nodes;   /**< 填充阶段：节点数组 */
    cjson_value **slots;  /**< 填充阶段：所有容器共用的子项指针槽 */
    char        *strings; /**< 填充阶段：字符串区 */
    size_t       next_node;   /**< 填充阶段：下一个可用节点索引 */
    size_t       next_slot;   /**< 填充阶段：下一个可用槽位索引 */
    size_t       next_string; /**< 填充阶段：字符串区写入位置 */

    size_t root_index; /**< 根节点索引 */

    parse_frame *frames;     /**< 显式解析栈 */
    size_t       frame_count; /**< 当前栈深 */
    size_t       frame_cap;   /**< 栈容量 */
} parser;

/**
 * @brief 判断字符是否为十进制数字。
 * @param c 待判断字符。
 * @return 是数字返回 1，否则返回 0。
 */
static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/**
 * @brief 判断字符是否为 JSON 允许的空白字符。
 * @param c 待判断字符。
 * @return 是空白返回 1，否则返回 0。
 */
static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * @brief 执行可检测溢出的 size_t 加法。
 * @param a 第一个加数。
 * @param b 第二个加数。
 * @param out 计算结果输出地址。
 * @return 没有溢出返回 1；溢出返回 0。
 */
static int add_size(size_t a, size_t b, size_t *out)
{
    /* a + b 超过 SIZE_MAX 时拒绝计算，避免池大小计算回绕。 */
    if (a > SIZE_MAX - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

/**
 * @brief 执行可检测溢出的 size_t 乘法。
 * @param a 第一个乘数。
 * @param b 第二个乘数。
 * @param out 计算结果输出地址。
 * @return 没有溢出返回 1；溢出返回 0。
 */
static int mul_size(size_t a, size_t b, size_t *out)
{
    /* 非零乘数才需要检查，避免除以 0。 */
    if (a != 0 && b > SIZE_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

/**
 * @brief 把大小向上对齐到 8 字节。
 * @param v 原始大小。
 * @param out 对齐后的结果输出地址。
 * @return 没有溢出返回 1；溢出返回 0。
 */
static int align8_size(size_t v, size_t *out)
{
    /* 加 7 可能溢出，需要先检查。 */
    if (v > SIZE_MAX - 7) {
        return 0;
    }
    *out = (v + 7) & ~(size_t)7;
    return 1;
}

/**
 * @brief 设置不包含源码位置的错误（内存/文件类错误）。
 * @param err 错误结构；可以为 NULL。
 * @param code 错误码。
 * @return 无。
 */
static void set_plain_error(cjson_error *err, cjson_error_code code)
{
    if (err) {
        err->code = code;
        err->offset = 0;
        err->line = 0;
        err->column = 0;
    }
}

/**
 * @brief 记录带字节偏移的语法错误，并换算出所在行列。
 * @param p 解析器。
 * @param code 错误码。
 * @param offset 出错的字节偏移。
 * @return 固定返回 0，便于调用处直接 return fail_at(...)。
 */
static int fail_at(parser *p, cjson_error_code code, size_t offset)
{
    if (p->err) {
        size_t line = 1;
        size_t column = 1;
        size_t i;

        /* 偏移不能超过输入长度，否则回扫会越界。 */
        if (offset > p->len) {
            offset = p->len;
        }

        /* 从开头扫到出错位置，统计 '\n' 数量及最后一行的列号。 */
        for (i = 0; i < offset; i++) {
            if (p->text[i] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }

        p->err->code = code;
        p->err->offset = offset;
        p->err->line = line;
        p->err->column = column;
    }
    return 0;
}

/**
 * @brief 记录内存不足错误。
 * @param p 解析器。
 * @return 固定返回 0。
 */
static int fail_oom(parser *p)
{
    set_plain_error(p->err, CJSON_ERR_OOM);
    return 0;
}

/**
 * @brief 跳过输入开头的 UTF-8 BOM。
 * @param p 解析器，调用前 pos 应为 0。
 * @return 无。
 */
static void skip_bom(parser *p)
{
    if (p->len >= 3 &&
        (unsigned char)p->text[0] == 0xEF &&
        (unsigned char)p->text[1] == 0xBB &&
        (unsigned char)p->text[2] == 0xBF) {
        p->pos = 3;
    }
}

/**
 * @brief 跳过空白和块注释。
 * @param p 解析器。
 * @return 成功返回 1；注释未闭合返回 0 并设置错误。
 */
static int skip_ws_comments(parser *p)
{
    for (;;) {
        /* 先吃掉连续空白。 */
        while (p->pos < p->len && is_ws((unsigned char)p->text[p->pos])) {
            p->pos++;
        }

        /* 遇到 "/" 且后面紧跟 "*" 时按块注释处理。 */
        if (p->len - p->pos >= 2 &&
            p->text[p->pos] == '/' && p->text[p->pos + 1] == '*') {
            size_t start = p->pos;

            p->pos += 2;
            /* 一直扫描到块注释结束符；不处理嵌套注释。 */
            while (p->pos + 1 < p->len &&
                   !(p->text[p->pos] == '*' && p->text[p->pos + 1] == '/')) {
                p->pos++;
            }
            if (p->pos + 1 >= p->len) {
                return fail_at(p, CJSON_ERR_UNTERMINATED_COMMENT, start);
            }
            p->pos += 2;
            continue; /* 注释后可能还有空白或注释。 */
        }
        return 1;
    }
}

/**
 * @brief 输出一个字节；统计阶段只计数，填充阶段同时写入。
 * @param dst 写指针地址；*dst 为 NULL 表示统计阶段。
 * @param count 已输出字节数，函数内递增。
 * @param b 要输出的字节。
 * @return 无。
 */
static void emit_byte(unsigned char **dst, size_t *count, unsigned char b)
{
    if (*dst) {
        **dst = b;
        (*dst)++;
    }
    (*count)++;
}

/**
 * @brief 把一个 Unicode 码点按 UTF-8 输出。
 * @param dst 写指针地址；*dst 为 NULL 表示统计阶段。
 * @param count 已输出字节数，函数内递增。
 * @param cp Unicode 码点。
 * @return 无。
 */
static void emit_cp(unsigned char **dst, size_t *count, unsigned int cp)
{
    if (cp <= 0x7F) {
        /* 1 字节：0xxxxxxx */
        emit_byte(dst, count, (unsigned char)cp);
    } else if (cp <= 0x7FF) {
        /* 2 字节：110xxxxx 10xxxxxx */
        emit_byte(dst, count, (unsigned char)(0xC0 | (cp >> 6)));
        emit_byte(dst, count, (unsigned char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        /* 3 字节：1110xxxx 10xxxxxx 10xxxxxx */
        emit_byte(dst, count, (unsigned char)(0xE0 | (cp >> 12)));
        emit_byte(dst, count, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        emit_byte(dst, count, (unsigned char)(0x80 | (cp & 0x3F)));
    } else {
        /* 4 字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
        emit_byte(dst, count, (unsigned char)(0xF0 | (cp >> 18)));
        emit_byte(dst, count, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
        emit_byte(dst, count, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
        emit_byte(dst, count, (unsigned char)(0x80 | (cp & 0x3F)));
    }
}

/**
 * @brief 读取 4 位十六进制数字。
 * @param p 解析器。
 * @param pos 第一个十六进制数字的位置。
 * @param out 解析出的 16 位数值输出地址。
 * @return 4 位都是合法十六进制数字返回 1，否则返回 0。
 */
static int read_hex4(const parser *p, size_t pos, unsigned int *out)
{
    unsigned int value = 0;
    size_t i;

    /* 剩余长度不足 4 位时不读取。 */
    if (pos > p->len || p->len - pos < 4) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)p->text[pos + i];
        unsigned int digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned int)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned int)(c - 'A' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    *out = value;
    return 1;
}

/**
 * @brief 解析一个 JSON 字符串字面量（包括对象键）。
 * @param p 解析器，pos 必须指向起始双引号。
 * @param out_data 解码后的字符串首地址输出；统计阶段返回 NULL。
 * @param out_len 解码后的字节数输出。
 * @return 成功返回 1；失败返回 0 并设置 p->err。
 */
static int parse_string(parser *p, const char **out_data, size_t *out_len)
{
    char *start = NULL;
    unsigned char *dst = NULL;
    size_t count = 0;

    /* 调用者必须把 pos 停在双引号上。 */
    if (p->pos >= p->len || p->text[p->pos] != '"') {
        return fail_at(p, CJSON_ERR_INVALID_STRING, p->pos);
    }
    if (p->build) {
        /* 填充阶段：字符串直接写到池内字符串区的当前位置。 */
        start = p->strings + p->next_string;
        dst = (unsigned char *)start;
    }
    p->pos++;

    for (;;) {
        unsigned char c;

        /* 输入在闭合引号前结束。 */
        if (p->pos >= p->len) {
            return fail_at(p, CJSON_ERR_INVALID_STRING, p->pos);
        }
        c = (unsigned char)p->text[p->pos++];

        /* 遇到闭合引号，字符串结束。 */
        if (c == '"') {
            break;
        }
        /* JSON 不允许未转义的控制字符。 */
        if (c < 0x20) {
            return fail_at(p, CJSON_ERR_INVALID_STRING, p->pos - 1);
        }
        /* 普通字节（包括原始 UTF-8）直接透传。 */
        if (c != '\\') {
            emit_byte(&dst, &count, c);
            continue;
        }

        /* 处理反斜杠后的转义字符。 */
        if (p->pos >= p->len) {
            return fail_at(p, CJSON_ERR_INVALID_ESCAPE, p->pos);
        }
        c = (unsigned char)p->text[p->pos++];
        switch (c) {
        case '"':
            emit_byte(&dst, &count, '"');
            break;
        case '\\':
            emit_byte(&dst, &count, '\\');
            break;
        case '/':
            emit_byte(&dst, &count, '/');
            break;
        case 'b':
            emit_byte(&dst, &count, 0x08);
            break;
        case 'f':
            emit_byte(&dst, &count, 0x0C);
            break;
        case 'n':
            emit_byte(&dst, &count, 0x0A);
            break;
        case 'r':
            emit_byte(&dst, &count, 0x0D);
            break;
        case 't':
            emit_byte(&dst, &count, 0x09);
            break;
        case 'u': {
            unsigned int cp;
            unsigned int lo;

            /* \uXXXX：先读出 16 位码元。 */
            if (!read_hex4(p, p->pos, &cp)) {
                return fail_at(p, CJSON_ERR_INVALID_UNICODE, p->pos);
            }
            p->pos += 4;

            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* 高代理项：只有紧跟合法的低代理项时才合并成一个码点。 */
                if (p->len - p->pos >= 6 &&
                    p->text[p->pos] == '\\' && p->text[p->pos + 1] == 'u' &&
                    read_hex4(p, p->pos + 2, &lo) &&
                    lo >= 0xDC00 && lo <= 0xDFFF) {
                    unsigned int full = 0x10000u +
                                        ((cp - 0xD800u) << 10) +
                                        (lo - 0xDC00u);
                    p->pos += 6;
                    emit_cp(&dst, &count, full);
                } else {
                    /* 孤立高代理项：替换为 U+FFFD，继续解析后面的内容。 */
                    emit_cp(&dst, &count, 0xFFFDu);
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                /* 孤立低代理项：替换为 U+FFFD。 */
                emit_cp(&dst, &count, 0xFFFDu);
            } else {
                /* 普通 BMP 码点。 */
                emit_cp(&dst, &count, cp);
            }
            break;
        }
        default:
            return fail_at(p, CJSON_ERR_INVALID_ESCAPE, p->pos - 1);
        }
    }

    if (p->build) {
        /* 填充阶段：补结束符并推进字符串区写指针。 */
        *dst = '\0';
        *out_data = start;
        p->next_string += count + 1;
    } else {
        /* 统计阶段：只累计解码后的字节数（含结尾 '\0'）。 */
        if (p->string_bytes > SIZE_MAX - count - 1) {
            return fail_oom(p);
        }
        *out_data = NULL;
        p->string_bytes += count + 1;
    }
    *out_len = count;
    return 1;
}

/**
 * @brief 给计数器加一，但把值限制在 2000000 以内，避免极端输入导致计数溢出。
 * @param value 计数器地址。
 * @return 无。
 */
static void count_up(size_t *value)
{
    if (*value < 2000000u) {
        (*value)++;
    }
}

/**
 * @brief 计算 10 的整数次幂。
 * @param exp 非负指数。
 * @return 10 的 exp 次幂；结果超出 double 范围时按 IEEE 规则得到无穷大。
 */
static double pow10_u(unsigned long exp)
{
    double result = 1.0;
    double base = 10.0;

    /* 反复平方法：按二进制位累乘，避免循环 exp 次。 */
    while (exp != 0) {
        if (exp & 1ul) {
            result *= base;
        }
        base *= base;
        exp >>= 1;
    }
    return result;
}

/**
 * @brief 把已经通过语法校验的数字 token 转成 double。
 *
 * 不使用 strtod，因此不受 locale 影响。最多保留 19 位有效数字，
 * 超出 double 上限时饱和为 ±DBL_MAX，下溢时饱和为 ±0.0。
 *
 * @param s 数字 token 首地址。
 * @param n token 字节数。
 * @return 转换后的 double 值。
 */
static double convert_double(const char *s, size_t n)
{
    size_t i = 0;
    int negative = 0;
    uint64_t mantissa = 0;
    size_t significant = 0;
    size_t dropped = 0;
    size_t fraction_digits = 0;
    int started = 0;
    long long explicit_exp = 0;
    long long e;
    long long frac_c;
    long long dropped_c;
    double magnitude;

    /* 处理负号。 */
    if (i < n && s[i] == '-') {
        negative = 1;
        i++;
    }

    /* 逐位读整数部分：跳过前导零，最多累计 19 位有效数字。 */
    while (i < n && s[i] != '.' && s[i] != 'e' && s[i] != 'E') {
        unsigned int d = (unsigned int)(s[i] - '0');
        if (d != 0 || started) {
            started = 1;
            if (significant < 19) {
                mantissa = mantissa * 10u + d;
                significant++;
            } else {
                /* 第 19 位之后的数字只影响十进制指数。 */
                count_up(&dropped);
            }
        }
        i++;
    }

    /* 小数部分：统计小数位数；有效数字同样最多取 19 位。 */
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] != 'e' && s[i] != 'E') {
            unsigned int d = (unsigned int)(s[i] - '0');
            count_up(&fraction_digits);
            if (d != 0 || started) {
                started = 1;
                if (significant < 19) {
                    mantissa = mantissa * 10u + d;
                    significant++;
                } else {
                    count_up(&dropped);
                }
            }
            i++;
        }
    }

    /* 指数部分：用饱和累加，避免指数位数过多造成整数溢出。 */
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        int exp_negative = 0;
        long long exp_value = 0;

        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            exp_negative = (s[i] == '-');
            i++;
        }
        while (i < n) {
            unsigned int d = (unsigned int)(s[i] - '0');
            if (exp_value < 1000000) {
                exp_value = exp_value * 10 + (long long)d;
            }
            if (exp_value > 1000000) {
                exp_value = 1000000;
            }
            i++;
        }
        explicit_exp = exp_negative ? -exp_value : exp_value;
    }

    /* 全零数字直接返回带符号的 0，避免后面出现 0 * inf。 */
    if (!started || mantissa == 0) {
        return negative ? -0.0 : 0.0;
    }

    /* 综合显式指数、小数位数和被丢弃的有效数字，得到尾数的十进制指数。 */
    frac_c = (fraction_digits > 2000000u) ? 2000000 : (long long)fraction_digits;
    dropped_c = (dropped > 2000000u) ? 2000000 : (long long)dropped;
    e = explicit_exp - frac_c + dropped_c;

    /* 超出 10^308 量级必然溢出，提前饱和，避免计算无穷大因子。 */
    if (e > 308) {
        return negative ? -DBL_MAX : DBL_MAX;
    }
    /* 小于 10^-400 量级必然下溢为零。 */
    if (e < -400) {
        return negative ? -0.0 : 0.0;
    }

    if (e >= 0) {
        /* 正指数：尾数乘以 10^e；若乘积溢出，后面再钳制到 DBL_MAX。 */
        magnitude = (double)mantissa * pow10_u((unsigned long)e);
    } else {
        /* 负指数：分块除法，避免 10^|e| 本身先溢出成无穷大。 */
        long long remaining = -e;

        magnitude = (double)mantissa;
        while (remaining > 0) {
            unsigned long chunk = (remaining > 308) ? 308ul : (unsigned long)remaining;
            magnitude /= pow10_u(chunk);
            remaining -= (long long)chunk;
            if (magnitude == 0.0) {
                break;
            }
        }
    }

    /* 乘法可能产生无穷大，统一钳制回 double 最大值。 */
    if (magnitude > DBL_MAX) {
        magnitude = DBL_MAX;
    }
    return negative ? -magnitude : magnitude;
}

/**
 * @brief 把纯整数 token 转成 int64_t。
 * @param s 数字 token 首地址（无小数点和指数）。
 * @param n token 字节数。
 * @param out 成功时写入转换结果。
 * @return 能放入 int64_t 返回 1；溢出返回 0。
 */
static int convert_int64(const char *s, size_t n, int64_t *out)
{
    size_t i = 0;
    int negative = 0;
    uint64_t magnitude = 0;
    uint64_t limit;

    if (i < n && s[i] == '-') {
        negative = 1;
        i++;
    }

    /* 负数允许到 2^63，正数只能到 2^63 - 1。 */
    limit = negative ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    for (; i < n; i++) {
        unsigned int d = (unsigned int)(s[i] - '0');

        /* 在相乘之前检查，防止 uint64_t 回绕。 */
        if (magnitude > (limit - d) / 10u) {
            return 0;
        }
        magnitude = magnitude * 10u + d;
    }

    if (negative) {
        if (magnitude == (uint64_t)INT64_MAX + 1u) {
            /* -2^63 不能直接取相反数，需要单独处理。 */
            *out = INT64_MIN;
        } else {
            *out = -(int64_t)magnitude;
        }
    } else {
        *out = (int64_t)magnitude;
    }
    return 1;
}

/**
 * @brief 按 JSON 文法扫描一个数字 token。
 * @param p 解析器，pos 指向数字首字符。
 * @param is_float 输出是否包含小数点或指数。
 * @param token_start 输出 token 起始偏移。
 * @param token_len 输出 token 字节数。
 * @return 成功返回 1；格式错误返回 0 并设置 p->err。
 */
static int scan_number(parser *p, int *is_float, size_t *token_start, size_t *token_len)
{
    size_t i = p->pos;
    size_t start = i;
    int floating = 0;

    /* 可选的负号。 */
    if (i < p->len && p->text[i] == '-') {
        i++;
    }
    if (i >= p->len || !is_digit((unsigned char)p->text[i])) {
        return fail_at(p, CJSON_ERR_INVALID_NUMBER, i);
    }

    /* 整数部分："0" 后面不能直接跟数字，其他情况读取连续数字。 */
    if (p->text[i] == '0') {
        i++;
        if (i < p->len && is_digit((unsigned char)p->text[i])) {
            return fail_at(p, CJSON_ERR_INVALID_NUMBER, i);
        }
    } else {
        while (i < p->len && is_digit((unsigned char)p->text[i])) {
            i++;
        }
    }

    /* 可选的小数部分，小数点后必须至少有一位数字。 */
    if (i < p->len && p->text[i] == '.') {
        floating = 1;
        i++;
        if (i >= p->len || !is_digit((unsigned char)p->text[i])) {
            return fail_at(p, CJSON_ERR_INVALID_NUMBER, i);
        }
        while (i < p->len && is_digit((unsigned char)p->text[i])) {
            i++;
        }
    }

    /* 可选的指数部分，e/E 后允许符号，但必须至少有一位数字。 */
    if (i < p->len && (p->text[i] == 'e' || p->text[i] == 'E')) {
        floating = 1;
        i++;
        if (i < p->len && (p->text[i] == '+' || p->text[i] == '-')) {
            i++;
        }
        if (i >= p->len || !is_digit((unsigned char)p->text[i])) {
            return fail_at(p, CJSON_ERR_INVALID_NUMBER, i);
        }
        while (i < p->len && is_digit((unsigned char)p->text[i])) {
            i++;
        }
    }

    *is_float = floating;
    *token_start = start;
    *token_len = i - start;
    p->pos = i;
    return 1;
}

/**
 * @brief 确保 child_counts 数组能容纳指定节点索引。
 * @param p 解析器。
 * @param index 需要访问的节点索引。
 * @return 成功返回 1；内存不足返回 0。
 */
static int ensure_counts(parser *p, size_t index)
{
    size_t new_cap;
    size_t *new_counts;

    if (index < p->child_counts_cap) {
        return 1;
    }

    /* 容量按倍数增长，至少扩到能覆盖 index。 */
    new_cap = p->child_counts_cap ? p->child_counts_cap : 32;
    while (new_cap <= index) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = index + 1;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(size_t)) {
        return fail_oom(p);
    }

    new_counts = (size_t *)realloc(p->child_counts, new_cap * sizeof(size_t));
    if (!new_counts) {
        return fail_oom(p);
    }
    p->child_counts = new_counts;
    p->child_counts_cap = new_cap;
    return 1;
}

/**
 * @brief 压入一个容器解析栈帧。
 * @param p 解析器。
 * @param node_index 容器节点索引。
 * @param type 容器类型（数组或对象）。
 * @param state 初始状态。
 * @return 成功返回 1；内存不足返回 0。
 */
static int push_frame(parser *p, size_t node_index, cjson_type type, frame_state state)
{
    parse_frame *frame;

    /* 栈满时按倍数扩容。 */
    if (p->frame_count == p->frame_cap) {
        size_t new_cap = p->frame_cap ? p->frame_cap * 2 : 16;
        parse_frame *new_frames;

        if (new_cap < p->frame_cap ||
            new_cap > SIZE_MAX / sizeof(parse_frame)) {
            return fail_oom(p);
        }
        new_frames = (parse_frame *)realloc(p->frames, new_cap * sizeof(parse_frame));
        if (!new_frames) {
            return fail_oom(p);
        }
        p->frames = new_frames;
        p->frame_cap = new_cap;
    }

    /* 初始化栈帧。 */
    frame = &p->frames[p->frame_count];
    frame->node_index = node_index;
    frame->count = 0;
    frame->state = state;
    frame->type = type;
    frame->key = NULL;
    frame->key_len = 0;
    p->frame_count++;
    if (p->frame_count > p->max_depth) {
        p->max_depth = p->frame_count;
    }
    return 1;
}

/**
 * @brief 登记一个新值节点。
 * @param p 解析器。
 * @param type 节点类型。
 * @param key 对象成员的键；数组元素/根节点为 NULL。
 * @param key_len 键长度。
 * @param out_index 节点索引输出地址。
 * @return 成功返回 1；内存不足返回 0。
 */
static int start_value(parser *p, cjson_type type,
                       const char *key, size_t key_len, size_t *out_index)
{
    size_t index;

    if (p->build) {
        /* 填充阶段：直接从节点数组取下一个槽位并写入类型/键。 */
        index = p->next_node++;
        p->nodes[index].type = type;
        p->nodes[index].key = key;
        p->nodes[index].key_len = key_len;
    } else {
        /* 统计阶段：只递增节点计数，并保证计数数组有对应位置。 */
        index = p->value_count++;
        if (!ensure_counts(p, index)) {
            return 0;
        }
        p->child_counts[index] = 0;
    }

    *out_index = index;
    return 1;
}

/**
 * @brief 把一个已完成的子节点挂到当前栈顶容器。
 * @param p 解析器。
 * @param child_index 子节点索引。
 * @return 成功返回 1；根节点没有父容器时也返回 1。
 */
static int append_child(parser *p, size_t child_index)
{
    parse_frame *frame;

    /* 栈为空表示当前完成的是根节点，不需要挂到父容器。 */
    if (p->frame_count == 0) {
        return 1;
    }
    frame = &p->frames[p->frame_count - 1];

    if (p->build) {
        /* 填充阶段：把子节点指针写进父容器预留的 slots 区。 */
        p->nodes[frame->node_index].u.container.items[frame->count] =
            &p->nodes[child_index];
    }
    frame->count++;
    return 1;
}

/**
 * @brief 关闭当前栈顶容器。
 * @param p 解析器。
 * @return 成功返回 1；栈为空返回 0。
 */
static int close_container(parser *p)
{
    parse_frame frame;

    if (p->frame_count == 0) {
        return 0;
    }
    frame = p->frames[p->frame_count - 1];
    p->frame_count--;

    if (p->build) {
        /* 填充阶段：记录最终子项数。 */
        p->nodes[frame.node_index].u.container.count = frame.count;
    } else {
        /* 统计阶段：保存子项数，供第二遍预留 slots。 */
        p->child_counts[frame.node_index] = frame.count;
    }

    /* 容器本身作为子节点挂到父容器（根容器则无操作）。 */
    return append_child(p, frame.node_index);
}

/**
 * @brief 打开一个数组或对象容器。
 * @param p 解析器。
 * @param type CJSON_ARRAY 或 CJSON_OBJECT。
 * @param key 该容器作为对象成员时的键；否则为 NULL。
 * @param key_len 键长度。
 * @param out_index 容器节点索引输出地址。
 * @return 成功返回 1；失败返回 0。
 */
static int open_container(parser *p, cjson_type type,
                          const char *key, size_t key_len, size_t *out_index)
{
    size_t index;

    if (!start_value(p, type, key, key_len, &index)) {
        return 0;
    }

    if (p->build) {
        cjson_value *value = &p->nodes[index];
        size_t child_count = p->child_counts[index];

        /* 按第一遍统计出的子项数，从全局 slots 区预留一段连续指针数组。 */
        value->u.container.items = p->slots + p->next_slot;
        value->u.container.count = 0;
        p->next_slot += child_count;
    }

    /* 压栈，后续由 step_container 按状态机解析容器内容。 */
    if (!push_frame(p, index, type,
                    type == CJSON_ARRAY ? FRAME_ARR_OPEN : FRAME_OBJ_OPEN)) {
        return 0;
    }
    *out_index = index;
    return 1;
}

/**
 * @brief 匹配一个 JSON 字面量（true/false/null）。
 * @param p 解析器。
 * @param literal 期望的字面量字符串。
 * @param n 字面量长度。
 * @return 成功返回 1；输入不足或内容不匹配返回 0。
 */
static int match_literal(parser *p, const char *literal, size_t n)
{
    if (p->len - p->pos < n) {
        return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
    }
    if (memcmp(p->text + p->pos, literal, n) != 0) {
        return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);
    }
    p->pos += n;
    return 1;
}

/**
 * @brief 解析一个 JSON 值，并把标量立即挂到父容器。
 * @param p 解析器。
 * @param key 值作为对象成员时的键；数组元素/根节点为 NULL。
 * @param key_len 键长度。
 * @param out_index 值节点索引输出地址。
 * @return 成功返回 1；失败返回 0。
 */
static int parse_value(parser *p, const char *key, size_t key_len, size_t *out_index)
{
    unsigned char c;
    size_t index;

    /* 值前面允许空白和注释。 */
    if (!skip_ws_comments(p)) {
        return 0;
    }
    if (p->pos >= p->len) {
        return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
    }

    c = (unsigned char)p->text[p->pos];

    /* 数组和对象：消耗起始符号后打开容器，由栈状态机继续解析。 */
    if (c == '{' || c == '[') {
        p->pos++;
        return open_container(p, c == '{' ? CJSON_OBJECT : CJSON_ARRAY,
                              key, key_len, out_index);
    }

    /* 字符串值。 */
    if (c == '"') {
        const char *data;
        size_t len;

        if (!parse_string(p, &data, &len)) {
            return 0;
        }
        if (!start_value(p, CJSON_STRING, key, key_len, &index)) {
            return 0;
        }
        if (p->build) {
            p->nodes[index].u.string.data = data;
            p->nodes[index].u.string.len = len;
        }
        if (!append_child(p, index)) {
            return 0;
        }
        *out_index = index;
        return 1;
    }

    /* true / false / null 字面量。 */
    if (c == 't' || c == 'f' || c == 'n') {
        const char *literal;
        size_t literal_len;
        cjson_type type;
        int boolean = 0;

        if (c == 't') {
            literal = "true";
            literal_len = 4;
            type = CJSON_BOOL;
            boolean = 1;
        } else if (c == 'f') {
            literal = "false";
            literal_len = 5;
            type = CJSON_BOOL;
        } else {
            literal = "null";
            literal_len = 4;
            type = CJSON_NULL;
        }
        if (!match_literal(p, literal, literal_len)) {
            return 0;
        }
        if (!start_value(p, type, key, key_len, &index)) {
            return 0;
        }
        if (p->build && type == CJSON_BOOL) {
            p->nodes[index].u.boolean = boolean;
        }
        if (!append_child(p, index)) {
            return 0;
        }
        *out_index = index;
        return 1;
    }

    /* 数字：语法扫描完成后，填充阶段再决定 int64 还是 double。 */
    if (c == '-' || (c >= '0' && c <= '9')) {
        int is_float = 0;
        size_t token_start;
        size_t token_len;
        int64_t integer_value = 0;
        double double_value = 0.0;
        cjson_type type;

        if (!scan_number(p, &is_float, &token_start, &token_len)) {
            return 0;
        }

        type = is_float ? CJSON_DOUBLE : CJSON_INT;
        if (p->build) {
            if (!is_float &&
                convert_int64(p->text + token_start, token_len, &integer_value)) {
                type = CJSON_INT;
            } else {
                /* 有小数/指数，或整数溢出 int64_t，都退化为 double。 */
                type = CJSON_DOUBLE;
                double_value = convert_double(p->text + token_start, token_len);
            }
        }

        if (!start_value(p, type, key, key_len, &index)) {
            return 0;
        }
        if (p->build) {
            if (type == CJSON_INT) {
                p->nodes[index].u.integer = integer_value;
            } else {
                p->nodes[index].u.number = double_value;
            }
        }
        if (!append_child(p, index)) {
            return 0;
        }
        *out_index = index;
        return 1;
    }

    return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);
}

/**
 * @brief 推进栈顶容器的一步解析。
 * @param p 解析器。
 * @return 成功返回 1；失败返回 0。
 */
static int step_container(parser *p)
{
    parse_frame *frame;

    if (p->frame_count == 0) {
        return 0;
    }
    frame = &p->frames[p->frame_count - 1];

    if (frame->type == CJSON_ARRAY) {
        switch (frame->state) {
        case FRAME_ARR_OPEN:
        case FRAME_ARR_COMMA:
            if (!skip_ws_comments(p)) {
                return 0;
            }
            if (p->pos >= p->len) {
                return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
            }

            /* 只有刚打开时允许空数组；逗号后直接 ']' 是尾逗号错误。 */
            if (p->text[p->pos] == ']') {
                if (frame->state == FRAME_ARR_COMMA) {
                    return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);
                }
                p->pos++;
                return close_container(p);
            }

            /* 先切换到“已读取值”状态，再解析子值；
             * parse_value 可能压栈并导致 frame 指针失效，因此不能再访问 frame。 */
            frame->state = FRAME_ARR_VALUE;
            {
                size_t child;

                if (!parse_value(p, NULL, 0, &child)) {
                    return 0;
                }
            }
            return 1;

        case FRAME_ARR_VALUE:
            if (!skip_ws_comments(p)) {
                return 0;
            }
            if (p->pos >= p->len) {
                return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
            }
            if (p->text[p->pos] == ',') {
                p->pos++;
                frame->state = FRAME_ARR_COMMA;
                return 1;
            }
            if (p->text[p->pos] == ']') {
                p->pos++;
                return close_container(p);
            }
            return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);

        default:
            return 0;
        }
    }

    switch (frame->state) {
    case FRAME_OBJ_OPEN:
    case FRAME_OBJ_COMMA:
        if (!skip_ws_comments(p)) {
            return 0;
        }
        if (p->pos >= p->len) {
            return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
        }

        /* 只有刚打开时允许空对象；逗号后直接 '}' 是尾逗号错误。 */
        if (p->text[p->pos] == '}') {
            if (frame->state == FRAME_OBJ_COMMA) {
                return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);
            }
            p->pos++;
            return close_container(p);
        }
        if (p->text[p->pos] != '"') {
            return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);
        }
        {
            const char *key;
            size_t key_len;

            /* 先解析键，暂存在栈帧里，等冒号和值都读完后交给子节点。 */
            if (!parse_string(p, &key, &key_len)) {
                return 0;
            }
            frame->key = key;
            frame->key_len = key_len;
            frame->state = FRAME_OBJ_KEY;
        }
        return 1;

    case FRAME_OBJ_KEY:
        if (!skip_ws_comments(p)) {
            return 0;
        }
        if (p->pos >= p->len) {
            return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
        }
        if (p->text[p->pos] != ':') {
            return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);
        }
        p->pos++;
        frame->state = FRAME_OBJ_COLON;
        return 1;

    case FRAME_OBJ_COLON:
        {
            const char *key = frame->key;
            size_t key_len = frame->key_len;
            size_t child;

            /* 取出待处理的键并清空栈帧字段，防止重复使用。 */
            frame->key = NULL;
            frame->key_len = 0;
            frame->state = FRAME_OBJ_VALUE;

            /* 解析值可能压栈，因此之后不再使用 frame 指针。 */
            return parse_value(p, key, key_len, &child);
        }

    case FRAME_OBJ_VALUE:
        if (!skip_ws_comments(p)) {
            return 0;
        }
        if (p->pos >= p->len) {
            return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
        }
        if (p->text[p->pos] == ',') {
            p->pos++;
            frame->state = FRAME_OBJ_COMMA;
            return 1;
        }
        if (p->text[p->pos] == '}') {
            p->pos++;
            return close_container(p);
        }
        return fail_at(p, CJSON_ERR_UNEXPECTED_CHAR, p->pos);

    default:
        return 0;
    }
}

/**
 * @brief 执行一遍完整解析。
 * @param p 解析器。
 * @param build 0 表示统计/校验阶段，1 表示填充阶段。
 * @return 成功返回 1；失败返回 0 并设置 p->err。
 */
static int run_parse(parser *p, int build)
{
    size_t root_index;

    /* 两遍解析都从输入开头开始，重置所有扫描/填充游标。 */
    p->build = build;
    p->pos = 0;
    p->frame_count = 0;
    p->next_node = 0;
    p->next_slot = 0;
    p->next_string = 0;
    p->root_index = 0;

    skip_bom(p);
    if (!skip_ws_comments(p)) {
        return 0;
    }
    if (p->pos >= p->len) {
        return fail_at(p, CJSON_ERR_UNEXPECTED_EOF, p->pos);
    }

    /* 先解析根值；如果是容器，后续循环会继续推进它的子项。 */
    if (!parse_value(p, NULL, 0, &root_index)) {
        return 0;
    }
    while (p->frame_count > 0) {
        if (!step_container(p)) {
            return 0;
        }
    }

    /* 根值结束后只允许空白/注释，不能有其他内容。 */
    if (!skip_ws_comments(p)) {
        return 0;
    }
    if (p->pos != p->len) {
        return fail_at(p, CJSON_ERR_TRAILING_CONTENT, p->pos);
    }

    p->root_index = root_index;
    return 1;
}

/**
 * @brief 根据统计结果分配解析结果唯一的一块内存，并切分各个区域。
 *
 * 布局：cjson_doc + 节点数组 + 子项指针槽 + 字符串区。
 *
 * @param p 解析器，value_count/string_bytes 必须已由统计阶段填好。
 * @return 成功返回 1；内存不足或大小溢出返回 0。
 */
static int allocate_doc(parser *p)
{
    size_t offset;
    size_t nodes_offset;
    size_t slots_offset;
    size_t strings_offset;
    size_t nodes_bytes;
    size_t slots_bytes;
    size_t total;
    size_t node_count = p->value_count;
    size_t slot_count;
    unsigned char *block;

    /* 合法文档至少有一个根值。 */
    if (node_count == 0) {
        return fail_oom(p);
    }
    /* 除根节点外，每个值都对应父容器的一个子项槽位。 */
    slot_count = node_count - 1;

    /* 计算节点区位置和大小。 */
    if (!align8_size(sizeof(cjson_doc), &offset) ||
        !mul_size(node_count, sizeof(cjson_value), &nodes_bytes)) {
        return fail_oom(p);
    }
    nodes_offset = offset;
    if (!add_size(offset, nodes_bytes, &offset) ||
        !align8_size(offset, &offset)) {
        return fail_oom(p);
    }

    /* 计算子项指针槽区位置和大小。 */
    slots_offset = offset;
    if (!mul_size(slot_count, sizeof(cjson_value *), &slots_bytes) ||
        !add_size(offset, slots_bytes, &offset) ||
        !align8_size(offset, &offset)) {
        return fail_oom(p);
    }

    /* 字符串区放在最后。 */
    strings_offset = offset;
    if (!add_size(offset, p->string_bytes, &total)) {
        return fail_oom(p);
    }

    /* 整份结果只分配一次。 */
    block = (unsigned char *)malloc(total ? total : 1);
    if (!block) {
        return fail_oom(p);
    }

    /* 把块首当作文档头，后面按偏移切出节点、槽位和字符串区。 */
    p->doc = (cjson_doc *)block;
    p->doc->root = NULL;
    p->doc->doc_memory_size = total;
    p->nodes = p->doc->nodes = (cjson_value *)(void *)(block + nodes_offset);
    p->slots = p->doc->slots = (cjson_value **)(void *)(block + slots_offset);
    p->strings = p->doc->strings = (char *)(block + strings_offset);
    p->next_node = 0;
    p->next_slot = 0;
    p->next_string = 0;
    return 1;
}

/**
 * @brief 从内存缓冲解析 JSON 文档。
 * @param text JSON 文本首地址，不要求以 '\0' 结尾。
 * @param len 文本字节数。
 * @param err 可选的错误输出；可以为 NULL。
 * @return 成功返回文档指针；失败返回 NULL。
 */
cjson_doc *cjson_parse(const char *text, size_t len, cjson_error *err)
{
    parser p;

    /* 初始化上下文和错误输出。 */
    memset(&p, 0, sizeof(p));
    if (err) {
        err->code = CJSON_OK;
        err->offset = 0;
        err->line = 0;
        err->column = 0;
    }
    if (!text) {
        if (err) {
            err->code = CJSON_ERR_UNEXPECTED_EOF;
            err->offset = 0;
            err->line = 1;
            err->column = 1;
        }
        return NULL;
    }

    p.text = text;
    p.len = len;
    p.err = err;

    /* 第一遍：完整校验 + 统计节点/字符串/深度。 */
    if (!run_parse(&p, 0)) {
        goto fail;
    }

    /* 一次性分配结果内存池。 */
    if (!allocate_doc(&p)) {
        goto fail;
    }

    /* 第二遍：把节点、子项指针和字符串填充进池。 */
    if (!run_parse(&p, 1)) {
        goto fail;
    }

    /* 写入根节点指针，释放临时数组，返回结果。 */
    p.doc->root = &p.nodes[p.root_index];
    free(p.child_counts);
    free(p.frames);
    return p.doc;

fail:
    /* 任一阶段失败：释放结果池和临时数组。 */
    free(p.doc);
    free(p.child_counts);
    free(p.frames);
    return NULL;
}

/**
 * @brief 把整个文件读入可增长的内存缓冲。
 * @param path 文件路径。
 * @param out_len 读到的字节数输出地址。
 * @param err 可选的错误输出。
 * @return 成功返回缓冲区（不保证以 '\0' 结尾，但实际会补）；失败返回 NULL。
 */
static char *read_entire_file(const char *path, size_t *out_len, cjson_error *err)
{
    FILE *file;
    char *buffer;
    size_t capacity = 8192;
    size_t length = 0;

    if (!path) {
        set_plain_error(err, CJSON_ERR_FILE);
        return NULL;
    }

    /* 二进制模式避免 Windows 文本模式改写换行。 */
    file = fopen(path, "rb");
    if (!file) {
        set_plain_error(err, CJSON_ERR_FILE);
        return NULL;
    }

    buffer = (char *)malloc(capacity + 1);
    if (!buffer) {
        fclose(file);
        set_plain_error(err, CJSON_ERR_OOM);
        return NULL;
    }

    for (;;) {
        size_t read_count;

        /* 缓冲满时扩容一倍，并额外留一个字节放 '\0'。 */
        if (length == capacity) {
            char *new_buffer;
            size_t new_capacity;

            if (capacity > SIZE_MAX / 2) {
                free(buffer);
                fclose(file);
                set_plain_error(err, CJSON_ERR_OOM);
                return NULL;
            }
            new_capacity = capacity * 2;
            new_buffer = (char *)realloc(buffer, new_capacity + 1);
            if (!new_buffer) {
                free(buffer);
                fclose(file);
                set_plain_error(err, CJSON_ERR_OOM);
                return NULL;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }

        read_count = fread(buffer + length, 1, capacity - length, file);
        length += read_count;
        if (read_count == 0) {
            break;
        }
    }

    if (ferror(file)) {
        free(buffer);
        fclose(file);
        set_plain_error(err, CJSON_ERR_FILE);
        return NULL;
    }
    fclose(file);

    /* 方便调试/打印，但不影响解析（解析使用长度）。 */
    buffer[length] = '\0';
    *out_len = length;
    return buffer;
}

/**
 * @brief 从文件读取 JSON 并解析。
 * @param path 文件路径。
 * @param err 可选的错误输出；可以为 NULL。
 * @return 成功返回文档指针；失败返回 NULL。
 */
cjson_doc *cjson_parse_file(const char *path, cjson_error *err)
{
    char *buffer;
    size_t length;
    cjson_doc *doc;

    if (err) {
        err->code = CJSON_OK;
        err->offset = 0;
        err->line = 0;
        err->column = 0;
    }

    /* 文件缓冲只在解析期间使用，结果文档会复制所有需要的数据。 */
    buffer = read_entire_file(path, &length, err);
    if (!buffer) {
        return NULL;
    }
    doc = cjson_parse(buffer, length, err);
    free(buffer);
    return doc;
}

/**
 * @brief 释放文档占用的整块内存。
 * @param doc 文档指针；可以为 NULL。
 * @return 无。
 */
void cjson_free(cjson_doc *doc)
{
    free(doc);
}

/**
 * @brief 取得文档根节点。
 * @param doc 文档指针；可以为 NULL。
 * @return 根节点指针；doc 为 NULL 时返回 NULL。
 */
cjson_value *cjson_root(const cjson_doc *doc)
{
    return doc ? doc->root : NULL;
}

/**
 * @brief 在对象中按名字线性查找成员。
 * @param object 目标对象节点。
 * @param key 以 '\0' 结尾的键字符串。
 * @return 匹配成员；重复键返回最后一个；找不到或参数非法返回 NULL。
 */
cjson_value *cjson_object_get(const cjson_value *object, const char *key)
{
    cjson_value *found = NULL;
    size_t key_len;
    size_t i;

    /* 类型和参数检查，允许调用者直接对非对象使用。 */
    if (!object || object->type != CJSON_OBJECT || !key) {
        return NULL;
    }
    key_len = strlen(key);

    /* 顺序遍历，不提前返回，从而让重复键返回最后一个。 */
    for (i = 0; i < object->u.container.count; i++) {
        cjson_value *item = object->u.container.items[i];

        if (item->key && item->key_len == key_len &&
            memcmp(item->key, key, key_len) == 0) {
            found = item;
        }
    }
    return found;
}

/**
 * @brief 按索引取得数组元素。
 * @param array 目标数组节点。
 * @param index 从 0 开始的下标。
 * @return 元素节点；越界或参数非法返回 NULL。
 */
cjson_value *cjson_array_get(const cjson_value *array, size_t index)
{
    if (!array || array->type != CJSON_ARRAY ||
        index >= array->u.container.count) {
        return NULL;
    }
    return array->u.container.items[index];
}

/**
 * @brief 打印栈中的一项，记录节点和缩进层级。
 */
typedef struct print_entry {
    const cjson_value *value; /**< 待输出的节点 */
    size_t             depth; /**< 缩进层级 */
} print_entry;

/**
 * @brief 把节点压入打印栈，必要时扩容。
 * @param stack 打印栈指针的地址。
 * @param capacity 当前栈容量地址。
 * @param count 当前栈元素数地址。
 * @param value 要压入的节点。
 * @param depth 节点缩进层级。
 * @return 成功返回 1；内存不足返回 0。
 */
static int print_push(print_entry **stack, size_t *capacity, size_t *count,
                      const cjson_value *value, size_t depth)
{
    /* 栈满时按倍数扩容。 */
    if (*count == *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2 : 32;
        print_entry *new_stack;

        if (new_capacity > SIZE_MAX / sizeof(print_entry)) {
            return 0;
        }
        new_stack = (print_entry *)realloc(
            *stack, new_capacity * sizeof(print_entry));
        if (!new_stack) {
            return 0;
        }
        *stack = new_stack;
        *capacity = new_capacity;
    }

    (*stack)[*count].value = value;
    (*stack)[*count].depth = depth;
    (*count)++;
    return 1;
}

/**
 * @brief 以调试格式把一棵值树打印到标准输出。
 *
 * 使用显式栈而非递归，因此深嵌套文档也不会耗尽 C 调用栈。
 * 输出内容仅用于人工检查，不是合法的 JSON 序列化结果。
 *
 * @param value 要打印的根值；可以为 NULL。
 * @return 无。
 */
void cjson_print(const cjson_value *value)
{
    print_entry *stack = NULL;
    size_t capacity = 0;
    size_t count = 0;

    if (!value) {
        return;
    }
    if (!print_push(&stack, &capacity, &count, value, 0)) {
        return;
    }

    while (count > 0) {
        print_entry entry = stack[--count];
        const cjson_value *node = entry.value;
        size_t i;

        /* 按层级输出缩进。 */
        for (i = 0; i < entry.depth * 2; i++) {
            putchar(' ');
        }

        /* 对象成员先输出 "key: "。 */
        if (node->key) {
            fwrite(node->key, 1, node->key_len, stdout);
            fputs(": ", stdout);
        }

        /* 按类型输出节点内容或容器统计信息。 */
        switch (node->type) {
        case CJSON_NULL:
            fputs("null\n", stdout);
            break;
        case CJSON_BOOL:
            fputs(node->u.boolean ? "true\n" : "false\n", stdout);
            break;
        case CJSON_INT:
            printf("%lld\n", (long long)node->u.integer);
            break;
        case CJSON_DOUBLE:
            printf("%.17g\n", node->u.number);
            break;
        case CJSON_STRING:
            putchar('"');
            fwrite(node->u.string.data, 1, node->u.string.len, stdout);
            fputs("\"\n", stdout);
            break;
        case CJSON_ARRAY:
            printf("[array count=%llu]\n",
                   (unsigned long long)node->u.container.count);
            break;
        case CJSON_OBJECT:
            printf("{object count=%llu}\n",
                   (unsigned long long)node->u.container.count);
            break;
        }

        /* 容器子项逆序压栈，出栈时就能按原始顺序打印。 */
        if (node->type == CJSON_ARRAY || node->type == CJSON_OBJECT) {
            size_t n = node->u.container.count;

            for (i = n; i > 0; i--) {
                if (!print_push(&stack, &capacity, &count,
                                node->u.container.items[i - 1],
                                entry.depth + 1)) {
                    free(stack);
                    return;
                }
            }
        }
    }
    free(stack);
}

/**
 * @brief 返回错误码对应的英文描述。
 * @param code 错误码。
 * @return 指向静态字符串的描述。
 */
const char *cjson_error_string(cjson_error_code code)
{
    switch (code) {
    case CJSON_OK:
        return "no error";
    case CJSON_ERR_OOM:
        return "out of memory";
    case CJSON_ERR_FILE:
        return "file error";
    case CJSON_ERR_UNEXPECTED_EOF:
        return "unexpected end of input";
    case CJSON_ERR_UNEXPECTED_CHAR:
        return "unexpected character";
    case CJSON_ERR_INVALID_NUMBER:
        return "invalid number";
    case CJSON_ERR_INVALID_STRING:
        return "invalid string";
    case CJSON_ERR_INVALID_ESCAPE:
        return "invalid escape";
    case CJSON_ERR_INVALID_UNICODE:
        return "invalid unicode escape";
    case CJSON_ERR_UNTERMINATED_COMMENT:
        return "unterminated comment";
    case CJSON_ERR_TRAILING_CONTENT:
        return "trailing content";
    default:
        return "unknown error";
    }
}

/**
 * @brief 判断两个 double 是否在相对误差范围内相等。
 * @param a 实际值。
 * @param b 期望值。
 * @param relative 允许的相对误差。
 * @return 在范围内返回 1，否则返回 0。
 */
int nearly_equal(double a, double b, double relative)
{
    double diff;

    /* 期望值为 0 时使用精确比较，避免相对误差失去意义。 */
    if (b == 0.0) {
        return a == 0.0;
    }

    diff = a - b;
    if (diff < 0) {
        diff = -diff;
    }
    if (b < 0) {
        b = -b;
    }
    return diff <= relative * b;
}
