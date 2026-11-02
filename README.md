# 轻量级JSON解析器

一个超轻量的c语言解析器，只需包含`c_json.c`和`c_json.h`文件即可

- 但只有解析的功能
- 支持`/* */`包裹的注释

通过`cjson_parse_file()`解析json文件，`cjson_parse()`解析json格式的字符串；一次解析就是一块内存，解析得到的数据结构大致如下

![cjson_struct](./cjson_struct.drawio.png)
