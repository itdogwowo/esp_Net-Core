cmake_minimum_required(VERSION 3.16)

# ── 專案層（<repo>/ports/，在 firmware/ 的上一層）：晶片 = 一個 build，板子 = 一份 JSON ──
# 這個 CMakeLists 是 ESP-IDF 工程的根；idf.py 必須在 firmware/ 下跑（或 idf.py -C firmware）。
set(PORT "S3/ESP32-S3-N16R8" CACHE STRING "Board port under <repo>/ports/")
set(PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../ports/${PORT}")

if(NOT EXISTS "${PORT_DIR}")
    message(FATAL_ERROR "port not found: ${PORT_DIR}")
endif()

# 共通預設 → 板子覆蓋（後者勝）
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${PORT_DIR}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${PORT_DIR}/sdkconfig.defaults")
endif()

# ── 測試專用（約定：action/test_action、app/test_*_tasks）──
# OFF 時整個 components/app 唔加入編譯：測試任務「唔存在」，而唔係靠命名或 runtime 開關。
option(NC_ENABLE_TESTS "測試指令與測試任務（test_action / test_*_tasks）" OFF)
option(NC_STACK_ANALYSIS "產生 .su/.ci（給 tools/stack_depth.py 算最壞棧深）" OFF)

# 正式組件（永遠編譯）
set(_nc_extra_dirs
    "${CMAKE_CURRENT_LIST_DIR}/components/sys"
    "${CMAKE_CURRENT_LIST_DIR}/components/action/file_action"
    "${CMAKE_CURRENT_LIST_DIR}/components/action/ota_action")
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

