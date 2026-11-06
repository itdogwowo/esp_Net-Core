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
        # 用 build-time 的 copy_if_different，而唔係 configure_file：
        # configure_file 只在「CMake 重跑」時複製，只改 JSON 內容唔會觸發
        # ⇒ 嵌進 firmware 的預置會係舊版（實測：改了 routes 但 seed 出嚟仍係舊值）。
        set(_preset_out "${CMAKE_CURRENT_BINARY_DIR}/preset_${_cfg}.json")
        add_custom_command(
            OUTPUT  "${_preset_out}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PORT_DIR}/data/config/${_cfg}.json" "${_preset_out}"
            DEPENDS "${PORT_DIR}/data/config/${_cfg}.json"
            COMMENT "預置 ${_cfg}.json → build")
        list(APPEND _preset_embed "${_preset_out}")
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

# esp_wifi 是「編譯期可得」的相依：P4 冇 radio 所以完全唔要；
# 非 P4（S3）一定要有，否則 CONFIG_NETCORE_WIFI=y 時 nc_wifi.c 編唔過。
# 真正決定「有冇 link 入 image」的是 CONFIG_NETCORE_WIFI：
#   關閉 → nc_wifi.c 只編 stub，冇任何 esp_wifi 符號 → linker 唔會拉入（0 B）。
set(_nc_priv_extra)
if(NOT "${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND _nc_priv_extra esp_wifi)
endif()
# 測試任務（test_*_tasks）只在 NC_ENABLE_TESTS=ON 時存在；OFF 時連 component 都冇。
set(_nc_test_reqs)
if(NC_ENABLE_TESTS)
    list(APPEND _nc_test_reqs test_fx_tasks test_hello_tasks test_bench_tasks)
endif()

idf_component_register(
    SRCS "main.c" "nc_wifi.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs esp_timer esp_partition esp_driver_gpio esp_driver_uart wear_levelling esp_eth esp_netif lwip esp_event esp_psram nvs_flash
                   espressif__esp_tinyusb nc4 mem config ota_action task_manager core transport store state nclog file_action
                   ${_nc_priv_extra}
    ${_preset_args}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_HAS_PRESET_TASKS=${_has_tasks_preset})
if(NC_ENABLE_TESTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NC_ENABLE_TESTS=1)
    # main 直接 include 測試任務的標頭（nc_fx.h / nc_bench.h …）。
    # 不靠 PRIV_REQUIRES：組件掃描階段（component manager）睇唔到本專案的
    # 變數（NC_ENABLE_TESTS 未定義），嗰次登記就會漏咗 include path。
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../components/tasks/test_fx_tasks/include"
        "${CMAKE_CURRENT_LIST_DIR}/../components/tasks/test_hello_tasks/include"
        "${CMAKE_CURRENT_LIST_DIR}/../components/tasks/test_bench_tasks/include"
        "${CMAKE_CURRENT_LIST_DIR}/../components/tasks/test_probe_tasks/include"
        "${CMAKE_CURRENT_LIST_DIR}/../components/tasks/test_tcp_tasks/include"
        "${CMAKE_CURRENT_LIST_DIR}/../components/action/test_action/include")
endif()

# 最小映像（只印心跳，不碰 USB/MSC/ETH）：用來確認新板子的基礎能不能跑
if(NETCORE_MINIMAL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_MINIMAL=1)
endif()
