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
| 通道存在性 | `system.json` 的 `links[].enable`（缺省 1）：未啟用 → **不宣告緩衝、不建 bus、路由跳過**；要開就改 config ＋ 重啟（2026-11-04） |
| Wi-Fi | **兩道閘**：編譯期 `CONFIG_NETCORE_WIFI`（預設 n，不編入 = 0 B）＋ 執行期 `network.json` `wifi.enable`（不啟用 = 0 heap） |

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

### 建置驗證（2026-11-04，S3 ＋ P4；**最新數字以此為準**）

| 項目 | S3 | P4 |
|---|---:|---:|
| DIRAM 已用（靜態） | **98,761 B** | **129,863 B** |
| DIRAM 剩餘（heap 上限） | **242,999 B（237 KB）** | **442,505 B（432 KB）** |
| `.bss` / `.data` | 33,328 / 23,246 | 39,880 / 21,837 |
| flash `.bin` | 406,736（Wi-Fi 未編入） | 685,680 |
| 開機後實際剩（估） | ~184–194 KB | ~330 KB |

> 這是 **Phase 1 之後**的數字（epoch／bench／monitor／測試 task 開關）。
> 上一個 commit（只有 epoch／bench）是 97,929 / 129,023；差值主要是 monitor 與 0x1005 的程式碼。

- 與 11-03 相比：S3 靜態 **−9,792 B**；P4 把 WS/UDP/測試殘留搬出 SRAM **≈ −54.8 KB**（逐符號計算）。
- Wi-Fi 編入但 `enable=0`：S3 **+48,992 B 靜態 ＋ 548,768 B flash**；所以預設 `CONFIG_NETCORE_WIFI=n`。
- 完整量測與「怎麼讀」見 [handoff-2026-11-04.md](handoff-2026-11-04.md) §2、[memory-budget.md](../03_notes/memory-budget.md) §9。

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

**現況**：`main.c` 只剩一張任務表（核心 `nc4` / `net` / `core` / `monitor` / `fx` ＋ 租戶 `probe` / `hello` / `bench_a/b/c`）
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

### 7.2 2026-11-04 追加（記憶體／通道閘）

| # | 項目 | 狀態 | 影響 |
|---|---|---|---|
| T | **`router_from_cfg()` use-after-free** | ✅ **已修（2026-11-04）** | 名稱先抄入 `s_route_names[]` 再 `nc_cfg_close()`；`nc4_router_set()` 之後拿到的是有效字串。太長的名字直接跳過（不再截斷） |
| U | **P4 測試 task（4 條）** | ✅ 已加開關（2026-11-04） | `tcp_sink`×2 / `tcp_src`×2 = 4 × 6,144 = **24,576 B**；`CONFIG_NETCORE_TEST_TASKS` 預設 **n**。`tcp_nc4_accept` 係正式通道，不在其中 |
| V | **P4 tinyusb 仍被 link（21,748 B）** | ❌ | 不是 main.c；`nc_store.c` 仍然建 MSC LUN。要 guard 整個 MSC 段 |
| W | `NC_TASK_MAX` 20 格（`s_slots` 7,680 B） | ❌ 不可縮 | 10 個固定任務 ＋ 8 個 `.fxe` = 18；縮到 12 會令 `.fxe` attach 失敗（靜默） |
| X | **狀態管理器（三層：熱／冷／封存）** | 🟡 只定案未實作 | 三層、fast 區純數字、字典放 rodata＋PSRAM、`keys` 解析鏈。**不會省 RAM**（+256 B），價值在單一事實 |
| Y | WS/UDP 改 PSRAM 後的吞吐 | ⚠️ 未量 | 要跑 `tools/bench/nc4_ws_test.py`；P4 的 PSRAM 較快，S3 沒 link 這兩條 |
| Z | `ssid_pw` 明文在 FAT | ⚠️ | 上游搬 btree／NVS；我們應搬 NVS |

### 7.3 2026-11-04 晚（Phase 1）

| # | 項目 | 狀態 | 說明 |
|---|---|---|---|
| A1 | P4 測試 task 開關 | ✅ | `CONFIG_NETCORE_TEST_TASKS`（預設 n）；4 條 task＝24,576 B runtime |
| A2 | `0x1005` 截斷 | ✅ | 改用緊湊清單 `nc_task_list_json()`（name/level/affinity/started/loops）；保證合法 JSON，放不下回 `{"truncated":1}` |
| A3 | round watchdog | ✅ | 新 SYSTEM 任務 `monitor`（core0、1 s、`no_epoch`）：`epoch.rounds` 停滯 5 s → log（含任務清單）；預設**不重啟** |
| A4 | `NC_TASK_MAX` 20→12 | ❌ 取消 | 10 固定 ＋ 8 `.fxe` = 18，20 格必要 |

> A3 的代價：monitor 自己一條 3,072 B 棧（runtime）。淨效果：P4 測試 task −24,576、
> monitor +3,072 → **約 −21.5 KB**。

### 7.4 2026-11-04 晚（死碼清理，行為不變）

依 [disposition.md](disposition.md) 的 B 節執行「真正該移除」那一批：

| # | 移除 | 說明 |
|---|---|---|
| B2b | `DEV_BENCH` mode 2/3/4 | erase／write OTA 分區：任何通道一幀就能擦 OTA slot。現在送 >1 的 mode 回 `{"err":"mode"}`（不再靜默當成 decode） |
| B3 | `NC_DECODE_CORE` 巨集 | router 已取代；原位留一行指出「核由 `routes[].out` 決定」 |
| B4 | `nc4_bus_start()` | 連 header 宣告、`s_task`、`DEFAULT_TASK_STACK/PRIO` 一起刪；decode0/1 由任務管理器建立 |
| B5 | `NC4_CMD_RESET_TO_DOWNLOAD` | 0x1019 在 `nc4.h` 留註解：保留給 `ASSET_RELOAD` |
| B6 | 重複 `#include "nc_task.h"` | — |

驗證：S3／P4 皆零 warning 建置成功；`tools/hosttest/run_all.sh` 8 項全過。
DIRAM 不變（S3 98,761／P4 129,863），flash 略減。

### 7.5 2026-11-04 晚（刪 slow_worker，落實 execution-rules §7）

| # | 項目 | 狀態 | 說明 |
|---|---|---|---|
| A1 | `slow_worker` + `s_task_core` | ✅ 已刪 | 指令層嚴格一問一答，「慢指令擋住後面」天然不存在（execution-rules §7 早已裁決） |
| A2 | plan `"jobs"`（8 槽）＋ `s_job_hub` | ✅ 已刪 | 少一個 hub、少 4 KB stack、少每秒 1000 次喚醒 |
| A3 | `0xF005 DEV_SLOW` / `0xF006 UART_BENCH` | ✅ 已刪 | `s_core_actions[]` 15 → 13 條 |
| A4 | UART driver 改開機安裝 | ✅ 修 bug | 以前只由 `UART_BENCH` lazy init → **UART 通道要打過 bench 才會通**；現由 `task_net_start()` 安裝 |

> flash：S3 −3,936 B（406,208 → 402,272）、P4 −4,992 B（685,072 → 680,080）。

### 7.6 2026-11-04 晚（測試歸位：`test_*_tasks` ＋ `NC_ENABLE_TESTS`）

按 [execution-rules §6](execution-rules.md) 的約定落實「測試唔污染功能結構」：

| 動作 | 內容 |
|---|---|
| 搬目錄 | `app/tasks/bench` → `app/test_bench_tasks`；`app/tasks/hello` → `app/test_hello_tasks`；`app/fx` → `app/test_fx_tasks`；`sys/monitor` → `app/test_monitor_tasks` |
| 分類 | `fx`（POC 燈效引擎）同 `monitor`（實驗 watchdog）都歸測試；release 唔出 |
| 開關 | `option(NC_ENABLE_TESTS … OFF)`：OFF = `components/tasks` 唔加入編譯（唔係 runtime 關） |
| 移除 | `CONFIG_NETCORE_TEST_TASKS`（合併入上面嘅開關） |
| main.c | 測試相關 include／宣告／註冊／`nc_fx_scan` 全部 `#ifdef NC_ENABLE_TESTS` |

建置矩陣（全部零 warning）：

| | tests OFF | tests ON |
|---|---:|---:|
| S3 | 390,928 B | 402,272 B |
| P4 | 667,424 B | 682,112 B |

> **未搬**（下一步）：`test_action` 的 8 條 `0xF00x` handler 同 `hub_w/r`、`tx_pack`。

### 7.7 2026-11-04 晚（續搬：probe／tcp 測試任務，刪 monitor）

| 動作 | 內容 |
|---|---|
| 新增 `app/test_probe_tasks/` | `probe` 由 `main.c` 搬出（declare/start/hub 自測），對外只係 `nc_test_probe_task()` |
| 新增 `app/test_tcp_tasks/` | `tcp_sink0/3`、`tcp_src0/1` 由 `main.c` 搬出，對外 `nc_test_tcp_start()`（P4 ETH 起來才呼叫） |
| 刪除 `app/test_monitor_tasks/` | monitor 只有一行 log；`epoch`／`STATUS_GET` 已可即時查；省 3 KB stack 同每秒跨核讀 |

建置（零 warning）：S3 OFF 390,928／ON 401,712；P4 OFF 667,424／ON 681,488。

### 7.8 2026-11-04 晚（最後一批：`test_action`）

| 動作 | 內容 |
|---|---|
| 新增 `action/test_action/` | 8 條測試指令（`DEV_BENCH`／`HUB_BENCH`／`TX_BENCH`／`BUS_STATS`／`NET_TUNE`／`BENCH_READY·DATA·RESULT`）＋ `hub_w`／`hub_r`／`tx_pack` 三條測試 task |
| 對外 | `nc_test_action_register(dec, hooks)`；hooks 只提供 RX 診斷計數（`rx_counter`／`rx_fill_us`／`frame_align`／`reset_rx`） |
| `main.c` | `s_core_actions[]` 只剩 **5 條真指令**；測試 handler 全部移走 |
| 效果 | tests OFF 時連 handler 都唔存在：**S3 −6,512 B、P4 −6,800 B**（之前 8 條 handler 一直編入正式映像） |

建置（零 warning）：S3 OFF **384,416**／ON **402,144**；P4 OFF **660,512**／ON **682,016**。hosttest 8/8。

### 7.9 2026-11-05 凌晨（bus 所有權：driver 喺所屬核初始化）

| 動作 | 內容 |
|---|---|
| 新增 `nc4_bus_cfg_t.owner_init` | bus 綁邊個核，就由嗰個核的 decode task **開 loop 之前**初始化 driver（UART 已改用它） |
| 刪 `rx_split` / `rx_core` | 核心歸屬只剩一個來源：router（`routes[].out` 的 `decodeN`）；NVS 兩個旋鈕作廢 |
| TCP | 固定 PULL（由 bus 所屬核的 decode 直接讀 socket）；刪 `tcp_nc4_rx_task`、`tcp_nc4_fill_frame`、`rx_wait_readable` |
| `0xF00A NET_TUNE` | 只再寫 `frame_align`（其餘兩個 key 已無意義） |

建置（零 warning）：S3 OFF **383,648**／ON **401,776**；P4 OFF **658,800**／ON **679,136**。

### 7.10 2026-11-05 凌晨（producer ＋ 條件式無限等）

| 動作 | 內容 |
|---|---|
| 新增 `nc4_bus_start_producer()` | 同核 task：讀 driver → push 插槽 → `kick`（同核 notify）；插槽滿／冇資料用 `taskYIELD()`（**唔用 vTaskDelay**，避免 core1 跨核 tick） |
| UART 改 PUSH | `mode = NC4_BUS_PUSH`、`uart_bus_read` 改 `portMAX_DELAY`（ISR 同核）；producer 名 `uart.rx`（prio 5） |
| decode 等待策略 | 該核**全部係 PUSH** → `portMAX_DELAY`（只等同核 kick）；有 PULL bus → 保留 10 ms（本核輪詢） |
| 預期 | core1（UART）**唔再**每秒 100 次跨核中斷；core0（CDC／TCP 仍 PULL）保持本地 10 ms |

建置（零 warning）：S3 OFF **385,872**／ON **402,480**；P4 OFF **659,632**／ON **681,504**。hosttest 8/8。

> ⚠️ 上板驗證未做：本沙盒冇 USB/serial 裝置權限（`/dev/cu.*` open 回 EPERM）。
> 開機應見 `core 1 等待策略：PUSH（portMAX_DELAY）`；要驗 UART 通道仍通。

### 7.11 2026-11-05（USB 服務組跟 router；`nc_core_run_on`）

| 動作 | 內容 |
|---|---|
| 新增 `sys/core`：`nc_core_run_on(core, fn, arg)` | 一次性 init 派送；同核直接跑，跨核用 `esp_ipc_call_blocking`（**只准開機 init**） |
| `usb_init` → `usb_install()` | TinyUSB device stack ＋ CDC-ACM ＋ console 全部喺 **CDC 所屬核**（router 的 CDC 行）執行 → USB ISR、TinyUSB task、之後的 RX callback／producer／kick 同核 |
| 原則 | 核心歸屬**唯一來源 = router**（唔加 `cores` map）；服務以「組」為單位綁核：USB 組（TinyUSB+CDC+console+MSC）、網絡組（ETH+lwIP+TCP+httpd，未來） |

建置（零 warning）：S3 OFF **385,584**／ON **402,704**；P4 不變（659,632／681,504）。

> 已補（2026-11-05 晚）：MSC driver install 已延後到 router 之後；`msc_async_io` 同
> `usb_reconn` 都釘喺 USB 服務組的核心（見 §7.13）。

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
| 交接 | [disposition.md](disposition.md)（移除／新做法／保留／待決）｜[next-steps.md](next-steps.md)（排序、決策點、驗收）｜[handoff-2026-11-04.md](handoff-2026-11-04.md)（2026-11-04：記憶體清理、link 閘、Wi-Fi 兩道閘、量測與待辦） |
| 教學 | [build_and_flash.md](../02_guides/build_and_flash.md)、[testing.md](../02_guides/testing.md)（§2.13–2.16 是量測總表） |
| 筆記 | [execution-model.md](../03_notes/execution-model.md)（A/B 執行模型取捨與 group 改裝）、[core-isolation.md](../03_notes/core-isolation.md)（核隔離不變式：唯一接口係緩衝）、[cross-core.md](../03_notes/cross-core.md)（跨核中斷與共享狀態清單）、[core-round.md](../03_notes/core-round.md)（每核大循環量測）、[config-layers.md](../03_notes/config-layers.md)、[writing-a-task.md](../03_notes/writing-a-task.md)、[task-lifecycle.md](../03_notes/task-lifecycle.md)、[rx-architecture.md](../03_notes/rx-architecture.md)、[failure-policy.md](../03_notes/failure-policy.md)、[p4-bringup.md](../03_notes/p4-bringup.md)、[buffer-manager.md](../03_notes/buffer-manager.md)、[memory.md](../03_notes/memory.md)、[memory-budget.md](../03_notes/memory-budget.md)（SRAM 帳）、[decode-routes.md](../03_notes/decode-routes.md)、[naming.md](../03_notes/naming.md)、[component-layout.md](../03_notes/component-layout.md)、[architecture-flow.md](../03_notes/architecture-flow.md)、[roadmap.md](roadmap.md) |
| 研究 | [effect-language.md](../03_notes/effect-language.md)、[extension-architecture.md](../03_notes/extension-architecture.md)、[fx-loader.md](../03_notes/fx-loader.md) |

### 7.12 2026-11-05 早上（實機測試：USB 通，UART 迴路抓到漏 kick）

**在板子上做的**（S3，USB CDC，經 `tools/ota.py` OTA 上板兩次）

| 測試 | 結果 |
|---|---|
| `STATUS_GET keys=?` | ✅ 71 B |
| `keys=sys` / `mem` / `sys,mem` / `storage` | ✅ keys 過濾有效 |
| `keys=epoch` | ❌→✅ 修好 `status_put_epoch()`（`nc_epoch_json` 係整份物件，要加 `"epoch":` 前綴） |
| `keys=tasks` | ✅ 2357 B |
| `0x1005 SYS_TASK_QUERY` | ✅ 556 B、9 個任務（768 B 截斷 bug 已消失） |
| `0xF001 DEV_BENCH` | ✅ 回 `0xF002`，test_action 生效 |
| **UART 迴路（IO7↔IO8 短接）** | ❌ `sent=100, rx=0`；`BUS_STATS` 顯示 UART `slots=[4,0,0,0] frames=0` |

**UART 判讀**：producer 成功（driver 在 core1 安裝、讀到資料、push 滿 4 格），
但 **decode1 從未消費**。原因：producer 喺 decode1 啟動前就 push（`s_decode_task[1]==NULL`
→ `nc4_bus_kick()` 直接 return）；插槽滿之後 producer 冇得再 push，decode1 就永遠等。

**修法（未做）**：
1. `nc4_bus_task_entry` 設完 `s_decode_task[core]` 後，先 drain 自己核每條 PUSH bus（唔等 kick）
2. `nc4_bus_kick()` 對未就緒 handle 記 pending，decode 起動時補發
3. producer 插槽滿時改用「decode 釋放插槽 → 通知 producer」，唔自旋

**另一未修 bug**：`epoch.rounds` 永遠 0 —— `decode0/1` 的 `fn` 係 `for(;;)` 永不返回，
trampoline 只喺 `fn` 返回後才 `nc_epoch_mark`，所以該核永遠完成唔到一輪。

**新增測試指令**（`action/test_action`，受 `NC_ENABLE_TESTS` gate）：
`0xF00B UART_LOOP`（寫 N 幀去 UART1）、`0xF00C LOOP_STAT`（讀收回幀數）、`0xF0FE LOOP_PING`（接收計數）。

### 7.13 2026-11-05 晚（USB 隨身碟初始化、P4 建置）

| 症狀 | 根因 | 修法 |
|---|---|---|
| 碟停在「需要格式化」／主機完全冇碟 | 原本的後備在 `nc_store_init()` 失敗時永遠到不了；而且用 `fw_new` 當條件，同 FAT 健不健康無關 | NVS `boot/fs_init`：**只有第一次初始化**才允許格式化；NVS `boot/fs_format`：`STATUS_SET` 的明確請求，下次開機重建。`nc_store_format()` 只在這些路徑呼叫，日常 mount 失敗唔會清資料 |
| 主機拔插／`file_mode` 切換後碟唔返 | `usb_reconn`、`msc_async_io` 冇 pin 核，同 TinyUSB task 跨核爭 USB 狀態 | 兩者都釘喺 MSC driver 安裝核（USB 服務組） |
| 同一個 WL handle 被掛到多個 pdrv | esp_tinyusb 預設喺 `tud_mount_cb`／`tud_umount_cb` 自動換 mount point，繞過 store | `auto_mount_off = 1`；主人只由 `switch_to()` 決定 |
| 指紋相同時 NVS handle 漏 close | `fw_fp_changed()` 只喺變更時交回 handle | 相同就即刻 close |
| 掛載失敗後 WL handle 漏／分區仍被佔 | IDF 的 `esp_vfs_fat_spiflash_mount_rw_wl()` 失敗路徑只 unregister pdrv／VFS，唔會 `wl_unmount` | `open_app()` 失敗時自己 `wl_unmount`，之後 `nc_store_format()` 才可以乾淨重掛 |
| P4 建置失敗（`cdc_region` implicit declaration） | `task_nc4_start()` 喺 P4 仍引用只在 `!P4` 定義的 CDC 符號 | 加 `#if` guard；`cfg_probe_once()` 兩塊板都呼叫 |

建置：S3 **381.4 KB**、P4 **649.9 KB**，零 warning。

> **兩個獨立旗標（NVS `boot` namespace）：**
> - `fs_init`：第一次初始化。未設 → `nc_store_init(part, true)`（真的掛唔上才 `nc_store_format()`）；
>   已設 → `nc_store_init(part, false)`，掛唔上保持原狀、**唔清資料**。
> - `fs_format`：**明確請求**。由 `0x1103 STATUS_SET {"System":{"fs_format":1}}` 設定；
>   下次開機 `nc_store_format()` 抹分區重建，成功後清旗標。`STATUS_GET storage` 的 `req` 可看。
> - `fw_new` 只決定「要不要刪 config」。
> - 用法：`STATUS_SET {"System":{"fs_format":1}} persist=1` → `0x100F REBOOT`。
>   工具：`tools/nc4_file.py ... set '{"System":{"fs_format":1}}' 1`，之後 `tools/nc4.py ... reboot`。

**上板實測（2026-11-05 晚，S3；CDC 指令通道）**

| 測試 | 結果 |
|---|---|
| 燒錄後重開 | ✅ 複合裝置 `esp_Net_Core` 列舉；`NC-2B4C` 7.3 MB 自動掛載 |
| 碟內容 | ✅ `config/{system,network,tasks}.json` 都係合法 JSON（2701／3451／768 B） |
| 主機寫入 → reboot | ✅ `test.txt` 重開後仍讀得返 |
| `file_mode=1`（claim） | ✅ `storage.mode=app`、`app_ready=1`；LUN 已拆 |
| `file_mode=0`（release） | ✅ 回 `mode=usb`，碟重新出現，`test.txt` 仍在 |
| 第一次開機（NVS 無 `fs_init`） | ✅ `storage.first=1`；FAT 有效所以唔抹，直接掛載並落旗標 |
| 第二次開機（旗標已落） | ✅ `storage.first=0`、`fmt=-99`；碟照常出現 |
| OTA 0.1.0 → 0.1.1 | ✅ 388,704 B／2.0–2.7 s（142–191 KB/s）；reboot 後 `sys.fw=0.1.1`、`first=1`（OTA 後第一次）、`fmt=0`（config reset 有生效） |
| host 把 FAT 改成 MBR/FAT32 → reboot（`first=0`） | ✅ 裝置**冇抹除**：反而掛上 host 建的 FAT32 分區、seed `config/`／`pixel/`；`first` 保持 0 |
| `STATUS_SET {"System":{"fs_format":1}}` → reboot | ✅ ACK `fs_format=1; 下次開機抹 data 分區並重建 FAT`；`storage.req=1` → reboot 後 `req=0`、`fmt=0`，碟以 `NC-2B4C` 回來、config 重新 seed |

> `nc_store_format()` 已由 `STATUS_SET → reboot` 實際呼叫（上表）。未測的只剩
> 「開機第一次 init 就掛唔上」這個入口 —— 要 raw erase `data` 的 WL metadata 才觸發得到
> （需要下載模式）；正常、第一次、第二次開機、host 改壞 FAT、STATUS_SET 明確格式化都已上板證實。
