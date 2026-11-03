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

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/sys"
                          "${CMAKE_CURRENT_LIST_DIR}/components/app"
                          "${CMAKE_CURRENT_LIST_DIR}/components/app/tasks"
                          "${CMAKE_CURRENT_LIST_DIR}/components/action")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_net_core)
