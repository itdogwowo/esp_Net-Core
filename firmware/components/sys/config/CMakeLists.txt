idf_component_register(
    SRCS "nc_cfg.c" "nc_cfg_patch.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
)
