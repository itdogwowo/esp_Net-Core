# esp_Net-Core 文件索引

> **用途**：全部文件的單一入口。依「你是誰」分三類，從這裡找你要的。
> **最後整理**：2026-10（建立三分類，對齊 mp_Net-Core 的 `doc/` 結構）

## 三個分類怎麼選

| 分類 | 看什麼的人 | 內容 |
|---|---|---|
| **01_protocol** 協議層 | 對接方 / 寫 transport / 寫 PC 工具的人 | NC4 在哪些通道上跑、每個通道要自己做什麼 |
| **02_guides** 使用教學 | 要 build / 燒錄 / 跑測試的人 | 工具怎麼用、測試怎麼跑、數字怎麼判讀 |
| **03_notes** 筆記 | 維護者 / 想了解設計脈絡的人 | 計劃、架構、分層、記憶體設計 |

**數量**：`00_status` 1 份、`01_protocol` 1 份、`02_guides` 2 份、`03_notes` 12 份。

> **趕時間先看**：[00_status.md](00_status.md) —— 一頁涵蓋「已驗證的數字、已定案的架構、還沒做的事、要測的場景」。

---

## 01_protocol — 協議層

| 文件 | 內容 |
|---|---|
| [commands.md](01_protocol/commands.md) | **指令現況與新增規則**：三條規則（先查舊 schema / 不開新域 / 測試放 0xFxxx）、已實作清單、不得不加的指令 |
| [transports.md](01_protocol/transports.md) | **通道支援範圍**：PULL / PUSH 兩種驅動、USB CDC / UART / TCP / ESP-NOW / I2C / SPI / CAN 各自要做的分段與時序、加一條通道的成本 |

> 待補：`nc4_wire.md`（封包格式 / CRC / schema 驅動 payload / 定址模型）。目前這些知識在
> 程式碼（`components/sys/nc4/`）與舊專案的 `doc/01_protocol/01_nc4_protocol.md`。

---

## 02_guides — 使用教學

| 文件 | 內容 |
|---|---|
| [build_and_flash.md](02_guides/build_and_flash.md) | **建置與燒錄**：IDF v6.0.3 安裝、macOS 26 的兩個地雷（libexpat / pip truststore）、`app-flash` vs `flash`、疑難排解 |
| [testing.md](02_guides/testing.md) | **測試方法與結果**：環境、9 個解碼器 case、benchmark 怎麼打、OTA 怎麼推、怎麼重跑、未測清單、紀錄表 |

---

## 03_notes — 筆記

| 文件 | 內容 |
|---|---|
| [rewrite-plan.md](03_notes/rewrite-plan.md) | **重寫計劃**：背景、已定案、非目標、Phase 0–5、開放決策 |
| [architecture-flow.md](03_notes/architecture-flow.md) | **執行期流程**：mode 決策、NC4 檔案編輯、frame pipeline、ownership matrix |
| [component-layout.md](03_notes/component-layout.md) | **三層分區與 component 佈局**：hw/sys/sw/action/tasks/ui/schema 七區、`ports/` 專案層、依賴方向 |
| [p4-bringup.md](03_notes/p4-bringup.md) | **ESP32-P4 上板紀錄**：IDF 版本、console/PSRAM 陷阱、乙太網吞吐調校（1.1 → 8 MB/s） |
| [failure-policy.md](03_notes/failure-policy.md) | **失敗處理**：config 壞掉/超範圍、PHY 協商失敗、拔線、USB MSC 不支援時的替代路徑（HTTP/NC4 file） |
| [config-layers.md](03_notes/config-layers.md) | **配置分層與開機順序**：System（硬體→driver→NC4 通道）vs Service；config.json 的七層；NC4 link 的 core/slots/slot_size 怎麼寫 |
| [writing-a-service.md](03_notes/writing-a-service.md) | **怎麼寫一個 Service（C）**：declare→start→run→stop 生命週期、跟平常 FreeRTOS 的差異、完整範例、檢查清單、目前限制 |
| [rx-architecture.md](03_notes/rx-architecture.md) | **收／解路徑正式架構（規劃）**：分核／零複製的能力邊界、缺什麼、component 切法、config 與記憶體、P0–P5 遷移 | 
| [buffer-manager-status.md](03_notes/buffer-manager-status.md) | **緩衝管理器實作現況**：三個原語、N 的語義、不變量、測試結果、已知缺陷 |
| [memory.md](03_notes/memory.md) | **記憶體原語**：pool / arena 兩種壽命、三種 heap class、為什麼熱路徑不 malloc、緩衝能不能靠 config 改 |
| [buffer-manager.md](03_notes/buffer-manager.md) | **緩衝管理器（設計定案、未實作）**：三階段 declare→commit→take、四種機制、三層 tier、撇除規則、S3 記憶體圖、P0–P4 分期、待決事項。**接手實作先讀這份** |
| [extension-architecture.md](03_notes/extension-architecture.md) | **擴充架構（兩個插槽、一個帳本、三個仲裁）**：為什麼 TFT 吃 config、LVGL 必須是 code；把 gpio_claim / hw=("lcd") / nc_plan 收斂成一個 ledger；接線 / 排程 / 畫面三個仲裁；六個現在就存在的衝突；P0–P5 計劃 |
| [fx-loader.md](03_notes/fx-loader.md) | **效果載入器實作**：`.fxe` 48 B 檔頭、載入六步 C 骨架、原子指標交換、八種失敗模式對照表、資料驅動替代方案、M1–M5 里程碑、**§10 服務化（三層：wasm / fx / module）與「彈性」的邊界** |
| [effect-language.md](03_notes/effect-language.md) | **分離式固件與燈效語言**：什麼進固件、什麼是檔案（+ 什麼時候才要再燒一次）、六條路線比較（params / IR / 自訂 DSL / WASM AOT / 原生 ELF / mpy）、為什麼選 WAMR + TinyGo、**效果模組 ABI v0**、記憶體放哪裡、P0 spike 清單 |

---

## 相關（不在 docs/ 下）

| 位置 | 內容 |
|---|---|
| `tools/flash.sh` | 燒錄（會自己找 ROM 埠、燒完 watchdog reset 回 app、印心跳） |
| `tools/ota.py` | 用 NC4 OTA 推 firmware，不需要 download mode |
| `tools/nc4.py` | 送單一 NC4 指令（reboot / dfu / raw / listen） |
| `tools/nc4_test.py` | 解碼器一致性測試（9 個 case） |
| `ports/<chip>/<board>/data/` | 出廠預置（`config.json`），build 時嵌進 firmware |
| `components/sys/nc4/` | NC4 framing + 解碼器 + 通道框架 |
| `components/sys/mem/` | pool / arena + config 驅動的尺寸 |
| `components/app/ota/` | OTA 0x22xx handler |
