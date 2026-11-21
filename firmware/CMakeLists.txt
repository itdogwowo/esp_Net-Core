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
# 臨時／實驗覆寫：-DNC_EXTRA_DEFAULTS="a.defaults;b.defaults" 追加在最後（後者勝）。
# 用途：不改 ports/ 的正式設定就跑一次極限配置（例如 16×600）。
# 注意：呢個變數一定要 append 喺上面三段之後，否則會被 set() 蓋掉。
if(NC_EXTRA_DEFAULTS)
    list(APPEND SDKCONFIG_DEFAULTS ${NC_EXTRA_DEFAULTS})
endif()

# ── 測試（名字就是分類：test_*）──
# 測試唔另開「用途資料夾」：目錄名 `test_` 開頭就係測試，後綴決定住邊一層
#   · `test_*_action` → components/action/（指令表）
#   · `test_*_tasks`  → components/tasks/（任務）
# 「測試」同「行為」係同一個座標系：同層，靠名字分辨，唔另開用途資料夾。
# OFF 時呢啲元件**唔加入 EXTRA_COMPONENT_DIRS**：唔編譯、唔 link、唔註冊 ——
# 「唔存在」而唔係靠命名或 runtime 開關。
# 約定：
#   · 測試「功能」→ 用外掛模組（.nct，動態加載）驗證，唔入固件；
#   · 測試「指令」（0xF0xx 量測）→ components/action/test_action（名字帶 test_）；
#   · 效能任務（bench_a/b/c、tcp_sink/src）2026-11-19 已刪 —— 量測改用 0xF0xx
#     指令＋nc_net_raw（rawbench）；
#   · 宿主能力（ncm loader／WAMR）→ components/host/ncm（暫時跟 tests 開）。
option(NC_ENABLE_TESTS "測試元件（components/<層>/test_*）" OFF)
# 測試 ID（分類；**只在 NC_ENABLE_TESTS=ON 時有意義**）：
#   0 = 公共（所有 build 都收，永遠允許）
#   1 = measure    2 = network    3 = pixel    4 = ncm    5..7 保留
# 用法：-DNC_TEST_IDS=0,2,3（0 會自動加入）
# 一個模組可以屬於多個 ID：native 在 build.json 寫 "test_ids": [..]（任何一個被允許就收）；
# C 碼用 #if NC_TEST_ALLOW_<id>。完整契約見 docs/03_notes/test-ids.md。
set(NC_TEST_IDS "0" CACHE STRING "允許的測試 ID（逗號分隔；0=公共，永遠允許）")
set(NC_TEST_ID_DEFS "")
if(NC_ENABLE_TESTS)
    string(REPLACE "," ";" _nc_ids "${NC_TEST_IDS}")
    string(REPLACE " " ";" _nc_ids "${_nc_ids}")
    list(APPEND _nc_ids 0)
    list(REMOVE_DUPLICATES _nc_ids)
    foreach(_id RANGE 0 7)
        list(FIND _nc_ids "${_id}" _pos)
        if(_pos GREATER -1)
            list(APPEND NC_TEST_ID_DEFS "NC_TEST_ALLOW_${_id}=1")
        else()
            list(APPEND NC_TEST_ID_DEFS "NC_TEST_ALLOW_${_id}=0")
        endif()
    endforeach()
    message(STATUS "測試 ID：${_nc_ids}（0 = 公共）")
endif()
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
    # ── 軟件（工具：演算法／math；唔碰硬體、唔開任務、唔養行為）──
    # pixel：環＋編碼器＋效果引擎（契約／registry／interpreter／抖動）。
    # 具體效果唔喺這裡：它們住 components/effects/<域>_effects/（見下一段）。
    "${CMAKE_CURRENT_LIST_DIR}/components/sw/pixel"
    # ── 效果實作（時間＋參數 → canonical 畫面）──
    # 一個域一個 component：components/effects/<域>_effects/。
    # 規則見 docs/03_notes/behavior-taxonomy.md §2.0（引擎 vs 實作）／§3。
    "${CMAKE_CURRENT_LIST_DIR}/components/effects/pixel_effects"
    # ── 任務（生產任務，不是測試）──
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/hw_gpio"
    # pixel_task：效果／Map 生產者；render_task：渲染（LED 現在，LCD 將來）
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/pixel_task"
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/render_task"
    # content_task：production 的「檔案驅動 show」生產者（/data/pixel 四層 + 0xF06B）。
    # 由 CONFIG_NETCORE_CONTENT 開關，CONFIG 關閉時 .c 內容為空、不登記任何東西。
    "${CMAKE_CURRENT_LIST_DIR}/components/tasks/content_task")
# effect_bench：純效果效能（0xF065），**測試／量測專用**。
# 只有建置 EffectPerf project 才加入 component；production／Show 映像連目錄都唔掃。
# 用 NC_PROJECT 環境變數（builder 每次 idf.py 都會帶）判定，避免 CONFIG_* 未載入
# 前（project() 之前）就讀 Kconfig 的時序問題。
if(DEFINED ENV{NC_PROJECT} AND "$ENV{NC_PROJECT}" MATCHES "EffectPerf")
    list(APPEND _nc_extra_dirs
         "${CMAKE_CURRENT_LIST_DIR}/components/tasks/effect_bench")
endif()
# 外部任務宿主（ncm）。NC_ENABLE_NCM=OFF 時連 component 都唔加入 →
# 唔編譯、唔 link、唔 fetch WAMR 依賴（components/host/ncm/idf_component.yml）。
# 注意：一定要放喺 components 之下第二層（components/*/ncm）—— IDF 會自動把
# components/ 第一層有 CMakeLists.txt 的目錄當 component，放第一層會連 release
# 都拉入嚟（C3/P4 冇 WAMR rule → resolve 失敗）。
if(NC_ENABLE_NCM)
    list(APPEND _nc_extra_dirs "${CMAKE_CURRENT_LIST_DIR}/components/host/ncm")
endif()
# 測試元件：唔逐一列出 —— 名字 `test_` 開頭就係登記。
# 位置由後綴決定（test_*_action 在 action/、test_*_tasks 在 tasks/），
# 所以新增測試元件只要放對層、叫對名，唔使再改呢個檔。
# NC_ENABLE_TESTS=OFF 時一個都唔加入 → 唔編譯、唔 link、唔存在。
if(NC_ENABLE_TESTS)
    file(GLOB _nc_test_dirs CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_LIST_DIR}/components/*/test_*")
    foreach(_nc_test_dir IN LISTS _nc_test_dirs)
        if(IS_DIRECTORY "${_nc_test_dir}"
           AND EXISTS "${_nc_test_dir}/CMakeLists.txt")
            list(APPEND _nc_extra_dirs "${_nc_test_dir}")
        endif()
    endforeach()
    unset(_nc_test_dirs)
    unset(_nc_test_dir)
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
