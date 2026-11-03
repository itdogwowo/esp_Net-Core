# 出廠預置：直接嵌進 firmware，開機時寫進 data 分區的 config/。
#
# 為什麼不用 fatfs_create_partition_image 預先燒映像：
#   MSC 走 wear levelling，WL 會把分區開頭拿去放自己的 metadata，
#   所以「燒在分區 offset 0 的 FAT 映像」經過 WL 之後對不上 —— 碟會顯示「沒有檔案系統」。
#   改成：firmware 內嵌 → 第一次開機自動格式化 → 寫入預置 → 交給 host。
#   使用者不需要手動格式化；換預置只要改 ports/<board>/data/config/*.json 再 build。
#
# 一個層一個檔：目前嵌 system / network / tasks（存在才嵌）；缺的層由 firmware 的預設補。
# 注意：ports/<board>/data/config/ 下的 hardware.json / driver.json 目前**沒有** embed，
#       firmware 也還沒讀它們 —— 別以為改了那兩個檔會生效。

set(_preset_embed)
foreach(_cfg system network tasks)
    if(EXISTS "${PORT_DIR}/data/config/${_cfg}.json")
        configure_file("${PORT_DIR}/data/config/${_cfg}.json"
                       "${CMAKE_CURRENT_BINARY_DIR}/preset_${_cfg}.json" COPYONLY)
        list(APPEND _preset_embed "${CMAKE_CURRENT_BINARY_DIR}/preset_${_cfg}.json")
    endif()
endforeach()
if(_preset_embed)
    set(_preset_args EMBED_FILES ${_preset_embed})
endif()

if(EXISTS "${PORT_DIR}/data/config/tasks.json")
    set(_has_tasks_preset 1)
else()
    set(_has_tasks_preset 0)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs esp_timer esp_partition esp_driver_gpio esp_driver_uart wear_levelling esp_eth esp_netif lwip esp_event esp_psram nvs_flash
                   espressif__esp_tinyusb nc4 mem config ota_action task_manager transport store file_action fx hello
    ${_preset_args}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_HAS_PRESET_TASKS=${_has_tasks_preset})

# 最小映像（只印心跳，不碰 USB/MSC/ETH）：用來確認新板子的基礎能不能跑
if(NETCORE_MINIMAL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_MINIMAL=1)
endif()
