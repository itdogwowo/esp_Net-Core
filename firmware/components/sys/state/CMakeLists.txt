# 狀態管理器（v1）：登記狀態、決定要唔要落盤。
# 現在只有 boot（NVS）一個用戶；config 層（FILE）下一步接上。
idf_component_register(
    SRCS "nc_state.c" "nc_boot.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_app_format
)
