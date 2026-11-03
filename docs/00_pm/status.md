# 專案現況總結（單頁）

> **用途**：所有「已經驗證的事實、已定案的設計、還沒做的事」集中在這一頁；
> 細節都連到對應文件。**最後更新**：2026-11-03。

---

## 1. 這是什麼

ESP32-P4（另有 S3 port）上的 **NC4 通訊核心**：多條傳輸通道 → 解碼 → 指令執行，
外加分核（RX core1 / decode core0）、零複製、可調的緩衝管理。

| 面向 | 狀態 |
|---|---|
| 晶片 | ESP32-P4 rev1.3（ECO2）、360 MHz、32 MB PSRAM、IDF 6.0.3 |
| 網路 | 100 Mbps full duplex（RMII + IP101），靜態 IP `10.10.1.50/16` |
| 通道 | TCP :6000、UDP :6001、WS :8080/nc4、HTTP :8080（設定） |
| 解碼 | 分核、零複製（一格一整幀）、task notification |
| 架構 | 系統任務（硬體→driver→通道）＋ 一般任務（生命週期）已分層，兩者同一套任務管理器 |
| 任務模型 | **核心（房東）／任務（租戶）**：`tasks.json` 只管租戶；`level` / `affinity` 由 config 定，**未設定 = `[0,0]` = 唔跑**；缺的條目開機自動補寫（寫 `[0,0]`，預設關閉）（[task-model.md](../03_notes/task-model.md)） |
| .fxe | 掃 `/data/pixel/effects/` 與 `/data/tasks/`，attach 成 runtime 任務；**WAMR 執行引擎未接** |

---

## 2. 已驗證的數字（2026-11-02，同一台板子）

### 通道吞吐（16 MB/筆，MB/s）

| 通道 | 下載 1K / 4K / 8K | 上傳 1K / 4K / 8K | RTT |
|---|---|---|---|
| **TCP**（分核＋零複製） | 5.65 / 7.13 / **7.40** | 6.05 / 7.43 / **7.42** | **3.56 ms** |
| **WS**（多幀 batch） | 4.30 / 5.72 / 5.26 | 4.99 / — / 6.98 | 6.87 ms |
| **UDP**（≤ MTU） | 1.93（掉 16%）/ ✗ / ✗ | **8.51**（掉 0%）/ ✗ / ✗ | **3.73 ms** |

### 上限與參考

| 項目 | 數值 | 卡在 |
|---|---|---|
| **NC4 單向上限** | **7.5–8.5 MB/s** | TCP 視窗 65,535 ÷ RTT；UDP 卡 MTU |
| TCP 全雙工 | 11.9–12.4 MB/s | 100 Mbps PHY（97–99%） |
| TCP 雙 stream | 9.4–9.9 MB/s | 同上 |
| 純解析（CPU） | 30.7 MB/s（267 µs/8 KB） | CRC32 |
| 純打包（CPU） | 28.8 MB/s（285 µs/8 KB） | CRC32 |
| hub 交換 | 326k–517k /s、torn=0 | 系統負載 |

### 零複製

| 模式 | `copied` | 每幀解析 | 吞吐 |
|---|---|---|---|
| PUSH-slot（兩次複製） | 33.5 MB / 16 MB | 495 µs | 7.18–7.87 |
| **PUSH-frame（零複製）** | **0 B** | **406–418 µs（−18%）** | 7.41–7.53 |

- soak：10 輪 × 16.4 MB → 20,020 幀全到、`resync=0`。
- 真正讓吞吐提升的是「**不要 10 ms 輪詢**」（task notification）：1 KB 幀 0.78 → 5.65 MB/s，
  RTT 10.1 → 3.6 ms。

### 建置驗證（2026-11-03，S3）

| 項目 | 結果 |
|---|---|
| 工具鏈 | ESP-IDF **v6.1.0**（WSL 內），target `esp32s3` |
| 結果 | **0 error、0 warning**，`esp_net_core.bin` = 408,256 B（398.7 KB） |
| 靜態 RAM | **108,463 B（105.9 KB）**＝ DRAM 段 31.7%（清 handler static 之前是 127,583 B） |
| 開機 heap | **233,296 B（227.8 KB）**（之前 214,176 B） |
| 涵蓋 | `nc4.c` / `nc4_bus.c` / **`nc4_router.c`（新檔）** / `main.c` / `nc_task.c` / `nc_cfg.c` / `nc_fx.c` / `nc_file_action.c` |

> 這次 build 順手修好一個**會令 CMake configure 直接失敗**的問題：
> `components/sys/task_manager/CMakeLists.txt` 宣告了 `INCLUDE_DIRS "include"`，
> 但該元件根本沒有 `include/` 目錄（`nc_task.h` 屬於 `../task`）。
> 因為 `build/` 是舊的、ninja 沒有重新 configure，所以一直沒人發現 ——
> **任何人 clone 之後第一次 build 都會撞**。已移除該行並在檔案內註明原因。
> 記憶體細節見 [memory-budget.md](../03_notes/memory-budget.md)。

---

## 3. 已定案的架構

| 主題 | 一句話 | 文件 |
|---|---|---|
| **系統任務 vs 一般任務** | 硬體/driver/NC4 通道是 `kind=system`（固定順序、不可卸載）；效果/UI 是 `kind=normal`（有生命週期）。**service 與 task 是同一件事** | [config-layers.md](../03_notes/config-layers.md) |
| **開機順序** | 硬體 → driver → **宣告（看實際硬體）** → carve → 通道 → 模組 → 任務 | 同上 |
| **任務模型** | 一份宣告寫完身份/接線/執行/生命：`kind`（system·normal·effect）、`level`、`mem[]`、`claims[]`、`provides[]`、`loops[]`、`on_start/on_stop`；管理器負責排序、開 loop、量測。**指令是 action，不在任務上** | [writing-a-task.md](../03_notes/writing-a-task.md) |
| **帳本** | 開機檢查重名 / 指令重複 / 接線獨占衝突 → 印一張表；衝突逐項擋，其他人照跑 | 同上 |
| **資源管理器** | `nc_plan`：declare → commit → get（沒有 release；runtime 模組改用父層 arena） | [buffer-manager.md](../03_notes/buffer-manager.md) |
| **收/解路徑** | 傳輸總線（ID 註冊表）→ 解碼引擎（可換核/可多實例）→ 執行 | [rx-architecture.md](../03_notes/rx-architecture.md) |
| **配置分層** | System / Hardware / Driver / Network / Tasks / Effects / UI | [config-layers.md](../03_notes/config-layers.md) |

**現況**：`main.c` 只剩一張任務表（核心 `nc4` / `net` / `core` / `fx` ＋ 租戶 `probe` / `hello`）
＋ 一個 `actions_register()` ＋ 六行生命週期呼叫（register → declare → start → boot）。
service 與 task 已合併成同一個 `nc_task_t`。
`tasks.json` 已接（含補寫）；`system.json` 只讀 `cID`、`network.json` 只讀 `bt.pin`，
其餘欄位仍是程式預設 + NVS；`hardware.json` / `driver.json` **尚未接線**（有出廠檔但沒 embed、沒讀）。
slots 還沒走 plan。

---

## 4. 失敗處理（已實作 / 待做）

| 類別 | 政策 | 狀態 |
|---|---|---|
| config 壞掉 | 用預設、驗證後才寫、壞的不覆蓋 | 🟡 讀寫已通（HTTP），載入未做 |
| PHY 協商失敗 | 讀寫 BMCR 重啟 autoneg，仍失敗就不啟用通道 | ✅ 實測救回 |
| 拔線 / link down | 關連線、停 accept，link up 重建 | 🟡 只有 log |
| 傳輸錯誤 | 錯誤一律收線、reset parser、閒著 ≠ 壞線 | ✅ |
| 服務失敗 | 逐項失敗，該服務不啟動、其他照跑 | ✅ |
| USB 不支援 | 走 HTTP / USB-JTAG 替代 | 🟡 HTTP 已做 |

詳細：[failure-policy.md](../03_notes/failure-policy.md)。

---

## 5. 硬體事實（踩過才知道）

| 事實 | 影響 |
|---|---|
| P4 這塊板的 USB-C **只接 USB-Serial-JTAG**，沒有接 USB-OTG | TinyUSB（CDC/MSC）永遠列舉不到；但**燒錄/reset 完全正常** |
| 啟用 TinyUSB 後 `tinyusb_console_init()` 會把 console 搬走 | 連 log 都看不到 → 已關閉 |
| **DTR/RTS → GPIO0/EN**（USB 橋或 USB-JTAG 都一樣） | 開埠時動到控制線 → reset + 下載模式（實測：`dtr=False,rts=False` 直接進下載） |
| 下載模式只看「reset 時 GPIO0 的電平」 | **送資料本身永遠不會觸發下載模式** |
| PHY 沒有 reset GPIO | MCU reset 不會重置 PHY → 需要軟體重啟 autoneg |
| UART 數量 | **P4 = 6 條、S3 = 3 條**；console 佔 UART0，其餘可給應用 |
| P4 console 腳位 | GPIO37/38（不是 S3 的 43/44） |
| ESP↔ESP 不能用 USB 對接 | 兩個都是 device，沒有 host；VBUS 還會互灌 → 請走 UART1/2 |

USB 的完整分析：[failure-policy.md §4](../03_notes/failure-policy.md)。

---

## 6. 工具

| 工具 | 用途 |
|---|---|
| `tools/board.sh` / `tools/board.py` | 建置/燒錄（會自動切 target）。`board.py --help` 在 Windows cp950 主控台會因 `↔` 編碼失敗，設 `PYTHONIOENCODING=utf-8` 可繞過 |
| `tools/bench/bench_all.py` | TCP 全表：純 TCP / NC4 上下傳 / CPU / hub |
| `tools/bench/nc4_tcp.py` / `nc4_tx_bench.py` | TCP 下載 / 上傳 |
| `tools/bench/nc4_ws_test.py` | WS：RTT / 吞吐 / 碎片 / 重連 / 並存（10 項） |
| `tools/bench/nc4_udp_test.py` | UDP：RTT / 吞吐 / 掉包 |
| `tools/bench/nc4_stats.py` | 零複製驗證（`copied` 應為 0）+ 邊界測試 |
| `tools/bench/hub_bench.py` | `0xF003` 雙核 hub 交換 |
| `tools/fx.py pack / dump` | 打包 / 檢視 `.fxe`（header + manifest + payload + 指令表） |
| `bash tools/hosttest/run_all.sh` | 一次跑完 7 支 host test（cfg / fxm / task / router / fx / hub / plan），全綠 = 冇 error 亦冇 warning |
| `tools/hosttest/*/run.sh` | 單支 host 測試，不用板子也不用 IDF：hub / plan / fx / **fxm** / **task** / **cfg** / **router**。Windows 上只有 `cfg` / `fxm` / `task` / `router` 有 `run.bat` |
| **WSL + ESP-IDF v6.1.0** | **唯一能 build 的地方**（Windows 冇 IDF）。`firmware/build/` 是兩邊共用的，所以 Windows 上看到的 map 其實是 WSL 上次 build 的結果 |
| `curl http://10.10.1.50:8080/config.json` | 讀寫設定（GET/PUT） |

---

## 7. 還沒做（依優先序）

| # | 項目 | 為什麼 |
|---|---|---|
| 1 | **link down/up state machine** | 拔線/插線後能自己恢復（目前只有 log） |
| 2 | **config loader**（含 clamp/預設） | 現在寫入只影響下次開機 |
| 3 | **指令搬進 `action/`**（`sys_action` / `status_action` / `bench_action` / `test_action`） | `main.c` 有 14 條 handler（10 條是測試）；搬完「看到 `action/` 就知道有指令」，`main.c` 只剩裝配。規則見 [commands.md](../01_protocol/commands.md) §1 |
| 4 | **內建最小預設**（每層一個骨架字串） | 某塊板冇 `ports/<board>/data/config/tasks.json` 時，那個檔**永遠不會被建立**（`NETCORE_HAS_PRESET_TASKS=0`）；見 [config-layers.md](../03_notes/config-layers.md) §0.5 |
| 5 | **慢工作佇列接真實工作**：`esp_ota_end`（整片 SHA-256）搬出解碼路徑 | 現在佇列只有假工作（`0xF005`/`0xF006`）；`esp_ota_end` 是真的會卡住兩核解碼的那種慢 |
| 6 | **loop ready barrier**（`nc_task_ready()`） | layer N 的 loop 未就緒就開 layer N+1；見 [task-lifecycle.md](../03_notes/task-lifecycle.md) §7 |
| 7 | **`nc4.routes[]`（路由器）** | ✅ 表已在固件（`nc4_router.c`，2026-11-03），`decode0/1` 由它決定開唔開；**未接 config**（`system.json` 的 `routes[]` 仍是下一步） |
| 8 | **目錄拆分**（tcp_nc4 / eth 抽成 component） | `main.c` 還是很大 |
| 9 | **USB-Serial-JTAG 指令通道** | P4 就能用 USB 送指令（不用 TinyUSB） |
| 10 | UART0 救援視窗（開機 1.5 s） | 所有路徑都壞時的最後一條 |
| 11 | HTTP `/fs/`（列表/上傳）+ NC4 file service | 沒有 MSC 時換 assets |
| 12 | slots 走 plan | 讓通道緩衝也由宣告決定（`nc_task_stop` 已完成） |
| 13 | 硬體管理器（**等真的有共享才做**） | 現在一裝置一 owner、人工分配就夠；觸發條件見 [hardware-bus.md](../03_notes/hardware-bus.md) |
| 14 | S3 實機重燒 + 長時 soak | 迴歸 |

### 7.1 架構審視：核心基建仍未做 / 漏洞（2026-11-03）

| # | 項目 | 狀態 | 影響 |
|---|---|---|---|
| A | **handler 的 static 緩衝** | ✅ **已清完（2026-11-03）** | 一共 −19,120 B：`reply_json` 2,102 ＋ status/bus 8,864 ＋ file_action `s_tx[8,205]`/`buf[2,048]`。做法：回覆一律砌入**呼叫者棧上的回覆幀**（`reply_open`/`reply_send`），或送出時才 `malloc`。所以兩個 decoder 同時回覆**唔會再撞** |
| B | **任務棧 high-water 量測** | ❌ 冇 | 6,144 夠唔夠全靠估；`nc_task.h` 註解寫「量測」但未實作 |
| C | **`nc_task_set_core()` 死 API** | ✅ **已刪（2026-11-03）** | 它是「停 → 改 core → 起」的**執行期換核**，0 呼叫者，而且同 router 的「開機決定邊個核開 decoder」互相矛盾。已由 `nc_task.h` 與 `nc_task.c` 移除，原位留註解說明「核心在開機時定死」。`nc_task_enable()` 保留（它只是開關，唔換核） |
| D | **router 未接 config、亦未接對外 JSON** | ✅ **已接（2026-11-03）** | 開機 ④ 由 `router_from_cfg()` 讀 `system.json` 的 `routes[]`，每行第一個 `decodeN` 決定那條線由第 N 核解；`decode0`/`decode1` 任務存在與否、CDC/UART/TCP 三條 bus 掛哪核，全部由此推導。讀不到就 `router_defaults()` 退回板子預設並印 `ESP_LOGW`。另加 `0x1105 ROUTE_GET` / `0x1106 ROUTE_RSP` 線上查表。**未做**：`out` 的 link 轉發、兩核各自一個 decoder |
| E | **`hardware.json` / `driver.json` 未 embed 亦未讀** | ❌ | 改那兩個檔完全冇效（`main/CMakeLists.txt` 已註明） |
| F | **S3 冇 WiFi** | ❌ | 只有 P4 有 ETH；S3 的無線能力未接。**實測：目前 image 完全冇 link Wi-Fi**（`libnet80211.a` 等 0 個成員被拉進來），所以現有 105.9 KB 靜態是「無 Wi-Fi」的數字。開 Wi-Fi 的帳：靜態 ≤14.3 KB ＋ 執行期 90–120 KB（見 [memory-budget.md](../03_notes/memory-budget.md) §7） |
| G | **WS / UDP 緩衝未入 plan**（41 KB BSS，P4 專屬） | ⚠️ | 只在 P4 link 進來；未宣告、不可見 |
| H | **TCP PUSH 插槽 65,640 B 未入 plan** | ⚠️ | `calloc`，`keys=mem` 看不到 |
| I | **OTA `esp_ota_end` 仍在解碼路徑** | ❌ | **更正**：唔係「兩核都卡」—— OTA 只會喺一個核跑。真正問題係**收 `OTA_END` 嗰條線嘅解碼迴圈**會停幾百 ms：ACK 遲到、主機可能 timeout 重送，同埋同一條線後面排隊的幀一齊等。佇列（`slow_job_t` + `nc_hub` 8 格）已經存在但未用 |
| J | **FXE 引擎冇執行後端** | ❌ 下一步計劃 | `.fxe` 載入成功但永遠唔跑（WAMR 未接）。**已寫進 [fxe-engine.md](../03_notes/fxe-engine.md)**，屬於下一階段，不是本輪的 bug |
| K | **`level` 只影響宣告/啟動次序，唔係 barrier** | ⚠️ 設計如此 | `level` 保證「呼叫次序」，唔保證「上一層真係開始跑」（`xTaskCreate` 返嚟唔等於 task body 已執行）。要「上一層 ready 才開下一層」就要 `nc_task_ready()`（見 [task-lifecycle.md](../03_notes/task-lifecycle.md) §7） |
| L | **CRC / 壞表頭統計** | ✅ 已加（2026-11-03） | `nc4_parser_errors()`，`BUS_STATS` 會出 `crc_err` / `bad_hdr` |
| M | **`task_manager` 宣告不存在的 `include/`** | ✅ 已修（2026-11-03） | 會令 CMake configure 直接失敗；舊 `build/` 掩蓋了它，**新 clone 第一次 build 必撞** |
| N | **tinyusb 佔 21.2 KB BSS**（`_cdcd_itf` 12,532 + `_mscd_epbuf` 8,192 + `_cdcd_epbuf` 1,024） | ⚠️ 未處理 | 清完 handler static 之後，它變成**我們這邊最大的單一項**；要再省 SRAM 應該先問「USB 一定要開嗎」 |
| O | **`build/` 跨平台共用** | ⚠️ | Windows 冇 IDF，build 只能在 WSL 做；在 Windows 開 `build/` 只會見到上次 WSL 的殘留，容易誤判「已經 build 過」 |
| P | **shell 腳本 CRLF → Linux 全掛** | ✅ 已修（2026-11-03） | `tools/{board,flash}.sh` ＋ `hosttest/{fx,hub,plan}_test/run.sh` 五行尾是 CRLF，在 WSL 跑就 `$'\r': command not found`。已全部轉 LF ＋ 加 `.gitattributes`（`*.sh` 強制 LF、`*.bat` 強制 CRLF） |
| Q | **`nc_fx_scan()` 路徑截斷** | ✅ 已修（2026-11-03） | `char path[NC_FX_PATH_MAX]`＝80 B，但 `d_name` 最長 255 ＋ `dir` 無上限 → 截斷後會**靜靜開到另一個前綴相同的檔**。現在先驗 `snprintf` 回傳值，放唔落就跳過並計入 `bad` |
| R | **FX 狀態 JSON 會送出半截** | ✅ 已修（2026-11-03） | 每個模組 ≈250 B 的 JSON，8 個 ≈2,000 B > 舊的 `part[1,900]` → 主機收到無效 JSON。現在偵測到放唔落就改寫一個**合法**的精簡物件（`"truncated":1` ＋ `"mods":N`） |
| S | **`STATUS_GET` 完整回覆本身會爆** | ✅ 已修（2026-11-03） | 完整 status ≈2.4 KB，但 `NC4_REPLY_JSON_MAX` 只有 2,100 → **一直靜靜截斷**。已提到 2,600，並改成「砌入回覆幀」 |

---

## 8. 待測場景（等使用者提供）

以下需要**實體接線/操作**才能確認，程式面已就緒或已避開：

| 場景 | 想確認什麼 | 需要的條件 |
|---|---|---|
| 拔線 / 插線 | link down→up 後通道是否自動恢復 | 可插拔的網路線 |
| ESP↔ESP（UART1/2） | 下載模式是否還會發生 | 兩塊板、只共地、不接 USB |
| 同一條 UART0 收指令 | console 與資料共存是否穩定 | USB 拔掉、只剩 UART0 RX |
| 壞 config | 開機是否仍能起來 | 手動改壞 `/data/config/{system,network}.json` |
| 電源不穩 | 是否 brownout | 可調電源 |

---

## 9. 文件索引

| 類別 | 文件 |
|---|---|
| 協議 | [commands.md](../01_protocol/commands.md)、[transports.md](../01_protocol/transports.md)、[frame.md](../01_protocol/frame.md)（幀格式權威版） |
| 教學 | [build_and_flash.md](../02_guides/build_and_flash.md)、[testing.md](../02_guides/testing.md)（§2.13–2.16 是量測總表） |
| 筆記 | [config-layers.md](../03_notes/config-layers.md)、[writing-a-task.md](../03_notes/writing-a-task.md)、[task-lifecycle.md](../03_notes/task-lifecycle.md)、[rx-architecture.md](../03_notes/rx-architecture.md)、[failure-policy.md](../03_notes/failure-policy.md)、[p4-bringup.md](../03_notes/p4-bringup.md)、[buffer-manager.md](../03_notes/buffer-manager.md)、[memory.md](../03_notes/memory.md)、[memory-budget.md](../03_notes/memory-budget.md)（SRAM 帳）、[decode-routes.md](../03_notes/decode-routes.md)、[naming.md](../03_notes/naming.md)、[component-layout.md](../03_notes/component-layout.md)、[architecture-flow.md](../03_notes/architecture-flow.md)、[rewrite-plan.md](roadmap.md) |
| 研究 | [effect-language.md](../03_notes/effect-language.md)、[extension-architecture.md](../03_notes/extension-architecture.md)、[fx-loader.md](../03_notes/fx-loader.md) |
