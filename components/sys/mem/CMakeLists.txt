idf_component_register(
    SRCS "nc_mem.c" "nc_mem_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES heap espressif__cjson
)
