# 專案現況總結（單頁）

> **用途**：所有「已經驗證的事實、已定案的設計、還沒做的事」集中在這一頁；
> 細節都連到對應文件。**最後更新**：2026-11-08。

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
| Wi-Fi | **兩道閘**：編譯期 `CONFIG_NETCORE_WIFI`（預設 n）＋ 執行期 `network.json` `wifi.enable`（不啟用 = 0 heap）。⚠️ 2026-11-08 更正：IDF 6 的 `CONFIG_ESP_WIFI_ENABLED` 是**無 prompt** 符號（S3 強制 y），blobs 用 linker fragment whole-archive 拉入 → **即使 `NETCORE_WIFI=n`，S3 映像仍含 WiFi**（實測 +75 KB flash、−18 KB 內部 RAM，見 §7.26） |

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

**現況**：`main.c` 只剩一張任務表（核心 `nc4` / `net` / `core` / `fx` ＋ 測試用 `probe` / `hello` / `bench_a/b/c`；
後者在 `NC_ENABLE_TESTS=OFF` 時完全不存在）＋ 一個指令表清單 ＋ 六行生命週期呼叫（register → declare → start → boot）。
service 與 task 已合併成同一個 `nc_task_t`。
`tasks.json` 已接（含補寫）；`system.json` 已接 `cID` / `links[]` / `routes[]` / `log_debug_mode`；
`hardware.json` 已接 **ETH 與 SDIO** 兩段（其餘段只計 unknown，**改它無效**，見 §7.24）；
`network.json` 只讀 `bt.pin`；`driver.json` **未 embed、未讀**。slots 還沒走 plan。

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
| S3 上 TinyUSB 一跑，`/dev/cu.usbmodem*` 就係**應用 CDC**，唔再係 ROM 的 USB-Serial-JTAG | `make.py flash` 會 `No serial data received`；日常更新走 OTA（`tools/ota.py`），實體燒錄要按住 BOOT 再 reset |

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
| `tools/nc4_demux.py` | **NC4 協議唯一實作**：`\x1e`…`\n` = log 行；`"NC"`+VER/LEN/CRC32 才算幀，失敗只前進 1 byte |
| `tools/nc4_log.py` | 看 log 管道（log 與幀共用 CDC） |
| `tools/bench/cdc_tx.py` | CDC TX 壓測（log／回覆爭出口時的行為） |
| `bash tools/hosttest/run_all.sh` | 一次跑完 8 支 host test（cfg / cfgstruct / fxm / task / router / fx / hub / plan），全綠 = 冇 error 亦冇 warning |
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
| E | **`hardware.json` / `driver.json` 未接線** | 🟡 部分 | `hardware.json` 已 embed，**ETH 與 SDIO 兩段真的會被讀**（2026-11-08，見 §7.24）；其餘段只計 unknown、改它冇效。`driver.json` 仍然**冇 embed、冇人讀** |
| F | **S3 冇 WiFi** | ❌ | 只有 P4 有 ETH；S3 的無線能力未接。~~目前 image 完全冇 link Wi-Fi~~ **2026-11-08 更正**：乾淨的 sdkconfig 下 IDF 6 會把 `libesp_wifi`／`libnet80211` 用 linker fragment 整包拉入（+75 KB flash、−18 KB 內部 RAM），`CONFIG_NETCORE_WIFI=n` 只保證**不初始化**（runtime heap 仍是 0）。舊的 sdkconfig 剛好關了 `ESP_WIFI_ENABLED` 才有「0 B」的數字；那個符號沒有 prompt，sdkconfig／Kconfig／CMake 都改不動（實測）。見 §7.26 |
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

按 [execution-rules §6](../03_notes/execution-rules.md) 的約定落實「測試唔污染功能結構」：

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
| 筆記 | [execution-model.md](../03_notes/execution-model.md)（A/B 執行模型取捨與 group 改裝）、[core-isolation.md](../03_notes/core-isolation.md)（核隔離不變式：唯一接口係緩衝）、[cross-core.md](../03_notes/cross-core.md)（跨核中斷與共享狀態清單）、[config-layers.md](../03_notes/config-layers.md)、[writing-a-task.md](../03_notes/writing-a-task.md)、[task-lifecycle.md](../03_notes/task-lifecycle.md)、[rx-architecture.md](../03_notes/rx-architecture.md)、[failure-policy.md](../03_notes/failure-policy.md)、[p4-bringup.md](../03_notes/p4-bringup.md)、[buffer-manager.md](../03_notes/buffer-manager.md)、[memory.md](../03_notes/memory.md)、[memory-budget.md](../03_notes/memory-budget.md)（SRAM 帳）、[decode-routes.md](../03_notes/decode-routes.md)、[naming.md](../03_notes/naming.md)、[component-layout.md](../03_notes/component-layout.md)、[architecture-flow.md](../03_notes/architecture-flow.md)、[roadmap.md](roadmap.md) |
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

### 7.14 2026-11-06（log 管道、型別化 config、boot record；epoch 量測層移除）

> 上游 `35f0e51` 一次改了四層；同日本機審查、上板驗證，並按用戶決定移除 `epoch`／`count`。
> **§7.13 的三個 NVS 旗標（`fs_init`／`fs_format`／`fw_new`）已被本節的 boot record 取代。**

| 層 | 之前 | 之後 | 檔案 |
|---|---|---|---|
| log | 每行直落 console，兩核爭同一個出口；USB 起來前那幾行收唔到 | 每核一個可變長 byte ring（只存文字、4 B header），單一 consumer 合批送 CDC；出口 OFFLINE 就唔消費 ring | `components/sys/nclog/` |
| config | 每次 `nc_cfg_open()` + 自己 parse cJSON（router UAF 來源） | `types`（struct = schema，無指標）／`defaults`／`load`（解析一次） | `config/nc_cfg_{types,defaults,load}.c` |
| 開機狀態 | 多個散旗標 | 一條 8 B `boot/record`（PROVISIONED／FORMAT_PENDING／DEGRADED／fw_hash）＋狀態管理器 | `state/nc_boot.c`、`state/nc_state.c` |
| 主機工具 | 各工具各自 framing | `nc4_demux.py` 唯一實作：`\x1e`…`\n` = log 行；`"NC"` + VER/LEN/**CRC32** 才算幀，失敗只前進 1 byte | `tools/nc4_demux.py` |

**行為變化**

1. `system.json` 的 `log_debug_mode`：0 = 簡（E/W/I、2 KB／512 B、USB 起來之後才記）；
   1 = 詳（全部等級、8 KB／1 KB、第一行開始記）。即時生效，並寫回 NVS（`boot/log`）。
2. log 與 NC4 幀**共用同一條 CDC**：每行 `\x1e` ＋ 核號 ＋ 空格。
3. **換韌體 = factory reset**：`app_elf_sha256` 前 4 B 改變 → 下次開機格式化 data 分區再 provision
   （`/data/pixel` 會清空）。
4. config 預設只寫一次；缺檔重建底座、壞檔用預設並標記 `degraded`。
5. **2 KB 環是刻意設定**（用戶只需要開機後的資訊）：有 host 讀時 `peak=1.4 KB`、`dropped=0`；
   冇 host 約 30 秒滿（設計上的 backlog 上限）。

**驗證（S3，2026-11-06）**

| 項目 | 結果 |
|---|---|
| hosttest | ✅ 8/8（`epoch_test` 已隨量測層移除） |
| S3／P4 建置 | ✅ 391.6 KB／661.3 KB，零 warning |
| OTA 上板 | ✅ 400,992 B／2.8 s（140.4 KB/s）、`END ok` |
| 換版偵測 | ✅ `storage.boot={prov:1,fmtq:0,degr:0,fw_new:1}`、`fmt=0` → 真的格式化重建 |
| `keys=?` | ✅ 11 個 KEY（冇 `epoch`） |
| `nc4_log.py` | ✅ log 行與 STATUS 幀共存，`crc_err=0 junk=0` |
| `keys=tasks` | ✅ loop 只回結構（name/core/layer/prio/period_ms/running/enabled） |

**審查發現（已修）**

| # | 問題 | 修法 |
|---|---|---|
| L1 | `ring_put` 環尾剩 1–3 B 時寫唔落 PAD，consumer 照讀 4 B → 讀過緩衝尾、垃圾 header | `ring_at`／`ring_migrate` 見 `off+4>cap` 當繞回 0 |
| L2 | `nc_log_json` 在 capture 前被問到 → `ring_used` 除零 | `cap==0` 回 0 |
| L3 | demux 丟棄被讀取切開的半個 SOF（`"N"`）→ 白丟一幀 | 留住尾端 `N`，`k<=0` 就 break |
| L4 | `nc_log.h` 級別註解（E/W、6 KB）同實作唔一致 | 改正註解 |
| L5 | `keys=?` 漏報 `cfg`／`log`／`rt` | 補齊（實機 11 KEY） |

**epoch／count：整層移除（2026-11-06）**

`decode0/1` 的 `fn` 係 `for(;;)`，trampoline 只喺 `fn` **返回後**才計數／mark → `rounds`、`count` 永遠 0。
留一個永遠 0 的指標比冇更差。已刪 `components/sys/epoch/`、`keys=epoch`、
`budget_us`／`no_epoch`、`count`／`avg_us`／`max_us`／`over`／`wrap_us`、`tools/hosttest/epoch_test`。
image 各少約 2.4 KB（S3 391.6 KB、P4 661.3 KB）。

**TinyUSB task stack：4,096 → 1,280**

壓力：開機列舉、MSC 4–6.3 MB 寫（65–76 KB/s）、CDC 400–1,000 請求/回覆、
**8 MB 的 8 KB 幀（0.47 MB/s，RAM 環路徑）**、OTA 上傳、2–3 次重新列舉。峰值 **976–1,008 B**。

| 設定 | 峰值已用 | 餘裕 | 判斷 |
|---|---:|---:|---|
| 4,096 | 976 | 3,120（76%） | 浪費 |
| 1,024 | 976–1,008 | 16–48（1–5%） | ❌ 少於一個中斷 frame，panic 風險 |
| **1,280（現在）** | **976** | **304（24%）** | ✅ 全部壓力測試通過 |
| 1,536 | 976 | 560（36%） | 安全但多 256 B |

**decode 是動態綁定（實測）**

`routes[].out` 的 `decodeN` 是唯一真相：`link_core()` 只查表（冇 fallback），bus 的 `cfg.core`
由它決定；decode 任務由 `nc4_router_core_used(N)` 決定開唔開。**冇「CDC→decode0」硬綁定**。

| 設定 | 開機 log 證據 | 結果 |
|---|---|---|
| CDC→decode0、UART→decode1（預設） | `cdc.rx 同 decode0 同喺 core 0`、`[UART] owner_init on core 1` | 兩條 decode 都建立 |
| CDC→decode1、UART→decode0（對調） | `cdc.rx 同 decode1 同喺 core 1`、`[UART] owner_init on core 0` | 跟住換 |
| 兩條都→decode0 | 只有 `解碼任務啟動（core 0）` | decode1 唔建立；heap +6,260 B（≈6,144 棧） |

緩衝同綁定無關：三組 `keys=mem` 都係 `nc4.rx 4,192` ＋ `uart.rx 1,024` ＝ 5,216 B（每條 link 一份）。

**decode stack：6,144 → 5,120**

峰值 4,680 B（91% 用盡），主要係固定成本：回覆 JSON 緩衝 2,601 B ＋ `nc4_ctx` ＋ 分派框架。

| 情境（decode0） | 已用 | 餘裕（5,120） |
|---|---:|---:|
| 開機後閒置 | 4,000 | 1,120 |
| 1,000 個 STATUS 回覆（tasks/mem/混合） | 4,680 | 440 |
| 8 MB 的 8 KB 幀 | 4,680 | 440 |
| OTA 上傳（400 KB） | 4,008 | 1,112 |

`decode1`（UART）目前只有 idle 數據（1,164 B）；有流量版本要 IO7↔IO8 短接。

### 7.15 2026-11-07（SD 卡：sdmmc 4-bit 上板成功）

新增 `SDcard` 層（`system.json`）：`enable`／`phat`（掛載點）／`config{slot,width,freq}`／
`GPIO{sck,cmd,data[4]}`。`enable=0` 時不初始化、不佔 RAM；指定 GPIO 時自動走 slot1（GPIO matrix，
因為 S3 的 slot0 係 IOMUX 固定腳）。`STATUS storage` 多了 `sd{en,mnt,mb,err}`。

**上板結果（S3，SD 卡 30,436 MB）**

| 嘗試 | 結果 |
|---|---|
| 腳位 `[5,4,16,15]`（照舊專案寫法）、4-bit 40 MHz | ❌ `send_scr` timeout（CMD1 過、SCR 讀不到） |
| 1-bit 400 kHz、同一組腳位 | ❌ 一樣 |
| **腳位 `[15,16,4,5]`**（即舊寫法反過來）、4-bit **40 MHz** | ✅ **`SD 已掛載 /sd：30436 MB（4-bit、40000 kHz、sector 512 B）`** |
| `STATUS storage` | ✅ `sd{en:1,mnt:"/sd",mb:30436,err:0}` |

**根因**：舊專案的 `data[]` 順序是 **D3,D2,D1,D0**，本韌體按 **D0,D1,D2,D3** 解讀。
另外第一張卡在 CMD1 就 timeout（`0x107`），換卡後 CMD1 才過 —— 即「卡冇插好／第一張卡有問題」＋「腳位次序錯」兩件事疊在一起。

**要注意**

1. `sck=7` 同 **UART1 TX=7** 撞腳：開 SD 就要在 `system.json` 把 `links[UART].enable` 設 0（實測這樣兩者都正常，UART 那條 decode1 也不會建立）。
2. 掛載點 `/sd` 目前只是掛起＋可由 STATUS 看到；NC4 的檔案指令仍然只認 `/data`（要對 `/sd` 讀寫要另外加 path 參數）。
3. 開 SD 令 image 由 391.6 → **440.4 KB**（+48.8 KB：sdmmc driver + VFS/FAT 路徑）。

### 7.16 2026-11-07（SD 的「慢」是假的：/sd 一直在讀寫內部 flash）

§7.15 之後一直以為 SD 寫入只有 **148 KB/s**、讀 6.4 MB/s，並由此懷疑「USB 隨身碟佔住 FAT」。
實測推翻了全部前提：**那些數字是內部 flash 的速度，SD 卡一次都沒有被 FAT 碰過。**

**三個獨立證據**

| 證據 | 觀察 |
|---|---|
| 檔案指紋 | bench 寫 `/sd/bench.bin` 2 MB；`FILE_QUERY /bench.bin`（讀 `/data`）找到**同一個大小、同一個 sha256** |
| 驅動計數 | 2 MB FAT 讀寫期間 `sdmmc_write_sectors` 被呼叫 **0 次**（反組譯確認 `ff_sdmmc_write` 確實 `call8 __wrap_sdmmc_write_sectors`，wrap 有效） |
| 主機視角 | `ls -f /Volumes/NC-D5F4` 直接看到 `bench.bin` 在隨身碟根目錄 |

**根因 1：FATFS 磁碟號被搶**

`store` 的唯讀視圖寫死 `RO_PDRV=1`／`RO_DRIVE="1:"`；SD 用 `esp_vfs_fat_sdmmc_mount()` 取「第一個
空槽」—— 開機次序剛好也是 1。`ff_diskio_register()` 對已佔用的槽是**靜靜覆寫 + free 舊 impl**，
不會報錯。於是 `/sd` 的每一次讀寫都落到 flash 的唯讀視圖上。

⇒ 唯讀視圖改成 `ff_diskio_get_drive()` 動態取槽；`CONFIG_FATFS_VOLUME_COUNT` 2 → 3
（flash / SD / 唯讀視圖各一個）。塞爆時 `ESP_LOGE`。

**根因 2：GPIO7 同時係 SD CLK 同 UART1 TX**

`sd_boot()` 成功（mount、`fopen` 探測都通），幾百 ms 之後 SD 全死；`sdmmc_send_cmd` 回 `0x107`
（`ESP_ERR_INVALID_STATE`）。開機 log 給出答案：

```
UART 迴路已啟動：TX=7 RX=8 baud=921600     ← 測試迴路把 GPIO7 變成 UART1 TX
SD: sck=7 cmd=6 d0..d3=15,16,4,5           ← 同一支腳
```

⇒ `uart_owner_init()` 見到 SD 已啟用且 clk/cmd 撞到 7/8 就**不啟動**測試迴路（`ESP_ERR_INVALID_STATE`）。
這也解釋了為何 SD 只在 `sd_boot()` 內正常：UART 那條 bus 比 SD 晚初始化。

**根因 3（順手）：NC4 指令表 32 格但註冊 36 條**

`core 5 + test 11 + test bench 9 + ota 9 + file 2 = 36`：最後 4 條靜靜失敗，
`STATUS_SET (0x1103)` 和 `FILE_QUERY (0x2005)` 完全冇反應（症狀同「線路壞」一模一樣）。
⇒ `NC4_MAX_HANDLERS` 32 → 48，滿了 `ESP_LOGE`。

**真正的 SD 速度（4-bit / 40 MHz、FAT32 64 KB cluster）**

| 項目 | KB/s |
|---|---|
| FAT 建立 2 MB 檔 | 9,241 |
| FAT 覆寫 2 MB（64 KB 區塊） | 8,947 ～ 9,262 |
| FAT 讀 2 MB | 15,557 ～ 15,807 |
| FAT 讀 4K / 16K / 64K | 5,159 / 11,162 / 15,695 |
| FAT 寫 4K / 16K / 64K | 1,835 / 4,330 / 9,144 |
| raw（繞過 FAT）讀 / 寫 | 16,001 / 9,538 |

**A/B：USB 隨身碟在不在，對 SD 零影響**

| 模式 | 寫 | 讀 |
|---|---|---|
| `mode=usb`（碟交給主機） | 8,947 KB/s | 15,807 KB/s |
| `mode=app`（裝置自己持有 FAT） | 9,262 KB/s | 15,557 KB/s |

差別在噪音範圍內。之前「轉手令 FAT 變慢」的假設不成立 —— 因為 SD 從來沒進過那條路。

**其他**

- `calls=83 / sectors=8211`：64 KB 區塊之下 FATFS 每次 `disk_write` 已經帶 ~99 個 sector
  （multi-block 正常）。慢的一直是「區塊細」而唔係「冇 batch」。
- `STATUS storage` 的 `sd` 新增 `drv`（實際磁碟號）、`fr`（f_mount FRESULT）、`probe`
  （開機時真的開一次檔的 errno）、`drives`（每個 FatFS 磁碟的 FRESULT + 容量）——
  同類「掛錯卡」以後一眼看得出來。
- `statvfs()` 對 FATFS 回 `ENOTSUP`，所以容量對帳改用 `f_getfree` + `probe`。
- 板子預設改成 `SDIO.enable=1`、`SDcard.enable=1`（換韌體會重建 `/data`，不然 SD 又關掉）。

#### 7.16.1 順帶：64 KB bench 緩衝搬離 .bss（PSRAM 實測）

`s_sd_bench_buf[64 KB]` 一直係 `.bss`（`0x3fc9b600`，佔 `.dram0.bss` 109,360 B 的 58%），
而 SD 那幾條測試指令只在 `NC_ENABLE_TESTS` 才註冊。改成**按需配置**、預設內部 DMA：

```c
heap_caps_aligned_alloc(512, 64*1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
```

| 項目 | 之前 | 之後 |
|---|---|---|
| `.dram0.bss` | 109,360 B | **44,096 B** |
| 開機剩餘（內部） | 211,623 B | **276,959 B**（+65,336） |
| 最大連續（內部） | 139,264 B | **204,800 B**（+65,536） |

**PSRAM 可不可以放？實測給出答案：可以放，但慢 2～3 倍**

| 緩衝位置 | 寫 | 讀 | 建立 |
|---|---|---|---|
| 內部 DMA（預設） | 9,152 KB/s | 15,304 KB/s | 9,263 KB/s |
| **PSRAM** | **3,271 KB/s** | **7,011 KB/s** | 3,151 KB/s |

原因寫在 IDF 原始碼裡：`SOC_SDMMC_PSRAM_DMA_CAPABLE` **只有 ESP32-P4 是 1**，S3 的
SDMMC DMA 到不了 PSRAM，所以 `sd_host_check_buffer_alignment()` 直接回 `false`，
協議層改用內部 bounce buffer 再複製一次（`calls` 由 87 升到 170、每次帶的 sector 由
~94 跌到 ~97/65）。同理，任何真正要搬檔案的 SD buffer 都應該留在內部 RAM。

PSRAM 適合的是「唔經 SD／唔需要外設 DMA 讀它」的大塊緩衝（JSON/HTTP 暫存、影像、
純 CPU 計算）。`0xF012` 第二個 byte = 1 可以即時切去 PSRAM 做對照。

**「細內部 + 大 PSRAM」值唔值？（0xF019 SD_STAGE，源頭 128 KB PSRAM）**

| 內部 staging | 寫入 | 佔滿速 | memcpy 佔用 |
|---|---|---|---|
| 4 KB | 2,124 KB/s | 23% | 40.9 / 964 ms |
| 16 KB | 4,216 KB/s | 46% | 39.3 / 486 ms |
| 32 KB | 5,970 KB/s | 65% | 39.4 / 343 ms |
| 64 KB | **8,460 KB/s** | **92%** | 38.7 / 242 ms |
| （對照）64 KB 全程內部 | 9,152 KB/s | 100% | — |
| （對照）PSRAM 直接寫（IDF 8 KB bounce） | 3,271 KB/s | 36% | — |

結論：**PSRAM 代替不了那 64 KB 內部 RAM**。2 MB 的 PSRAM→內部 memcpy 固定約 39 ms
（≈52 MB/s），佔比由 4 KB 的 4% 升到 64 KB 的 16% —— 真正的差別在**每次寫入的 chunk 大小**，
唔係複製本身。4 KB 只有 23% 速度，而且比「整塊 PSRAM 直接寫」更差（IDF 自己用 8 KB bounce）。

⇒ 設計原則：內部 DMA 一次只需「一個 chunk」；chunk ≥ 32 KB 才有意義，64 KB 才接近滿速。
其餘資料放 PSRAM（128 KB / 1 MB 都可以），每次搬 64 KB 入內部再寫。

**更正（2026-11-07）：bench 迴圈的 `vTaskDelay(1)` 污染了 4K/16K 數字**

`0xF012`/`0xF014`/`0xF015`/`0xF018` 的計時迴圈每個 chunk 都 `vTaskDelay(1)`，
而 `CONFIG_FREERTOS_HZ=1000` → **1 tick = 1 ms**。4 KB chunk 寫 2 MB = 512 次 = 512 ms，
佔總時間 1,142 ms 的 45%。已經全部移除（計時迴圈內唔可以讓 CPU）。

| 項目 | 舊（有延遲） | 新（移除後） |
|---|---|---|
| FAT 寫 4K | 1,835 | **2,317** |
| FAT 寫 16K | 4,330 | **4,681** |
| FAT 寫 64K | 9,144 | **10,404** |
| FAT 建立 2 MB | 9,263 | **10,282** |
| raw 寫（MBR 空隙） | 9,538 | **10,282** |
| raw 讀 64K | 16,001 | 15,981（讀本來就冇延遲） |
| FAT 讀 4K / 16K / 64K | 5,159 / 11,162 / 15,695 | 4,930 / 10,716 / 15,557（變異） |

`0xF019 SD_STAGE` 本來就冇延遲，所以 staging 那組不受影響（2,124 → 2,189 屬變異）。
正確的比較是 **2,189（staging 4 KB，含 memcpy）vs 2,317（直接 4 KB）**，即複製成本約 5%。

**修正後的結論更清楚**：64 KB chunk 之下 raw 寫（10,282）同 FAT 寫（10,404）已經打平 ——
瓶頸純粹是卡本身，FAT 一層冇額外成本。

**1-bit 對照（同卡、同 40 MHz、`hardware.json` 的 `width` 1 ↔ 4）**

| 項目 | 1-bit | 4-bit | 倍數 |
|---|---|---|---|
| raw 讀 4K / 16K / 64K | 3,391 / 4,391 / **4,655** | 6,398 / 12,178 / **15,981** | 3.4× |
| raw 寫 | **4,073** | **10,282** | 2.5× |
| FAT 讀 2 MB | **4,678** | **15,604** | 3.3× |
| FAT 寫 2 MB | **4,021** | **10,459** | 2.6× |
| FAT 建立 2 MB | 3,918 | 10,282 | 2.6× |

**判讀**：1-bit @ 40 MHz 的理論上限是 40 MHz ÷ 8 = **5,000 KB/s**，實測讀 4,655 = **93%**
—— 即 1-bit 係**匯流排飽和**，卡本身仲有餘力。4-bit @ 40 MHz 理論 20,000 KB/s，實測讀
15,604 = 78%，**呢邊才是卡的天花板**。所以 1-bit 換 4-bit 拿到 3.3～3.4×（唔到 4×，
因為 1-bit 那側受制於理論上限)。

寫比讀慢（10,282 vs 15,981）係卡本身的 program 時間，兩種寬度都一樣。

註：切換寬度要先改 `ports/S3/ESP32-S3-N16R8/data/config/hardware.json` 的 `SDIO.list[0].width`
再換韌體（SD 只在開機掛載一次）。量完已改回 4。

### 7.17 2026-11-07（SD 當第二個 LUN：主機看到兩顆磁碟）

**為什麼不是 `NC-D5F4/sd/`**：FAT 冇 mount point 呢個概念。主機看到的是 MSC 交出去的
**磁區**，而 `NC-D5F4` 就是 flash data 分割區的全部磁區；要它裡面再出現另一個檔案系統，
等於一個 FAT 磁碟裡面放另一個 FAT 磁碟，協議層做不到。（§7.16 之前那個「看起來有」的
`/sd` 其實是別名到 flash 的 bug。）

**參考實作：CircuitPython 的 `CIRCUITPY_SDCARD_USB`** —— 同一個模型：`LUN_COUNT = 2`，
主機看到 `CIRCUITPY` + `SDCARD` 兩顆獨立磁碟（[issue #10965](https://github.com/adafruit/circuitpython/issues/10965)、
[TinyUSB MSC Dual LUN](https://docs.tinyusb.org/en/stable/examples/device/msc_dual_lun.html)）。
同一份 issue 亦記錄了 **macOS 對 LUN 1 的 probe 有坑**（RP2040 上完全不 probe；RP2350 正常）。

**實作**（esp_tinyusb 本來就支援 `TINYUSB_MSC_STORAGE_MAX_LUNS = 2`）

- `nc_store.h` 新增 `nc_store_sd_attach(card, mount_app, unmount_app, arg)`：main 做完
  `sdmmc_card_init()` 之後把卡交進來，store 負責「交給 host ↔ 收回」。
- 交出（usb）：`sd_vfs_unmount()` → `tinyusb_msc_new_storage_sdmmc()`。
  收回（app）：`tinyusb_msc_delete_storage()` → `sd_vfs_mount()`。
- 次序不可反：兩邊同時寫同一份 FAT 一定爛，所以**永遠只有一邊掛住**。
- 磁碟號每次都重新 `ff_diskio_get_drive()`（唔可以假設同一個）。

**實測（S3 + macOS 26）**

```
/dev/disk5  external physical   ← flash（NC-D5F4）
/dev/disk6  external physical   ← SD 卡（31.9 GB、FDisk_partition_scheme、DOS_FAT_32 "NO NAME"）
```

來回都通：

| 方向 | 結果 |
|---|---|
| 裝置 → 主機 | 裝置寫的 `/sd/bench.bin`、`/sd/stage.bin`（各 2 MB）出現在主機磁碟上 |
| 主機 → 裝置 | 主機寫的 `from_mac.txt` 在裝置收回後同一張卡上可見（Free 30,425 → 30,421 MB） |

**兩個實際坑**

1. `STATUS_SET file_mode` 切換會**等主機做完手上的 I/O**（macOS 在 Spotlight 索引時可以幾秒），
   原本 `nc4_file.py` 只等 2 秒 → 收不到 ACK（但切換其實成功了）。已改成等 ACK 最多 10 秒。
2. release 之後 macOS 偶爾留住舊的 mount（`Permission denied`）：
   `diskutil unmount force /dev/diskN` 再 `diskutil mount /dev/diskN` 就回來。

**現況**：開機預設交出（跟 flash 一樣），所以主機一插上就看到兩顆碟；`/sd` 在
`mode=usb` 時裝置端不可用（`drives` 會顯示 `fr=12`），claim 之後才掛回。

### 7.17.1 調校：速度、名稱、內部路徑

**速度（host ↔ SD，8 MB `dd`，重新掛載後無快取）**

| 方向 | 實測 | USB FS 理論上限 | 佔比 |
|---|---|---|---|
| 主機寫入 | 1,027 KB/s | 1,216 KB/s | 84% |
| 主機讀取 | 914 KB/s | 1,216 KB/s | 75% |

SD 卡本身有 10 MB/s，所以瓶頸 **100% 在 USB FS**（S3 只有 FS，沒有 HS PHY）。
`CONFIG_TINYUSB_MSC_BUFSIZE` 在 S3 的 Kconfig range 是 `64..8192` —— **8192 已經係上限**。

> 踩過的坑：改成 16384 會被 clamp 成 **512**，而 `tinyusb_msc_new_storage_spiflash()`
> 要求緩衝 ≥ WL sector（4096）→ 建立 storage 失敗 → **flash 碟完全唔出現**，裝置靜靜
> 回落 app 模式（log 只有一行 `建立 USB storage 失敗：ESP_ERR_NOT_SUPPORTED`）。已改回 8192。

**名稱**

SD 的 FAT 卷標由裝置寫入：**`NCSD-D5F4`**（規則同 flash 的 `NC-D5F4`：prefix + cID 後 4 碼；
使用者自己命名過就不覆寫）。修了兩件事：

1. `label_is_ours()` 只認 `NC-` 前綴 → SD 的 `NCSD-` 會被當成「使用者命名」而永不更新。
   改成按傳入的 prefix 判斷，並且「只有前綴」（cid 未定時寫下的）也視為自己的。
2. `sd_boot()` 原本排在 `node_identity_init()` **之前** → 當時 `s_cid` 還是 0xFFFF，
   卷標只寫成 `NCSD-`。已把身分初始化移到 SD 之前。

現在開機 macOS 直接掛成 `NCSD-D5F4`，唔再需要 `diskutil unmount force` 重掛。

**內部路徑**

- 掛載點集中定義：`NC_STORE_SD_MOUNT "/sd"`（`nc_store.h`），main 與 file_action 共用。
- **`FILE_QUERY` 支援 `/sd/…`**：app 模式下讀的是真值；主機持有時回 busy（唔會假裝有答案）。
  ```
  /sd/bench.bin     2,097,152 B  sha e609118b…   ← 裝置自己寫的
  /sd/from_mac.txt  44 B         sha 6bdbd872…   ← 主機透過 MSC 寫的
  ```
  `free` 對 `/sd` 回 0：`f_getfree()` 要磁碟號，而 VFS 唔會暴露；`size`/`sha` 才是重點。
- `STATUS storage` 的 `sd` 多了 `lbl`（卷標）與 `host`（主機持有中 = 1）。

### 7.18 2026-11-07（SD 設定收斂：只在 hardware.json 宣告一次）

`system.json` 的 `SDcard { enable, bus:{type,index} }` 已刪 —— 它是「SPI 與 SDIO 二選一」年代
的產物；SDSPI 路徑根本沒有實作，而 SDIO 的 `width: 1|4` 已經涵蓋「一線版本」，所以那個
bus 引用純粹是重複。`driver.json` 裡面那份死的 `SDcard`（enable=0）也一起刪。

現在 SD 跟 ETH 一樣**只宣告一次**：

```json
"SDIO": { "enable": 1, "list": [ { "freq": 40000000, "width": 4,
                                  "GPIO": { "sck": 7, "cmd": 6, "data": [15,16,4,5] } } ] }
```

程式側：`nc_sd_cfg_t` / `parse_sd()` 移除；`sd_boot()` 只看 `hw->n_sdio`（enable=0 時 n_sdio=0）。
掛載點固定 `/sd`（`NC_STORE_SD_MOUNT`）。

**`GPIO.data` 的次序是 D0,D1,D2,D3**（跟舊專案的 D3..D0 相反）

- 程式：`slot.d0 = e->d[0]; slot.d1 = e->d[1]; slot.d2 = e->d[2]; slot.d3 = e->d[3];`
- 實證：1-bit 模式（只用 D0）用同一份 config 跑出 4,655 KB/s —— 次序若反了，1-bit 會去用
  GPIO5 而完全不通（§7.16 的 1-bit 對照）。
- 所以 `[15,16,4,5]` = **D0=15, D1=16, D2=4, D3=5**。
- `STATUS storage` 新增 `sd.pins` 直接顯示這個對照，唔需要翻 config：
  `"pins":"CLK7 CMD6 D0=15 D1=16 D2=4 D3=5"`

順手修掉一個 JSON bug：`sd.drives` 之前是被引號包住的字串（裡面又有 `"`），整個
`STATUS storage` 回覆其實不是合法 JSON。已改成真正的陣列。

### 7.19 2026-11-07（main 的棧：把兩塊大緩衝搬離任務棧）

`main.c` 的靜態緩衝其實早就清得差不多（`.bss` 只剩 4,374 B，其中 4,096 是測試用的
`s_bench_pat`）。真正有問題的是**放在任務棧上的兩塊**：

| 緩衝 | 大小 | 原本住哪裡 | 現在 |
|---|---|---|---|
| NC4 回覆幀（`nc4_reply_t.frame`） | 2,615 B | 每個 handler 的區域變數 → decode 棧 | decoder 持有（**PSRAM**，每 decoder 一份） |
| SHA-256 讀取緩衝（`sha256_of`） | 2,048 B | 區塊內 `uint8_t buf[2048]` | 每次呼叫 `malloc`，用完即還 |

**為什麼一定要搬**：`FILE_QUERY` 之後 decode0 只剩 **256 B** 餘裕（5,120 B 的棧用了 4,864）。
一次深一點的呼叫就會爆。搬完之後：

| 項目 | 之前 | 之後 |
|---|---|---|
| decode0 峰值 | 4,864 B（剩 256） | **2,844 B（剩 1,252）** |
| decode1 峰值 | — | 1,184 B（剩 2,912） |
| decode 棧 | 5,120 | **4,096**（−1 KB × 2 核） |

回覆幀放在 decoder 上是安全的：同一個 decoder 一次只跑一個 handler（有鎖），而且它只被
CPU 讀寫、不經 DMA（CDC 是 memcpy 入 FIFO）。`STATUS sys` 新增 `rep_ps` 直接確認它真的在
PSRAM（目前 `rep_ps: 1`）。

**順手抓到的第二個問題：main 任務的棧幾乎見底**

`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 是 2,560，實測 `main` 只剩 **424 B**（某些路徑 **56 B**）。
開機路徑（cJSON 解析 + store + 網絡 + TinyUSB）把它吃到 2,504。已改成 **4,096**，
現在剩 1,512 B。呢個唔係「省 RAM」，係**避免一次深呼叫就 panic**。

**代價／收益**：內部 RAM 幾乎打平（decode 棧 −2 KB、main 棧 +1.5 KB），
但兩個危險的餘裕（256 B / 56 B）變成 1.2 KB / 1.5 KB。

`STATUS sys` 亦加了三個 heap 數字（`h_int` / `h_dma` / `h_ps`）—— 以後講「搬去 PSRAM
有冇用」不用再靠估。

### 7.20 2026-11-07（棧深：由「採樣」改成「算 + 走遍」）

HWM 只覆蓋走過的路 —— 這個說法容易被聽成「不可預測」。其實呼叫是明確的，
問題在**用什麼方法**取得。兩個新工具：

**1. `tools/stack_depth.py`（靜態算）**

用 GCC 的 `-fcallgraph-info=su` 產生的 `.ci`（945 個、9,558 個函數）併成呼叫圖，
從任務入口算最壞路徑。

| 根 | 靜態最壞 | 實測 |
|---|---|---|
| `app_main` | 2,352 B | 2,056（開機）～2,584（運行中） |
| `nc4_bus_task_entry` | **1,408 B** | **2,944 B** ← 差很遠 |

**為什麼 decode 差這麼多（重點）**：handler 是**函數指標**（`entry.handler(ctx, f)`）。
在整份圖裡，`on_file_query` **一條入邊都沒有**，所有間接呼叫都變成指向
`__indirect_call`（2,431 條邊）。也就是說 decode 最深的那一段（VFS → FATFS → SDMMC）
在靜態圖上**不存在**。這不是「不確定」，是「這個方法看不到」。

**2. `tools/stack_cover.py`（走遍）**

既然看不到，就改成列舉：新加 `STATUS keys=cmds` 回傳已註冊的 37 條指令（含 owner），
工具逐條打一次，HWM 就覆蓋全部 handler —— 有限、可重複，不再是抽樣。

第一次跑就抓到問題：**`0xF003`（HUB_BENCH）會令板子重啟** —— 它會開兩個 task 跑 3 秒，
主機在它未完之前再打一條，等於同一個 static 狀態被兩邊同時用。測試指令，已整段跳過。

覆蓋結果（跳過測試/OTA）：**decode0 用 2,236／4,096（剩 1,860）、decode1 用 1,184**。
OTA 那 9 條不敢亂打，但今天十幾次 OTA 都在 4,096 的棧上成功 —— 實務上已覆蓋。

**結論**：靜態算得到「直接呼叫」的上界，間接呼叫要靠列舉；兩者合起來才是完整答案。
`tools/stack_cover.py` 之後每次改 handler 都可以重跑一次。

### 7.21 2026-11-07（指令是怎樣被支援的；加一條指令要幾多資源）

**流程（一條指令從進來到執行完）**

```
CDC ISR / UART task
  └ nc4_bus_push()        插槽（CDC 4×256 B、UART 4×1,024 B）
      └ decodeN 任務（core N，4,096 B 棧）
          └ 取插槽 → 抄入該通道的 parser ring（CDC 4,192 B、UART 1,024 B）
              └ nc4_parser_feed：找 SOF、驗 CRC32、跳過垃圾
                  └ parser_dispatch
                      ├ ADDR 過濾（廣播，或 = 我的 cID）
                      ├ decoder_lookup（線性掃 ≤48 格）
                      └ decoder_lock → handler(ctx, frame) → unlock
```

- **零複製**：`frame.data` 直接指向 parser ring，handler 回傳後失效。
- **一個 decoder、一把鎖**：decode0/decode1 是兩個 worker，但 handler 執行是**序列化**的
  —— 第二核平行的是「解析」，不是「執行 handler」。
- 回覆：`reply_open()` 拿的是 **decoder 持有的那一塊**（`NC4_REPLY_FRAME_MAX` = 2,615 B，
  在 PSRAM），不在任務棧上。

**4 KB 指令夠不夠？分通道**

| 通道 | parser 緩衝 | 可收的最大 payload | 4,096 B 指令 |
|---|---|---|---|
| CDC | 4,192 B | 4,192 − 9 − 4 = **4,179 B** | ✅ 收得下 |
| UART | 1,024 B | **1,011 B** | ❌ **靜靜丟棄**（`total > parser->cap` → `start += 1`、`n_bad_hdr++`、節流警告） |

回覆上限是 decoder 的 2,600 B（`NC4_REPLY_JSON_MAX`）。所以「4 KB 指令」在 CDC 上完整可行，
在 UART 上不行 —— 要就改 `system.json` 的 `links[UART].region`（plan 會跟），要就唔好走 UART。

**加一條指令要幾多資源：0**

- 指令表是**固定陣列**：`nc4_entry_t table[48]`，每格 `cmd(2) + handler(4) + owner(4)` = 12 B，
  整個 decoder 約 600 B，建立時一次過配置。
- 每加一條只佔**陣列裡面的一格**，不會新增任何緩衝、不會新增任務、不會新增棧。
- 唯一的成本是那條 handler **執行時**用到的棧（跟其他 handler 共用 decode 的 4,096 B），
  以及它自己 malloc 的東西。
- 上限 48 格（測試映像 37 條、正式映像 16 條）；滿了會 `ESP_LOGE` 並註冊失敗。

**設計上「怪」的地方（老實講）**

~~兩個 decode 任務共用同一個 decoder，所以 handler 永遠不會並行~~ →
**已於 §7.26 改成每個核心一個 decoder**（指令表仍然只有一份、唯讀）。

### 7.22 2026-11-07（Action 管理器：統計 → 一次配置 → 封存）

**問題**：指令表以前是 decoder 內部寫死的 `table[48]`，各模組用 `on_owner()` 逐條塞。
48 是魔術數字；滿了只會靜靜失敗（真的發生過：STATUS_SET / FILE_QUERY 完全冇反應）。

**改法**：模組**宣告**自己的 `const` 表（放 flash），管理器在開機時統計總數，
**一次 malloc 剛好大小**、填入、按 cmd 排序、封存。

```
core   5 條 ┐
bench 10 條 ┤
test  11 條 ├─ nc_action_init() → 統計 37 → malloc(37 × 12 = 444 B) → 排序 → 封存
ota    9 條 ┤
file   2 條 ┘
```

| | 之前 | 之後 |
|---|---|---|
| 表格 | `.bss` 固定 48 格 = 576 B | heap **444 B**（剛好 37 格） |
| 上限 | 48（魔術數字） | 無 —— 加一行就自動變大 |
| 查找 | 線性掃描 | **二分搜尋**（已排序） |
| 滿了 | 靜靜失敗 → 現在會 log | 不會發生；重複 cmd 會 `ESP_LOGE` 並合併 |
| 開機內部 RAM | 268,895 B | **269,031 B**（+136） |

**形狀**：
- `nc_action_table_t {svc, list, count}`，每個模組一個（`nc_ota_action_table` 等）。
- `main` 只負責列出「有邊幾張表」（5 行），統計／配置／排序全在管理器內。
- `nc4_decoder_on*()` 保留簽名但回 `ESP_ERR_NOT_SUPPORTED` —— 表是靜態的，唔應該執行期改。
- `nc_action_find()` 二分搜尋；`STATUS keys=cmds` 直接遍歷排序後的表。

**為什麼不會碎片**：整個生命週期只有一次 `malloc`、永不 `free`、永不 `realloc`，
大小由「所有表的總和」決定 —— 加指令只會令這唯一一次配置大一點。

驗證：37 條（core 15 / test 11 / ota 9 / file 2）；FILE_QUERY、0xF014、STATUS 全部正常。

**驗證（Action 管理器改完之後）**

`STATUS keys=cmds` 現在附自我檢查：

```json
"verify": {"n":37, "unsorted":0, "lookup_bad":0, "null_fn":0, "bytes":444}
```

`lookup_bad: 0` = 每一條都查得回自己（二分搜尋與表一致）；`unsorted: 0` = 排序正確。

| 類別 | 指令 | 怎樣驗的 |
|---|---|---|
| 核心查詢 | 0x1005 / 0x1016 / 0x1101 / 0x1105 | 剛才逐條打，全部有回應 |
| 檔案 | 0x1103 / 0x2005 | 剛才逐條打（claim/release、FILE_QUERY 回正確 sha） |
| OTA 查詢 | 0x2206 / 0x2210 / 0x2218 / 0x221A | 剛才逐條打 |
| OTA 寫入 | 0x2201/02/03/05/20 | 今日 20+ 次真實 OTA 都成功 |
| SD bench | 0xF012–0xF019 | 今日實測（有速度數字，見 §7.16） |
| CDC bench | 0xF00D / 0xF011 | 今日實測（0xF00D 缺參數會回 err，dispatch 正常） |
| 其餘測試 | 0xF001/03/07/08/0A/0B/0C/0FE、0x1811-13 | **未逐一執行**（會開 task／跑數秒）；其中 0xF003 有問題 |

**已知問題：`0xF003`（HUB_BENCH）會令板子重啟** —— 單獨打也一樣（不是重疊造成的）。
它會開兩個 4 KB 任務跑 3 秒；懷疑是 hub_w/hub_r 的棧或 hub 內部 assert。測試指令，
不在產品路徑上，先記錄未修。

### 7.23 2026-11-08（開機階段拆出來：boot.c 管順序／計時／政策）

`app_main` 以前是一支 420 行的直落函數，所有階段靠註解分段；「邊一步失敗會點」
散落在各個 `if` 裡面，出事之後冇一個地方可以一眼看到跑到邊。

現在：**順序變成資料**。

```c
// main.c
static const nc_boot_phase_t phases[] = {
    { "banner+nvs", ph_boot },      { "storage+cfg", ph_storage },
    { "hardware",   ph_hardware },  { "declare",     ph_declare },
    { "commit",     ph_commit },    { "start",       ph_start },
    { "usb",        ph_usb },       { "channels",    ph_channels },
    { "account",    ph_account },
};
(void)nc_boot_run(phases, NC_BOOT_COUNT(phases));   // boot.c
```

- `boot.c`：只負責跑 + 計時 + 記錄（**零硬體知識**）。失敗唔會自動停 —— 政策留給呼叫者。
- `main.c`：每個階段一支 `static esp_err_t ph_xxx(void)`，內容同以前一樣。
- 加一步 = 表加一行；唔需要再記住「它應該放喺邊」。

**實測（開機一行睇完）**

```
boot 摘要：9.66 | 126.92 | 0.33 | 10.45 | 0.76 | 5.39 | 52.01 | 1.81 | 0.19 ms（9 個階段）
           banner  storage  hw   declare  commit  start   usb   channels account
```

最大的兩塊：`storage+cfg` 126.9 ms（flash 掛載＋config 解析）、`usb` 52.0 ms（列舉）。
總共約 **207 ms**。

**順手發現的一件事**：`app_main` 尾部那條 `for(;;)` 心跳迴圈被「account」階段吞了，
所以 `boot[9/9]` 永遠唔會印 —— 現在明確搬去階段之後（佢唔係階段，永遠唔返回）。

**未做**：階段實作仍然住喺 `main.c`（因為佢們共用 20+ 個 static）。下一步可以逐個搬去
`boot.c`，但每次都要連同它用到的 static 一齊決定歸屬，所以分開做。

### 7.24 2026-11-08（hardware.json 的 ETH 真的接上線；修 config 層三個潛在坑）

**起點**：`hardware.json` 的 ETH 段一直是裝飾品 —— 腳位在 `main.c` 寫死成
`#define ETH_MDC_GPIO 31`，`ph_hardware()` 也無條件啟動 ETH。兩邊數字剛好一樣，
所以「改了 JSON 沒反應」不會被發現。

**改法**：ETH 跟 SDIO 一樣進 `nc_hardware_cfg_t`，由 `nc_cfg_apply_hardware()` 解析；
`main.c` 只留一組**內建值**做 fallback。

| | 之前 | 之後 |
|---|---|---|
| ETH 腳位 | `main.c` 的 `#define`（唯一來源） | `hardware.json` 的 `ETH{}`；`#define` 只在「config 冇寫 ETH 段」時用 |
| `ETH.enable` | 不存在（有 ETH 就一定開） | `0` = 唔建立 netif、唔宣告 TCP 通道（log 寫 `want=0`） |
| 改 JSON | 完全冇效 | 重開機即生效 |
| 缺檔／壞檔 | （無此概念） | `present=0` → 用內建腳位並印 `ESP_LOGW`，板子仍然上得了網 |

**`present` 與 `enable` 是兩件事**（刻意分開）：
`present=0` = config 沒講 → 用內建值（fail-open，避免一個壞 JSON 就把有線網弄死）；
`present=1, enable=0` = 用戶明確關閉（fail-closed）。型別錯一律當「沒講」，不會被誤讀成「關閉」。

**順手修掉的三個坑**（都是「看起來正常，換個呼叫端就爆」）：

| 坑 | 症狀 | 修法 |
|---|---|---|
| `nc_cfg_apply_hardware()` 冇 `memset(st)` | `s_hw_stat` 是 static 所以看不出來；host test 用棧變數就拿到垃圾計數 | 跟 system／tasks 一致，入口清零；另補 `root` 不是 object → `bad_json` |
| `get_str()` 把型別錯記成 `defaulted` | STATUS 上「打錯字」與「冇填」分不出來，違反檔頭自己寫的規則 | 型別錯 → `invalid`，只有「冇寫」才是 `defaulted` |
| SDIO 的 GPIO 上限寫死 48 | P4 有 GPIO 52（MDIO 就在用）；P4 的 SDIO／ETH 一填 >48 就靜靜變 -1 | 改用 `NC_CFG_GPIO_MAX = 54` |

**同時清掉的過期敘述**（會誤導下一個人）：

- `main/CMakeLists.txt`：寫「hardware.json 沒有 embed、firmware 沒讀它」→ 實際有 embed，只是只有 ETH/SDIO 會被讀。
- status §3：寫「system.json 只讀 cID、hardware.json 尚未接線」→ 早已不是。
- `tools/board.py` 的 schema 用 `debug_level`，實際鍵是 `log_debug_mode` → `board.py check` 永遠有 3 個假警告（現為 0 error、0 warning）。
- `ports/P4/.../system.json` 仍留著 7.18 已刪的 `SDcard` 段（當時只刪了 S3）。

**意外的收穫：P4 與 release 建置本來就是壞的**

這次為了編 P4（上一次 P4 建置是 11-07 00:05、commit `35f0e51`，即 SD 改動之前）才發現：

| 問題 | 症狀 | 修法 |
|---|---|---|
| SD 段被包在 `#if !CONFIG_IDF_TARGET_ESP32P4`，但 STATUS／`uart_owner_init()`／`ph_storage()` 照樣用 `sd_hw()`／`sd_boot()`／`s_sd_*` | P4 **編不過**（6 個 `-Werror`：implicit declaration、undeclared） | SD 段移出晶片 guard —— SDMMC 兩顆晶片都有，P4 這塊板只是 `SDIO.enable=0`；用 config 擋，唔用晶片擋 |
| `-Wl,--wrap=sdmmc_*` 無條件加，但 `__wrap_sdmmc_*` 的定義在 `#ifdef NC_ENABLE_TESTS` 裡 | **release 映像 link 失敗**（`undefined reference to __wrap_sdmmc_write_sectors`）。之前的 build 目錄剛好帶著 `NC_ENABLE_TESTS=ON` 的 cache，所以一直沒撞 | `--wrap` 收窄到 `if(NC_ENABLE_TESTS)`：包住那些 sector 的是 `0xF012` 的測試指令 |
| `esp_ptr_external_ram()` 靠別的標頭間接引入 | P4 缺宣告 | 明確 `#include "esp_memory_utils.h"` |
| `sd_bench_buf_get()` 在 release 從未被呼叫 | `-Wunused-function`（P4 先爆） | 整塊 bench 緩衝包進 `#ifdef NC_ENABLE_TESTS` |

> 教訓與 §7.20 同一個：**「上次編得過」不代表現在編得過**。這次 P4 有 2 天沒建置，
> 兩個回歸都是「一方改動、另一方沒驗」。之後每次動 `main.c` 都應該兩邊各 build 一次。

**驗證**：

| 建置 | 結果 |
|---|---|
| S3 release（tests OFF） | ✅ 0 warning，**0x70a70（450.6 KB）** |
| S3 tests ON | ✅ 0 warning，0x77220（476.5 KB） |
| P4 release（tests OFF） | ✅ 0 warning，**0xb6460（729.1 KB）**（上一顆 11-07 的 0xa5530／661 KB 少了 SD／Action／boot 全部改動，+0x10F30） |

`tools/hosttest/run_all.sh` 8/8（`cfgstruct_test` 新增 38 項、合共 **93 項**：
ETH 的缺段／正常／明確關閉／型別錯／未知鍵，以及兩塊板**真的出廠 hardware.json**
零 invalid、零 unknown）；`python3 tools/board.py check` 0 錯誤、0 警告。

**未做**：`PIN/SPI/I2C/I80/UART/I2S/PWM/ENC/XL9555` 仍然只有「認得這個鍵」；
preset/merge 與 arena→PSRAM 見 [hardware-config.md](../03_notes/hardware-config.md) §7。
**P4 只在 build 層驗過**（板子不在手邊），ETH 改由 config 決定的行為要上板確認：
開機應見 `hardware: eth=開 mdc=31 mdio=52 refclk=50 IP101@1` 與 `ETH started (... 來源=hardware.json)`。

### 7.25 2026-11-08（緩衝區貼身化：UART −15 KB、測試回覆幀 −2.6 KB 棧、雙核隔離量測）

**目標**：每個緩衝都要有實測依據（不再「先給大」）；UART 照 CDC 的形狀；雙核互不干擾；
吞吐不得退步。全部在 S3 上以 [buf_probe.py](../../tools/bench/buf_probe.py) 量測。

**先抓到四件「看起來正常、其實在燒資源」的事**

| # | 問題 | 證據 | 修法 |
|---|---|---|---|
| 1 | UART 的 `uart_read_bytes(cap, portMAX_DELAY)` 會**等收滿 cap** 才返回（IDF 是 `while(length)` 迴圈） | 送 1 幀 77 B → `rx` 永遠 0 | 改成「阻塞等第一個 byte → 搬走環裡已有的」＝ CDC 的語義 |
| 2 | `test_action` 的回覆幀仍是 **2,615 B by-value struct**（7.19 只搬了 main.c 那份） | 單一條 `0xF00C` → decode0 HWM 2,580 → **364**；4 KB 洪水時只剩 **124 B** | 改用 `nc4_decoder_reply_buf()`（PSRAM） |
| 3 | UART 的環是「照抄 CDC 再放大」：slot 4×1,024、RX 4,096、TX 8,192 ＝ **16.4 KB** | 實測 `max_read=246`、slot 峰值 **1**、`slot_full=0` | 全部貼身（見下表） |
| 4 | 測試指令由 handler 直接寫 UART：揸住 decoder 鎖 → decode1 不能消費 → 溢位 | 200 幀×77 B 只收到 62/200 | 新增 `0xF00E UART_STREAM`（獨立 task） |

**尺寸（前 → 後）**：UART slot 4×1,024→**4×256**、RX ring 4,096→**1,024**、TX ring 8,192→**1,024**、
`uart.rx` 棧 4,096→**1,536**（實測 888）、`cdc.rx` 1,536→**1,280**（實測 936）、
decode0/1 4,096→**3,072**（全 handler 走遍 2,232，剩 840）、main 4,096→**3,072**（實測 2,120）、
`nc_log` 3,072→**2,560**（實測 1,804）。CDC region 4,192 **不動**（OTA 的 4,096 chunk 需要），
CDC slot 4×256 **不動**（已經是 1 KB）。
單項合計 **約 −19.7 KB**（開機實測 +8.5 KB，差額來自 FATFS/MSC 等非本次項目）。

**判「夠快」用達標率**（實測 ÷ 物理上限；方法見 [measurement-method.md §4](../02_guides/measurement-method.md)）：UART 98–100%、CDC TX ~85–100%、CDC RX ~96%、4 KB 幀 ~95%；指令 245/s 只有 ~25% 的理論上限，但單條 1.08 ms 已夠用，不是瓶頸。

**不降性能**（前後各 3 次）：UART 500×16 B 172→**161–166 ms**、200×64 B 220→**169–178 ms**、
100×256 B 339→**294–301 ms**（全部 `lost=0`，≈90 KB/s＝921600 baud 的 98%，在線速上限）；
CDC RTT 1.084→**1.07–1.13 ms**；CDC TX 1 MB 1.025→**1.024 s**；CDC RX 500×256 **500/500 不變**。

**雙核獨立**：兩條 bus 的 `remote`（跨核 kick）都是 **0**；UART 400×64 B 在 core0 狂打
STATUS（108 條、p50 2.29 ms）時由 339.9→**334.9 ms（−1.5%）**。殘留耦合是**共用一個
decoder**（handler 執行序列化）—— 反映在回覆延遲，不影響 UART 吞吐；per-core decoder
仍是未做項（§7.21）。

**soak 與 CRC 追查**：UART 在滿載混合 soak 偶發 `crc_err`。六輪對照排除了插槽（`slot_full=0`）、parser、環容量（RX 1 KB→2 KB 照樣出現）；真兇是 `fx` 測試任務與 `uart.rx` **同優先級**（都是 3）→ 10 ms/tick 時間切片剛好壓過 1 KB 環的 11 ms。fx 降到 prio 2 後，事件率由 ≈1/900 降到 **≈1/3,150 循環**（30 分鐘、4,460 循環、2 次；每一輪 300 幀都收齊，沒有真的掉幀）。殘留那次已排除緩衝因素，下一步是數 RX FIFO overrun。規則：**同核的租戶任務不得與 producer 同優先級**。

**靜態分區**：`s_entries`（plan 宣告）64→16 格、`s_buses` 8→4 格，合計 **−3,136 B**；
開機內部 heap `287,383 → 290,519`（正好 +3,136），`STATUS mem` 顯示 `decl=4 fail=0`、
兩條 bus 都在。滿了會回錯並印帳，不會靜默。

**未做／取捨**：CDC 4 KB 連續洪水仍受 4 格插槽限制（74/100，與改動前相同；OTA 有流控）；
UART 仍然**只收不回**（`send=NULL`，所以 TX ring 只留 1 KB；要回覆得先接 `uart_send()`）；
IO7 同時是 SD CLK 與 UART 迴路 TX，兩者不能同時用（需要換腳位）。
細節與量測表：[buffer-tightening.md](../03_notes/buffer-tightening.md)；下次照著跑的流程：[measurement-method.md](../02_guides/measurement-method.md)。

### 7.26 2026-11-08（每核一個 decoder：接收與發送都不再共用一把鎖）

**問題**：兩個 decode 任務共用**同一個** `nc4_decoder_t`，handler 執行由那一把鎖序列化
——一條 CDC 指令在跑，core1 的 UART 指令只能等（§7.21 把它列為「設計上怪的地方」）。

**改法**：每個核心一個 decoder（`s_decoder[2]`）。decoder 只管「執行與回覆」的狀態：

| 每個核自己的 | 全程序共用（唯讀） |
|---|---|
| 執行鎖（`lock`） | 指令表 `nc_action`（排序後的唯讀陣列，`nc4_decoder_lookup` 直接查它） |
| 回覆幀（2,615 B，PSRAM，每核一份） | 協定常數／CRC |
| 位址過濾狀態（cid、master_cid、dropped） | parser 的**每個 bus** 仍然獨立（本來就是） |

接線：`nc4_bus_add()` 依 bus 的核心取 decoder（CDC→core0、UART/TCP→各自核）、
WS→core0（httpd 在 core0）、UDP→core1（udp task 釘在 core1）；
`SET_MASTER` 同步到**所有** decoder（回覆位址必須一致）。指令表不用複製。

**唯一需要另外保護的**：OTA 是跨指令的狀態機（`s_ota`）——整組用一把 mutex 串起來
（`OTA_GUARD` wrapper），其他 handler 不受影響。test_action 的 bench statics 仍是測試專用，
跨核同時打同一個 bench 本來就不安全（0xF003 已知會重啟，§7.22）。

**並行性驗收**（新測試指令 `0xF00F UART_INJECT`：把任意 NC4 幀寫進 UART → 迴路 → decode1 執行）：

| 情境 | 結果 |
|---|---|
| core1 被 `0xF001 DEV_BENCH`（3,000 iter ≈ 280 ms）**完全佔住** | 同期 50 條 ping 在 core1 上一條都沒被處理（rx=0 @ 277 ms） |
| 同時間 core0 的 CDC RTT | **1.17 ms**（閒置 1.13）→ 不受影響 |
| 20 條指令排隊的吞吐 | **1.21 ms/條**（改動前 1.26） |
| UART 吞吐／CDC TX | 341.7 fps ／ 0.97 MB/s（與改動前相同） |
| 跨核 kick（`net.bus[].remote`） | 兩條 bus 都是 **0** |

**代價**：+2,615 B PSRAM（第二個回覆幀）、內部 heap −240 B、OTA 多一把 mutex。

**順帶發現（記憶體帳要更新）**：用乾淨 sdkconfig 重建後，映像由 489 KB 變 567 KB、內部 heap
由 290,479 變 271,911 —— 追下去是 **IDF 6 把 WiFi 的 binary blobs 整包 link 入 S3 映像**：
`CONFIG_ESP_WIFI_ENABLED` 是沒有 prompt 的符號（S3 `default y`），寫 sdkconfig、加 Kconfig
`depends`、在 CMake 用 normal variable 覆蓋，三種方法都被 IDF 蓋回（實測）。它用 linker
fragment whole-archive 拉入，所以即使 `CONFIG_NETCORE_WIFI=n`（無任何引用）也照樣進映像。
→ 帳面更正：S3 映像 **+75 KB flash、−18 KB 內部 RAM**；`NETCORE_WIFI=n` 保證的是
「不呼叫 esp_wifi_init」（runtime heap 0），不是「不 link」。文件已同步（§1、§7.1 F、
[memory-budget.md](../03_notes/memory-budget.md)）。

**量測時發現的坑（重要，含一次自我更正）**：CDC RX 合成洪水（2,000×256 B）在
「per-core 版本」量到 1,711 fps、在「舊的單 decoder 版本」量到 1,827 fps（−6%）。

- 先做歸因實驗：把新程式碼**只建一個 decoder** → 仍是 1,710 ⇒ 不是 per-core 架構造成的。
- 再試 `-falign-functions=64`：量到 1,772–1,789（回春 4%）。**但後來檢查發現那個 build
  的映像大小與未對齊版完全相同（0x8a8c0），代表旗標根本沒有套用到（IDF 的 toolchain
  會覆蓋 `CMAKE_C_FLAGS`）** —— 那 4% 是 **flash 與 flash 之間的量測變異**，不是對齊效果。
- 結論要保留不確定性：**同一個 binary 的數字很穩（±0.5%），但「換一次 OTA／重新列舉 USB」
  可以差到 4%**。所以 −6% 裡有多少是真的、多少是列舉變異，目前不能下定論；
  唯一確定的是**不是 decoder 的接線**（單 decoder 對照同樣慢）。
- 要再往前一步：把熱路徑（`nc4_parser_feed`／dispatch）放 `IRAM_ATTR` 做 A/B，
  並且**每次都量同一顆 binary 三次**才算一組。這也寫進
  [measurement-method.md §7 陷阱 11](../02_guides/measurement-method.md)。

### 7.27 2026-11-08（發送／轉寄路徑：今天的行為與下次的設計）

> **本節是當日早上的結論**；同日晚上 §7.28 已把 6.1／6.2／6.3／6.4 落地（UART 會發、
> link 出口與 router 轉寄已實作、回覆超限不再截斷）。下面的「未實作」保留作歷史對照。

使用者問「轉寄 4 KB 夠不夠？PSRAM 會不會中斷？要不要 DMA？是不是一定要等送完？」——
答案整理成 [tx-path-design.md](../03_notes/tx-path-design.md)。摘要：

| 通道 | 今天「交出去」的緩衝 | 要不要等送完 |
|---|---|---|
| CDC | TinyUSB FIFO **4,096 B**（>回覆上限 2,615） | **不用**（memcpy 進 FIFO 即返回；線路時間與下一指令重疊） |
| UART | `send = NULL` | 根本不發 |
| TCP（P4） | socket sndbuf **5,760 B** | **>5,760 B 要等**（8 KB 幀一定阻塞到對端 ACK） |
| 轉寄 | **未實作**（router 只有 `line → core`） | — |

- 「分三批」不會解放流水線：`frame.data` 指向 parser 環，handler 返回前不能失效。
  要解放只能 **copy 進目的 ring**（µs）或 **pin 插槽**。
- S3 **不需要** 8 KB：CDC 4,192／UART 1,024 已經對得上「命令小包、OTA 4 KB」；8 KB 只發生在 P4 TCP（16,410 region＋8×8,205 插槽）。
- DMA：UART **沒有 TX DMA**、CDC FS 沒有、P4 ETH 有；DMA 不減少緩衝需求，只改變誰排空。
- 回覆超 2,600 B 目前是**靜默截斷**（`reply_send`／`reply_json`）→ 列為要修的缺陷。

### 7.28 2026-11-08（發送路徑落地：回覆超限、link 出口、router 轉寄、UART TX）

**背景**：§7.27 的四個問題。當日把「低風險那幾項」做完，並且上板驗證。
細節與取捨：[tx-path-design.md](../03_notes/tx-path-design.md)。

| # | 項目 | 做法 | 驗收 |
|---|---|---|---|
| 6.1 | 回覆超 2,600 B 不再靜默截斷 | `reply_send` 改回合法錯誤物件 `{"err":"reply_too_big","need":N,"cap":2600}`；`reply_json` 唔再 copy 落去（避免越界） | 上板 `0xF01A 3000` → 收到該物件；`2600` → 原樣（2,602 B 幀） |
| 6.3 | **`nc4_link` 出口登記表**（新檔案） | 名字 → write fn；`nc4_link_send` = 整幀交出去，成功計 `frames/bytes`，短送／錯誤計 `drops`（**不能重試**：來源緩衝即時失效） | hosttest `link_test` 8 case；上板 `0xF01B FWD CDC` → 主機真的收到轉寄幀 |
| 6.4 | **router `out` 支援 link 轉寄** | `nc4_route_t` 存完整 out；`nc4_router_links()` 濾 `decodeN`／自我／重複；`parser_dispatch` **先轉寄、後本地 dispatch**；開機做 link 稽核 | 上板：`CDC out=["decode0","UART"]`，送 1 條 PING → `LOOP_STAT rx` **+2**（本地＋UART 迴路回來） |
| 6.2 | UART TX 接上（沿用 1 KB 環，**零 RAM**） | `uart_send` = 整幀 `uart_write_bytes`（阻塞到排空） | 上板（暫時關 SD）：`0xF01B FWD UART` → `ESP_OK`，`frames 9 / bytes 175 / drops 0` |
| — | **`make.py flash --tests` 會燒錯映像** | `flash()` 冇把 `tests` 傳給 `_idf_args()` → 先建測試版、`app-flash` 時又重編成 release 才寫入。已修 | 今天實際踩到（log 由 0x78590 變 0x71d30） |
| — | **重複指令碼行為不確定** | 原本 qsort 之後 dedupe，相等 key 次序冇保證 → 同一 cmd 在兩顆 binary 可以是不同 handler。改成填入時**先到先得＋`ESP_LOGE`** | 今天真的撞到：0xF012／0xF013 已被 SD bench 佔用 → 新測試指令改用 **0xF01A／0xF01B** |

**跨核轉寄的副作用（已處理）**：CDC／TCP 的「一幀分多個 chunk 寫」以前只在單核成立；
現在 `routes[].out` 可以令 core1 送 CDC／TCP，兩個核的 chunk 會交錯成兩條壞幀。
→ `cdc_send`／`tcp_nc4_send` 各加一把**整幀 mutex**；`s_tx_reply` 的寫入移到 critical
section（另一個核的 log 才看得到）。log 路徑**唔攞鎖**，所以 log 永遠唔會卡回覆。

**上板量測**（S3、tests 映像；暫時 `SDIO=0` 讓 GPIO7 做 UART 迴路，測完已還原）
- 直接 link：`0xF01B FWD UART`（21 B）→ `{"link":"UART","len":21,"err":"ESP_OK","frames":3,"bytes":74,"drops":0}`；`LOOP_STAT rx` 0 → 2。
- router：`CDC → ["decode0","UART"]`，主機送 1 條 21 B PING → `rx` 由 2 → 4（本地 1 ＋ 迴路 1）。
- 統計：`links.UART frames 9 / bytes 175 / drops 0`；UART bus `frames 12 / crc_err 0 / bad_hdr 0`。
- 開機稽核：`轉寄：CDC → UART ok`、`轉寄：1 條可用、0 條冇出口（出口登記 2 個）`。
- 還原後：`轉寄：0 條可用…`、`UART 測試迴路唔啟動：GPIO7/8 已經係 SD 的 clk/cmd`（與改動前一致）。

**大小**（同一棵樹、release）
| 建置 | 前 | 後 | 差 |
|---|---|---|---|
| S3 release | 0x70e80（451.6 KB） | **0x71d30（455.3 KB）** | **+3,760 B** |
| S3 tests | — | 0x78580（481.4 KB） | 含 0xF01A 的 4,097 B 測試緩衝 |
| P4 release | 0xb6460（§7.24） | **0xb7b60（734.8 KB）** | +5.8 KB（含 §7.25／7.26，不能全記在本次） |

靜態 RAM 約 **+1.6 KB**（link 表 288 B＋router out 名稱 512 B＋開機路由 JSON 768 B）；
tests 映像另有 4,097 B 的 `0xF01A` 緩衝。

**測試**：`bash tools/hosttest/run_all.sh` **9/9**（新增 `link_test`，`router_test` 加 out 的
9 個 case）；S3／P4 零 warning；上板 OTA 兩輪（含一次配置切換與還原）。

**仍未做**：UART 環 4,192（+3.2 KB，等決定）；轉寄滿了的政策（跟各 transport）；
forward-only route；環狀拓撲；OTA flash 停頓對 `uart.rx`。見
[next-steps §6](next-steps.md)。

### 7.29 2026-11-09（共用緩衝池、跨核轉寄、PSRAM 實測）

**背景**：§7.28 之後，使用者要把「每條線自己一份緩衝」收斂成共用 pool，並且問
「PSRAM 到底可不可行」。這節只記落地與量測；設計與取捨在
[buffer-pool.md](../03_notes/buffer-pool.md)。

| # | 項目 | 做法 | 驗收 |
|---|---|---|---|
| 7.1 | **`nc_pool` 共用緩衝池**（新檔案） | 三個固定 size class（small 1 KB／bulk 4 KB／forward 4 KB）＋ descriptor（state／src_link／refcnt／gen）；`portMUX` 保護 free list；`nc_pool_alloc/commit/retain/release` | 上板 `STATUS mem.pool`：small 1024×8、bulk 4160×3、forward 4160×2；`init=1`、`fails=0` |
| 7.2 | **`nc4.pool` 設定** | `system.json` 的 `nc4.pool[]`：name/size/count/where/enable；開機 `ph_pool` 一次配置；`where=psram` 可選 | S3／P4 system.json 已加；預設全 internal；P4 forward 8,208 |
| 7.3 | **跨核轉寄 queue** | `parser_forward()`：目的核 ≠ 來源核 → 借 forward slot、copy、入目的核 queue（深度 8）、`nc4_bus_kick_core()`；目的核 `nc4_fwd_poll()` 送完 release | 上板 CDC→UART：6 幀 → `fwd.core1.err=6`、`links.UART.drops=6`、`forward.peak=1`、`fails=0`（UART driver 未開，見下） |
| 7.4 | **`0xF01C MEM_BENCH`** | 量 internal／PSRAM 的 memset／read／copy MB/s＋`msync` µs | 見下表 |
| — | **PSRAM 可行性** | 實測 21–33 MB/s（不是 160）；38.4 KB 一幀 copy 1.83 ms | 60 fps 下一次 copy 11%、3 passes 33%：可行但要預算 |

**上板量測**（S3、tests、無 flash 活動；`0xF01C 256 4`／`1024 4` 結果一致）

| 區域 | 大小 | memset | read（32-bit） | copy | msync（dirty） |
|---|---:|---:|---:|---:|---:|
| internal | 32 KB | 483–507 MB/s | 42–51 MB/s | 133–252 MB/s | — |
| PSRAM | **16 KB** | 212 MB/s | 52 MB/s | **152 MB/s** | 316 µs（cache-hot） |
| PSRAM | 64 KB | 29 MB/s | 30 MB/s | **20 MB/s** | 521 µs |
| PSRAM | 256 KB／1 MB | **33 MB/s** | **32 MB/s** | **21 MB/s** | **515 µs** |

- 38.4 KB frame（整幀 streaming）：PSRAM 寫 1.16–1.32 ms、copy 1.8–1.9 ms、read 1.28 ms、msync 0.4–0.5 ms；
  internal copy 0.15–0.3 ms。
- **修正**：16 KB 的 152 MB/s 是「同一塊重複 copy」的 cache-to-cache；`0xF01D` 量到新鮮 40 KB
  分塊 copy 只有 **16 MB/s**（full 12），tiling 只快 1.3×，因為每個 byte 仍要寫入 PSRAM。
- 分塊的真正好處是 bound latency（9.6 KB flush ~0.1–0.2 ms）同 multi-pass 局部性，不是單 pass 頻寬。
- 38.4 KB frame：寫 1.2 ms＋讀 1.2 ms＋flush 0.4 ms ≈ **2.8 ms（17% @60fps）**；
  每多一個全幀 pass 多 ~1.2 ms。多 pass 效果應 copy 入 internal tile 做完再寫回一次。
- `pool` 本身 **28,992 B**＋descriptor；上板 `sys.h_int` 237,403（tests 任務有跑）／
  253,983（USB storage 模式，部分測試任務未跑）。
- 轉寄測試後 `system.json` **逐位元組還原**（sha256 `086bbbd7…`），`net.fwd` 全 0。
- 大小：S3 tests 498,896 B（OTA 138–141 KB/s）；P4 release 零 warning。
- 測試：hosttest **9/9**（`cfgstruct_test` 新增 `[22] nc4.pool` 解析：size/count/where/enable、
  `where` 打錯 → internal＋invalid）；S3／P4 建置零 warning；上板 OTA 兩輪、轉寄測試一次。
- 未做：`where=psram` 的**上板** override（USB volume 在 app 模式冇重新掛載；hosttest 已驗解析）。
- **`0xF01D TILE_BENCH`（40 KB、8 次）**：整幀 12 MB/s；tile 2–16 KB 都係 **16 MB/s**（1.3×）；
  tile 32 KB 13 MB/s。→ 單 pass streaming 是 **bus-bound**，tiling 只 bound latency／幫 multi-pass，
  不是「切 16 KB 就 150 MB/s」。設計見 [psram-surface.md](../03_notes/psram-surface.md)。


**仍未做**：TTL／leak watchdog、tap subscriber、bulk 放 PSRAM 實測、
forward 成功路徑（要 UART 腳位或第二條可送 link）、pool 接到 RX（header-first）、
多實例（UART0/1/2、I2C、SPI）。見 [buffer-pool.md §5](../03_notes/buffer-pool.md)。

### 7.30 2026-11-09（通訊緩衝定案：轉發走 PSRAM 的 nc_hub）

**背景**：`dd46be4` 的 `nc_hub` 有了 strict FIFO／generation／destroy／核心統計；
使用者接受「轉發慢一點、走 PSRAM」。這節只定案，未改 code。

| 決定 | 內容 |
|---|---|
| RX | 留在 internal（per-link parser／PUSH slots），不變 |
| 轉發 | **`nc_hub` per（目的 link × 來源核）**，PSRAM，strict FIFO，writer mutex；同核也走 hub |
| `nc_pool.forward` | **取消**；`nc_pool` 只留 small／bulk，預設 `enable:0` |
| 回覆幀 | 每核 2,615 B PSRAM（不變） |
| 記憶體 | S3 16 hubs × 2 × 4,160 = **133 KB PSRAM**（internal 新增 0）；P4 8 × 2 × 8,256 = 132 KB |
| 延遲 | 4 KB 轉發 **+0.4 ms**（PSRAM 寫＋讀）；8 KB +0.8 ms |
| 失敗 | hub 滿 → drop＋`drop_full`；send 失敗 → `drop_send`；registry 滿 → 開機 fail-visible |

- 完整設計：[comm-buffers.md](../03_notes/comm-buffers.md)。
- 下一步：`nc4_fwd` hub 版（P1），保留舊 queue 做 A/B；P2 才改預設。
- 未做：tap／refcount（用 `nc_hub_generation`）、P4 8K 上板、per-link `max_frame`。

### 7.31 2026-11-10（緩衝工具收斂成一個：`nc_buf`；先統一字眼）

**背景**：討論 P1 時發現真正的問題不是「hub 版怎麼寫」，而是**同一類東西有四個名**
（plan／pool／hub／環），`STATUS` 的帳散在四處，每加一條線都要重新決定用邊種。
使用者要求「整個項目只用一個緩衝工具，不要零散的概念」。

| 決定 | 內容 |
|---|---|
| 唯一工具 | **`nc_buf`** ＝ N 個等長格 ＋ 每格狀態。`where`（S／P／DMA）與順序（FIFO／隨機）是**參數**，不是型別 |
| 三條鐵律 | ① 開機一次要齊 ② **永不釋放**（冇 `destroy`）③ 執行期只准在 arena 內 `carve`，唔准向 heap 要 |
| 碎片 | **冇 free 就冇碎片** —— 舊文件把功勞歸給 plan 的「一次 carve」，更正為「冇 free」；plan 降級成 `nc_buf` 內部的 **arena** |
| S2P／P2S／S2S／P2P | 不是四種 buffer，是同一個 `put()` 的四種引數（格在哪 × 資料由哪來）；`DMA` class 收到 PSRAM 指標強制 copy 入 S |
| 退役 | `hub`（借來的名，易誤會成集線器）、`pool`（別處＝通用池）、`環`（兩種意思撞名）、`plan`（改名 arena） |
| 帳 | `STATUS keys=mem` 只剩一個 `mem.buf`（含 `s2p/p2s/s2s/p2p` 累計 bytes、`viol`） |
| 佈局 | 熱路徑→S、冷路徑→P、DMA→S；PSRAM 頻寬全核共用（兩核同時 copy 21→10 MB/s）；大幀用 S tile 做完再一次寫回 P |

- 完整設計與 S3 逐條佈局表：[buffers.md](../03_notes/buffers.md)；字眼：[glossary.md](../03_notes/glossary.md)。
- 取代 `buffer-manager.md`／`buffer-manager-status.md`／`buffer-pool.md`／`comm-buffers.md` 的**設計部分**
  （後三者保留量測數字）。
- 下一步 P1：`nc_buf` 核心（FIFO＋隨機＋S/P copy＋`mem.buf`）；**`NC_BUF_MAX` 滿了要回 NULL，不可靜默**
  —— 這正是 `nc_hub` 今日的缺陷（registry 滿只係唔登記）。
- 未做：可等待模式、tap／多讀者、per-link `max_frame`、arena 切法（S／P 各一個）。

### 7.32 2026-11-10（`nc_buf` 落地＋上板驗收：統一緩衝的第一步）

**背景**：§7.31 的定案（一個工具、永不 free、換手、`off`／`len`）要有可跑的實作。
今天落的是**機制**（P1）；把 plan／pool／hub 搬過去是 P2–P5，未做。

| # | 項目 | 內容 | 驗收 |
|---|---|---|---|
| 7.32.1 | `nc_buf` 核心 | `create`／`at`、FIFO／隨機、`put_begin(reserve)`／`put`／`put_end(len)`、`get`／`get_end`、`handover`、`nc_buf_json` | hosttest `buf_test`、上板 `0xF032` |
| 7.32.2 | 全部 32-bit | `cap`／`len`／`off`／`gen` 一律 **uint32**（今日 `nc_pool` 的 uint16 在 LCD 一幀會爆、`gen` 5,000 幀／秒 13 秒就 wrap） | 上板建 100,000 B 的格、`put_end(100000)` 唔截斷 |
| 7.32.3 | 每格範圍 | `off`（reserve，自己放表頭）＋`len`（實際用量）——`nc_hub` 今日完全冇 `len`，只能送滿格 | reserve 項：`off=16`、`len=100`、表頭冇被蓋 |
| 7.32.4 | S／P 記帳 | `put` 時按「格在哪 × 來源在哪」分 `s2s`／`s2p`／`p2s`／`p2p` 累計 bytes | 上板四項各記到（64／512／128／64 B） |
| 7.32.5 | 換手 | 全部格 FREE 才准；`generation++`；**記憶體位置不變** | 有格未還 → `INVALID_STATE`；還清 → OK |
| 7.32.6 | `mem.buf` | STATUS `keys=mem` 多一段；零值欄位省略、放唔落有 `truncated`、**唔會出半條 JSON** | 上板 858 B（上限 2,600） |
| 7.32.7 | 零複製監測 | `src_buf_of()` 判斷來源落在哪條 buffer；跨 buffer 的 copy 計入 **`xcopy`** —— 工具唔會自動幫你改寫，但「本來可以唔抄」會現形 | 上板 `xcopy=128`（P2S 由 `t.psram` 抄入 `t.sram`）、其餘路徑 `xcopy=0` |

**上板結果**（S3、tests 映像、2026-11-10 11:14，`0xF032` ＋ `tools/buf_test.py`）

- 9 項全 pass：`align`／`fifo`／`full`／`reserve`／`s2s`／`s2p`／`p2s`／`p2p`／`handover`，`viol=0`、`big=0`。
- 測試 buffer：`t.sram` 256×4（S）、`t.psram` 1024×2（P），合計 **3,072 B**；重跑唔會再建（registry 唔漏，`count` 一直 2）。
- `keys=mem` **858 B**（plan 360／hub 91／pool 262／buf 250 平均值 125 B 一條）；
  13 條 buffer ≈ 1,600 B，仍然放得落 2,600。
- 建置 **527.2 KB**（+336 B vs 上一顆）、**零 warning**；hosttest **10/10**。
- 板子：`app` hash `ba926135`、`h_int` 211,507、`h_ps` 7,847,272、`rep_ps=1`。
- **追加（同日）**：加 `xcopy` 之後重上板 —— 9 項仍然全 pass，`t.sram` 出現 `xcopy=128`、
  `t.psram` 冇（因為 P2P 係同一條 buffer 內部，唔算跨 buffer）。設計說明見
  [buffers.md §4.1](../03_notes/buffers.md)。

**順手修的兩個既有測試問題**（同 `nc_buf` 無關，但「全套測試」要綠才睇得到）：

- `tools/hosttest/cfgstruct_test/run.sh` 缺 `-D_POSIX_C_SOURCE=200809L` →
  `nc_cfg.c` 的 `fileno()` 在 glibc 嚴格模式變成 implicit declaration → 整支**從未跑得過**。
- 同一支測試的「4 列預設 route」已過時：預設在 11-10 加了 ESPNOW，實際 **5 列**。

**未做**：arena（把 plan 吸收進來）、把回覆幀／TX 環／pool／hub 搬到 `nc_buf`（P2–P5）、
`handover` 在生產路徑真正使用、可等待模式、tap／多讀者。

### 7.33 2026-11-10（任務 ↔ 緩衝區契約成文；三份任務文件統一）

**背景**：§7.32 把 `nc_buf` 上板了，但「任務怎麼拿緩衝區、停止之後還在不在、
誰可以換手」只散在幾份文件裡，而且寫法互相矛盾。這一節把契約寫成**一份規範**，
其餘三份任務文件改成指向它。

| 文件 | 改什麼 |
|---|---|
| [buffers.md §9](../03_notes/buffers.md) | **新增契約 C1–C11**（宣告／建立／交付／生命／擁有權／讀寫權／熱路徑／對外可見／`.fxe`／上限／驗證）＋現況對照表＋「為什麼是這些條」 |
| [writing-a-task.md](../03_notes/writing-a-task.md) | §0／§1／§2／§3.1／§3.6／§5／§6／§7／§8 全部改講「**資源＝緩衝區**」；`pulse` 範例由 `nc_hub` 改成 `nc_buf_at` |
| [task-lifecycle.md](../03_notes/task-lifecycle.md) | 欄位表、四步圖、失敗語義、歸還表統一；**更正 `NC_PLAN_MAX_DECLS` 64 → 16**；補 `NC_BUF_MAX 32` |
| [task-model.md](../03_notes/task-model.md) | 加「記憶體＝緩衝區」指引 |
| [glossary.md](../03_notes/glossary.md) | 加「契約／宣告窗口／owner」三條 |
| [next-steps.md](next-steps.md) §9 | 加 9.10 |

**統一過程抓到的三個事實錯誤**（都已改）：

1. `task-lifecycle.md` 寫 `NC_PLAN_MAX_DECLS = 64`，實際 **16**（[nc_plan.h](../../firmware/components/sys/mem/include/nc_plan.h)）。
2. 原本的講法「逐項失敗 → 任務收到 NULL 自己退化」**是錯的**：實作是
   **全有或全無** —— 一項拿不到，整個任務不啟動、`on_start` 不被呼叫
   （[task-lifecycle.md §2 ③](../03_notes/task-lifecycle.md)）。契約已按實作改。
3. `writing-a-task.md §6` 的範例仍然教人用 `nc_hub`（那是要被取代的舊路徑）。

**契約的核心一句話**：*任務只宣告、管理器建立、`on_start` 之後只有一條指標；
停止唔會消失，要給下一個用戶就 `handover`。*

### 7.34 2026-11-10（總線盤點：工具補了 STREAM 與「唔會卡死」語義）

**背景**：問「總線那邊需不需要更新這個工具」。盤點 `nc4_bus` 之後，答案係
**總線有一份手寫的同類實作，而工具缺一個形狀**。

| 總線的東西 | 形狀 | 對 `nc_buf` |
|---|---|---|
| PUSH 插槽（TCP／WS／UDP） | `slot_state[16]` ＋ `slot_len[16]` ＋ FIFO 指標 | **同 FIFO 模式一樣** → 重複實作，應該搬 |
| PULL parser 環（CDC／UART） | 線性 ＋ `memmove` 壓實、可前移 1 byte | 需要 **STREAM 模式** |
| TX 環 | driver 自己 | 唔搬 |
| 回覆幀 | 2,615 B heap | 可搬（P2） |

**工具補了兩樣**（都已上板）：

| # | 補什麼 | 為什麼 |
|---|---|---|
| 7.34.1 | **STREAM 模式**：`write`／`read_ptr`／`skip`／`pending`／`space`，尾部唔夠位自動壓實 | parser 環要「部分寫、部分讀、重同步 `skip(1)`」；`read_ptr` 保證係**一段連續** |
| 7.34.2 | **FIFO 唔標 WRITING ＋ `put_abort()`** | 總線的註解明講「少一個狀態就少一個可以卡死的地方」；工具原本 `put_begin` 會標 WRITING，producer 一死就永遠卡住 |
| 7.34.3 | `mem.buf` 加 `stream`／`space`／`abort` | 串流的可觀察性 |

**上板**（`0xF032` 擴充到 11 項）：全 pass —— `stream=1`、`abort=1`；
`t.stream` 1024 B（`stream:1`、`space:1024`、`s2s:1124`）。
建置 528.9 KB、零 warning；hosttest 仍 10/10（`buf_test` 加兩組：STREAM、abort）。

**未做**：總線真正搬過去（B1 PUSH 插槽 → B2 parser 環 → B3 回覆幀／TX），
見 [buffers.md §11.2](../03_notes/buffers.md)。

### 7.35 2026-11-10（parser 環改成 STREAM：A/B 跑分結果）

**做了什麼**：`nc4_parser` 內部由手寫 `{buf, start, end}` ＋ `memmove` 壓實，
改成**一條 `nc_buf` STREAM**（`CDC.rx`／`UART.rx`／`TCP.rx` 出現在 `mem.buf`）。
`nc4_parser_feed()` 改用 `nc_buf_write`／`read_ptr`／`skip`；framing 邏輯（SOF 掃描、
壞幀前移 1 byte、過大幀跳過）**逐行保留**。為了 runtime 生命週期，`nc_buf` 加了
`NC_BUF_F_RUNTIME` ＋ `nc_buf_release()`（只准還 runtime 建立的；開機建立的仍然拒絕）。

**A/B 方法**：同一支腳本、同一塊板、同一個 workload（500 幀 STATUS_GET，burst 16）；
「before」係把上一顆映像（`cd2e4223`）OTA 燒回去量的，「after」係新映像（`d968dc12`）。

| 指標 | BEFORE `cd2e4223` | AFTER `d968dc12` | 判讀 |
|---|---|---:|---|
| RTT p50（n=40，keys=sys） | 15.811 ms | 15.854 ms | **一樣**（+0.3%，噪音內） |
| RTT p95 | 16.226 ms | 16.765 ms | +3% |
| RTT min | 1.66 ms | 1.25 ms | 稍快 |
| 500 幀 → parser 實際見到 | 108、108 | 99、105、102 | −5%（方向一致） |
| **`copied`（固定 18,036 B）** | 18,036 | **18,036** | **完全一樣 → copy 次數冇變** |
| `parse_ms` | 222–223 | 231–237 | +4–6% |
| `slot_full` | +299／+308 | +313／+328 | +5% |
| `mem.buf` 可見性 | 冇 | **`CDC.rx` 4,192 STREAM** | 新增 |

**結論**：
1. **形狀改變係中性**：RTT 一樣、copy 次數一個 byte 都冇變（18,036 → 18,036）。
   輕微的 +4–6% 開銷來自每次 feed 多了一次空間檢查與 `account_copy` 的 registry 掃描。
2. **真正的瓶頸唔在 parser**：500 幀只有 ~100 幀入到 parser，`slot_full` 約 **300** ——
   即係**六成幀在 slot 環就被丟掉**，根本冇到 parser。這個數字 before/after 都存在。
3. **CDC 今日每個 byte 走 3 次 copy**：TinyUSB FIFO → slot → chunk → parser ring
   （`st_copied` 每幀約 167 B ≈ 3×56 B）。真正的收益在「producer 直接寫 STREAM 環」，
   可以做到 **1 次 copy ＋ 刪掉 slots**，而唔係改 parser。

**下一步（改寫 §11.2 的 B1）**：唔係「PUSH 改 PULL」，而係
**producer 讀到 bytes 就 `nc_buf_write` 入 STREAM 環，decode 直接 parse**
—— 保留事件驅動的低延遲，同時省掉 2 次 copy 與 slot 環。

### 7.36 2026-11-10（量測工具的真 bug：所有 RTT 都被多算 10–50 ms）

**症狀**：PUSH 模式的 CDC 量到 RTT p50 = **15.8 ms**，但程式註解寫 PULL 當年係 10.0 ms
—— 換成 PUSH 反而更慢，講唔通。

**真正原因**：`tools/nc4_demux.py` 的 `Reader.poll()` 用 `ser.read(4096)`。
pyserial 的 `read(N)` 在不足 N bytes 時會**等滿埠的 timeout** 才返回，
所以每次量測都多了一個 timeout（工具用 0.01～0.2 s）。**唔關 firmware 事**。

**怎麼驗證**：

| 實驗 | 結果 | 說明 |
|---|---|---|
| 一次送 4 幀，看回覆到達時間 | 15.01／15.02／15.03／15.03 ms（**一起到**） | 4 幀一齊到 → 唔係每幀慢，是一個固定延遲 |
| 改成只讀 `in_waiting`（唔用阻塞 read） | RTT p50 **1.63 / 1.68 ms**、p95 1.8–2.2、max 2.6、lost 0（n=60） | 真實延遲 |

**修正**：`Reader.poll()` 改成「有資料就 `read(in_waiting)`，冇資料就睡 1 ms」。
影響所有主機工具（nc4.py／ota.py／bench／selftest）—— 它們的延遲數字一齊變準。

**更正兩個結論**：
1. **PULL 的 10 ms 仍然成立**，但原因是**裝置側**：PULL 模式 decoder 自己
   `ulTaskNotifyTake(pdMS_TO_TICKS(10))`（`DEFAULT_POLL_MS`），所以每 10 ms 才讀一次。
   → **PUSH 比 PULL 快約 6 倍**（1.6 vs 10 ms），「PULL 比較快」係錯的。
2. §7.35 的 A/B（15.811 → 15.854）**兩邊都用舊 reader**，差值仍然有效（形狀改變中性），
   但絕對值要按這裡的修正理解。

**未變**：CDC 每個 byte 仍然走 3 次 copy；`slot_full` 在洪水下仍然六成丟幀 ——
那才是下一個要做的（producer 直接寫 STREAM 環）。

### 7.37 2026-11-10（對照使用者 2026-11-05 的事件驅動紀錄：冇回歸，差別在平台）

**背景**：[core-isolation.md §11](../03_notes/core-isolation.md) 記錄了 CDC 事件驅動 receiver 的成果
—— RTT p50 **0.59 ms**（相對 PULL 10 ms 快 19 倍）。今日用修好的 reader 量到 **1.65 ms**，
所以要查「係唔係回歸」。

**量測（同一個 reader、同一塊板、同一個 `keys=sys`）**：

| 映像 | RTT p50 | p95 | min |
|---|---:|---:|---:|
| 改 parser 之前 `cd2e4223` | **1.673 / 1.658 ms** | 1.86–1.90 | 1.23 |
| 改 parser 之後 `d968dc12` | **1.649 / 1.648 / 1.665 ms** | 1.78–1.79 | 1.22 |

→ **改 parser 唔係原因**（差 0.5%，噪音內）；`copied` 兩邊同為 18,036 B。

**0.59 → 1.65 的真正差別：平台。** `core-isolation.md §11` 那批數字係 **macOS**
（tools 原本寫死 `/dev/cu.usbmodem*`）；今日係 **Windows `usbser.sys`**，
min RTT 1.20–1.23 ms 就係它的下限。所以當年的 0.59 ms 成果仍然成立，唔需要「恢復」。

**順手更正一個危險的建議**：§7.35／§11.2 曾寫「producer 直接寫 STREAM 環」，
這是**錯的** —— STREAM 寫入會 `memmove` 壓實，而 decode 在 `parser_dispatch()`
期間仍揸住 ring 的指標；producer 一壓實，資料就喺指標腳下移動（靜默損壞）。
使用者現時的設計（producer 寫 slots、decode 自己讀）正好把讀寫分開，係**正確**的。
安全的一次性優化只剩：decode 直接由 slot 解析（省 `slot→chunk` 那一次 copy），見
[buffers.md §11.2](../03_notes/buffers.md) 的 B1。

### 7.38 2026-11-10（板內量測：撞到 decoder 互斥鎖自我死鎖）

**動機**：主機量到的 RTT 一定夾住 USB（1 ms 幀 ＋ OS 驅動），睇唔到 firmware 自己的成本。
所以加**板內量測**：專用 parser ＋ 假 send sink，一次灌 N 幀，量「解析 + dispatch + handler」；
另外在 bus 加 `kick_us`（commit → decode 接手的喚醒延遲），直接量事件驅動那段。

**踩到的坑（重要，已寫進程式註解）**：

> **handler 內唔可以再 `nc4_parser_feed()` 同一個 decoder。**
> `parser_dispatch()` 會 `decoder_lock()`，而 FreeRTOS 的 mutex **唔係遞迴**；
> handler 已經持有那把鎖，再 feed → 再 dispatch → 再 lock → **自我死鎖，永遠卡住**。
> 實測：`0xF033` 一發，整個 decoder 停住，連 `status` 都唔答；task watchdog 救唔到
> （decode 只係 block，idle 仍然跑）⇒ 要人手 RST。

**修正**：bench 改成**獨立任務**（prio 2、core 0）跑，唔喺 handler 裡面做。
`0xF033` 只負責起任務（回 `started`），`0xF034` 讀結果。

**學到什麼**：量測程式碼同樣要守「一個 decoder 一次一個 handler」的不變量；
要跑第二條解碼路徑，就一定要用**另一個 decoder** 或**另一個任務**。

### 7.39 2026-11-10（板內量測跑起來：一個優先級倒置令喚醒慢 7.7 倍、六成丟幀）

**板內量測（完全唔經 USB）**：`0xF033` 起一條獨立任務，灌 N 幀進專用 parser（假 send sink），
`0xF034` 讀結果。

| 幀 | 每幀 | 吞吐 |
|---:|---:|---:|
| 21 B | **16.5–17.5 µs** | 1.2–1.3 MB/s |
| 269 B | **44.1 µs** | 6.1 MB/s |
| 4,109 B | **460 µs** | **8.9 MB/s** |

→ 小幀成本固定（17 µs，主要係每幀一次 mutex ＋ dispatch）；大幀 460 µs 才是 CPU 上限。

**事件驅動的喚醒延遲（`kick_us`：`push_commit` → decode 接手）**：

| | 平均 | 最大 | `slot_full`（插槽滿而丟） |
|---|---:|---:|---:|
| 改之前（producer prio **5** > decode **4**） | **478 µs** | 1,772 µs | **+122**（300 幀 workload） |
| 改之後（producer prio **3** < decode **4**） | **62 µs** | 250 µs | **0** |

**根因**：producer 優先級高過 decode ⇒ 它會一路讀到冇資料才讓 decode 接手；
改成「producer 低於 decode」之後，producer 只在 decode 睡著（等 kick）時才跑，
push 完 notify 即刻被更高的 decode 搶佔。

**副作用（好的）**：`slot_full` 變 0 —— 之前「洪水六成丟幀」其實是**這個優先級問題**造成，
不是插槽太少。**唔需要**改插槽數量，也唔需要改 buffering 形狀。

**未變**：RTT 仍然 1.66 ms（下限由 USB／Windows 驅動決定，min 1.28），
`0xF032` 11/11、`selftest` 21/21、hosttest 10/10。

### 7.40 2026-11-10（把 NC 核心抬到保留區 7–15：丟幀歸零、解析成本 3× 改善）

**做法**：新增 [nc_prio.h](../../firmware/components/sys/task/include/nc_prio.h)，
把「NC 核心（收 → 解 → 回）」訂成**保留區 7–15**：`decode0/1 = 8`、bus producer = 7、
其他任務（含 TinyUSB 5／fx 2／log 2／app 1）一律 ≤ 6。規則寫進
[execution-rules §1.3](../03_notes/execution-rules.md)。

**為什麼要保留區**：核心是整個系統的節拍來源；沒有具名常數，下一個人會直接寫 `prio = 8`
把核心擠掉，而且症狀只會是「偶爾慢」。實測也證明相對次序是硬規則（producer 高過 decode
= 478 µs 喚醒 ＋ 插槽讀爆）。

**量測（同一塊板、同一個 workload）**：

| 指標 | decode 4 / producer 3 | **decode 8 / producer 7** |
|---|---:|---:|
| 300 幀 → 實際解析到 | ~100 | **301** |
| `slot_full`（插槽滿而丟） | +122 | **0** |
| `parse`（µs/幀） | 1,311–1,843 | **510–518** |
| `kick_us`（平均 / 最大） | 62 / 250 | **42 / 130** |
| `copied`（B/幀） | 138–182 | **36** |
| RTT p50（主機，Windows） | 1.66 ms | 1.67 ms（USB 下限） |

**注意**：`kick_max` 的**累積**值會被 `selftest` 的 256 幀 burst 拉高（看到 6.7 ms）——
那是刻意的最壞情況壓力，唔係日常路徑；單獨跑 300 幀 benchmark 時最大 130 µs。

**整合測試（同日，全部綠）**：

| 測試 | 結果 |
|---|---|
| 解碼器一致性 `nc4_test.py` | **9/9**（正常／黏包／拆包／壞 CRC／假 SOF／錯 addr／錯版本／超長 LEN／純文字） |
| 專案自測 `selftest.py` | **21/21**（含 burst 128/256、OTA 資訊） |
| 統一緩衝 `0xF032` | **11/11** |
| 路由表 `0x1105` | `set=1`、core0=2／core1=1、`nctx→decode0`、`CDC→decode0`、`UART→decode1` ✅ 與 system.json 一致 |
| link 出口 `0xF01B` → UART | `ESP_FAIL`、`drops=1` —— **已知**：這塊板 SD 佔用 GPIO7/8，UART driver 未開 |
| hosttest | **10/10** |

**未做**：TCP／WS／UDP 只在 P4 才建立，S3 上冇法測那三條；UART 要等腳位。

### 7.41 2026-11-10（三項調整：管理器管優先級、移除「隨機」、緩衝先做完）

**使用者複審的三點，全部落實**：

| # | 調整 | 實作 |
|---|---|---|
| 1 | **優先級改由任務管理器管** | `nc_task_loop.c` 在開 loop 前檢查：非 `SYSTEM` 任務的 prio 落入保留區 7–15 → **夾到 6 ＋ 印警告**。`nc_task_rt_t` 加 `kind` 欄位。規則：[contracts.md §3](../03_notes/contracts.md) |
| 2 | **移除 `nc_buf` 的「隨機／任意空格」模式** | 只剩 **FIFO**（唯一格模式）＋ **STREAM**。理由：冇真實用戶，而且它要多一個 `WRITING` 狀態（writer 死掉就卡死環）。修正過程中 host test 抓到一個真 bug：`put_end`／`get_end` 只在 `FIFO` flag 存在時才推進指標，預設（flags=0）會永遠寫同一格 —— 已改成無條件推進 |
| 3 | **緩衝區先做完** | 下一步順序固定為 P2（回覆幀）→ P3（pool／hub）→ P4（TX 環），見 [buffers.md §8](../03_notes/buffers.md) |

**驗收（改完之後上板）**：

| 項目 | 結果 |
|---|---|
| 優先級階梯 | `decode0/1 = 8`、`cdc.rx = 7`、`uart.rx = 7`、`TinyUSB 5`、`fx/nc_log 2`、`main/hello 1` ✅ |
| `0xF032` | **11/11** |
| 300 幀 burst | **301/300 幀**、`slot_full` **0**、`parse` 475 µs/幀 |
| `kick_us` | **38 / 104 µs**（平均／最大） |
| hosttest | **10/10** |
| 建置 | 零 warning |

### 7.42 2026-11-10（P2：回覆幀搬進 plan ＋ `nc_buf`；線路的緩衝現況）

**做了什麼**：`nc4.reply0`／`nc4.reply1` 由 `task_nc4_declare()` 宣告（P，各 2,615 B），
`task_nc4_start()` 用 `nc_buf_at()` 建成 n=1 的緩衝區，再經新 API
`nc4_decoder_set_reply()` 交給 decoder（`destroy` 唔會 free 外部提供的）。
plan 失敗時仍然退回原本的 heap 分配並印警告。

**上板驗收**：`plan` 見到 `nc4.reply0/1`、`mem.buf` 見到兩條 P 緩衝區（2,615 B）、
`rep_ps=1` 不變、RTT 1.678 ms、`0xF032` 11/11、`selftest` 21/21、hosttest 10/10。

**線路的緩衝：邊啲已經用 `nc_buf`**（回答「線路是不是已經用了緩衝工具」）

| 線路的緩衝 | 今日 | 用了 `nc_buf`？ |
|---|---|---|
| parser 環（RX 湊幀，CDC／UART／TCP） | `CDC.rx`／`UART.rx`（STREAM） | ✅ |
| 回覆幀（每核一條） | `nc4.reply0/1`（P，n=1） | ✅（今日 P2） |
| **PUSH 插槽環**（producer → decode） | `nc4_bus` 自己手寫 `slots` ＋ `slot_state[16]` ＋ `slot_len[16]` | ❌ |
| **`chunk`**（decode 的中轉） | `b->chunk` | ❌ |
| TX 環 | UART driver／CDC FIFO（driver 擁有） | ❌（唔搬） |
| 跨核轉寄 | `nc_pool.forward` ＋ 每核 queue | ❌（P3） |

→ **2026-11-10 晚已補齊**（見 §7.43）：插槽環亦已收進 `nc_buf`，線路只剩 TX 環（driver 擁有）。

### 7.43 2026-11-10（P3：插槽環收進 `nc_buf`；壓力測試：4002 幀零丟、copy 減半）

**做了什麼**：
- `nc4_bus` 手寫的 `slot_state[16]`／`slot_len[16]`／`slot_w`／`slot_r`／`push_idx`／`rd_idx`／`rd_off`／`frame_idx`
  全部刪除，改成一條 `nc_buf` FIFO（`<link>.slots`，例如 `CDC.slots`）。
- `push_begin/commit/abort`、`frame_begin/end`、`reset` 全部走 `nc_buf`。
- decode 迴圈 **直接由插槽解析**（`nc_buf_get` → `nc4_parser_feed(ptr)` → `nc_buf_get_end`），
  唔再經 `b->chunk` —— 少一次 copy。`st_copied` 由 `got*2` 改成 `got`。
- `nc_buf` 加三個診斷 accessor（`state_at`／`writing`／`reading`），STATUS 的 `slots` 陣列照舊有意義。

**壓力測試（同一個 reader、同一塊板）**：

| 測試 | 結果 |
|---|---|
| 2000 幀 burst 32（第一次） | **+2001 幀**、`slot_full` **0**、`copied` **18 B/幀**（改前 36）、`parse` 483 µs/幀 |
| 2000 幀 burst 32（第二次） | **+2001 幀**、`slot_full` **0**、`copied` 18 B/幀、`parse` 487 µs/幀 |
| 累積（含 selftest 的 128/256 burst） | frames 4,166、`us_per_frame` 533、`kick` 68 / 5,628 µs、`peak` 3/4、`crc_err` 2、`bad_hdr` 8（後兩者是 `nc4_test` 刻意餵的壞幀） |
| 板內解析（`0xF033`） | 小幀 **17.8 µs**、4 KB **460 µs（8.9 MB/s）** —— 與 P2 前一樣 |
| RTT | **1.663 ms**（p95 2.00、min 1.19） |
| `mem.buf` | 9 條緩衝區：`nc4.reply0/1`、`CDC.slots`、`UART.slots`、`CDC.rx`、`UART.rx`、`t.sram`、`t.psram`、`t.stream` |
| 其他測試 | `nc4_test` 9/9、`selftest` 21/21、`0xF032` 11/11、hosttest 10/10、零 warning |

**結論**：線路的接收側（parser 環、插槽環）與回覆側（reply）**100% 走同一個緩衝工具**；
只剩 TX 環（UART driver／CDC FIFO）—— 那是 driver 擁有，唔應該搬。

### 7.44 2026-11-10（P4：刪掉 `nc_pool`；轉寄改成一條 `nc_buf` FIFO；internal +32.8 KB）

**發現**：`nc_pool` 有三個 class，但 `nc_pool_alloc()` 的**唯一呼叫者**是跨核轉寄的
`NC_POOL_FORWARD`；`small`／`bulk` **零用戶**，而 `system.json` 預設 `enable:1`
—— 即係白佔 **28.3 KB internal**（8,192 + 12,480 + 8,320）。

**做法**：
1. **刪掉整個 `nc_pool`**（`nc_pool.c`／`.h` 移除，CMakeLists 同步）。
   `ph_pool()` 只留一句 log；`nc4.pool` 的 config 仍然讀得入（唔會壞檔）但唔再建池。
2. **跨核轉寄**改成每核一條 `nc_buf` FIFO（`fwd.c0`／`fwd.c1`，PSRAM、按需建立）：
   格 = `NC4_LINK_NAME_MAX`(16) ＋ `NC4_MAX_FRAME`(8,205)，深度 4；
   目的 link 名放格首（用 `put_begin(reserve)`），整幀放後面。
   多個來源 task 同時轉寄 → 每核一把 mutex（契約 C6）。
3. `net.fwd` 的 JSON 不變（frames／drops／err 照舊）。

**上板驗收**：

| 項目 | 結果 |
|---|---|
| `sys.h_int` | 209,467 → **242,239 B**（**+32.8 KB** internal） |
| `mem` 段 | 只剩 `plan`／`hub`／`buf`（`pool` 消失） |
| `net.fwd` | 仍然有 `core0/core1` 的 frames／drops／err（全 0，因為 S3 冇轉寄路由） |
| `nc4_test` | **9/9** |
| `selftest` | **21/21** |
| `0xF032` | **11/11** |
| hosttest | **10/10** |
| RTT | **1.63 ms** |
| 1000 幀 burst | 1001 幀、`slot_full` 0、`copied` 18 B/幀 |
| 建置 | 零 warning |

**未做（收尾清單）**：`nc_hub` 只剩測試用戶（probe／0xF003／0xF01F–0xF024）；
`nc_buf_carve()` 未做；`nc_plan` 公開 API 仍在（P5）；UART TX 環決定**唔加大**（RX 優先）。

### 7.45 2026-11-10（收尾：`nc_hub` 定位 ＋ loop 心跳落地）

**`nc_hub` 的處置**：它只剩測試用戶（probe 的開機自測、`0xF003`、`0xF01F`–`0xF024` 共 7 條指令），
生產路徑**零引用**。決定：**保留、明確標為測試專用**，唔搬去 `nc_buf`
（搬 7 條測試指令風險高、收益低）；`mem.hub` 只會顯示測試用的環。

**loop 心跳（停滯偵測）落地**：舊 `monitor` 任務 2026-11-04 已刪、`epoch` 2026-11-06 已移除，
之後板上**完全冇機制**發現某條 loop 卡死。現在補一個最便宜的版本：

| 項目 | 做法 |
|---|---|
| 計數 | 每 loop 一個 4 B `beats`（`nc_task_rt_t`） |
| 短 fn | 管理器 trampoline 每輪 `++` |
| **長跑 fn** | 自己叫 `nc_task_beat()`（`decode0/1`、`fx` 已加） |
| 停滯判斷 | **主機**比對兩次 `STATUS keys=tasks` 的 `b` —— 板子零成本、零新任務 |
| 曝光 | `keys=tasks` 每個 loop 多一個 `b` 欄位 |

**上板實測（2 秒窗口）**：

| loop | 心跳 |
|---|---:|
| `decode0` / `decode1` | **42 / 40**（閒置 ~21 Hz，受 50 ms 安全網限制） |
| `fx` | **200**（10 ms 週期 ＝ 100 Hz） |
| `hello` | 2（1 s 週期） |
| `bench_a/b/c` | 0（`affinity [0,0]` 未啟用，符合預期） |

**同時全綠**：`nc4_test` 9/9、`selftest` 21/21、`0xF032` 11/11、hosttest 10/10、
RTT 1.619 ms、零 warning。

### 7.46 2026-11-10（三個收尾：audit 文件更正、`nc_buf_carve()`、P5 第一刀）

| # | 項目 | 結果 |
|---|---|---|
| 1 | **`claims[]` audit** | 程式**早已修好**（舊寫法在迴圈 `nclaims--` → 跳過一條且刪錯條；現用 `claim_off` 位元標記），`task_test` 亦有覆蓋（`gamma` 保留 `pin:16`、`delta` 被擋）。**差的是文件** — 已更正 |
| 2 | **`nc_buf_carve()`** | ✅ 已實作：無狀態 bump（呼叫端自己保管 `size_t cursor`）、64 B 對齊、唔夠位回 NULL（唔會部分配置）、自動帶 `NC_BUF_F_RUNTIME`（可以 release）。hosttest 第 12 組 9 項全 pass |
| 3 | **P5 第一刀** | `nc_task_res_t` 加 `slot_bytes`／`n`／`flags`；宣告 `slot_bytes > 0` 的資源由**管理器**建 `nc_buf`，`on_start` 直接收 `res[i].buf`。`nc4.reply0/1` 已改成這個形狀（任務唔再自己 `nc_buf_at`） |

**上板驗收**：`nc4.reply0/1` 仍以 P 2,615 B 出現（由管理器建立）、`rep_ps=1`、
`0xF032` 11/11、`selftest` 21/21、`nc4_test` 9/9、hosttest **10/10**（`task_test` 的
run.sh 補了 `nc_buf.c`）、建置零 warning。

### 7.47 2026-11-10（P5 完成：宣告「格 × 格數」，`on_start` 直接收 `nc_buf_t *`）

| 資源 | 之前 | 現在 |
|---|---|---|
| `nc4.reply0/1` | 任務在 `on_start` 自己 `nc_buf_at()` | 宣告 `slot_bytes=2,615, n=1` → 管理器建 |
| `nc4.rx` | 任務 `nc_plan_get()` → parser 自己 `nc_buf_at()` | 宣告 `slot_bytes=region, n=1, flags=STREAM` → 管理器建，bus 用 `cfg.rx_buf` 收 |
| `uart.rx`／`tcp.rx` | 同上 | 同上 |
| `fx.arena` | 純區域 | 宣告 `slot_bytes=512 KB, n=1` → 管理器建（`.fxe` 之後用 `nc_buf_carve()` 由它切） |
| `probe.hub` | 純區域 | 保持（probe 自己建 hub，測試專用） |

新增 API：`nc4_parser_create_buf()`（adopt 一個已存在的 STREAM buffer）＋
`nc4_bus_cfg_t.rx_buf`。

**結果：全 repo 已經冇任何任務 call `nc_plan_get()`** —— `nc_plan` 只剩管理器
（declare／get）與開機 commit（`nc_plan_commit`／`nc_plan_json`）在用，對外等於內部實作。

**上板驗收**：

```
plan: nc4.reply0, nc4.reply1, nc4.rx, uart.rx, probe.hub, fx.arena
buf : nc4.reply0/1（P 2615 n=1）、nc4.rx（S 4192 stream）、uart.rx（S 1024 stream）、
      fx.arena（P 524288 n=1）、CDC.slots、UART.slots、t.*
```

| 測試 | 結果 |
|---|---|
| `nc4_test` / `selftest` / `0xF032` / hosttest | **9/9 · 21/21 · 11/11 · 10/10** |
| RTT | **1.637 ms** |
| 1000 幀 burst | +1001、`slot_full` 0、`parse` 466 µs/幀 |
| heap | 239,347 B |
| 建置 | 零 warning |


### 7.48 2026-11-10（驅動層開工：`components/driver/`，第一個 driver = UART）

**背景**：`main.c` 3,793 行，一半是硬體細節（USB／SD／ETH／UART／GPIO 測試），同「組裝」混在一起。
舊專案 `slave/driver/*_drv.py` 的分法很清楚：**driver（匯流排）→ lib/hw（晶片）→ lib/sys（owner 帳本）**。

**做法**：
- 新增 `firmware/components/driver/`，加入 `EXTRA_COMPONENT_DIRS`
  （一個周邊一個 component，名字 `drv_<peripheral>`，避免同 IDF 撞名）。
- 第一個抽出 **`drv_uart`**：`nc_uart_drv_install/ready/read/write/send`（~120 行），
  阻塞語義（「等第一個 byte 再搬走現有的」）寫在 header。
- **政策留組裝層**：SD 佔用 GPIO7/8 → 唔啟動 UART 這條檢查仍然在 `main.c`；
  driver 只回「安裝成唔成功」。
- `nc4_bus_cfg_t.read/.send` 與 `nc4_link_register` 改指 driver 的函式
  （`nc4_link` 的簽名帶 arg，driver 那支唔需要 → 一支薄轉接）。

**驗收**：建置零 warning、hosttest 10/10、`selftest` 21/21、
UART bus 仍然存在（`mode=push-slot, core=1`，driver 未安裝 → `frames 0`，與改之前一樣）。

**文件**：[driver-layer.md](../03_notes/driver-layer.md)（三層對照、六條規則、遷移順序）。

**下一步**：SD → ETH（P4）→ **USB/CDC 最後做**（熱路徑剛調好，要重跑整套 RTT／burst）。

### 7.49 2026-11-10（分層修正：`sys/transport` 移出 sys，成為通用匯流排 `bus/`）

**背景**：使用者指出分類怪 —— 網絡相關（WiFi／transport）唔應該放 `sys`。
再討論之後定清楚：中間應該有一層 **`bus/`（通用匯流排）**，**唔綁 NC4**：
NC4 只是第一種 personality，將來 LED（SPI／I80 波形）同 LCD（I80／RGB 面板）都係同一層。

**三層判準**：

| 層 | 一句話 | 現況 |
|---|---|---|
| `driver/` | 怎麼讀寫一個硬體介面 | `drv_uart` ✅；`drv_usb`／`drv_sd`／`drv_eth` ⏸ |
| `bus/` | 一條資料流怎麼排隊、誰擁有、幾時喚醒、每輪幾多 | `bus_net` ✅（今日）；`bus_nc4` ⏸；`bus_led`／`bus_lcd` 未有 |
| `sys/` | 房東：解碼、任務、記憶體、config、store、log | 不變（但唔再放 transport） |

**今日做了**：`firmware/components/sys/transport/` → `firmware/components/bus/bus_net/`
（component 名 `bus_net`；`components/bus` 加入 `EXTRA_COMPONENT_DIRS`；
`main` 的 REQUIRES 由 `transport` 改 `bus_net`）。

**驗收**：建置零 warning、`selftest` 21/21、`0xF032` 11/11、
500 幀 burst `slot_full` 0（`parse` 490 µs/幀）、hosttest 10/10。

**未搬（下一輪）**：`sys/nc4/nc4_bus.c` ＋ `nc4_link.c` → `bus/bus_nc4`（熱路徑，要重跑整套）；
`main/nc_wifi.c` → `sys/net`（PHY，唔係一條線）；`sys/transport/nc4_http.c` → 跟 WS 一起
（設定頁同 httpd 同一個實例，暫時唔拆）。

### 7.50 2026-11-10（分層再修正：`bus` = 統一線路 API；NC4 唔綁網絡）

**使用者第二次修正**（第一次是「transport 唔應該在 sys」）：

1. `bus` 不是「一條線的實作」，而是**統一線路 API** —— 用家唔需要知道底下是什麼總線
   （對照舊專案 `lib/hw/TFT.py`：上面只管畫，下面 SPI／I80 一樣）。
2. **雙重保障**：統一 API（read／write／ioctl）＋ `nc_bus_native()` 逃生門拿原生 handle。
   唔會為統一而封印硬體能力。
3. **NC4 是幀協議，唔應該同網絡協議綁定**：`sys/nc4` 只管幀／parser／router；
   TCP／WS／UDP 只是 `bus` 的一種線（socket），NC4 是用家之一。

**今日落地**：

| 組件 | 內容 |
|---|---|
| `bus/bus_core` | `nc_bus_t`（name／kind／ops／ctx）＋ 統一 API ＋ `nc_bus_native()`；核心刻意做到最薄（只轉發＋檢查） |
| `bus/bus_uart` | UART adapter：`nc_bus_uart_open()` 把 `drv_uart` 包成統一線路；`ioctl(SET_BAUD)` 留一個例子 |
| `bus/bus_net` | 原 `sys/transport`（TCP／WS／UDP／HTTP），暫放；下一步改成 socket 線的 adapter |
| `driver/drv_uart` | 已抽（原生 API） |

**驗收**：建置零 warning、hosttest 10/10、`selftest` 21/21、`nc4_test` 9/9。

**兩個操作注意（今日實測）**：

1. **OTA 之後唔可以立即跑 burst 測試**：`up_s` 雖然已 10+ 秒，主機仍然會 write timeout；
   等 ~30–60 秒再跑就正常（`selftest` 21/21）。已排除是 crash（`up_s` 單調上升、無 panic）。
2. **未接線的組件會被 linker GC 掉**：`bus_core`／`bus_uart` 目前冇人引用 →
   映像同之前**完全一樣**（`app` hash 不變是正常的，不是 OTA 冇生效）。
   要真正驗證，下一步必須把 UART 線接上 `bus`。

**下一步**：main 的 UART 線改經 `bus_uart`（低風險，本板 UART 未安裝）；
然後 `bus_net` 改成 socket 線；最後才搬 `nc4_bus`／`nc4_link`（熱路徑）。
