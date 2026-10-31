# 出廠預置：直接嵌進 firmware，開機時寫進 data 分區。
#
# 為什麼不用 fatfs_create_partition_image 預先燒映像：
#   MSC 走 wear levelling，WL 會把分區開頭拿去放自己的 metadata，
#   所以「燒在分區 offset 0 的 FAT 映像」經過 WL 之後對不上 —— 碟會顯示「沒有檔案系統」。
#   改成：firmware 內嵌 → 第一次開機自動格式化 → 寫入預置 → 交給 host。
#   使用者不需要手動格式化；換預置只要改 ports/<board>/data/config.json 再 build。

set(_preset_embed)
if(EXISTS "${PORT_DIR}/data/config.json")
    set(_preset_embed EMBED_FILES "${PORT_DIR}/data/config.json")
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs esp_timer esp_partition esp_driver_gpio wear_levelling nvs_flash
                   espressif__esp_tinyusb nc4 mem ota tasks
    ${_preset_embed}
)
