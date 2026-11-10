idf_component_register(
    SRCS "nc4.c" "nc4_bus.c" "nc4_router.c" "nc4_link.c" "nc_action.c"
    INCLUDE_DIRS "include"
    # task：要 nc_prio.h（NC 核心的優先級保留區），見 sys/task/include/nc_prio.h
    REQUIRES esp_rom esp_timer mem freertos task
)
