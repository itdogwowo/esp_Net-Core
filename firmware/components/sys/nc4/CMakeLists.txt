idf_component_register(
    SRCS "nc4.c" "nc4_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_rom esp_timer
)
