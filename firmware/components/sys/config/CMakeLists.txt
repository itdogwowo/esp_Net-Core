idf_component_register(
    SRCS "nc_cfg.c" "nc_cfg_patch.c" "nc_cfg_defaults.c" "nc_cfg_load.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
)
