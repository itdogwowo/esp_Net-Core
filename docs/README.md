# esp_Net-Core 文件索引

> **用途**：全部文件的單一入口。依「你是誰」分四類，從這裡找你要的。
> **最後整理**：2026-11（新增 `00_pm/` 項目管理層，把「計劃」與「技術文件」分開）

## 四個分類怎麼選

| 分類 | 看什麼的人 | 內容 | 份數 |
|---|---|---|---|
| **00_pm** 項目管理 | 想知道「做到哪、接下來做什麼」的人 | 現況、路線圖、決策與待辦 | 3 |
| **01_protocol** 協議層 | 對接方 / 寫 transport / 寫 PC 工具的人 | NC4 在哪些通道上跑、每個通道要自己做什麼 | 3 |
| **02_guides** 使用教學 | 要 build / 燒錄 / 跑測試 / 選連線的人 | 工具怎麼用、測試怎麼跑、連線怎麼選、數字怎麼判讀、**量測方法怎麼重跑** | 4 |
| **03_notes** 設計筆記 | 維護者 / 想了解設計脈絡的人 | 架構、分層、路由、任務模型、記憶體、研究、上板紀錄 | 24 |

> **趕時間先看**：[00_pm/status.md](00_pm/status.md) —— 一頁涵蓋「已驗證的數字、已定案的架構、還沒做的事、要測的場景」。

---

## 00_pm — 項目管理

| 文件 | 內容 |
|---|---|
| [status.md](00_pm/status.md) | **單頁現況**：已驗證的數字（各通道吞吐／RTT／上限）、已定案架構、未完成事項、待測場景 |
| [roadmap.md](00_pm/roadmap.md) | **重寫計劃（歷史分期）**：背景、已定案、非目標、Phase 0–5 現況、開放決策。Phase 0／1 已完成，近期清單看 status.md §7 |
| [README.md](00_pm/README.md) | 這一層放什麼、不放什麼（判準） |

## 01_protocol — 協議層

| 文件 | 內容 |
|---|---|
| [commands.md](01_protocol/commands.md) | **指令現況與新增規則**：三條規則（先查舊 schema / 不開新域 / 測試放 0xFxxx）、已實作清單、**`0xFxxx` 整個區段都是假指令**、上游 `sys` 域逐條對照（含 `0x1007 SYS_TASK_SET` 未做） |
| [frame.md](01_protocol/frame.md) | **幀格式與協議參數**：`SOF / VER / ADDR / CMD / LEN / DATA / CRC32` 逐欄、CRC 覆蓋範圍、`8192` 是經驗參數不是規格、`MAX_PAYLOAD` / `RX_BUF_SIZE` / `SEND_CAP` 三個數字別搞混、`DATA` 與「回覆緩衝」的分別 |
| [transports.md](01_protocol/transports.md) | **通道支援範圍**：PULL / PUSH 兩種驅動、USB CDC / UART / TCP / ESP-NOW / I2C / SPI / CAN 各自要做的分段與時序、加一條通道的成本 |

> 封包格式 / CRC / 定址模型已寫入 `frame.md`；schema 驅動的 `DATA` 欄位仍以上游
> `slave/schema/*.json` 為權威表。

---

## 02_guides — 使用教學

| 文件 | 內容 |
|---|---|
| [build_and_flash.md](02_guides/build_and_flash.md) | **建置與燒錄**：IDF v6.0.3 安裝、macOS 26 的兩個地雷（libexpat / pip truststore）、`app-flash` vs `flash`、疑難排解 |
| [testing.md](02_guides/testing.md) | **測試方法與結果**：環境、9 個解碼器 case、benchmark 怎麼打、OTA 怎麼推、怎麼重跑、未測清單、紀錄表 |
| [measurement-method.md](02_guides/measurement-method.md) | **量測方法（可重跑）**：四條原則（含**用物理上限當參考線／達標率**）、S0–S6 流程、`buf_probe.py` 每個子命令回答什麼、尺寸公式（region/slot/slots/stack/環）、雙核＋同核隔離驗收、偶發事件追法、**十條踩過的陷阱**、紀錄模板（多了「物理上限／達標率」兩欄）、OTA-only 的安全規則 |
| [connectivity.md](02_guides/connectivity.md) | **連線方式怎麼選**：`lan` / `wifi` / `bt` / `espnow` 的分別、每個參數的意思、能力矩陣（哪塊板支援什麼）、一起開會怎樣、藍牙專章 |

---

## 03_notes — 設計筆記

| 文件 | 內容 |
|---|---|
| [architecture-flow.md](03_notes/architecture-flow.md) | **執行期流程**：mode 決策、NC4 檔案編輯、frame pipeline、ownership matrix |
| [decode-routes.md](03_notes/decode-routes.md) | **解碼路線與接收緩衝（現況）**：五條通道各自跑在哪一核、用哪塊緩衝、分派路徑；「執行完才能收下一個」對一半；heap/BSS 的隱形緩衝清單與優先序 |
| [task-model.md](03_notes/task-model.md) | **任務模型（房東／租戶）**：核心 vs 任務的分界、三個 component、開機流程、`tasks.json` 格式與補寫規則、`.fxe` 怎麼變任務、已驗證清單 |
| [task-lifecycle.md](03_notes/task-lifecycle.md) | **任務生命週期**：建立前要準備什麼（欄位／上限）、四步各做什麼與失敗路徑、資源怎麼加總與 carve、`level` vs `layer` 的實際保證（含缺口）、停止順序與**歸還表**（plan 記憶體不還）、狀態機與 log 對照、6 個已知缺口 |
| [task-fx-open-questions.md](03_notes/task-fx-open-questions.md) | **任務與 fx 的未定問題清單**：已定的 20 條、待決定、已移出的 fx 細節 |
| [routing.md](03_notes/routing.md) | **解碼路由表（Router × 引擎 × 任務）**：舊專案 `self` / `vbus` 在分什麼、引擎為何不用宣告（decode 由 routes 推、fx 由 manifest 推）、`out` 四種組合、舊→新對照、優先權與配額、開機表、`.fxe` 與 manifest 兩個格式、與現況程式的六項落差 |
| [naming.md](03_notes/naming.md) | **命名慣例**：一個子系統一個前綴（`nc4_`/`nc_plan_`/`nc_task_`/`nc_store_`/`nc4_ws_`/`nc_fx_`）、**`<域>_task` 與 `<域>_action` 後綴**（同名域不混淆，例 pixel）、檔名＝前綴＋角色、型別與列舉規則、新舊對照、**§5 術語表（FXE 引擎／FXE 模組／FXE 容器／pixel 渲染器）** |
| [component-layout.md](03_notes/component-layout.md) | **三層分區與 component 佈局（目標架構）**：hw/sys/sw/action/tasks/ui/schema 七區、`ports/` 專案層、依賴方向。⚠️ 目前只落地 `sys/` 與 `app/` |
| [file-write-path.md](03_notes/file-write-path.md) | **分段寫入可行性**：mp 版 `.tmp`→`sha256`→`.bak`→rename 演算法、空間預算（覆蓋要 2×）、實測 141 KB/s、斷電恢復、四個必做條件 |
| [fat-ownership.md](03_notes/fat-ownership.md) | **FAT 擁有者（互斥模型）**：為什麼不能同時掛碟與讀檔、為什麼只切 mount_point 不夠、LUN 拆建＋USB 重新列舉、實測數據 |
| [config-layers.md](03_notes/config-layers.md) | **配置分層（一個層一個檔案）**：`config/hardware.json`→`driver`→`system`→`network`→`tasks`→`effects`→`ui`；每層自己讀、缺檔用預設、壞檔只退化該層；開機順序（硬體先起來才讀分配）；`system.json` 的 `links[]` / `routes[]` 與 `tasks.json` 怎麼寫 |
| [p4-bringup.md](03_notes/p4-bringup.md) | **ESP32-P4 上板紀錄**：IDF 版本、console/PSRAM 陷阱、乙太網吞吐調校（1.1 → 8 MB/s） |
| [failure-policy.md](03_notes/failure-policy.md) | **失敗處理**：config 壞掉/超範圍、PHY 協商失敗、拔線、USB MSC 不支援時的替代路徑（HTTP/NC4 file） |
| [writing-a-task.md](03_notes/writing-a-task.md) | **怎麼寫一個 Task（C）**：`nc_task_register()` 之後全交給管理器（plan 宣告 / 開 loop / 量測 / 狀態）；身份（kind）/接線（mem·claims·**provides**）/執行（loops：core·layer·節奏）/生命（on_start·on_stop·attach·detach）；**§3.2 指令＝action，不是任務的財產** |
| [hardware-bus.md](03_notes/hardware-bus.md) | **硬體歸屬（設計紀錄，未實作）**：現在是裝配層人工分配（一裝置一 owner）、為什麼不需要執行期仲裁、哪天要做時的形狀與位置（task 層），以及三個觸發條件 |
| [rx-architecture.md](03_notes/rx-architecture.md) | **收／解路徑正式架構（規劃）**：分核／零複製的能力邊界、缺什麼、component 切法、config 與記憶體、P0–P5 遷移 |
| [buffer-manager-status.md](03_notes/buffer-manager-status.md) | **緩衝管理器實作現況**：三個原語、N 的語義、不變量、測試結果、已知缺陷 |
| [memory-budget.md](03_notes/memory-budget.md) | **記憶體預算（S3 實測 map）**：內部 SRAM 126 KB 靜態 + 40 KB 執行期、開機剩 ~168 KB、與 MicroPython 對比、**開 WiFi 的 90–120 KB 成本**（Espressif 官方表）、怎麼自己量 |
| [buffer-tightening.md](03_notes/buffer-tightening.md) | **緩衝區貼身化（2026-11-08 實測）**：每條 link／任務的緩衝逐一量測再收斂（UART 4×256、RX/TX 1 KB、棧 1.5 KB；decode/main 3 KB）；抓到 UART「等收滿才返回」、測試回覆幀 2.6 KB 上棧兩個真問題；雙核隔離與前後吞吐對照 |
| [buffer-manager.md](03_notes/buffer-manager.md) | **緩衝管理器（設計定案、未實作）**：三階段 declare→commit→take、四種機制、三層 tier、撇除規則、S3 記憶體圖、P0–P4 分期、待決事項。**接手實作先讀這份** |
| [memory.md](03_notes/memory.md) | **記憶體原語**：pool / arena 兩種壽命、三種 heap class、為什麼熱路徑不 malloc、緩衝能不能靠 config 改 |
| [extension-architecture.md](03_notes/extension-architecture.md) | **擴充架構（兩個插槽、一個帳本、三個仲裁）**：為什麼 TFT 吃 config、LVGL 必須是 code；把 gpio_claim / hw=("lcd") / nc_plan 收斂成一個 ledger；接線 / 排程 / 畫面三個仲裁；六個現在就存在的衝突；P0–P5 計劃 |
| [fx-contract.md](03_notes/fx-contract.md) | **fx 模組契約**：安全邊界（模組只能碰自己的線性記憶體）、交接緩衝區佈局與時序、ABI 表、失敗模型、狀態機、P0–P4 計劃 |
| [fx-loader.md](03_notes/fx-loader.md) | **FXE 容器格式與載入流程**：`.fxe` 48 B 檔頭、**§3.1 manifest（模組自我宣告：kind / core / layer / fps / px / 預算 / 指令）**、載入六步 C 骨架、原子指標交換、八種失敗模式對照表、資料驅動替代方案、M1–M5 里程碑、**§10 服務化（三層：wasm / fx / module）與「彈性」的邊界** |
| [fxe-engine.md](03_notes/fxe-engine.md) | **FXE 引擎是什麼**：**延後綁定**機制（把本來要重燒才能改的功能變成換檔案就換）；什麼時候用／不要用（UI → 驅動 → 效果）；六件專責；三個規則（單一 active / tick / 預算 strike）；arena 是宣告值、只給被載入的模組；與 task manager / plan / backend 的接口；名詞表 |
| [effect-language.md](03_notes/effect-language.md) | **分離式固件與燈效語言**：什麼進固件、什麼是檔案（+ 什麼時候才要再燒一次）、六條路線比較（params / IR / 自訂 DSL / WASM AOT / 原生 ELF / mpy）、為什麼選 WAMR + TinyGo、**效果模組 ABI v0**、記憶體放哪裡、P0 spike 清單 |

---

## 相關（不在 docs/ 下）

| 位置 | 內容 |
|---|---|
| [../README.md](../README.md) | 專案入口：兩層地圖 + 四種配置在哪 |
| [../firmware/README.md](../firmware/README.md) | ESP-IDF 工程：目錄、配置疊層、出廠預置怎麼進裝置 |
| [../tools/README.md](../tools/README.md) | 主機端工具：哪支工具、需要板子嗎 |
| [../ports/README.md](../ports/README.md) | 板子配置：怎麼加一塊新板子 |
| `firmware/components/sys/nc4/` | NC4 framing + 解碼器 + 通道框架 |
| `firmware/components/sys/mem/` | pool / arena + config 驅動的尺寸 |
| `firmware/components/action/ota_action/` | OTA 0x22xx（action） |
| `firmware/components/action/file_action/` | 檔案查詢 / STATUS_SET（action） |
