# esp_Net-Core

ESP32-P4（另有 S3 port）上的 **NC4 通訊核心**：多條傳輸通道 → 解碼 → 指令執行，
外加分核（RX core1 / decode core0）、零複製、可調的緩衝管理。C / ESP-IDF v6.0.3。

> 這份 README 是**唯一入口**，只回答一件事：**什麼東西在哪一層、要去哪裡找。**
> 深入內容從 [docs/README.md](docs/README.md) 進去。

---

## 1. 兩層：專案管理 vs 實際原始碼

```text
esp_Net-Core/
│
├─ README.md                    ← 你在這裡
├─ make.py                      ← 建置入口（python3 make.py p4）
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
│  ├─ components/               可重用元件
│  │  ├─ sys/                   nc4（framing/解碼/通道框架）、mem、tasks、transport
│  │  └─ app/                   ota
│  ├─ sdkconfig.defaults        共通建置配置（晶片層）
│  ├─ partitions.csv            分割表
│  └─ build/  managed_components/  sdkconfig  ← 生成物，不進 git
│
├─ ports/                       ──【配置層】每塊板子的差異，不屬於程式 ──
│  ├─ S3/ESP32-S3-N16R8/data/config.json
│  └─ P4/ESP32-P4-ETH/{sdkconfig.defaults, data/config.json}
│
└─ tools/                       ──【工具層】主機端，不會進 firmware image ──
   ├─ ncbuild/                  建置工具實作（make.py 的引擎）
   ├─ board.py  board.sh        板子清單 / 檢查 / 新增（board.sh 是薄殼）
   ├─ flash.sh                  逃生門燒錄
   ├─ nc4.py  ota.py  selftest.py  nc4_test.py   對設備說話
   ├─ bench/                    量測（要板子）
   ├─ hosttest/                 主機端 C 測試（不需要板子）
   └─ poc/                      原型驗證（fx_poc：效果模組 ABI）
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
| 怎麼跑測試、數字怎麼判讀 | [docs/02_guides/testing.md](docs/02_guides/testing.md) |
| 分層原則（七個程式分區） | [docs/03_notes/component-layout.md](docs/03_notes/component-layout.md) |
| 實際的 C 程式 | `firmware/main/`、`firmware/components/` |
| 這塊板子要什麼 | `ports/<chip>/<board>/` |
| 怎麼操作板子 | [tools/README.md](tools/README.md) |

⚠️ `docs/03_notes/component-layout.md` 描述的是**目標架構**（hw/sys/sw/action/tasks/ui/schema 七區）。
**目前實作只有 `sys/` 與 `app/` 兩區**，其餘仍在 `firmware/main/main.c` 裡。讀那份文件時請分清楚「終點」與「現況」。

---

## 3. 五種「配置」，別再搞混

| 配置 | 檔案 | 誰改 | 進 git |
|---|---|---|---|
| **板子身份證**（硬體事實，機器可讀） | `ports/<chip>/<board>/board.json` | 開發者手寫 | ✅ |
| 板子建置設定（L1，後者勝過共通） | `ports/<chip>/<board>/sdkconfig.defaults` | 開發者手寫；檔尾碎片由 `sync` 產生 | ✅ |
| 能力碎片（L2：ETH / WS / UDP） | `ports/_profiles/*.defaults` | 開發者手寫 | ✅ |
| 共通建置設定 | `firmware/sdkconfig.defaults` | 開發者手寫（**不准放板子事實**） | ✅ |
| 分割表 | `firmware/partitions.csv` | 開發者手寫 | ✅ |
| 出廠執行期內容（L3） | `ports/<chip>/<board>/data/config.json` | 出廠預置；上機後屬使用者 | ✅ |
| 完整展開後的設定 | `firmware/sdkconfig` | **機器** | ❌ 生成物 |
| 依賴鎖定 | `firmware/dependencies.lock` | 機器 | ❌ 生成物 |

**為什麼要分三層**：`flash 大小 / flash 頻率 / PSRAM / CPU 頻率` 在 bootloader 階段就決定，
**永遠不可能用 `config.json` 換**；能 runtime 換的只有 gpio、服務開關這類。混在一起寫，
就會出現「改了沒反應」或「同一份 binary 燒上去開不了機」。

```bash
python3 tools/board.py list      # 板子 × 能力一覽
python3 tools/board.py check     # 抓出「board.json 說 A、sdkconfig 卻是 B」與放錯層的設定
python3 tools/board.py new …     # 新增板子（自動產生四個檔，不用改腳本或 CMake）
```

**建置時唯一真相 = `sdkconfig.defaults` + 板子覆蓋。** `firmware/sdkconfig` 是產物：
它存在時改 `sdkconfig.defaults` **不會生效**，要先刪掉（`board.py check` 會提醒）。
細節見 [ports/README.md](ports/README.md)。

---

## 4. 快速開始

```bash
python3 make.py doctor              # 先看環境（IDF / esptool / 板子設定 / 磁碟）
python3 make.py list                # 插著哪塊板子 + repo 支援哪些
python3 make.py build  p4           # 建置（板子名 / 晶片名都可以；會自動處理 target 切換）
python3 make.py flash  p4           # 建置 + 燒錄（預設 app-flash，不動碟上的資料）
python3 make.py monitor p4          # 序列埠監看
python3 make.py outputs             # 歷史產物（artifacts/）

# 之後的更新不用按鍵
tools/ota.py --port /dev/cu.usbmodem* artifacts/ESP32-P4-ETH_20261102_022530.bin
tools/selftest.py --port /dev/cu.usbmodem*     # 一鍵自測：STATUS / 解碼 / OTA
```

每次建置成功會在 `artifacts/` 留一顆帶時間戳的 image 加一份 `.json` 側錄
（git / IDF / sha256 / 尺寸），OTA 回滾測試直接拿它。細節見 [tools/README.md](tools/README.md)。

直接操作 IDF 時，`idf.py` 要在 `firmware/` 下跑：

```bash
source ~/esp/esp-idf/export.sh
cd firmware && idf.py -DPORT=P4/ESP32-P4-ETH -DIDF_TARGET=esp32p4 build
# 或留在 repo 根：idf.py -C firmware ...
```

---

## 5. 現況

Phase A：USB composite（MSC + CDC）已通；NC4 解碼器 / 通道框架 / OTA / 記憶體原語已完成並在板上驗證。

ESP32-P4 乙太網：純 TCP 7.4 / 7.1 MB/s；NC4 over TCP 在 PUSH 收斂模式（RX core1、解析 core0）
**6.0–6.4 MB/s**（1K/4K/8K 差距 < 3%），跨核 hub 交換 **2.0 µs/次、零撕裂**。
量測與踩雷見 [p4-bringup.md](docs/03_notes/p4-bringup.md)、[testing.md](docs/02_guides/testing.md)。

下一步：真零複製（parser 直接在插槽上解析）、PUSH 長時 soak、OTA rollback、
第二條 CDC（NC4 與 log 分離）、多通道轉接頭。

環境：ESP-IDF v6.0.3（`~/esp/esp-idf`）。2026-11-02 從乾淨的 `build/` 重建通過 —
P4 `0x9b830`（≈623 KB，含 WS/UDP/HTTP）、S3 `0x57050`（≈349 KB，無 WS）。
安裝與踩雷記錄見 [build_and_flash.md](docs/02_guides/build_and_flash.md)。
