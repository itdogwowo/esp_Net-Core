# 出廠預置：直接嵌進 firmware，開機時寫進 data 分區的 config/。
#
# 為什麼不用 fatfs_create_partition_image 預先燒映像：
#   MSC 走 wear levelling，WL 會把分區開頭拿去放自己的 metadata，
#   所以「燒在分區 offset 0 的 FAT 映像」經過 WL 之後對不上 —— 碟會顯示「沒有檔案系統」。
#   改成：firmware 內嵌 → 第一次開機自動格式化 → 寫入預置 → 交給 host。
#   使用者不需要手動格式化；換預置只要改 project/.../data/config/*.json 再 build。
#
# 兩層分工：
#   hardware/wiring.json → 由 tools/ncportgen.py 編成 C 預設（nc_port.c）＋ 產生
#                          hardware.json 出廠預置；**不是**在 runtime 解析 JSON。
#   project/data/config/ → 原樣嵌進 firmware，開機 seed 到 /data/config/。
# 一個層一個檔：system / network / tasks（存在才嵌）；缺的層由 firmware 的預設補。

# IDF 的 component_get_requirements 會在另一個 CMake context 重新 include 這份檔，
# 那時 top-level CMakeLists 的普通變數（PORT_DIR／NC_PROJECT_DIR）唔一定繼承；
# -DPORT/-DPROJECT 的 cache 變數也不一定見得到。唯一可靠的是環境變數 NC_PORT／NC_PROJECT。
if(NOT PORT)
    set(PORT "$ENV{NC_PORT}")
endif()
if(NOT PROJECT)
    set(PROJECT "$ENV{NC_PROJECT}")
endif()
if(NOT PORT_DIR)
    set(PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../ports/hardware/${PORT}")
endif()
# 同 top-level 一樣：用 NC_PROJECT_DIR，唔好用 PROJECT_DIR（CMake project() 會蓋）。
if(NOT NC_PROJECT_DIR AND PROJECT)
    set(NC_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../ports/project/${PROJECT}")
endif()
if(NOT EXISTS "${PORT_DIR}/board.json")
    message(FATAL_ERROR "hardware port not found or incomplete: ${PORT_DIR} (PORT=${PORT})")
endif()


# ── 硬體層 → firmware 預設（build-time codegen）──
# 輸出的 nc_port.c 直接是 nc_hardware_cfg_t 的 static const：
#   · 字典字串（OUT/IN、UP/DOWN、IP101、spi/i2c…）在這裡對標成數值
#   · 只有數值，沒有 JSON 文字；比同時嵌 wiring.json ＋ hardware.json 少一半以上
#   · nc_cfg_defaults.c 透過 weak symbol 取用它；host test 沒有生成檔時自動退回內建表
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/ncport_gen")
set(_gen_c   "${_gen_dir}/nc_port.c")
set(_gen_h   "${_gen_dir}/nc_port.h")
set(_gen_hw  "${_gen_dir}/preset_hardware.json")
set(_gen_dep "${PORT_DIR}/board.json" "${PORT_DIR}/wiring.json"
             "${CMAKE_CURRENT_LIST_DIR}/../../tools/ncportgen.py")
# 晶片能力表（cores／USB link）也是 ncportgen 的輸入；漏了它，改 _chips/*.json
# 不會觸發 regen，生成的 nc_port.c 會與 _chips 講的唔同（曾經發生）。
# CONFIGURE_DEPENDS 只可以在正常 configure 用；script mode（component_get_requirements）
# 會 reject，所以整段要 guard。
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB _chip_caps CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../ports/hardware/_chips/*.json")
    list(APPEND _gen_dep ${_chip_caps})
endif()
if(EXISTS "${PORT_DIR}/driver.json")
    list(APPEND _gen_dep "${PORT_DIR}/driver.json")
endif()
# configure 時先跑一次：確保 EMBED_FILES 在 configure 階段就見得到檔案；
# build 時再由下面的 custom command 按 JSON mtime 重新產生。
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/../../tools/ncportgen.py"
            --hardware "${PORT_DIR}" --out "${_gen_dir}"
    RESULT_VARIABLE _gen_rc)
if(NOT _gen_rc EQUAL 0)
    message(FATAL_ERROR "ncportgen failed for ${PORT_DIR}")
endif()
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_custom_command(
        OUTPUT  "${_gen_c}" "${_gen_h}" "${_gen_hw}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/../../tools/ncportgen.py"
                --hardware "${PORT_DIR}" --out "${_gen_dir}"
        DEPENDS ${_gen_dep}
        COMMENT "hardware JSON → firmware 預設（nc_port.c）"
        VERBATIM)
    add_custom_target(ncport_gen DEPENDS "${_gen_c}" "${_gen_h}" "${_gen_hw}")
endif()

# hardware.json：如果 project 層有手寫／已提交的檔案，優先用它（用戶空間看得到、改得到）；
# 否則用 ncportgen 由 wiring.json 產生的 preset_hardware.json。
set(_preset_hw "${_gen_hw}")
if(NC_PROJECT_DIR AND EXISTS "${NC_PROJECT_DIR}/data/config/hardware.json")
    set(_preset_hw "${CMAKE_CURRENT_BINARY_DIR}/preset_hardware.json")
    # configure 階段先複製一份，EMBED_FILES 才見得到
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NC_PROJECT_DIR}/data/config/hardware.json" "${_preset_hw}")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        add_custom_command(
            OUTPUT  "${_preset_hw}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NC_PROJECT_DIR}/data/config/hardware.json" "${_preset_hw}"
            DEPENDS "${NC_PROJECT_DIR}/data/config/hardware.json"
            COMMENT "預置 hardware.json → build")
    endif()
endif()
set(_preset_embed "${_preset_hw}")
foreach(_cfg system network tasks resources)
    if(NC_PROJECT_DIR AND EXISTS "${NC_PROJECT_DIR}/data/config/${_cfg}.json")
        # 用 build-time 的 copy_if_different，而唔係 configure_file：
        # configure_file 只在「CMake 重跑」時複製，只改 JSON 內容唔會觸發
        # ⇒ 嵌進 firmware 的預置會係舊版（實測：改了 routes 但 seed 出嚟仍係舊值）。
        set(_preset_out "${CMAKE_CURRENT_BINARY_DIR}/preset_${_cfg}.json")
        if(NOT CMAKE_SCRIPT_MODE_FILE)
            add_custom_command(
                OUTPUT  "${_preset_out}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${NC_PROJECT_DIR}/data/config/${_cfg}.json" "${_preset_out}"
                DEPENDS "${NC_PROJECT_DIR}/data/config/${_cfg}.json"
                COMMENT "預置 ${_cfg}.json → build")
        endif()
        list(APPEND _preset_embed "${_preset_out}")
    endif()
endforeach()
if(_preset_embed)
    set(_preset_args EMBED_FILES ${_preset_embed})
endif()

if(NC_PROJECT_DIR AND EXISTS "${NC_PROJECT_DIR}/data/config/system.json")
    set(_has_system_preset 1)
else()
    set(_has_system_preset 0)
endif()
if(NC_PROJECT_DIR AND EXISTS "${NC_PROJECT_DIR}/data/config/network.json")
    set(_has_network_preset 1)
else()
    set(_has_network_preset 0)
endif()
if(NC_PROJECT_DIR AND EXISTS "${NC_PROJECT_DIR}/data/config/tasks.json")
    set(_has_tasks_preset 1)
else()
    set(_has_tasks_preset 0)
endif()
if(NC_PROJECT_DIR AND EXISTS "${NC_PROJECT_DIR}/data/config/resources.json")
    set(_has_resources_preset 1)
else()
    set(_has_resources_preset 0)
endif()

# esp_wifi 是「編譯期可得」的相依：P4 冇 radio 所以唔要原生 esp_wifi；
# 非 P4（S3）一定要有，否則 CONFIG_NETCORE_WIFI=y 時 nc_wifi.c 編唔過。
# P4 走外掛協處理器時改要 esp_wifi_remote：它提供**同一個** esp_wifi.h，
# 所以 nc_wifi.c 不需要任何 #ifdef。真正決定「有冇 link 入 image」的仍然是
# CONFIG_NETCORE_WIFI：關閉 → nc_wifi.c 只編 stub → linker 唔會拉入（0 B）。
set(_nc_priv_extra)
if(NOT "${IDF_TARGET}" STREQUAL "esp32p4")
    list(APPEND _nc_priv_extra esp_wifi)
else()
    # P4：esp_wifi 是 remote shim，esp_hosted 才是傳輸層。兩個都列 —— 不能用
    # CONFIG_NETCORE_WIFI_HOSTED 判斷：component CMake 第一輪 configure 睇唔到 CONFIG_*。
    list(APPEND _nc_priv_extra esp_wifi esp_wifi_remote)
endif()
# USB（TinyUSB）只在有 USB OTG 控制器的晶片上存在。用 IDF_TARGET 而不是
# CONFIG_NETCORE_USB_OTG：第一次 configure 時 CONFIG_* 還未生成（同上面 Wi-Fi 的理由）。
# C3／C6／H2 沒有 OTG，TinyUSB 本身就編不過（tusb_mcu.h #error）→ 不能進依賴圖。
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND _nc_priv_extra espressif__esp_tinyusb)
endif()
# 外部任務宿主（ncm）只在 NC_ENABLE_NCM=ON 時存在；OFF 時連 component 都冇。
# 效能測試任務只在 NC_ENABLE_TESTS=ON 時存在（components/dev）。
set(_nc_test_reqs)
if(NC_ENABLE_NCM)
    list(APPEND _nc_test_reqs ncm)
endif()
if(NC_ENABLE_TESTS)
    list(APPEND _nc_test_reqs test_bench_tasks test_tcp_tasks)
endif()

idf_component_register(
    SRCS "main.c" "nc_wifi.c" "boot.c" ${_gen_c}
    INCLUDE_DIRS "." "${_gen_dir}"
    PRIV_REQUIRES fatfs esp_timer esp_partition esp_driver_gpio esp_driver_uart esp_driver_sdmmc
              wear_levelling esp_eth esp_netif lwip esp_event esp_psram nvs_flash
              # driver/：匯流排驅動器（對照舊專案 slave/driver/*_drv.py）
              drv_uart drv_pwm bus_core bus_uart hw hw_gpio
                   nc4 mem config ota_action task_manager core bus_net store state nclog file_action
                   esp_driver_usb_serial_jtag
                   ${_nc_priv_extra}
    ${_preset_args}
)
# IDF 6.1 的 component_get_requirements 會用 `cmake -P`（script mode）include 這份檔，
# 那時 add_custom_command／add_dependencies／target_* 全部不是 scriptable。
# 只有 idf_component_register 要在 script mode 跑（它會記錄 requirements）；
# 其餘 build-system 命令一律跳過。
if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_dependencies(${COMPONENT_LIB} ncport_gen)

    target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_HAS_PRESET_TASKS=${_has_tasks_preset})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_HAS_PRESET_SYSTEM=${_has_system_preset})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_HAS_PRESET_NETWORK=${_has_network_preset})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_HAS_PRESET_RESOURCES=${_has_resources_preset})
    if(NC_ENABLE_NCM)
        # main 直接 include ncm 標頭（nc_mod.h / nc_mod_engine.h）。
        target_include_directories(${COMPONENT_LIB} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../components/host/ncm/include")
    endif()
    if(NC_ENABLE_TESTS)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE NC_ENABLE_TESTS=1)
        # main 直接 include 效能測試任務的標頭（nc_bench.h …）。
        # 不靠 PRIV_REQUIRES：組件掃描階段（component manager）睇唔到本專案的
        # 變數（NC_ENABLE_TESTS 未定義），嗰次登記就會漏咗 include path。
        target_include_directories(${COMPONENT_LIB} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../components/dev/test_bench_tasks/include"
            "${CMAKE_CURRENT_LIST_DIR}/../components/dev/test_tcp_tasks/include"
            "${CMAKE_CURRENT_LIST_DIR}/../components/dev/test_action/include")
    endif()

    # 最小映像（只印心跳，不碰 USB/MSC/ETH）：用來確認新板子的基礎能不能跑
    if(NETCORE_MINIMAL)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE NETCORE_MINIMAL=1)
    endif()

    # SD 速度量測（sd_boot 內）：-DNC_SD_BENCH=1 才編入，正常映像唔會有。
    # FAT 寫入量測：包住 sdmmc_write_sectors 計 sector 數。
    # ★ 只可以在測試映像加 --wrap：__wrap_sdmmc_* 的定義在 main.c 的 NC_ENABLE_TESTS 區塊裡
    #   （0xF012–0xF019 的 SD bench 本身就是測試指令）。無條件加就會令 release 映像
    #   出現 undefined reference to `__wrap_sdmmc_write_sectors` —— 這裡之前就是這樣。
    if(NC_ENABLE_TESTS AND NOT IDF_TARGET STREQUAL "esp32p4")
        idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=sdmmc_write_sectors" APPEND)
        idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=sdmmc_read_sectors" APPEND)
    endif()

    if(NC_SD_BENCH)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE NC_SD_BENCH=1)
    endif()
endif()
