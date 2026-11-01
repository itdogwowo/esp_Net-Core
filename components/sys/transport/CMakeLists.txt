idf_component_register(
    SRCS "ws_nc4.c" "udp_nc4.c" "http_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server nc4 lwip espressif__cjson
)
