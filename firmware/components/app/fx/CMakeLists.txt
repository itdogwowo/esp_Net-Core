idf_component_register(
    SRCS "nc_fx.c" "nc_fx_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES mem esp_rom freertos esp_timer tasks
)
