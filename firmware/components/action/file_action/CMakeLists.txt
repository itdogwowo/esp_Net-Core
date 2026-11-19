idf_component_register(
    SRCS "nc_file_action.c"
    INCLUDE_DIRS "include"
    REQUIRES nc4 store state nclog mbedtls fatfs espressif__cjson
    # 指令表由 NC_ACTION_TABLE_DEF 的 constructor 自己登記；呢個 component
    # 冇任何 symbol 被 main 引用，唔加 WHOLE_ARCHIVE 就會成個 object 被丟掉。
    WHOLE_ARCHIVE
)