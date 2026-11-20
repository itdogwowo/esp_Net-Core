# pixel_task（tasks 層）：效果／Map 生產者。
#
# 只做「按自己的節奏算一格，寫進環」；送燈／送 LCD 是另一個任務（render_task）。
# 環、編碼器、效果計算在 components/sw/pixel —— 這裡只有任務生命週期與節奏。
idf_component_register(
    SRCS "nc_pixel_task.c"
    INCLUDE_DIRS "include"
    REQUIRES task pixel pixel_effects
    PRIV_REQUIRES freertos esp_timer
    WHOLE_ARCHIVE
)
