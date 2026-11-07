idf_component_register(
    SRCS "nc4.c" "nc4_bus.c" "nc4_router.c" "nc_action.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_rom esp_timer
)
