idf_component_register(
    SRCS "nc_file_action.c"
    INCLUDE_DIRS "include"
    REQUIRES nc4 store mbedtls fatfs espressif__cjson
)
