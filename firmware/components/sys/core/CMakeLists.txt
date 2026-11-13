# 一次性 init 的核心派送（開機用）。
idf_component_register(
    SRCS "nc_core.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system freertos log
)
