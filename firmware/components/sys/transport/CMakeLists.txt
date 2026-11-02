idf_component_register(
    SRCS "nc4_ws.c" "nc4_udp.c" "nc4_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server nc4 lwip espressif__cjson
)
