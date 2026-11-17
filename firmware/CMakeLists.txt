cmake_minimum_required(VERSION 3.16)

# ── 兩層 port：硬體板（ports/hardware/）＋ project（ports/project/）──
# 硬體板 = 這塊板「有什麼」；project = 這塊板「跑什麼」。
# 這個 CMakeLists 是 ESP-IDF 工程的根；idf.py 必須在 firmware/ 下跑（或 idf.py -C firmware）。
set(PORT "S3/ESP32-S3-N16R8" CACHE STRING "Hardware board under <repo>/ports/hardware/")
set(PROJECT "" CACHE STRING "Project under <repo>/ports/project/ (optional)")
set(PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../ports/hardware/${PORT}")

if(NOT EXISTS "${PORT_DIR}")
    message(FATAL_ERROR "hardware port not found: ${PORT_DIR}")
endif()

# ⚠️ 唔可以叫 PROJECT_DIR：CMake 的 project() 會把 PROJECT_DIR 設成「本 project
# 的 source dir」（= firmware/），子目錄的 CMakeLists 見到的是被蓋掉的值。
# 所以我們的 project 路徑一定要用 NC_ 前綴。
set(NC_PROJECT_DIR "")
if(PROJECT)
    set(NC_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../ports/project/${PROJECT}")
    if(NOT EXISTS "${NC_PROJECT_DIR}")
        message(FATAL_ERROR "project not found: ${NC_PROJECT_DIR}")
    endif()
endif()

# IDF 6.1 的 component_get_requirements 在另一個 context 重新 include 元件 CMake；
# 那裡看不到上面的普通變數。把身分放進環境，子 CMake 一定繼承得到。
set(ENV{NC_PORT} "${PORT}")
set(ENV{NC_PROJECT} "${PROJECT}")

# 建置設定（SDKCONFIG_DEFAULTS）＝ 共通預設 → 硬體板覆蓋（後者勝）。
# project 層**唔參與 build**：它只有 runtime 設定（project.json ＋ data/config），
# 冇 sdkconfig.defaults。能力碎片（eth-rmii／wifi-hosted-*／nc4-ws／nc4-udp）
# 全部寫在 board.json 的 profiles，由 tools/ncbuild 用 -DSDKCONFIG_DEFAULTS 傳入。
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${PORT_DIR}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${PORT_DIR}/sdkconfig.defaults")
endif()
if(PROJECT AND EXISTS "${NC_PROJECT_DIR}/sdkconfig.defaults")
    message(WARNING "${NC_PROJECT_DIR}/sdkconfig.defaults 存在但**唔會被使用**："
                    "project 層只放 runtime 設定，build 設定請搬到 hardware 層。")
endif()

# ── 測試專用（約定：action/test_action、app/test_*_tasks）──
# OFF 時整個 components/app 唔加入編譯：測試任務「唔存在」，而唔係靠命名或 runtime 開關。
option(NC_ENABLE_TESTS "測試指令與測試任務（test_action / test_*_tasks）" OFF)
option(NC_STACK_ANALYSIS "產生 .su/.ci（給 tools/stack_depth.py 算最壞棧深）" OFF)

# 正式組件（永遠編譯）
set(_nc_extra_dirs
    "${CMAKE_CURRENT_LIST_DIR}/components/sys"
    "${CMAKE_CURRENT_LIST_DIR}/components/action/file_action"
    "${CMAKE_CURRENT_LIST_DIR}/components/action/ota_action"
    # ── 匯流排驅動器（對照舊專案 slave/driver/*_drv.py）──
    # 一個周邊一個組件：driver/drv_uart、driver/drv_usb、driver/drv_sd、driver/drv_eth
    "${CMAKE_CURRENT_LIST_DIR}/components/driver"
    # ── 匯流排（通用：NC4 幀、LED(SPI)、LCD(I80) 都會用）──
    # 一個「線的實作」一個組件：bus/net（TCP/WS/UDP）、之後 bus/nc4、bus/led、bus/lcd
    "${CMAKE_CURRENT_LIST_DIR}/components/bus"
    # ── 硬體 owner（生產任務，不是測試）──
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/hw_gpio")
# 測試組件（NC_ENABLE_TESTS=OFF 時唔加入 → 唔編譯、唔 link、唔存在）
if(NC_ENABLE_TESTS)
    list(APPEND _nc_extra_dirs
        "${CMAKE_CURRENT_LIST_DIR}/components/action/test_action"
        "${CMAKE_CURRENT_LIST_DIR}/components/tasks")
endif()
set(EXTRA_COMPONENT_DIRS ${_nc_extra_dirs})

# ── 靜態棧深分析（tools/stack_depth.py）──
# 每個 .c 額外產生 .su（每個函數的框大小）與 .ci（呼叫圖，含框大小）。
# 有它們就可以「算」出最壞情況深度，而唔係靠 runtime 採樣（HWM 只覆蓋走過的路）。
# 必須在 project() 之前設定，才會套用到所有組件。
if(NC_STACK_ANALYSIS)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-usage -fcallgraph-info=su")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fstack-usage -fcallgraph-info=su")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_net_core)
