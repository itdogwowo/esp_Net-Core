idf_component_register(
    SRCS "ota_cmds.c"
    INCLUDE_DIRS "include"
    REQUIRES nc4 app_update mbedtls esp_app_format esp_timer
)
