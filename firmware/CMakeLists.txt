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

# 建置設定（SDKCONFIG_DEFAULTS）＝ 共通預設 → 硬體板覆蓋 → project 覆蓋（後者勝）。
# project 主要是 runtime 設定（project.json ＋ data/config）；但同一塊硬體可以掛
# 不同的外接組合（例如 SuperMini ＋ LED Textkit 佔 GPIO1–5），那組腳位是 build-time
# 的事實，所以 project 可以選擇性帶一份 sdkconfig.defaults 覆寫。沒有這個檔的
# project（絕大多數）行為完全不變。
# 注意：sdkconfig 已經存在時改 defaults 不會自動生效 —— 要 `make.py build … --fresh`。
# 能力碎片（eth-rmii／wifi-hosted-*／nc4-ws／nc4-udp）
# 仍然寫在 board.json 的 profiles，由 tools/ncbuild 用 -DSDKCONFIG_DEFAULTS 傳入。
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${PORT_DIR}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${PORT_DIR}/sdkconfig.defaults")
endif()
if(PROJECT AND EXISTS "${NC_PROJECT_DIR}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "${NC_PROJECT_DIR}/sdkconfig.defaults")
endif()

# ── 測試／開發專用（components/dev）──
# OFF 時整個 components/dev 唔加入編譯：測試指令與效能測試任務「唔存在」，
# 而唔係靠命名或 runtime 開關。
# 約定（2026-11-17 重組）：
#   · 測試「功能」→ 用外掛模組（.nct，動態加載）驗證，唔入固件；
#   · 效能測試（bench／rgb matrix／tcp 吞吐）→ 留 dev 固件（components/dev）；
#   · 宿主能力（ncm loader／WAMR）→ components/ncm（暫時跟 tests 開）。
option(NC_ENABLE_TESTS "測試指令與效能測試任務（components/dev）" OFF)
option(NC_STACK_ANALYSIS "產生 .su/.ci（給 tools/stack_depth.py 算最壞棧深）" OFF)

# 外部任務宿主（ncm：loader＋WAMR engine）。現階段測試映像先開；
# 將來 production 要支援外掛就單獨開 NC_ENABLE_NCM（唔使連測試任務一齊）。
option(NC_ENABLE_NCM "外部任務宿主（ncm：loader＋WAMR 引擎）" OFF)
if(NC_ENABLE_TESTS)
    set(NC_ENABLE_NCM ON)   # 測試映像一定要有動態加載能力（測試用外掛做）
endif()

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
    # ── 軟件（效果／math；唔碰硬體、唔開任務）──
    # pixel：環＋編碼器＋內建效果生產者（host 測試編同一份）
    "${CMAKE_CURRENT_LIST_DIR}/components/sw/pixel"
    # ── 任務（生產任務，不是測試）──
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/hw_gpio"
    # pixel_task：效果／Map 生產者；render_task：渲染（LED 現在，LCD 將來）
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/pixel_task"
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/render_task")
# 外部任務宿主（ncm）。NC_ENABLE_NCM=OFF 時連 component 都唔加入 →
# 唔編譯、唔 link、唔 fetch WAMR 依賴（components/host/ncm/idf_component.yml）。
# 注意：一定要放喺 components 之下第二層（components/*/ncm）—— IDF 會自動把
# components/ 第一層有 CMakeLists.txt 的目錄當 component，放第一層會連 release
# 都拉入嚟（C3/P4 冇 WAMR rule → resolve 失敗）。
if(NC_ENABLE_NCM)
    list(APPEND _nc_extra_dirs "${CMAKE_CURRENT_LIST_DIR}/components/host/ncm")
endif()
# 開發／效能測試組件（NC_ENABLE_TESTS=OFF 時唔加入 → 唔編譯、唔 link、唔存在）
if(NC_ENABLE_TESTS)
    list(APPEND _nc_extra_dirs "${CMAKE_CURRENT_LIST_DIR}/components/dev")
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

# ── 第三方 component 的 IDF 6 補丁（WAMR）────────────────────────
# managed_components/ 唔入 git：依賴 resolver 喺 configure（project() 裡面）抓一份
# 乾淨的上游落嚟。呢度喺 project() 之後、真正編譯之前套補丁；腳本 idempotent，
# 已補過唔會再改（唔會令 WAMR 每次 configure 都重新編譯）。
# 詳情同四個補丁位見 tools/patch_wamr.py 頂部註解。
set(_nc_wamr_dir "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__wasm-micro-runtime")
if(NOT CMAKE_SCRIPT_MODE_FILE AND EXISTS "${_nc_wamr_dir}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_LIST_DIR}/../tools/patch_wamr.py"
                    --component "${_nc_wamr_dir}"
            RESULT_VARIABLE _wamr_patch_rc
            OUTPUT_VARIABLE _wamr_patch_out)
        if(NOT _wamr_patch_rc EQUAL 0)
            message(WARNING "WAMR 補丁套用失敗：${_wamr_patch_out}")
        endif()
    endif()
endif()
