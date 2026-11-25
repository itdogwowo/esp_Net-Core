# esp_Net-Core

ESP32-P4（另有 S3 port）上的 **NC4 通訊核心**：多條傳輸通道 → 解碼 → 指令執行，
外加分核（RX core1 / decode core0）、零複製、可調的緩衝管理。C / ESP-IDF v6.0.3。

> **AI／第一次接觸專案**：先讀 [AGENTS.md](AGENTS.md)（雷霆大文件）——
> 入面有鐵律、USB NC4 OTA 流程、建置／測試／pixel 管線與 AI 工作 SOP。
>
> 這份 README 是**人類入口**，只回答一件事：**什麼東西在哪一層、要去哪裡找。**
> 深入內容從 [docs/README.md](docs/README.md) 進去。

---

## 1. 兩層：專案管理 vs 實際原始碼

```text
esp_Net-Core/
│
├─ README.md                    ← 你在這裡
├─ make.py                      ← 建置入口（python3 make.py p4）
├─ ★ 建置啟動腳本（repo 根；同網頁工具分開）
│  ├─ build.sh                  macOS／Linux：建置／燒錄／健檢
│  └─ make.bat                  Windows：同上（= python make.py）
├─ LICENSE
│
├─ docs/                        ──【文件層】只放文字，不含可執行物 ──
│  ├─ 00_pm/                    項目管理：現況、路線圖、決策、待辦
│  ├─ 01_protocol/              協議契約：對接方 / 寫 transport 的人看
│  ├─ 02_guides/                操作指南：要 build / 燒錄 / 測試的人看
│  └─ 03_notes/                 設計筆記：維護者看的（含研究與上板紀錄）
│
├─ firmware/                    ──【原始碼層】ESP-IDF 工程本體，可獨立 build ──
│  ├─ CMakeLists.txt            IDF 工程根（idf.py 在這裡跑）
│  ├─ main/                     裝配 + 目前還住在 main.c 的業務邏輯
│  ├─ components/               可重用元件（路徑＝層，名字後綴＝種類）
│  │  ├─ sys/                   nc4（framing/解碼）、mem、task（型別+loop）、task_manager、transport
│  │  ├─ action/                *_action：指令表（file／ota／test）
│  │  ├─ tasks/                 *_tasks：任務（hw_gpio、pixel、render）
│  │  ├─ host/ncm/              外部任務宿主（loader＋WAMR 引擎；NC_ENABLE_NCM）
│  │                            測試＝名字 test_*（同層，冇 dev/）；OFF 時唔編譯、唔存在
│  ├─ sdkconfig.defaults        共通建置配置（晶片層）
│  ├─ partitions.csv            分割表
│  └─ build/  managed_components/  sdkconfig  ← 生成物，不進 git
│
├─ web_tools/                   ──【網頁工作臺】ESP WebMaster（全部收喺呢個資料夾）──
│  ├─ README.md                 唯一入口文件（啟動／頁面／API／分類規則）
│  ├─ settings.json             ★ 基礎數值（port／host／autoconnect／esptool／UI 首頁）
│  ├─ settings.local.json       ★ 本機覆寫（選用，gitignore）
│  ├─ WebMaster.bat  web.ps1    Windows 入口（雙擊 / 完整啟動器）
│  ├─ web.sh  run_web.sh        macOS／Linux 入口
│  ├─ WebMaster.command         macOS 雙擊
│  └─ webmaster/                程式本體
│     ├─ settings.py            讀設定／驗證／合併（＋內建預設池）
│     ├─ run.py／launch.py      直接起 server；launch.py 會建 venv 裝 deps
│     ├─ server.py              REST／WS（status／檔案／OTA／容器／腳本／設定／格式化）
│     ├─ scripts.py             啟動腳本清單：分類 × 平台（UI「啟動」頁的來源）
│     ├─ formatting.py          JSON 格式化（server 係唯一真相）
│     ├─ container.py           .nct／.nce／.nca 容器解析
│     └─ static/                index.html ＋ js/（12 個模組，零 build step）
├─ modules/                    ──【外掛模組層】獨立編譯（唔屬於 firmware）──
│  ├─ tasks/                   任務模組，leaf 以 _tasks 結尾 → out/<name>.nct
│  ├─ actions/                 指令模組，leaf 以 _actions 結尾（build.json cmds）
│  ├─ effects/                 效果模組，leaf 以 _effects 結尾 → out/<name>.nce
│  ├─ lab/                     實驗／量測，leaf 以 _lab 結尾（唔係任務、唔會收納）
│  ├─ build/                   中間產物（.wasm；gitignore）
│  └─ out/                     最終成品（只有 .nct／.nce；gitignore）
├─ ports/                       ──【配置層】每塊板子的差異，不屬於程式 ──
│  ├─ hardware/<chip>/<board>/   這塊板「有什麼」：board.json / wiring.json /
│  │                              driver.json / sdkconfig.defaults / partitions.csv
│  ├─ project/<chip>/<board>/    這塊板「跑什麼」：project.json / data/config/*.json
│  ├─ _profiles/                 能力碎片（eth-rmii / nc4-ws / nc4-udp …）
│  └─ _chips/                    別名 chip profile（driver.json 引用）
│
└─ tools/                       ──【工具層】主機端，不會進 firmware image ──
   ├─ ncbuild/                  建置工具實作（make.py 的引擎）
   ├─ ncportgen.py              hardware JSON → firmware 預設（build-time codegen）
   ├─ board.py  board.sh        板子清單 / 檢查 / 新增（board.sh 是薄殼）
   ├─ flash.sh                  逃生門燒錄
   ├─ nc4.py  ota.py  selftest.py  nc4_test.py   對設備說話
   ├─ ncm.py                     打包／檢視 .nct（容器格式）
   ├─ ncm_build.py               外掛任務獨立編譯（modules/tasks/<name> → .nct；--dir 支援外部專案）
   ├─ bench/                    量測（要板子）
   ├─ hosttest/                 主機端 C 測試（不需要板子）
   └─ poc/                      原型驗證（mod_poc：效果模組 ABI）
```

判準：**程式在 `firmware/`，板子差異在 `ports/`，給人看的在 `docs/`，給機器跑的在 `tools/`。**

---

## 2. 我要找的東西在哪

| 你想找 | 去哪 |
|---|---|
| 專案現在做到哪、量到什麼數字 | [docs/00_pm/status.md](docs/00_pm/status.md) |
| 接下來要做什麼、分幾期 | [docs/00_pm/roadmap.md](docs/00_pm/roadmap.md) |
| NC4 指令有哪些、怎麼加新指令 | [docs/01_protocol/commands.md](docs/01_protocol/commands.md) |
| 有哪些通道、各自要實作什麼 | [docs/01_protocol/transports.md](docs/01_protocol/transports.md) |
| 怎麼建置、怎麼燒、踩過哪些雷 | [docs/02_guides/build_and_flash.md](docs/02_guides/build_and_flash.md) |
| 某個設定放哪、改完怎樣生效（WiFi／燈／幀率） | [docs/02_guides/configuration.md](docs/02_guides/configuration.md) |
| 怎麼跑測試、數字怎麼判讀 | [docs/02_guides/testing.md](docs/02_guides/testing.md) |
| 要調緩衝／棧、要證明「沒變慢」 | [docs/02_guides/measurement-method.md](docs/02_guides/measurement-method.md) |
| 有線／無線／藍牙／ESP-NOW 怎麼選 | [docs/02_guides/connectivity.md](docs/02_guides/connectivity.md) |
| 分層原則（七個程式分區） | [docs/03_notes/component-layout.md](docs/03_notes/component-layout.md) |
| 實際的 C 程式 | `firmware/main/`、`firmware/components/` |
| 這塊板子要什麼 | `ports/hardware/<chip>/<board>/`（硬體）＋ `ports/project/<chip>/<board>/`（項目） |
| 怎麼操作板子 | [tools/README.md](tools/README.md) |
| **打開網頁工作臺（WebMaster）** | Windows：雙擊 `web_tools\WebMaster.bat`；macOS：雙擊 `web_tools/WebMaster.command`；Linux：`./web_tools/web.sh` → http://127.0.0.1:8010/ |
| 燒錄有三條路（NC4 OTA／server esptool／頁面燒錄） | 見 [web_tools/README.md](web_tools/README.md) 的「燒錄：三條路徑」 |
| 環境缺咩、點裝 ESP-IDF | UI 左邊「環境」頁；或 `GET /api/env` |
| 網頁工具的文件／API／全部程式 | [web_tools/README.md](web_tools/README.md)（**唯一入口**） |
| 唔記得邊個啟動腳本啱我部機 | 見上面「啟動腳本對照」表；或 `curl /api/scripts` |

### 啟動腳本對照

**網頁工具全部喺 `web_tools/`；建置腳本喺 repo 根。** 兩者不再混在一起。

| 用途 | Windows | macOS | Linux |
|---|---|---|---|
| **開網頁工作臺**<br>（`web_tools/`） | `WebMaster.bat`（雙擊）<br>`.\web_tools\web.ps1 [-Serial COM13]` | `WebMaster.command`（雙擊）<br>`./web_tools/web.sh` | `./web_tools/web.sh`<br>`./web_tools/run_web.sh` |
| **建置／燒錄／健檢**<br>（repo 根） | `make.bat <args>` | `./build.sh <args>` | `./build.sh <args>` |
| 等於 | `python make.py <args>` | `python3 make.py <args>` | `python3 make.py <args>` |

- Windows 第一次跑會自動建 `web_tools\webmaster\.venv` 並裝 deps（`web.ps1` 負責；
  `WebMaster.bat` 只係揀 Python 再交棒，所以唔使改 ExecutionPolicy）。
- **冇參數 = 真板優先**：跟 [`web_tools/settings.json`](web_tools/settings.json)——`autoconnect: "auto"` 會自動連插住的 ESP（VID 白名單，排除 Intel AMT／藍牙），
  搵唔到板就唔連、**唔會偷偷開模擬**；要模擬就 `"sim"` 或 `--sim`。
- 真板：`.\web_tools\web.ps1 -Serial COM13`（Windows）／`./web_tools/web.sh --serial /dev/cu.usbmodemXXXX`。
- 換 port：改 `web_tools/settings.json` 的 `server.port`，或者 `--port 8011`（命令列贏設定檔）；
  port 被佔用時**預設會自動揀下一個**（`server.auto_port`），唔會再卡住啟動。
- 其他基礎數值（預設連邊個裝置、esptool 的 chip／baud／before、UI 首頁）全部喺
  [`web_tools/settings.json`](web_tools/settings.json)（**同啟動器同層**；可以只寫想改的 key，其餘落返預設池）；本機差異寫 `web_tools/settings.local.json`。
  睇實際生效值：`python run.py --print-config` 或 UI「啟動」頁的**目前設定**卡。
- 上面呢張表喺 UI 都有（**啟動** 頁會自動偵測你嘅平台並排前、其他平台變暗、卡片有位置徽章）。

⚠️ `docs/03_notes/component-layout.md` 描述的是**目標架構**（hw/sys/sw/action/tasks/ui/schema 七區）。
**目前實作只有 `sys/` 與 `app/` 兩區**，其餘仍在 `firmware/main/main.c` 裡。讀那份文件時請分清楚「終點」與「現況」。

---

## 3. 五種「配置」，別再搞混

| 配置 | 檔案 | 誰改 | 進 git |
|---|---|---|---|
| **硬體板身份證**（硬體事實，機器可讀） | `ports/hardware/<chip>/<board>/board.json` | 開發者手寫 | ✅ |
| **硬體接線**（腳位／匯流排；firmware 預設的唯一來源） | `ports/hardware/<chip>/<board>/wiring.json` | 開發者手寫 | ✅ |
| 板子建置設定（L1，後者勝過共通） | `ports/hardware/<chip>/<board>/sdkconfig.defaults` | 開發者手寫；檔尾碎片由 `sync` 產生 | ✅ |
| 能力碎片（L2：ETH／WS／UDP，由 hardware `profiles` 引用） | `ports/_profiles/*.defaults` | 開發者手寫 | ✅ |
| 項目選擇（這塊板跑什麼：seed 清單） | `ports/project/<chip>/<board>/project.json` | 開發者手寫 | ✅ |
| 共通建置設定 | `firmware/sdkconfig.defaults` | 開發者手寫（**不准放板子事實**） | ✅ |
| 分割表 | `ports/hardware/<chip>/<board>/partitions.csv` 或 `firmware/partitions.csv` | 開發者手寫 | ✅ |
| 出廠執行期內容（L3） | `ports/project/<chip>/<board>/data/config/*.json` | 出廠預置；上機後屬使用者 | ✅ |
| 完整展開後的設定 | `artifacts/build/<chip>_<board>/sdkconfig` | **機器** | ❌ 生成物 |
| 依賴鎖定 | `firmware/dependencies.lock` | 機器 | ❌ 生成物 |

**為什麼要分三層**：`flash 大小 / flash 頻率 / PSRAM / CPU 頻率` 在 bootloader 階段就決定，
**永遠不可能用 `config.json` 換**；能 runtime 換的只有 gpio、服務開關這類。混在一起寫，
就會出現「改了沒反應」或「同一份 binary 燒上去開不了機」。

**為什麼再分 hardware / project**：`wiring.json`（腳位）是硬體事實，換項目不變；
`system.json` 的 `links[]` / `routes[]` / `tasks[]` 是「這個項目要收哪幾條線、跑哪些任務」。
同一塊硬體可以有多個 project；`-DPORT`（硬體）決定 build 設定，`-DPROJECT` 只決定**嵌入哪一份 runtime 預置**（`data/config`），唔會改 Kconfig／能力。

```bash
python3 tools/board.py list      # 硬體板 × project 一覽
python3 tools/board.py check     # 抓出「board.json 說 A、sdkconfig 卻是 B」與放錯層的設定
python3 tools/board.py new …     # 新增板子（自動產生兩層骨架，不用改腳本或 CMake）
```

**建置時唯一真相 = 共通 `sdkconfig.defaults` → hardware 覆蓋。**
project 層只放 runtime 設定（`project.json`＋`data/config`），**冇 build 設定**；
能力碎片（eth-rmii／wifi-hosted-*／nc4-ws／nc4-udp）全部寫在 `hardware/board.json` 的 `profiles`。
每塊板的 `artifacts/build/<chip>_<board>/sdkconfig` 是產物：它存在時改 defaults
**不會生效**，要 `python3 make.py build <board> --fresh`（`board.py check` 會提醒）。
細節見 [ports/README.md](ports/README.md)。

---

## 4. 快速開始

```bash
python3 make.py doctor              # 先看環境（IDF / esptool / 板子設定 / 磁碟）
python3 make.py list                # 插著哪塊板子 + repo 支援哪些
python3 make.py build  ESP32-P4-ETH # 硬體 + default project（冇 default = 純硬體）
python3 make.py build  ESP32-P4-ETH --project Demo   # 換 project（只換 runtime 預置）
python3 make.py flash  ESP32-P4-ETH # 建置 + 燒錄（預設 app-flash，不動碟上的資料）
python3 make.py monitor ESP32-P4-ETH # 序列埠監看
python3 make.py outputs             # 歷史產物（artifacts/）

# 之後的更新不用按鍵
tools/ota.py --port /dev/cu.usbmodem* artifacts/ESP32-P4-ETH.bin
tools/selftest.py --port /dev/cu.usbmodem*     # 一鍵自測：STATUS / 解碼 / OTA
```

建置產物全部集中在 `artifacts/`：

```text
artifacts/
├─ <board>.bin              最新 app image（OTA 用這個）
├─ <board>/                 bootloader.bin / partition-table.bin / app / flash_args
├─ build/<chip>_<board>/    所有 CMake / ninja 中間檔（每塊板一個）
└─ logs/
```

`artifacts/<板子>.bin` 永遠是最新那顆；再建一次時，上一顆才退成帶時間戳的歷史檔
（附 `.json` 側錄：git / IDF / sha256 / 尺寸）。`firmware/` 不會再出現
`build/`、`build_p4/`、`build_p4c6/` 或 `sdkconfig`。要建全部板子：
`python3 make.py build --all`。細節見 [tools/README.md](tools/README.md)。

直接操作 IDF 時，`idf.py` 要在 `firmware/` 下跑；要跟 `make.py` 用同一個 build dir：

```bash
source ~/esp/esp-idf/export.sh
cd firmware
idf.py -B ../artifacts/build/P4_ESP32-P4-ETH \
       -DSDKCONFIG=../artifacts/build/P4_ESP32-P4-ETH/sdkconfig \
       -DPORT=P4/ESP32-P4-ETH -DIDF_TARGET=esp32p4 build
```

---

## 5. 現況

Phase A：USB composite（MSC + CDC）已通；NC4 解碼器 / 通道框架 / OTA / 記憶體原語已完成並在板上驗證。

ESP32-P4 乙太網：純 TCP 7.4 / 7.1 MB/s；NC4 over TCP 在 PUSH 收斂模式（RX core1、解析 core0）
**6.0–6.4 MB/s**（1K/4K/8K 差距 < 3%），跨核 hub 交換 **2.0 µs/次、零撕裂**。
量測與踩雷見 [p4-bringup.md](docs/03_notes/p4-bringup.md)、[testing.md](docs/02_guides/testing.md)。

下一步：真零複製（parser 直接在插槽上解析）、PUSH 長時 soak、OTA rollback、
第二條 CDC（NC4 與 log 分離）、多通道轉接頭。

環境：ESP-IDF **v6.1**（`/home/user/git/s31-verify/esp-idf`；另有 v6.0.3 的
`~/esp/esp-idf`）。2026-11-16 用兩層 ports 從乾淨的 `build/` 重建通過 —
P4-ETH `0xff040`（1020 KB，含 WS/UDP/HTTP）、S3-N16R8 `0x118e90`（1.1 MB，無 WS）。
（2026-11-02 在 IDF 6.0.3 的量測是 P4 623 KB／S3 349 KB；6.1 的元件與設定不同，不要直接比。）
安裝與踩雷記錄見 [build_and_flash.md](docs/02_guides/build_and_flash.md)。
