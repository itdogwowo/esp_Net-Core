idf_component_register(
    SRCS "nc4_ws.c" "nc4_udp.c" "nc4_http.c" "nc4_line_net.c" "nc4_ota_http.c" "nc4_web.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server nc4 lwip freertos app_update espressif__cjson
)
