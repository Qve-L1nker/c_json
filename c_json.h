#ifndef C_JSON_H
#define C_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief JSON 值的类型标签。
 */
typedef enum cjson_type {
    CJSON_NULL = 0, /**< null 值 */
    CJSON_BOOL,     /**< true / false */
    CJSON_INT,      /**< 无小数、无指数且能放入 int64_t 的整数 */
    CJSON_DOUBLE,   /**< 浮点数，或超出 int64_t 范围的数字 */
    CJSON_STRING,   /**< 字符串 */
    CJSON_ARRAY,    /**< 数组 */
    CJSON_OBJECT    /**< 对象 */
} cjson_type;

typedef struct cjson_value cjson_value;

/**
 * @brief 解析结果中的单个 JSON 值节点。
 *
 * 节点、字符串和子项指针数组全部位于同一块内存池中，
 * 生命周期与所属的 cjson_doc 相同。
 */
struct cjson_value {
    cjson_type  type;    /**< 节点类型，决定联合体 u 中哪个成员有效 */
    const char *key;     /**< 对象成员的键；数组元素和根节点为 NULL */
    size_t      key_len; /**< 键的字节数（不含结尾 '\0'） */
    union {
        int     boolean; /**< CJSON_BOOL：0 为 false，非 0 为 true */
        int64_t integer; /**< CJSON_INT：整数取值 */
        double  number;  /**< CJSON_DOUBLE：浮点取值 */
        struct {
            const char *data; /**< 解码后的字符串首地址，末尾补 '\0' */
            size_t      len;  /**< 字符串字节数；data 内可能含 '\0'，以 len 为准 */
        } string;             /**< CJSON_STRING 的字符串数据 */
        struct {
            cjson_value **items; /**< 指向池内连续子节点指针数组 */
            size_t        count; /**< 子节点数量 */
        } container;             /**< CJSON_ARRAY / CJSON_OBJECT 的子项集合 */
    } u;
};

/**
 * @brief 解析错误码。
 */
typedef enum cjson_error_code {
    CJSON_OK = 0,                   /**< 没有错误 */
    CJSON_ERR_OOM,                  /**< 内存分配失败 */
    CJSON_ERR_FILE,                 /**< 文件打开、读取失败 */
    CJSON_ERR_UNEXPECTED_EOF,       /**< 输入意外结束 */
    CJSON_ERR_UNEXPECTED_CHAR,      /**< 遇到不符合语法的字符 */
    CJSON_ERR_INVALID_NUMBER,       /**< 数字格式非法 */
    CJSON_ERR_INVALID_STRING,       /**< 字符串未闭合或含未转义控制字符 */
    CJSON_ERR_INVALID_ESCAPE,       /**< 非法转义序列 */
    CJSON_ERR_INVALID_UNICODE,      /**< \uXXXX 不是四位十六进制数字 */
    CJSON_ERR_UNTERMINATED_COMMENT, /**< 块注释未闭合 */
    CJSON_ERR_TRAILING_CONTENT      /**< 根值之后还有非空白内容 */
} cjson_error_code;

/**
 * @brief 一次解析失败的详细信息。
 */
typedef struct cjson_error {
    cjson_error_code code; /**< 错误码 */
    size_t offset;         /**< 出错的字节偏移，从 0 开始 */
    size_t line;           /**< 出错行号，从 1 开始；OOM/FILE 错误时为 0 */
    size_t column;         /**< 出错列号，从 1 开始（按字节计）；OOM/FILE 时为 0 */
} cjson_error;

/**
 * @brief 解析结果文档句柄，同时也是结果内存池的起始地址。
 */
typedef struct cjson_doc {
    cjson_value *root; /**< 根节点指针，同时是池首地址；解析成功时非 NULL */
    cjson_value *nodes;   /**< 节点数组 */
    cjson_value **slots;  /**< 所有容器共用的子项指针槽 */
    char        *strings; /**< 字符串区 */
    size_t doc_memory_size; /**< 文档所占的池的内存大小 */
} cjson_doc;

/**
 * @brief 从内存中的 JSON 文本解析出文档。
 * @param text JSON 文本首地址，不要求以 '\0' 结尾。
 * @param len 文本字节数。
 * @param err 可选的错误输出；可以为 NULL。
 * @return 成功返回文档指针；失败返回 NULL。
 */
cjson_doc *cjson_parse(const char *text, size_t len, cjson_error *err);

/**
 * @brief 从文件读取 JSON 文本并解析出文档。
 * @param path 文件路径。
 * @param err 可选的错误输出；可以为 NULL。
 * @return 成功返回文档指针；失败返回 NULL。
 */
cjson_doc *cjson_parse_file(const char *path, cjson_error *err);

/**
 * @brief 释放解析结果占用的整块内存。
 * @param doc 要释放的文档；可以为 NULL。
 * @return 无。
 */
void cjson_free(cjson_doc *doc);

/**
 * @brief 取得文档的根节点。
 * @param doc 文档指针；可以为 NULL。
 * @return 根节点指针；doc 为 NULL 时返回 NULL。
 */
cjson_value *cjson_root(const cjson_doc *doc);

/**
 * @brief 按名字查找对象的成员。
 * @param object 目标对象节点。
 * @param key 以 '\0' 结尾的键字符串。
 * @return 匹配的成员节点；重复键返回最后一个；找不到或参数非法返回 NULL。
 */
cjson_value *cjson_object_get(const cjson_value *object, const char *key);

/**
 * @brief 按下标取得数组元素。
 * @param array 目标数组节点。
 * @param index 元素下标，从 0 开始。
 * @return 元素节点；越界或参数非法返回 NULL。
 */
cjson_value *cjson_array_get(const cjson_value *array, size_t index);

/**
 * @brief 以调试格式打印一棵值树到标准输出。
 *
 * 输出全部类型的节点、对象键和缩进层级，用于人工检查解析结果；
 * 它不是把文档重新序列化为 JSON 文本。
 *
 * @param value 要打印的根值；可以为 NULL（此时不输出任何内容）。
 * @return 无。
 */
void cjson_print(const cjson_value *value);

/**
 * @brief 取得错误码的英文描述。
 * @param code 错误码。
 * @return 指向静态字符串的描述。
 */
const char *cjson_error_string(cjson_error_code code);

/**
 * @brief 判断节点是否为 null。
 * @param value 节点指针。
 * @return 是 CJSON_NULL 返回 1，否则返回 0。
 */
inline int is_null(const cjson_value *value)
{
    return value && value->type == CJSON_NULL;
}

/**
 * @brief 判断节点是否为指定布尔值。
 * @param value 节点指针。
 * @param expected 期望的布尔值（0 或 1）。
 * @return 匹配返回 1，否则返回 0。
 */
inline int is_bool(const cjson_value *value, int expected)
{
    return value && value->type == CJSON_BOOL && value->u.boolean == expected;
}

/**
 * @brief 判断节点是否为指定 int64_t 整数。
 * @param value 节点指针。
 * @param expected 期望值。
 * @return 匹配返回 1，否则返回 0。
 */
inline int is_int(const cjson_value *value, int64_t expected)
{
    return value && value->type == CJSON_INT && value->u.integer == expected;
}

/**
 * @brief 判断两个 double 是否在相对误差范围内相等。
 * @param a 实际值。
 * @param b 期望值。
 * @param relative 允许的相对误差。
 * @return 在范围内返回 1，否则返回 0。
 */
int nearly_equal(double a, double b, double relative);

/**
 * @brief 判断节点是否为近似等于期望值的 double (误差范围:1e-12)
 * @param value 节点指针。
 * @param expected 期望值。
 * @return 匹配返回 1，否则返回 0。
 */
inline int is_double(const cjson_value *value, double expected)
{
    return value && value->type == CJSON_DOUBLE &&
           nearly_equal(value->u.number, expected, 1e-12);
}

/**
 * @brief 判断节点是否为指定内容的字符串。
 * @param value 节点指针。
 * @param expected 以 '\0' 结尾的期望字符串。
 * @return 类型、长度和内容都匹配时返回 1，否则返回 0。
 */
inline int is_string(const cjson_value *value, const char *expected)
{
    size_t len = strlen(expected);

    return value && value->type == CJSON_STRING &&
           value->u.string.len == len &&
           memcmp(value->u.string.data, expected, len) == 0 &&
           value->u.string.data[len] == '\0';
}

/**
 * @brief 判断 double 是否为负零。
 * @param value 待判断的 double。
 * @return 是 -0.0 返回 1，否则返回 0。
 */
inline int is_negative_zero(double value)
{
    union {
        double   value; /**< 按 double 解释 */
        uint64_t bits;  /**< 按位查看符号位 */
    } raw;

    raw.value = value;
    return value == 0.0 && (raw.bits >> 63) != 0;
}

/**
 * @brief 判断节点是否为数组。
 * @param value 节点指针。
 * @return 是数组返回 1，否则返回 0。
 */
inline int is_array(const cjson_value *value)
{
    return value && value->type == CJSON_ARRAY;
}

/**
 * @brief 判断节点是否为对象。
 * @param value 节点指针。
 * @return 是对象返回 1，否则返回 0。
 */
inline int is_object(const cjson_value *value)
{
    return value && value->type == CJSON_OBJECT;
}

#endif
