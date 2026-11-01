idf_component_register(
    SRCS "file_cmds.c"
    INCLUDE_DIRS "include"
    REQUIRES nc4 store mbedtls fatfs espressif__cjson
)
