// #include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_json.h"

// gcc c_json.c test.c -o text.exe

int main()
{
    const char* file = "test.json";
    cjson_error err;
    cjson_doc *doc = cjson_parse_file(file, &err);

    if (!doc) {
        printf("FAIL: parse file '%s': %s (line %llu, column %llu)\n",
               file, cjson_error_string(err.code),
               (unsigned long long)err.line,
               (unsigned long long)err.column);
        return 0;
    }
    // printf("root ptr:0x%p\n", doc->root);
    // printf("nodes ptr:0x%p\n", doc->nodes);
    // printf("slots ptr:0x%p\n", doc->slots);
    // printf("strings ptr:0x%p\n", doc->strings);
    printf("this json doc memory size:%lld\n", doc->doc_memory_size);

    cjson_value* root = cjson_root(doc);
    if (!root) {
        cjson_free(doc);
        return 1;
    }

    printf("root container count:%d\n", root->u.container.count);

    cjson_value* err_number = cjson_object_get(root, "err number");
    cjson_value* n1 = cjson_object_get(err_number, "too large");
    cjson_value* n2 = cjson_object_get(err_number, "too small");
    cjson_value* n3 = cjson_object_get(err_number, "too large int");
    printf("too large double:%e\n", n1->u.number);
    printf("too small double:%e\n", n2->u.number);
    printf("too large int's type:%s, and number=%e\n", (n3->type == CJSON_DOUBLE) ? "double":"int64", n3->u.number);

    cjson_value* array = cjson_object_get(root, "array");
    printf("array[0]:%f\n", cjson_array_get(array, 0)->u.number);
    printf("array[1]:%f\n", cjson_array_get(array, 1)->u.number);
    printf("array[2]:%f\n", cjson_array_get(array, 2)->u.number);
    printf("array[3]:%s\n", cjson_array_get(array, 3) ? "valid":"null");
    /* 用库内打印函数输出整棵树，便于人工核对。 */
    // printf("\nParsed tree of test.json:\n");
    // cjson_print(root);

    cjson_free(doc);

    return 0;
}