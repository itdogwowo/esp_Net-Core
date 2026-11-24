# 專案現況總結（單頁）

> **用途**：所有「已經驗證的事實、已定案的設計、還沒做的事」集中在這一頁；
> 細節都連到對應文件。**最後更新**：2026-11-21。

---

## 1. 這是什麼

ESP32-P4（另有 S3 port）上的 **NC4 通訊核心**：多條傳輸通道 → 解碼 → 指令執行，
外加分核（RX core1 / decode core0）、零複製、可調的緩衝管理。

| 面向 | 狀態 |
|---|---|
| 晶片 | ESP32-P4 rev1.3（ECO2）、360 MHz、32 MB PSRAM、IDF 6.0.3 |
| 網路 | 100 Mbps full duplex（RMII + IP101），**DHCP**（2026-11-17 起唔再寫死靜態 IP；`network.json` 的 `lan.ip.mode=static` 未實作） |
| 通道 | TCP :6000、UDP :6001、WS :8080/nc4、HTTP **:80**（static GET/HEAD；冇設定端點） |
| 解碼 | 分核、零複製（一格一整幀）、task notification |
| 架構 | 系統任務（硬體→driver→通道）＋ 一般任務（生命週期）已分層，兩者同一套任務管理器 |
| 任務模型 | **核心（房東）／任務（租戶）**：`tasks.json` 只管租戶；`level` / `affinity` 由 config 定，**未設定 = `[0,0]` = 唔跑**；缺的條目開機自動補寫（寫 `[0,0]`，預設關閉）（[task-model.md](../03_notes/task-model.md)） |
| .nct / .nce | 掃 `/data/pixel/effects/` 與 `/data/tasks/`，attach 成 runtime 任務；**WAMR 執行引擎未接** |
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
| 涵蓋 | `nc4.c` / `nc4_bus.c` / **`nc4_router.c`（新檔）** / `main.c` / `nc_task.c` / `nc_cfg.c` / `nc_mod.c` / `nc_file_action.c` |

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

**現況**：`main.c` 只剩一張任務表（核心 `nc4` / `net` / `core` / `ncm` ＋ 測試用 `probe` / `hello` / `bench_a/b/c`；
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
| `tools/ncm.py pack / dump` | 打包 / 檢視 `.nct` / `.nce`（header + manifest + payload + 指令表） |
| `tools/nc4_demux.py` | **NC4 協議唯一實作**：`\x1e`…`\n` = log 行；`"NC"`+VER/LEN/CRC32 才算幀，失敗只前進 1 byte |
| `tools/nc4_log.py` | 看 log 管道（log 與幀共用 CDC） |
| `tools/bench/cdc_tx.py` | CDC TX 壓測（log／回覆爭出口時的行為） |
| `bash tools/hosttest/run_all.sh` | 一次跑完 8 支 host test（cfg / cfgstruct / ncd / task / router / mod / hub / plan），全綠 = 冇 error 亦冇 warning |
| `tools/hosttest/*/run.sh` | 單支 host 測試，不用板子也不用 IDF：hub / plan / mod / **ncd** / **task** / **cfg** / **router**。Windows 上只有 `cfg` / `ncd` / `task` / `router` 有 `run.bat` |
| **ESP-IDF 環境** | macOS：`~/esp/esp-idf` **v6.0.3**（2026-11-20 實測可以直接 `make.py build` ＋ OTA）；Windows：WSL 內 v6.1.0（Windows 本身冇 IDF）。`firmware/build/` 兩邊共用，看 map 時留意是哪邊 build 的 |
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
| J | **NCM 引擎冇執行後端** | ❌ 下一步計劃 | `.nct` / `.nce` 載入成功但永遠唔跑（WAMR 未接）。**已寫進 [ncm-engine.md](../03_notes/ncm-engine.md)**，屬於下一階段，不是本輪的 bug |
| K | **`level` 只影響宣告/啟動次序，唔係 barrier** | ⚠️ 設計如此 | `level` 保證「呼叫次序」，唔保證「上一層真係開始跑」（`xTaskCreate` 返嚟唔等於 task body 已執行）。要「上一層 ready 才開下一層」就要 `nc_task_ready()`（見 [task-lifecycle.md](../03_notes/task-lifecycle.md) §7） |
| L | **CRC / 壞表頭統計** | ✅ 已加（2026-11-03） | `nc4_parser_errors()`，`BUS_STATS` 會出 `crc_err` / `bad_hdr` |
| M | **`task_manager` 宣告不存在的 `include/`** | ✅ 已修（2026-11-03） | 會令 CMake configure 直接失敗；舊 `build/` 掩蓋了它，**新 clone 第一次 build 必撞** |
| N | **tinyusb 佔 21.2 KB BSS**（`_cdcd_itf` 12,532 + `_mscd_epbuf` 8,192 + `_cdcd_epbuf` 1,024） | ⚠️ 未處理 | 清完 handler static 之後，它變成**我們這邊最大的單一項**；要再省 SRAM 應該先問「USB 一定要開嗎」 |
| O | **`build/` 跨平台共用** | ⚠️ | Windows 冇 IDF，build 只能在 WSL 做；在 Windows 開 `build/` 只會見到上次 WSL 的殘留，容易誤判「已經 build 過」 |
| P | **shell 腳本 CRLF → Linux 全掛** | ✅ 已修（2026-11-03） | `tools/{board,flash}.sh` ＋ `hosttest/{mod,hub,plan}_test/run.sh` 五行尾是 CRLF，在 WSL 跑就 `$'\r': command not found`。已全部轉 LF ＋ 加 `.gitattributes`（`*.sh` 強制 LF、`*.bat` 強制 CRLF） |
| Q | **`nc_mod_scan()` 路徑截斷** | ✅ 已修（2026-11-03） | `char path[NC_MOD_PATH_MAX]`＝80 B，但 `d_name` 最長 255 ＋ `dir` 無上限 → 截斷後會**靜靜開到另一個前綴相同的檔**。現在先驗 `snprintf` 回傳值，放唔落就跳過並計入 `bad` |
| R | **FX 狀態 JSON 會送出半截** | ✅ 已修（2026-11-03） | 每個模組 ≈250 B 的 JSON，8 個 ≈2,000 B > 舊的 `part[1,900]` → 主機收到無效 JSON。現在偵測到放唔落就改寫一個**合法**的精簡物件（`"truncated":1` ＋ `"mods":N`） |
| S | **`STATUS_GET` 完整回覆本身會爆** | ✅ 已修（2026-11-03） | 完整 status ≈2.4 KB，但 `NC4_REPLY_JSON_MAX` 只有 2,100 → **一直靜靜截斷**。已提到 2,600，並改成「砌入回覆幀」 |

### 7.2 2026-11-04 追加（記憶體／通道閘）

| # | 項目 | 狀態 | 影響 |
|---|---|---|---|
| T | **`router_from_cfg()` use-after-free** | ✅ **已修（2026-11-04）** | 名稱先抄入 `s_route_names[]` 再 `nc_cfg_close()`；`nc4_router_set()` 之後拿到的是有效字串。太長的名字直接跳過（不再截斷） |
| U | **P4 測試 task（4 條）** | ✅ 已加開關（2026-11-04） | `tcp_sink`×2 / `tcp_src`×2 = 4 × 6,144 = **24,576 B**；`CONFIG_NETCORE_TEST_TASKS` 預設 **n**。`tcp_nc4_accept` 係正式通道，不在其中 |
| V | **P4 tinyusb 仍被 link（21,748 B）** | ❌ | 不是 main.c；`nc_store.c` 仍然建 MSC LUN。要 guard 整個 MSC 段 |
| W | `NC_TASK_MAX` 20 格（`s_slots` 7,680 B） | ❌ 不可縮 | 10 個固定任務 ＋ 8 個 `.nct` = 18；縮到 12 會令 `.nct` attach 失敗（靜默） |
| X | **狀態管理器（三層：熱／冷／封存）** | 🟡 只定案未實作 | 三層、fast 區純數字、字典放 rodata＋PSRAM、`keys` 解析鏈。**不會省 RAM**（+256 B），價值在單一事實 |
| Y | WS/UDP 改 PSRAM 後的吞吐 | ⚠️ 未量 | 要跑 `tools/bench/nc4_ws_test.py`；P4 的 PSRAM 較快，S3 沒 link 這兩條 |
| Z | `ssid_pw` 明文在 FAT | ⚠️ | 上游搬 btree／NVS；我們應搬 NVS |

### 7.3 2026-11-04 晚（Phase 1）

| # | 項目 | 狀態 | 說明 |
|---|---|---|---|
| A1 | P4 測試 task 開關 | ✅ | `CONFIG_NETCORE_TEST_TASKS`（預設 n）；4 條 task＝24,576 B runtime |
| A2 | `0x1005` 截斷 | ✅ | 改用緊湊清單 `nc_task_list_json()`（name/level/affinity/started/loops）；保證合法 JSON，放不下回 `{"truncated":1}` |
| A3 | round watchdog | ✅ | 新 SYSTEM 任務 `monitor`（core0、1 s、`no_epoch`）：`epoch.rounds` 停滯 5 s → log（含任務清單）；預設**不重啟** |
| A4 | `NC_TASK_MAX` 20→12 | ❌ 取消 | 10 固定 ＋ 8 `.nct` / `.nce` = 18，20 格必要 |

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
| 搬目錄 | `app/tasks/bench` → `app/test_bench_tasks`；`app/tasks/hello` → `app/test_hello_tasks`；`app/fx` → `app/test_mod_tasks`；`sys/monitor` → `app/test_monitor_tasks` |
| 分類 | `ncm`（POC 燈效引擎）同 `monitor`（實驗 watchdog）都歸測試；release 唔出 |
| 開關 | `option(NC_ENABLE_TESTS … OFF)`：OFF = `components/tasks` 唔加入編譯（唔係 runtime 關） |
| 移除 | `CONFIG_NETCORE_TEST_TASKS`（合併入上面嘅開關） |
| main.c | 測試相關 include／宣告／註冊／`nc_mod_scan` 全部 `#ifdef NC_ENABLE_TESTS` |

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
| 研究 | [effect-language.md](../03_notes/effect-language.md)、[extension-architecture.md](../03_notes/extension-architecture.md)、[ncm-loader.md](../03_notes/ncm-loader.md) |

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

緩衝同綁定無關：三組 `keys=mem` 都係 `usb.rx 4,192` ＋ `uart.rx 1,024` ＝ 5,216 B（每條 link 一份）。

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
preset/merge 與 arena→PSRAM 見 [ports-layout.md](../03_notes/ports-layout.md) §4。
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

**soak 與 CRC 追查**：UART 在滿載混合 soak 偶發 `crc_err`。六輪對照排除了插槽（`slot_full=0`）、parser、環容量（RX 1 KB→2 KB 照樣出現）；真兇是 `ncm` 測試任務與 `uart.rx` **同優先級**（都是 3）→ 10 ms/tick 時間切片剛好壓過 1 KB 環的 11 ms。ncm 降到 prio 2 後，事件率由 ≈1/900 降到 **≈1/3,150 循環**（30 分鐘、4,460 循環、2 次；每一輪 300 幀都收齊，沒有真的掉幀）。殘留那次已排除緩衝因素，下一步是數 RX FIFO overrun。規則：**同核的租戶任務不得與 producer 同優先級**。

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
| [buffers.md §9](../03_notes/buffers.md) | **新增契約 C1–C11**（宣告／建立／交付／生命／擁有權／讀寫權／熱路徑／對外可見／`.nct` / `.nce`／上限／驗證）＋現況對照表＋「為什麼是這些條」 |
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
| 回覆幀 | plan 的 `nc4.reply0/1`（2,615 B） | ✅ 已搬（P2） |

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
其他任務（含 TinyUSB 5／ncm 2／log 2／app 1）一律 ≤ 6。規則寫進
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
| 優先級階梯 | `decode0/1 = 8`、`cdc.rx = 7`、`uart.rx = 7`、`TinyUSB 5`、`ncm/nc_log 2`、`main/hello 1` ✅ |
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
| **長跑 fn** | 自己叫 `nc_task_beat()`（`decode0/1`、`ncm` 已加） |
| 停滯判斷 | **主機**比對兩次 `STATUS keys=tasks` 的 `b` —— 板子零成本、零新任務 |
| 曝光 | `keys=tasks` 每個 loop 多一個 `b` 欄位 |

**上板實測（2 秒窗口）**：

| loop | 心跳 |
|---|---:|
| `decode0` / `decode1` | **42 / 40**（閒置 ~21 Hz，受 50 ms 安全網限制） |
| `ncm` | **200**（10 ms 週期 ＝ 100 Hz） |
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
| `usb.rx` | 任務 `nc_plan_get()` → parser 自己 `nc_buf_at()` | 宣告 `slot_bytes=region, n=1, flags=STREAM` → 管理器建，bus 用 `cfg.rx_buf` 收 |
| `uart.rx`／`tcp.rx` | 同上 | 同上 |
| `ncm.arena` | 純區域 | 宣告 `slot_bytes=512 KB, n=1` → 管理器建（`.nct` / `.nce` 之後用 `nc_buf_carve()` 由它切） |
| `probe.hub` | 純區域 | 保持（probe 自己建 hub，測試專用） |

新增 API：`nc4_parser_create_buf()`（adopt 一個已存在的 STREAM buffer）＋
`nc4_bus_cfg_t.rx_buf`。

**結果：全 repo 已經冇任何任務 call `nc_plan_get()`** —— `nc_plan` 只剩管理器
（declare／get）與開機 commit（`nc_plan_commit`／`nc_plan_json`）在用，對外等於內部實作。

**上板驗收**：

```
plan: nc4.reply0, nc4.reply1, usb.rx, uart.rx, probe.hub, ncm.arena
buf : nc4.reply0/1（P 2615 n=1）、usb.rx（S 4192 stream）、uart.rx（S 1024 stream）、
      ncm.arena（P 524288 n=1）、CDC.slots、UART.slots、t.*
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


### 7.51 2026-11-10（`nc_hw` 第一版：owner／command／snapshot；GPIO owner 上板）

**動機**：UI 同實操會同時需要硬體，但唔想每次讀寫都經中央管理器。定案係
**中央門面＋分散執行**：owner task 係唯一 writer；UI／NC4 只送 command 或讀 snapshot。

**落地**：

| 組件 | 內容 |
|---|---|
| `sys/hw` | `nc_hw` registry（32）、command queue（8）、snapshot（256 B seqlock）、claim（64） |
| `tasks/hw_gpio` | resource `"gpio"` owner；CFG／SET、10 ms 採樣、保留腳位／已 claim 腳位拒絕 |
| `main` | `ph_hwcheck`（storage 之後、hardware 之前）：claim ETH／SDIO 腳位、validate |
| 測試 | `0xF035 HW_GPIO`、`0xF036 HW_SNAP`、`0xF037 HW_INFO` |

**上板（S3）**：

- `claims`：SDIO 7／6／15／16／4／5；
- CFG pin 21 output high → `levels=2097152`；SET low → `0`；
- SET pin 7（SD clk）被 owner 拒絕；
- `hw_gpio` heartbeat 正常；`nc4_test` 9/9、hosttest 全綠。

**未做**：全部 `hardware.json` 段的 claim（等 driver 型別化）、UI／config owner、
pixel／audio owner、board／wiring／user config 拆分。


**2026-11-10 追加（config 分家第一版）**：

- `ports/<board>/wiring.json`（build-time，CMake embed）成為板級接線 base；
- `/data/config/hardware.json` 改成用戶覆蓋層；`nc_cfg_boot_ex()` 先 base 後 merge；
- `driver.json` 移到 `ports/<board>/driver.json`（build-time；runtime 尚未讀）；
- S3 以「minimal user hardware + wiring base」開機，`0xF037` 仍見 SDIO claims；
- 2026-11-10 修正：`data/config/hardware.json` 的 seed 改成**完整 wiring 預設**
  （GPIO／PWM／SPI／I2C／I80／UART／I2S／SDIO／ENC／XL9555 都在），
  用戶可直接改；未寫的欄位仍由 wiring 補。A 板已用完整檔重開，SDIO claims 不變、WiFi 自動連線仍正常。
- P4 release build 通過；S3 tests build 通過。


**2026-11-10 追加（chip profile 表）**：

- `ports/_chips/{ST7789,GT1151Q,PCM5102,APA102,PCA9685,HUSB238,UART_MOTOR}.json`
  放「同一顆晶片的常識」；
- 兩塊板的 `driver.json` 改成 `profile`＋板級差異；`tools/board.py check` 會驗證
  `profile` 是否存在；
- `driver.json` 仍在 build-time，runtime 未讀；profile merge 仍是下一刀。


### 7.52 2026-11-10（Wi-Fi 憑證管理：NVS 最多 10 組＋掃描配對）

**移植自**舊專案 `network_manager.py`：`CRED_MAX=10`、btree 持久化、move-to-end、
掃描按 RSSI、已知 SSID 優先、hidden 盲連、成功才寫入。

**落地**：

| 項目 | 內容 |
|---|---|
| NVS | namespace `nccred`，key `creds`，blob 10 組 `{ssid[33], pw[65]}` |
| API | `nc_wifi_cred_add/remove/clear/count/json` |
| 設定 | `network.json` wifi `credentials[]`；`ssid_pw` 兼容 `password_pw`／`password` |
| manager | `wifi_mgr`（core1、prio 3、stack 6144）；掃描／配對／盲連／30 s 重試 |
| 指令 | `0xF038 WIFI_CRED`（list／add／remove／clear） |

**上板（S3 B）**：add／list／remove／clear、重開機持久化 ✅。
測試憑證只用 `TESTB`／`TESTB2`／`PERSIST`。

**自動連線已驗（A 板）**：`wifi.enable=1`、NVS 一組憑證；開機後
`state=on, ssid=TestAP, ip=1, rssi=-65, cred=1, connected=1`。
修了兩個 reset：NVS namespace `wifi` → `nccred`；`wifi_ap_record_t aps[32]`
由 task stack 移到 BSS、stack 4096→6144。
**未完成**：AP fallback、`mode=ap/apsta` 未移植；B 板測試後未重新列舉。
詳見 [wifi-credentials.md](../03_notes/wifi-credentials.md)。


### 7.53 2026-11-10（PIN／PWM／UART typed config ＋ 驅動）

**照舊專案補齊三段**：

| 段 | 內容 |
|---|---|
| `PIN` | `{GPIO,label,mode,pull,initial}`；`hw_gpio` owner 開機套用、發佈 snapshot |
| `PWM` | `{GPIO,freq,resolution,duty,label}`；LEDC driver（最多 8 通道，timer 共用；**預設 12-bit、duty 0–4095**） |
| `UART` | `{id,baudrate,rxbuf,txbuf,en_settle_ms,GPIO{tx,rx,en}}`；RS485 DE 同步切換 |

**上板（A）**：

- `0xF03B UART_INFO` → `{"ready":1,"port":1,"baud":115200}`
- `0xF03A PWM_INFO` → `count=1`；`0xF039` 設 12-bit duty 0／2048／4095 成功；`0xF03C PWM_STAY` 回 dStay=2048、`0xF03D PWM_ZERO` 回 0
- `0xF036 HW_SNAP` → pin14（input pull-up）= 1
- **OTA 換韌體後設定仍然保留**：`nc_boot` 不再因 `fw_hash` 變更格式化 data；
  用戶 hardware/network config 保留，只補缺檔。

**未 typed**：SPI／I2C／I80／I2S／ENC／XL9555；`driver.json` runtime merge 未做。
詳見 [hw-pwm-uart.md](../03_notes/hw-pwm-uart.md)。


### 7.54 2026-11-10（LED Controller 規劃，不盲寫 backend）

**背景**：目前沒有 16-lane LED 硬件可測。先寫契約，不再在沒有硬件時盲寫 I80 backend。

- `nc_led` = struct＋ops（init／show／clear／set_pixel）；`dStay` 是停止／歸零的中性值。
- `led.frame`（P，38,400×2）、`led.wave`（S，28,800×2 或 38,400×2）。
- backend 順序：mock（無硬件）→ I80 1 lane → 16 lane → pixel domain。
- 50 FPS 閘門：24-bit 18 ms／55.5 FPS；GRBW 32-bit 24 ms／41.7 FPS；未拍板。
- **只建議先做 L0**（interface＋mock＋host test）；L1 之後要真硬件。

詳見 [led-controller-plan.md](../03_notes/led-controller-plan.md)。


### 7.55 2026-11-11（TCP／UDP／WS 統一在 nc4_bus；S3 WiFi 首測）

**做法**：新增 `nc4_line_net`，三條 adapter 只提供 `read()/send()`；
NC4 framing／parser／link 全部走同一個 `nc4_bus`（**PUSH，跟 UART／CDC 一樣**）。
每條線一個 producer → slots → kick decode。`NC4_MAX_BUSES` 4→8。
S3 WiFi 連上後開 TCP 6000、UDP 6001、WS 8080。

**上板（A，WiFi）**：

| 線 | RTT p50 | p95 | reply/s |
|---|---:|---:|---:|
| TCP | **8.70 ms** | 80.51 | 57.4 |
| UDP | **8.60 ms** | 66.48 | 51.3 |
| WS | **16.75 ms** | 23.40 | 56.3 |

- 已改成 PUSH producer（同 UART／CDC），p50 由 ~12 ms 降到 8.6–8.7 ms；WS 16.8 ms。
- p95 仍 66–80 ms（WiFi／排程抖動）；burst 未重測，仍不可當高吞吐。
- 流量（window=2、20 幀、STATUS sys）：TCP 14.13 KB/s、UDP 28.36 KB/s、WS 11.17 KB/s（NC4 payload）。
- **緩衝管理器有實際用到**：`mem.buf` 見 TCP.slots／TCP.rx／UDP／WS；network 三條線約 57 KB internal，
  `h_int` 只剩約 23 KB；`TCP.slots` abort 182,061（no-client spin）。
  下一步要搬 PSRAM／縮 WS slot／修 abort，否則不可加負載。
- 下一步：P4 ETH 接同一層；buffer 搬 PSRAM；producer／slot 調校。

工具：`tools/net_bench.py`。詳見 [net-lines.md](../03_notes/net-lines.md)。


### 7.56 2026-11-11（NETBENCH 真實統計；network 測試暴露 loss）

- 新增 `0xF040 NETBENCH`：op3 一幀只記帳不回覆；op2 result 回
  `frames／bytes／seq_max／expected／lost／missing／ooo／fps／kbps`。
- **完整 batching 測試（內部 seq 驗證）**：
  有效：TCP 1024 **2.17 MB/s**、WS 4096 **7.26 MB/s**、UDP 512 paced **147 KB/s**。
  不通過：TCP 4096 39/50、WS 512 110/100、UDP 1024 invalid timer、UDP 無 pacing lost 18。
- 50 KB/s 是「一幀一次 send」的 overhead；batching 後到 MB/s 級，但矩陣未全過。
- PSRAM：network slots PSRAM，`h_int` 10.5 KB → 68 KB；lwIP 窗口／mailbox 已放大；
  WiFi `WIFI_PS_NONE`；RSSI -47 dBm，天線不是主因。
- 路由 network↔CDC 尚未完成可重現測試；repo routes 已還原。
- 工具：`tools/net_oneway.py`。


### 7.57 2026-11-11（CDC 式零複製 producer；路由測試卡住）

- TCP/UDP 改 `select → push_begin → recv 直接入 slot`，同 CDC 一樣；WS 用 queue peek＋slot。
- 有效速度：TCP 1024 **1.95 MB/s**、WS 1024 **6.57 MB/s**、UDP 512 paced **159 KB/s**。
- `h_int` 約 68 KB；network slots PSRAM；lwIP/window/socket/WiFi PS 已調。
- **路由 network↔CDC 測試失敗**：同時開 TCP/UDP/WS→CDC 與 CDC→TCP 後，
  A 板 NC4 over USB 完全冇回覆（CDC log 仍出）。已還原 A 的 routes，但未 reboot；
  需要 physical reset，之後一次只測一個方向。**路由器尚未 PASS。**


### 7.58 2026-11-11（網絡 OTA＋網頁 v0；未上板）

- `POST /ota`（raw `.bin`）→ `esp_ota_begin/write/end/set_boot` → reboot。
- `GET /` 最小頁：上傳 `.bin`、看 `/config.json` 前 4000 B。
- 同 WS／`/config.json` 共用 8080 httpd（S3）。
- 安全：**冇 auth／TLS／簽章，只可信 LAN**。
- A 板舊 image 的 8080 仍回 404（server 活著），NC4 無回應；需要 USB 重燒最新 image／physical reset 才可測。
- 完整 Web UI 是獨立任務；v0 只做「開得到、可以 OTA」。


### 7.59 2026-11-11（路由器 PASS；網絡 OTA 0.1.1→0.1.2）

- 路由：network→CDC（TCP/UDP/WS）＋ CDC→network（TCP/UDP/WS）全部 PASS，
  一次只開一個方向；測試 routes 已還原。
- 網絡 OTA：`POST /ota` raw `.bin`，1.18 MB 約 **14.8 s**，0.1.1→0.1.2，
  app hash 改變、reboot 正常；`GET /` 200。
- reboot delay 800 ms → **3 s**，避免 curl 未收到 200 就斷。
- 注意：HTTP 係 single-threaded；同時開雙向 routing 之前要再壓測。


### 7.60 2026-11-11（雙向 stress；緩衝結構；P4 確認）

- TCP↔CDC 雙向各 100 幀：**100/100、100/100**，0.567 s，板子正常。
- 全線雙向（TCP/UDP/WS↔CDC，N=50）：
  net→CDC **134/150**（有掉）；CDC→TCP/UDP/WS **50/50 each**；
  另一次跑到一半 **IWDT reset（rst=5）**。**未 PASS**，要修 CDC TX／forwarding 或降載。
- routes 已還原；A 板 0.1.2 正常。
- 緩衝：`mem.buf` 14 個、total 643 KB；network slots PSRAM，parser rx 仍 SRAM；
  `h_int` 約 80 KB。詳見 [buffer-topology.md](../03_notes/buffer-topology.md)。
- 新 USB `/dev/cu.usbmodem1401` 用 esptool 確認是 **ESP32-P4 rev1.0**（MAC <P4-MAC>）；
  **P4 冇內建 Wi-Fi**，所以「P4 Wi-Fi」不能測。若係 P4+C6 companion 或 C6 板，
  要提供型號／接法；repo 目前只有 P4 ETH 和 S3 WiFi，沒有 C6 port。


### 7.61 2026-11-11（8×8 WS2812 矩陣：I80 波形 ＋ 分區抖動，測試任務）

- 新板 `ports/S3/ESP32-S3-Matrix`（4 MB、無 PSRAM、240 MHz、GPIO14 = LED_DIN）＋
  4 MB 分割表（app 1.5 MB × 2 ＋ FAT 896 KB）。
- 新任務 `test_rgb_led_matrix_tasks`：I80（4 MHz、5 cycles/bit、D0 = GPIO14）
  送 64 顆 × 24 bit ＝ 7,936 B／幀（1.98 ms）→ 預設 **450 fps**，硬上限 460。
- 抖動：效果算出 8.8 定點亮度 → 每幀誤差回饋送 floor／ceil；8 個邏輯區
  （每區一橫行）各自 rate 1..2、phase 錯開。效果：OFF／ZONE1／BREATH／WAVE／
  SOLID／RAMP。
- 指令 `0xF060 MATRIX`（op：stats／effect／dither／color／gain／fps／zone／zmask）。
- **已上板（2026-11-11 21:00）**：USB 首次 `--full` 燒錄、之後 **OTA over NC4**
  （734 KB／4.1 s）。開機正常、CDC → decode0、`rgbmatrix` 跑 core1。
- 時序修正：4 MHz／5 cycles 實測一幀 2.44 ms（只 408 fps，**每幀 late**）——
  esp_lcd 的 I80 每筆交易有 0.24–0.45 ms 固定開銷。改成 **8.889 MHz／8 cycles**
  （12,544 B／幀）＋ **3 格 DMA buffer** → **450.0 fps、late=0、tx_err=0**，全速 606 fps。
- 配置持久化踩雷：舊 firmware 的 NVS `provisioned=1` 令第一次 boot 唔重建 FAT，
  `tasks.json` 冇 seed → `rgbmatrix` 被補成 `[0,0]`（loop 唔開）。用
  `0x1103 STATUS_SET {"System":{"fs_format":1}}` ＋ reboot 重建 FAT 之後正常。
  之後更新一律 `tools/ota.py`。
- 順手修：`tools/board.py list` 讀 `psram: null` 會 crash；C3 port 的
  board.json 與 sdkconfig 對不上（flash 40M、console UART0）→ `board.py check` 0 錯誤。
- 詳見 [rgb-matrix-dither.md](../03_notes/rgb-matrix-dither.md)。


### 7.62 2026-11-14（RING 的 SPSC 修正：consumer 唔再 reset producer）

- 覆核 §7.61 之後的 RING 提交時發現：`nc_buf_skip()` 在消費清空時把 `st_in`／`st_out`
  一齊歸零，違反「producer 只寫 in 側、consumer 只寫 out 側」的 SPSC 契約。
  producer 在 `memcpy` 與發佈 `st_in` 之間被歸零，發布的位置就會指錯。
- Host 兩 thread 重現（64 B RING、每次 1 B）：3,000,001 B 錯位 **2,226 次**；
  修正後三次都 0。
- 修法：加 `st_rd`／`st_wr` 物理位置各自維護（`nc_buf_t` +8 B），熱路徑少一次 `%`；
  `nc_buf_handover()` 同步歸零。
- 驗證：`buf_test` 新增單 thread 斷言＋`RING SPSC` 兩 thread 測試（舊實作 FAIL）；
  `run_all.sh` **10/10**；
  S3-Matrix（`--tests`）、P4-C6、C3 建置通過。**板上未驗**（今日冇接板）。
  現時 parser 係同 task 寫入＋解析，所以板上未見症狀；B1 改成跨 task 才會踩到。
- 詳見 [buffers.md §12.6](../03_notes/buffers.md)。

### 7.63 2026-11-14（上板驗證 RING 修正；0xF032 支援無 PSRAM 板）

- 兩塊板都換成含 §7.62 修正的 `--tests` 映像（S3 `08ca63ad`、P4 `359bd9b8`）。
- `0xF02F PARSER_SELFTEST`：S3／P4 都 `parser_ring:true`、2,000 幀全到
  （S3 ring 157.2 ms vs 線性 165.0 ms；P4 76.7 vs 75.2 ms）。
- `0xF032 BUF_TEST`：P4 **11/11**；S3-Matrix（4 MB、無 PSRAM）修正前因
  `t.psram` 建不到，整個測試被外層 `if` 跳過、誤報 11 項失敗。改成
  「SRAM 照跑、PSRAM 三項（s2p／p2p／p2s）標 skipped」之後，S3 回
  `fail:0, total:8, psram:0`。
- P4 第一次經 USJ OTA 1.06 MB：**175 s（~6 KB/s）**；S3 CDC 661 KB：6.8 s
  （~97 KB/s）。根因量到了：主機 raw write 4 KB 到 USJ = **640 ms**（6.4 KB/s）
  ＝ 64 B USB packet × 10 ms FreeRTOS tick；不是 flash／OTA handler。
  與 [usb-config §0.1](../03_notes/usb-config.md) 的「USJ 4 KB 上傳 0.94 s」
  同一個數量級，是拉取式輪詢的既有上限。
- P4 下載模式（ROM `rst:0x17`、`boot:0x317 DOWNLOAD`）用 esptool 燒 app 至
  `0x20000`，讀回 SHA-256 與 artifact 一致；燒完要**放開 BOOT** 才會離開
  download mode 並 boot 新 app。

### 7.64 2026-11-14（mutex 成本、atomic A/B、USJ 大回覆）

- 新測試指令 `0xF042 MUTEX_BENCH`：無競爭 mutex `take+give` 一對
  **S3 4,051 ns／P4 3,956–3,985 ns**（約 2 µs/op）。
  decoder 鎖每個有 handler 的幀一次（1 KB 幀 parse ~78 µs → ~5%）；
  forward 鎖每轉寄幀一次（＋一次 copy）。
- `NC_BUF_RING_ATOMIC`（0=plain、1=C11 atomic）A/B：
  S3 `ring_real` 261.5→259.8 MB/s（**−0.6%**）、`0xF02F` 量不到差；
  P4 的 ring／linear 同步快 5%（linear 唔經 RING），屬 build／環境變異，
  無法從 P4 分辨 macro 效果。**default 保持 plain**，見
  [buffers.md §12.7–12.8](../03_notes/buffers.md)。
- P4 USJ 的 `0xF02B`（~2–3 KB 回覆）會 timeout：`usb_jtag_send` 100 ms deadline
  截斷；`0xF02F`／`0xF032` 等小回覆正常。要量 P4 大結果，先處理 TX deadline
  或分塊回覆。

### 7.65 2026-11-16（ports 兩層：hardware／project；firmware 預設 codegen）

- `ports/` 拆成 `hardware/<chip>/<board>/`（硬體事實：board.json／wiring.json／driver.json／
  sdkconfig／partitions）與 `project/<chip>/<board>/`（項目：project.json／data/config）。
  同一塊硬體可以配多個 project；build 用 `-DPORT` ＋ `-DPROJECT`。
- 新 `tools/ncportgen.py`：build 時把 `wiring.json` 編成 `nc_port.c`（static const
  `nc_hardware_cfg_t`）＋ `preset_hardware.json`。firmware 不再 embed／runtime 解析 wiring JSON；
  `nc_cfg_defaults_hardware()` 直接回傳 generated 表（weak symbol，host test 退回 `k_hw`）。
- 字典→數值：`phy_type` → enum；link 的 `iface`／`service`／`mem` 由 char[8]/[6] → 各 1 byte
  （每個 link 省 22 B），比較不再 `strcmp`。JSON 對外仍然寫字串。
- 能力碎片分層：`eth-rmii`／`wifi-hosted-c6` 留在 hardware；`nc4-ws`／`nc4-udp` 移到 project。
  `board.py sync` 分別寫兩層的 `sdkconfig.defaults`。
- P4-ETH 補 32 MB 分割表（`hardware/P4/ESP32-P4-ETH/partitions.csv`）：app slot offset 不變，
  `data` 由 7 MB 增到 23 MB。套用要 `--full`；OTA **不能**改分割表。
- 驗證：`board.py check` 0 error；`ncportgen` 五塊板都產生成功；host 端用 Zig cc
  編譯 `cfgstruct_test` **133/133 通過**、generated `nc_port.c` 可編譯並跑出正確預設。
- **完整 IDF 6.1 build 通過**（WSL Ubuntu + `/home/user/git/s31-verify/esp-idf` v6.1）：
  P4-ETH `0xff040`（1020 KB，app 分區剩 75%）、S3-N16R8 `0x118e90`（1.1 MB，剩 73%）。
  P4 分割表實際解出 `data 0x820000 23M`，與 `partitions.csv` 一致。
- build 期間修了三個 IDF 6.1 相容問題：
  1. `component_get_requirements` 用 `cmake -P` 重新 include `main/CMakeLists.txt`，
     不能在裡面跑 `add_custom_command`／`add_dependencies`／`target_*`；
     已用 `CMAKE_SCRIPT_MODE_FILE` 分開 script mode 與正常 configure。
  2. 那個 context 也看不到 `-DPORT`／`-DPROJECT` cache 變數；`make.py` 改為在命令前
     加 `NC_PORT`／`NC_PROJECT` 環境變數，top-level CMake 亦 `set(ENV{...})` 傳下去。
  3. `NC_MEM_PSRAM` 與 `nc_mem.h` 的 heap class 撞名；config 的 enum 全部加 `NC_CFG_`
     prefix（`NC_CFG_MEM_*`／`NC_CFG_IFACE_*`／`NC_CFG_SVC_*`／`NC_CFG_PHY_*`）。
- 清理：刪除 `docs/03_notes/hardware-config.md`（preset 表設計已被 generated defaults 取代）、
  舊 `ports/<chip>/` 空目錄、`main.c.bak`、0-byte 的 `sdkconfig.p4c6`／`_p4_usb.log`。

### 7.66 2026-11-16（建置產物集中：`artifacts/build/<chip>_<board>/`）

- `firmware/build`、`build_c3`、`build_p4`、`build_p4c6`、`build_s3` 全部移除（共 ~1 GB）。
  新版 `make.py` 用 `-B artifacts/build/<chip>_<board>` 和
  `-DSDKCONFIG=.../sdkconfig`，每塊板有自己的 build dir 與 sdkconfig，不再互相洗 target。
- `.bin` 集中：`artifacts/<board>.bin`（最新 app）＋ `artifacts/<board>/`
  （`esp_net_core.bin`、`bootloader.bin`、`partition-table.bin`、`ota_data_initial.bin`、
  `flash_args`、`manifest.json` sha256）。中間檔只留在 `artifacts/build/`。
- `make.py clean` 清 `artifacts/build/`＋舊 `firmware/build*`／`sdkconfig*`；
  `--all` 連 `managed_components`／`dependencies.lock`。
- `make_config.json` 新增 `build.build_dir`（預設 `<output_dir>/build`，可指到專案外）。
- 驗證（WSL IDF 6.1）：P4-ETH `0xff040`、S3-N16R8 `0x118e90`；兩個 build dir 並存，
  S3 build 沒有再出現「target 由 esp32p4 改成 esp32s3」的清理步驟。

### 7.67 2026-11-16（config 分層：link schema 正規化＋最小預設）

- `system.json` 的 link 改成 canonical：`kind` 只寫 transport、`iface` 分開、
  `port`／`rx` 在 link 頂層；`cfg.port`／`cfg.rx` 仍然接受但 `board.py check` 會提醒。
  冇 `name` 時由 `iface+kind+index` 生成（`eth_tcp0` / `wifi_udp1`）。
- 五塊板的 project seed 已全部正規化；舊 composite kind 與 `cfg` 只留兼容路徑。
- `main.c` 補第 ① 層最小預設：冇板級 `system`／`network`／`tasks` 時，
  `seed_presets()` 會寫入 `{"version":1,...}` 骨架，`config/` 四個檔一定存在。
- 驗證：`cfgstruct_test` **140/140**；P4-ETH `0xff2e0`、S3-N16R8 `0x1190e0` 建置通過。
- 下一步缺口：`network.json` 仍然是 `nc_wifi.c`／`nc_net_lines.c`／`nc_net_raw.c`／`main.c`
  各自讀，沒有單一 typed `nc_cfg_network()`；`driver.json` 仍然 build-time。

### 7.68 2026-11-16（ports 命名公約；P4-C6 正名 FireBeetle2；新增 S3-Mini-Nano）

- **命名公約（新）**：`ports/README.md` §10。硬體板目錄名／`name`／project 名一律
  **`<CHIP>-<Identity>`**（晶片在最前、ASCII 英數與 `-`、每個詞大寫開頭）；
  品牌／型號／板本不放名字，寫 `board.json` 的 `brand`／`model`／`rev`；
  project 名 = 目錄名（多 project → `<硬體板>-<用途>`）。
  **改名必須留 `aliases`**：舊名照樣 `make.py build` 得到，alias 只影響解析，
  不改 `-DPORT`／`-DPROJECT`、不影響 `artifacts/<board>.bin`。
- `tools/board.py`：`check` 新增代號格式、晶片前綴、`name`↔目錄一致、`aliases` 格式與
  **alias 撞名** 檢查；`new` 支援 `--brand/--model/--rev/--alias`，並修正 `--wifi`
  對「晶片原生 radio」（S3／C3…）誤寫 `wifi-hosted-c6` 碎片的問題。
- **改名**：`ESP32-P4-C6` → **`ESP32-P4-FireBeetle2`**（DFRobot FireBeetle 2
  ESP32-P4／SKU **DFR1172**，ESP32-P4R32）。hardware 與 project 目錄、`name`、
  `project.json.hardware` 全部同步；`aliases` 保留
  `ESP32-P4-C6`／`P4-C6`／`ESP32-P4-FireBeetle`／`FireBeetle2`。
  實測 `board.py resolve ESP32-P4-C6`、`resolve P4-C6`、`resolve FireBeetle2`
  都命中同一塊板。
- **同一塊板的事實修正**（rename 時一併做）：
  1. `features.usb` `"otg-fs"` → **`"otg-hs"`**。2026-11-11 實測另一邊 USB-C
     枚舉 `303A:4003`，走 UTMI HS（pin 49/50）；RST 旁那個孔是 USB-Serial-JTAG
     （FS_PHY1 GPIO24/25，`303A:1001`）。兩個孔互換不了。
  2. **板子冇乙太網**（DFR1172 官方介面冇 ETH）：`features.eth` `"rmii-ip101"` →
     `"emac-only"`；`wiring.json` 的 `ETH.enable` 1→0、腳位 -1。
     `eth-rmii` profile 保留只為令 P4 的 EMAC 程式碼保持可編（`CONFIG_SOC_EMAC_SUPPORTED`），
     runtime 唔會再嘗試起 ETH。
  3. `README.md` 由 `board.py new` 模板改成真實板卡說明（兩個 USB-C 分工、hosted C6、
     TF／SDIO 腳位未從 schematic 核對）。
- **新增** `ESP32-S3-Mini-Nano`（通用白牌「S3-MINI Nano」／模組 S3-N16R8）：
  16 MB flash @80 MHz DIO、8 MB octal PSRAM @80 MHz、CPU 240 MHz、Wi-Fi。
  `console` 暫定 `usb-serial-jtag`；`wiring.json` 全部 `enable=0`（腳位未實測）。
  project seed：`system.json`（`usb_jtag` ＋ `UART` 兩條 link，decode0／decode1）、
  `network.json`（`wifi.enable=0`）。`aliases`：`S3-Mini-Nano`。
  README 寫明上板要先確認兩件事：USB 係原生（USJ）定 CH340 橋（要改 `console=uart0`）、
  GPIO 排針腳位。
- **驗證**（本機 macOS／IDF **6.0.3**，非平時的 WSL 6.1）：
  - `board.py list`：**7 塊硬體板 × 7 個 project**；
    `board.py check` **0 error**（4 個「冇 driver.json」warning，全部 USB-only 板）。
  - `ncportgen` 7 塊板全部產生成功：FireBeetle2 `ETH present=true enable=false`、
    S3-Mini-Nano 冇任何已啟用匯流排。
  - build 通過：`ESP32-S3-Mini-Nano` **`0x114f90`（1.1 MB，app 73% free）**、
    `ESP32-P4-FireBeetle2` **`0xfd5f0`（1013.5 KB，75% free，16 MB 分割表）**。
  - host tests：10/11 OK；`cfgstruct_test` 在 macOS/clang 因 weak symbol
    `nc_port_hw_defaults` 連結失敗 —— **pre-existing**（本次冇改 `sys/config`；
    `portgen_test` 反而 OK）。Linux/WSL 上應照舊全綠。
- **未做**：S3-Mini-Nano 上板（USB 型態／腳位／console 要實測才定案）；
  FireBeetle2 只改名＋修正事實，冇重燒（今日未確認接住的是哪塊板）。

### 7.69 2026-11-16（修正：project 層只放 runtime 設定；能力碎片回 hardware；清舊殘渣）

- **設計修正**：`project`（`ports/project/<chip>/<board>/`）**只放 runtime 設定**
  （`project.json` ＋ `data/config/*.json`），**冇 `sdkconfig.defaults`、冇 `profiles`**。
  理由：同一塊硬體的 build 只有一個（binary 是 per chip/board）；project 是部署／用戶設定，
  換部署不應改變 build 能力。之前把 `nc4-ws`／`nc4-udp` 放 project，等於同一塊板
  每個 project 一個 binary，與「硬體板 = 一份描述」矛盾。
- 改動：
  - 刪 `ports/project/P4/ESP32-P4-ETH/sdkconfig.defaults`。
  - `nc4-ws`／`nc4-udp` 由 `project.json.profiles` → **`hardware/P4/ESP32-P4-ETH/board.json`
    的 `profiles`**（連同原有 `eth-rmii`，共 3 個）；`board.py sync` 後 hardware
    `sdkconfig.defaults` 出現 `eth-rmii`／`nc4-ws`／`nc4-udp` 三個 marker 區塊。
  - 7 個 `project.json` 全部移除 `profiles`；`board.py check` 見到 project 有 `profiles`
    或 `sdkconfig.defaults` → **error**，`cmd_new --project` 亦唔再寫 `profiles`。
  - `firmware/CMakeLists.txt` 唔再 append project 的 sdkconfig（若見到只出 WARNING）；
    `tools/ncbuild/doctor.py` 的 defaults mtime 唔再計 project；
    `make.py config` 只顯示 hardware profiles。
  - 精確語意：project 不影響 Kconfig／能力；但其 `data/config` 會作為 `F.preset`
    **嵌進 image**，所以換 project 預置內容仍然要重 build。
- **舊殘渣清理**：
  - `firmware/build/`（**318 MB**；舊版 build，`project_description.json` 仍指向已刪的
    `ports/P4/ESP32-P4-C6`）、`firmware/sdkconfig`（112 KB）、`firmware/sdkconfig.old`（92 KB）
    → 全部刪除（新版 make.py 只用 `artifacts/build/<chip>_<board>/sdkconfig`）。
  - 舊 `ports/P4/`、`ports/S3/` 空目錄（只剩 `.DS_Store`）→ 刪除。
  - 全 repo `.DS_Store` → 刪除。
  - `artifacts/` 舊板名 `ESP32-P4-C6*`（14 個 bin/json）→ 搬到
    `artifacts/legacy/ESP32-P4-C6/`（保留可回滾，不再混在 `artifacts/` 頂層）。
- **驗證**：
  - `board.py sync` 7 塊板；`board.py check` **0 error**（4 個 USB-only 板冇 driver.json 的 warning）。
  - project 層 scan：`find ports/project -name 'sdkconfig*' -o -name '*.defaults'` → 空。
  - `python3 make.py build ESP32-P4-ETH` ✅ `0xfacd0`（1003.2 KB，app 分區 76% free）；
    IDF 6.0.3／macOS。build 後 hardware sdkconfig 三個 profile 區塊齊全。
  - `make.py config ESP32-P4-ETH` 只列 hardware profiles
    `eth-rmii, nc4-ws, nc4-udp`（project 一行唔再顯示 profiles）。
- 未做：FireBeetle2／S3-Mini-Nano 上板；`network.json` typed 層仍是缺口。

### 7.70 2026-11-16（建置可選 project：`--project`／`--no-project`／default；修 CMake preset 從未嵌入）

- **建置選擇（快捷版）**：
  - `python3 make.py build ESP32-P4-ETH` → 硬體 ＋ **default project**
  - `python3 make.py build ESP32-P4-ETH --project Demo` → 指定 project
    （短名可、不分大小寫；`demo` 會 suffix-match `ESP32-P4-ETH-Demo`）
  - `python3 make.py build ESP32-P4-ETH --no-project` → **純硬體**
  - **default 判定**：`project.json` 的 `"default": true`；否則同名 project
    （`ports/project/<chip>/<board>/`）就係慣例 default；每塊板最多一個；
    兩者都冇 → 純硬體（冇 project 預置）。
  - 7 個現有 project 已全部明寫 `"default": true`。
  - 非 default 的 build 用獨立 build dir／artifacts 名：
    `<硬體板>__<project 目錄>`；`--no-project` 而塊板有 default 時叫 `<硬體板>__bare`，
    唔會蓋走 default 的 image。
  - 建 project：`board.py new --project Demo --hardware ESP32-P4-ETH`
    → 自動命名 `ESP32-P4-ETH-Demo`；加 `--default` 就設為該板 default
    （已有 default 會拒絕並指出係邊個）。
  - 檢視：`board.py projects`／`make.py projects`（default 會標）、`board.py list`
    （default ＋ `(+N)`）、`make.py config <板> --project …` 會印輸出名。
- **修到一個大 bug：project 的 `data/config` 由 ports 兩層重構後從未嵌入 firmware。**
  - 原因：top-level `firmware/CMakeLists.txt` 用普通變數 `PROJECT_DIR` 指向
    `ports/project/...`，但 CMake 的 `project()` 會把 `PROJECT_DIR` 設成
    「本 project 的 source dir」= `firmware/`。component CMakeLists（`main/CMakeLists.txt`）
    見到嘅 `PROJECT_DIR` 已經係 `firmware`，所以
    `EXISTS "${PROJECT_DIR}/data/config/system.json"` 永遠 false →
    `NETCORE_HAS_PRESET_*=0`，seed 永遠只用 `main.c` 的內建最小骨架。
    症狀：無論揀邊個 project，燒上去的 `/data/config` 都係同一份 —— project 形同虛設。
  - 修法：改名 **`NC_PROJECT_DIR`**（top-level ＋ `main/CMakeLists.txt`），避開 CMake
    內建變數；加註解提醒。
- 連帶修 `-Werror=unused-const-variable`：preset 齊全時 `k_min_system`／`k_min_network`／
  `k_min_tasks` 同 `seed_text()` 變成 unused → 用 `#if !NETCORE_HAS_PRESET_*` 包住
  （純硬體 build 仍然用得著骨架）。
- **驗證**（IDF 6.0.3／macOS，P4-ETH）：

  | build | image | 實際嵌入的 preset（`nm` 對 ELF） |
  |---|---:|---|
  | default | `0xfcff0`（1012.0 KB，**+8,992 B**） | hardware＋system＋network＋tasks |
  | `--project Demo` | `0xfad60`（1003.3 KB） | hardware＋system＋network |
  | `--no-project` | `0xfacd0`（1003.2 KB） | hardware only |

  - 三個 build dir／artifacts 名並存，互不覆蓋；
    `ESP32-P4-ETH.bin`、`ESP32-P4-ETH__ESP32-P4-ETH-Demo.bin`、`ESP32-P4-ETH__bare.bin`。
  - `board.py check` 0 error（4 個 USB-only 板冇 driver.json 的 warning）。
  - 測試用 Demo project 與其 build／artifacts 已刪除（`board.py new --project Demo ...`
    一條指令即可重建）。
- 文件：`ports/README.md` §3／§3.1、`docs/03_notes/ports-layout.md` §3、`README.md`
  快速開始、`make.py --help`。

### 7.71 2026-11-16（回顧啟動程序；更新 boot／core-timeline／architecture 三張圖）

- **`boot.svg`**（[tools/boot_diagram.py](../../tools/boot_diagram.py)）按現行程式重寫，並補回
  最近兩個改動的影響：
  - **B02 storage+cfg**：掛 FAT／WL → boot record（首次／`fs_format` 才格式化）→
    **seed 出廠 preset（build 揀的 default／--project，嵌進 firmware）** →
    讀 config（system／hardware／tasks；network 仍由各模塊自己開檔）→ cID／SD。
  - B03：核數收斂 ＋ USB 人格由「晶片能力＋links[]」定；B04 claim 範圍寫明
    SDIO／ETH／PIN／PWM／UART；B05 改「PWM 安裝、ETH `enable=1` 才起、
    `system.routes` 定線→組」（FireBeetle 的 ETH.enable=0 是實例）；
    B06 寫明「有線到 core1 才建第二個 decoder」；B07 net lines 宣告＋Wi-Fi 兩道閘；
    B08 測試映像掃 `.nct` / `.nce`＋帳本；B10 改為 net lines start＋USB／UART bus。
  - **新增 Panel C「B02 的 seed：三個來源、一個方向」**：
    `F.default`（`nc_port.c`）→ `F.preset`（build 揀的 project）→ `F.live`
    （`/data/config`；只缺檔才寫，存在就一個 byte 都唔改）。
  - Panel A 更正：ctx = **6 個決定欄位 ＋ ready 能力位帳**（之前寫「六個欄位」）。
  - Panel B 更正位元帳：只到 B02 = **0x0007**、過 B03 = **0x001f**
    （舊圖寫 0x0017，錯）。
  - 表頭「① 階段」縮短，避免同「② 做什麼」重疊。
- **`core-timeline.svg`**（[tools/core_timeline.py](../../tools/core_timeline.py)）：
  B02 描述改為「掛 FAT／WL、boot record、seed 出廠 preset、config（system／hardware／tasks）」；
  ctx 註腳改「cores／usb_*／usb_core＋ready 帳」。
- **`architecture.svg`**（[tools/arch_diagram.py](../../tools/arch_diagram.py)）：
  Panel 05 的 storage+cfg 卡改「掛 FAT；seed→nc_port 預設」；
  footer 配置鏈改「`F.wiring`（build 時編成 `nc_port.c`）→ `F.preset`（build 揀的 project）
  → `F.live`（`/data/config`）→ NVS」。
- 連帶修正：[boot.h](../../firmware/main/boot.h) 的 ctx 欄位數與 `NC_CAP_CONFIG`
  註解（唔再寫「network 已載入」）；[naming.md](../03_notes/naming.md) 的 B02／B05 對照。
- **驗證**：三個 generator 重跑（boot 1680×1404、core-timeline 1720×1658、
  architecture 1720×1510）；用 headless Chrome 逐張 render 檢查文字冇重疊／爆格。
- **未改**：core-timeline 的 ms 數字仍是 2026-11-08 舊 build 的量測值。
  project preset 真正嵌入之後，B02 要多寫幾 KB seed（成本可能微升），要重新量才更新。

### 7.72 2026-11-16（網絡線路圖重畫：service claim＋HTTP static 邊界；修正舊模型）

- **問題**：`network-lines.svg` 同 7.71 更新過的圖仍然用舊網絡模型 ——
  HTTP 寫 `/config.json`、`/ota`、`POST .bin`；architecture 仲寫「網絡 OTA」；
  boot／core-timeline 的 B10 仲寫「net／WS／UDP」舊路徑。另外 `net-lines.md`
  寫「冇 iface、冇 service」，同最新 canonical（kind＋iface＋index）相反。
  使用者指出「圖片唔係最新設定」——正確。
- **先對照現行 code**（2026-11-16 HEAD）：
  - `nc_net_lines.c` 的 `service_claim()`：`kind=http` 或 `tcp:80` → HTTP；
    `kind=ws` 或 `tcp:8080` → WS；其他 net kind → NC4。
  - `nc4_web.c` 只剩 static GET/HEAD；`nc4_ota_http.c`／`nc4_http.c` 已刪；
    HTTP 冇任何寫入 endpoint。唯一 OTA＝NC4 `0x22xx`。
  - WS 係 line task 自己 handshake／binary frame → `nc4_bus` push slot → decoder。
  - `nc4.routes[].out` → line core；HTTP 固定 core 0。
  - `iface` 仍只係 metadata（bind `INADDR_ANY`）；legacy 路徑仍在；
    `rawbench` 係測試 endpoint；上限 `NC_CFG_MAX_LINKS`／`NET_MAX_LINES`／
    `NC4_MAX_BUSES` = 8。
- **`network-lines.svg` 重寫**（[tools/net_arch_diagram.py](../../tools/net_arch_diagram.py)，1800×1340）：
  - 01 canonical config（`kind:"tcp", iface:"wifi", index:1, port:6000, rx…` →
    `wifi_tcp1`）＋ managed line 規則。
  - 02 line 卡：TCP／UDP（NC4）、WS（自寫 handshake，NO HTTPD）、HTTP（STATIC ONLY）。
  - 03 service claim 表：`tcp:80`→HTTP、`tcp:8080`／`kind=ws`→WS、其他→NC4；
    config 的 `service` 只係 legacy／提示。
  - 04A NC4 path（socket／frame → line slots → nc4_bus → decoder → action）；
    04B web path（httpd → static handlers；NO NC4、冇 `/config.json`、冇 `/ota`）。
  - 05 boot order（B07 line buffer 先、B10 `nc_net_lines_start`）。
  - 06 限制／legacy（iface 未 bind、上限、舊路徑、rawbench）；
    07 驗證數字（P4-ETH／S3 WiFi／P4-C6，2026-11-16）。
- **其他圖／文件同步**：
  - `architecture.svg`：L2 改「bus_net：TCP／UDP／WS；HTTP 係獨立 static service」；
    動態路徑句改「NC4 的線…同一個 parser；HTTP 係獨立 static service，唔經 decoder」；
    PUSH slots 改「格數／大小由 `links[].rx` 決定」。
  - `boot.svg` B10 改「net lines start：NC4／WS service 各自 listener；HTTP 獨立 static」。
  - `core-timeline.svg` B10 改「`nc_net_lines_start()`：listener／httpd（service claim）」，
    移除舊 `nc4_udp` 描述。
  - [net-lines.md](../03_notes/net-lines.md) 重寫：canonical schema、service claim、
    HTTP 邊界、限制／legacy。 [line-identity.md](../03_notes/line-identity.md)
    加 banner：該頁「複合 kind、無 iface／service」係中間設計，已被取代。
- **驗證**：四個 generator 重跑；XML parse；headless Chrome 逐張 render 檢查，
  修正 03 service claim note 壓住最後一列、architecture 動態句子與 PUSH 卡文字。
  預覽存 `artifacts/previews/`。
- **未改**：legacy 路徑仍在 code（network-architecture-audit §2）；要 compile out 係另一步。
  `iface` 真正 bind／同 port ETH＋WiFi 分離仍未做。

### 7.73 2026-11-16（啟動階段圖詳細化：boot.svg 重畫＋新增 boot-cores.svg）

- **問題**：boot.svg 只有 11 行抽象摘要，冇講每階段實際做什麼、決定什麼、什麼條件下
  某個通道／任務才會啟動、又喺邊個核跑。使用者要求更仔細。
- **boot.svg 重畫**（[tools/boot_diagram.py](../../tools/boot_diagram.py)，1960×1702）：
  每階段五欄：① 階段（B01…）② **做什麼（實際順序）** ③ **決定／分支（◇D1–D12）**
  ④ **產出／去哪裡** ⑤ **失敗／跳過**；底部再列階段語義（needs→SKIP、fn→FAIL、
  optional→WARN）、10 個能力位、ctx 欄位、四條硬性次序。
- **新增 boot-cores.svg**（[tools/boot_cores_diagram.py](../../tools/boot_cores_diagram.py)，1960×1858）：
  - **D. 決策表（12 項）**：D1 boot record（格式化？）／D2 config 缺壞／D3 核數／
    D4 USB 人格／D5 ETH／D6 routes→核／D7 managed vs legacy／D8 每條 line 建唔建／
    D9 任務 level／affinity／D10 WiFi／D11 .nct／D12 rawbench；
    每項寫「條件（輸入）→ 決定（結果）→ 影響／落點」。
  - **C. 核落點**：6 條規則（app_main 固定 core0；line core＝routes 第一個 decodeN；
    producer 必須同 bus 同核；SYSTEM／tenant 任務核；WiFi＝core1 clamp；測試／ncm）
    ＋ core0／core1 兩條 lane，逐個 object 寫明「什麼條件下才會出現在這個核」。
  - **S. 唔啟動矩陣（9 項）**：ETH／TCP、新 net line、legacy 網絡、USB stack、MSC、
    WiFi、任務 loop、.nct、rawbench 各自的「唔啟動條件」。
  - **F. 總閘**：B02 `storage+cfg` 失敗 → CONFIG 唔 give → B03 之後全部 SKIP
    （B11 account 仍跑，之後只剩心跳）；壞 config ≠ storage 失敗，degraded 仍會開機。
- **對照來源**：`main.c` ph_boot…ph_account、`boot.c` needs/gives、`nc_task.c`
  （level／affinity／enabled／ layer）、`nc_net_lines.c`（service claim／line core／
  buffer）、`nc_wifi.c`（兩道閘／mode／credentials）、`nc4_router.c`（routes→核）、
  `nc_net_raw.c`、`nc4_web.c`。
- **驗證**：兩個 generator 重跑；XML parse；headless Chrome render 檢查
  （修 fail 欄越界、加自動斷行、決策表兩行條目間距）。
- **其他**：`docs/00_pm/README.md` 加「圖」清單；`naming.md` 圖表清單加 boot-cores.svg；
  預覽存 `artifacts/previews/`。今次只改圖／文件，冇改 firmware code。

### 7.74 2026-11-16 晚（line schema 回到 composite kind；buffer 預設改成量測值）

- **決策**：line 的身份回到 **`kind + index`**，`kind` 是 composite（`eth_tcp`／`wifi_tcp`／
  `eth_udp`／`wifi_udp`…），**config 不再寫 `iface`／`service`**；`name` 由 `kind + index`
  生成，仍然可以用 `name` 覆寫。這是 2026-11-16 凌晨 `1862675` 的方向，之後短暫改成分開
  `kind`＋`iface`，現回到 composite（`line-identity.md` 的模型）。
- **Config**：七個 project 的 `system.json` 全部轉 composite kind＋index；
  `uart`／`usb_jtag`／`cdc` 都補 `index` 並移除 `name`，全部由 `kind + index` 生成；
  firmware 的 UART lookup 已改成 by-kind（USB 本來就係）。P4-ETH／N16R8 的 `rx` 移除，
  改用預設；Matrix（4MB／無 PSRAM）保留 1-slot 覆寫。
- **硬體設定整理**：`ports/hardware/_chips/*.json` 的 `usb.link` 對齊
  `nc_cfg_chip.c` 的 `usb_jtag0`／`cdc0`；CMake 補 `_chips/*.json` dependency，
  改晶片表會真正觸發 `ncportgen`（之前改了不會 regen，生成檔與來源不一致）；
  C3 `console` 正規化為 `uart0`；S3 四塊板 CPU 預設一律 240 MHz（N16R8 由 160 改 240；
  要降頻改該板 `sdkconfig.defaults`＋`board.json`，`--fresh` 才生效）；
  Mini-Nano `system.json` 補 `usb`／`log_debug_mode`；四個未 seed `tasks` 的 project
  全部補 `tasks.json`＋seed（否則 `nc_cfg_append_obj()` 缺檔會靜默失敗）；
  清掉 `.DS_Store`；[ports/README.md](../../ports/README.md) 新增 §10.5 現有硬體設定表。
- **Buffer 預設**：`nc_cfg_rx_default()` 由舊的 TCP 8 slots／WS 4×1,024／UDP 4×1,024，
  改成 2026-11-15 量測公約數：`TCP 2×8,205`、`UDP 1×8,205`、`WS 2×8,192`
  （`net-test-matrix.md` §4.6）。4MB／無 PSRAM 的板仍然要自己覆寫 1 slot，
  否則會把內部 RAM 食光（S3 2 slots SRAM 會餓死 WiFi）。
- **Route 容錯**：`routes[].out` 指向不存在／未啟用的 link → 只忽略該格（同行
  `decode0` 照常生效）；`in` 不存在／未啟用 → 跳過該行。`link_audit()` 由 `ESP_LOGE`
  降為 warning。打錯名不再令整張路由表被拒收或令開機停。
- **`tools/board.py check`**：改為檢查目標 schema —— 網絡 kind 必須 composite、
  不可有 `iface`／`service`、必須有 `index`、`(kind,index)` 不可重複；
  舊 `cfg.port`／`cfg.rx` 仍然提醒。
- **驗證**：`python3 tools/board.py check` 0 error；S3-Matrix build 成功
  （`0x1138e0`，app 分區剩 28%）＋ P4-ETH（`0xFCFA0`，75%）
  ＋ N16R8（`0x1178c0`，73%）。文件同步：`net-lines.md`、`line-identity.md`、
  `config-layers.md`。
- **未完成**：service registry／claim API；`iface` 真正 bind（ETH／WiFi 同 port 分離）。

### 7.75 2026-11-16 晚（USB 配置地圖、S3-Matrix OTG MSC、CPU 240、NCM 前 gap）

- **文件**：
  - 新增 [usb-layers.md](../03_notes/usb-layers.md)：晶片能力 → 板子 → build → 用戶
    config 的五層、每顆晶片幾條 USB 通道、S3 開 MSC 的完整步驟與 console 取捨。
  - 新增 [cpu-frequency.md](../03_notes/cpu-frequency.md)：build-time 預設 vs 執行期
    調頻（未來測試）；現階段 S3 全部 240 MHz。
- **S3-Matrix 與 S3-Mini-Nano 改為 USB OTG MSC**：
  - `system.json`：`links[].kind="cdc"`、`usb.msc=1`、`usb.cdc_log=1`；route `cdc0→decode0`。
  - `sdkconfig.defaults`：`CONFIG_ESP_CONSOLE_NONE=y`、`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
    （OTG 與 USJ 共用 PHY，不能同時用 USJ console）。
  - 上板後預期主機見到 `NC-xxxx` MSC ＋ CDC serial；bootloader 早期 log 冇，app log 由 CDC 出。
  - build：Matrix `0x112b10`（28% free）、Mini-Nano `0x114ab0`（73% free）。
  - **現時連接的是 Mini-Nano**（`/dev/cu.usbmodemD885ACD42F2C1`，原生 USB）；
    未 flash／未上板實測 MSC。
  - 其餘 S3（N16R8／SuperMini）仍是 USJ console、`msc=0`。
  - P4-FireBeetle2 補 `CONFIG_NETCORE_USB_OTG_HS=y`：`features.usb="otg-hs"` 之前
    只在 board.json 寫，build 沒開 HS；現在一致，build `0xfe2c0`（75% free，未上板實測）。
- **S3 CPU**：四塊全部 240 MHz（N16R8 由 160 改）；要降頻改
  `board.json`＋`sdkconfig.defaults`，`--fresh` 才生效。執行期調頻未實作。
- **圖**：`network-lines.svg`、`boot.svg`、`boot-cores.svg` 已按 composite kind、
  route 容錯、USB console／OTG、D11（WAMR 未接）重畫；`architecture.svg` 沿用。
- **NCM 前仍未做好**：
  1. NCM 執行後端（WAMR）未接；`.nct` / `.nce` 現在「登記得到、跑不到」。
  2. D9 外部任務掃描要拆「probe→declare」與「B08 load」兩步；manifest v2
     （name／level／require／provide／param／doc TLV）未做。
  3. 效果掃描（`/data/pixel/effects`）與 service registry／claim API 未做。
  4. USB 多通道（P4 三條 PHY）未做；目標改成**資源註冊＋claim**
     （`nc_usb_port_register()`／`nc_usb_claim()`），唔係 `usb.ports[]`／`functions`
     配對（2026-11-17 修正）；`usb.rx`／`msc`／`cdc_log` 仍是 global；
     `driver.json` USB 段與 `board.json features.usb` 仍未讀。
  5. iface-specific bind（同 port ETH／WiFi 分離）未做。
  6. CPU 執行期調頻（PM/DFS）未做；先維持 240。

### 7.76 2026-11-17（Network 只 claim Hardware；USB 資源模型）

- **Network 不再重複硬件**：`network.json` 的 `lan` 段移除 `driver`／`phy_type`／
  `phy_addr`／`gpio`（C3／P4-ETH／P4-FireBeetle2／N16R8 四份）。ETH 的 GPIO／PHY
  只在 `hardware.json` 的 `ETH`（由 `wiring.json` 生成），Network 只 claim／用。
  以前那幾個 key runtime 根本冇人讀，改了也沒有作用。
- **命名方向**：`hardware.json`＝Interface／Hardware；`system.json` 的 `links`＝Channels；
  `network.json`＝Connectivity（不是 Interface）。見 [config-layers.md §0.6／§0.7](../03_notes/config-layers.md)。
- **USB 資源模型**：目標是 `sys/usb` 註冊 port 能力 → consumer claim；
  **不是** `usb.ports[]`＋`functions` 配對。Router 只做 data plane，不擁有 USB port。
  見 [usb-layers.md §5.2](../03_notes/usb-layers.md)。
- **P4-FireBeetle2**：`CONFIG_NETCORE_USB_OTG_HS=y` 放在 profile 區塊外，`board.py sync`
  後 `board.py check` 0 error；build `0xfe2c0`（75% free）。
- **ETH 兩種模式**：`hardware.json` 的 `ETH.driver` 接受 `"rmii"`（現在）／`"spi"`；
  SPI 模式只寫 `GPIO.spi`（`spi_list` 的 index）＋ `GPIO.cs`／`rst`／`int`，
  **不自己填 SPI 腳位**；SPI 匯流排由 Hardware 的 `SPI.list` 註冊成 `spi_list`。
  這跟 mp_Net-Core 的「線名 + index」一致（`spi_list[GPIO.spi]`），沒有另創 `bus` 名；
  `driver:"spi"` 開機明確報「未實作」。
- **ETH 預設 DHCP**：移除 `net_eth_start()` 寫死的 10.10.1.50 static IP，回到 esp_netif
  預設 DHCP；P4-ETH 的 `network.json` `lan.ip.mode` 改 `dhcp`。static 未接。
- **硬體 JSON 進入用戶空間**：`ports/project/<chip>/<board>/data/config/hardware.json`
  由 `tools/sync_hardware_presets.py` 從 `wiring.json` 產生（generated snapshot）；
  build 時優先嵌入它，開機 seed 到 `/data/config/hardware.json`，用戶插 USB 就看得見、改得到。
- **SPI 的 `phy_addr`**：未用時寫 `-1`（不是 0）；RMII 才係 0–31。`nc_eth_cfg_t.phy_addr`
  改 `int8_t`，parser 按 driver 預設（SPI=-1、RMII=1）。
- **USB 進入 hardware.json**：七塊板的 `wiring.json` 加 `USB.list`；`phy` 只有實際值
  `usj-fs`／`otg-fs`／`otg-hs`。S3 在 `usj-fs`／`otg-fs` 二選一（Matrix／Mini=otg-fs、
  N16R8／SuperMini=usj-fs），同一對 GPIO19/20；C3 `usj-fs`、P4-ETH 空、
  P4-FireBeetle2 `usj-fs`＋`otg-hs`。`personalities`／`speed`／`fs`／`shared` 已刪除。
- **USB GPIO**：`USB.list[].GPIO` 記錄固定 `dm`／`dp`（S3 19/20、C3 18/19、
  FireBeetle2 24/25＋49/50），**唯讀、不可 remap**。`USB.list` **冇 `id`**，
  list index 就是 identity（線名 + index）。
- **name 預設 key**：**所有** link 的 config 都已經寫上生成名做預設，而且 `name`
  放 object 最前（`usb0`／`wifi_tcp0`／`eth_tcp0`／`uart0`）；用戶可以改，或寫 `""`
  fallback 生成名。`links[].msc` 是 per-link，預設 0；唔再 fallback global `usb.msc`。
- **USB link 已 migrate**：`system.json` 的 USB link 現在是
  `{ "name":"usb0", "kind":"usb", "index":0, "msc":1 }`（`name` 放最前）；
  `kind:"usb"` 由 generated `nc_port_usb_kind(index)` 解析成 CDC／USJ；
  `msc` 預設 0、per-link，USJ 冇 capability 就跳過。
  routes 用 `usb0`；舊 `cdc0`／`usb_jtag0` 已遷移。
- **其他 link 也統一 name 在前**：所有 `links[]` 的第一個 key 都是 `name`。
- **runtime**：`kind:"usb"` 已 parse（generated kind table）；`USB.list` 的 GPIO
  仍然只是 metadata，未 parse。`sys/usb` claim／多 port 未實作。
- **Build**：P4-ETH `0xfd160`（75% free）；Matrix `0x1130e0`（28% free）。
- **未做**：`lan.enable` 的 claim 仍未檢查 hardware（`hardware.json` 冇 ETH 段時仍 fallback
  內建腳位）；SPI PHY driver 未實作；`network.lan.ip` static 未接；USB `sys/usb` registry 未實作。

---

### 7.77 2026-11-17（同日）— b68795f 的收尾：build 修復、kind:"usb" 真正解析

b68795f 本身 **build 唔過**、`kind:"usb"` 亦只做了一半。同日補完：

**建置（阻斷級）**

- `ncportgen` 生成的 `nc_port_usb_kind()` 用了 `__attribute__((used, retain))`；xtensa
  GCC 15（IDF 6.0.3）未實作 `retain` → `'retain' attribute ignored [-Werror=attributes]`，
  **所有板子都 build 失敗**。改成只有 `used`；把 `nc_port.o` 拉入改用 `main.c` 的
  **強引用函數指標**（唔再靠 attribute）。
- `cfgstruct_test` 因 `usb_family` set-but-unused 在 `-Werror` 下編譯失敗；同時斷言仍是
  舊出廠名（`CDC`／`UART`／`usb_jtag`）。已更新斷言、加 USB stub 同 `[23b] kind:"usb"` 測試。
  現時 `tools/hosttest/run_all.sh` **11 支全過**。

**USB schema（commit 賣點，之前未接）**

- `nc_cfg_load.c` 的 `kind:"usb"` 以前**寫死** `NC_LINK_CDC`（S3／C3 的 USJ 會靜靜變 OTG、
  C3 直接冇 USB 通道）。現在依 `index`（或舊式 `"usb":N`）查
  `nc_cfg_usb_default_kind()` → 真 kind；查不到（`0xFF`）＝這塊板冇該 port → 該 link
  不建立並計 `invalid`。
- `msc` 兩條路徑（缺檔預設 vs `parse_links`）以前一個 `-1`、一個 `0`，`-1` 在
  `msc != 0` 之下等於 truthy → **冇 `system.json` 的板子會自動交 FAT**。兩邊統一 `0`；
  global `usb.msc` 已停用（per-link 才有效）。`main.c` 殘留的「`-1` 才 fallback global」
  分支亦一併刪（default 0 之下永遠唔會行），改成**開機一行 warning**提醒舊寫法；
  `nc_cfg_types.h`／`Kconfig.projbuild`／`usb-concepts.md`／`config-layers.md` 的註解同步。
- `hardware.json` 的 `USB` 段**仍然未 parse**（只是 generated 鏡子）；personality 只認
  build-time 的 `wiring.json`。文件已改成這樣寫，唔再叫人去改裝置上的 `hardware.json`。
- ETH `driver:"spi"`：hwcheck 以前一律 claim mdc/mdio/ref_clk（SPI 下全是 -1，靜靜漏了
  cs/rst/int）；ph_hardware 又把「未實作」印成「未啟用（want=0）」。兩處已修。

**工具鏈**

- `make.py flash`：區域變數 `build` 遮蔽 `build()` → `TypeError: 'WindowsPath' object is
  not callable`（**任何板子、任何平台**）。已改名 `bdir`。
- `make.py`／`board.py`：Windows cp950 console 遇到 `✔／✘／↔` 直接 `UnicodeEncodeError`
  （`--help`、`check`、`doctor`、`flash` 錯誤路徑全中）。入口加 `stdout.reconfigure(errors="replace")`。
- `sync_hardware_presets.py`：Windows 下 `read_text()` 用 cp950 讀 UTF-8 → 全滅；
  而且用同名目錄猜 default project。已加 encoding（read/write 都 UTF-8、比對不看行尾）、
  改用 `default_project_for_hardware()`。
- `resolve_board()`：塊板「有 project 但冇 default」時，指名 project 會被靜默降級成純硬體。
- `make.py build --all`：以前靜默忽略 `--project`／`--no-project`／`--no-doctor`；
  現在前兩者明確報錯，`--no-doctor` 生效。
- `board.py new --eth spi`：能力碎片不存在，以前會先寫一半檔案再炸，留下 `check 2 errors`
  的半塊板；現在**寫之前**就報錯。
- `board.py sync` 的 help 由「兩層」改成「hardware 層」。

**文件**

- `usb-layers.md`：personality 來源（build-time `wiring.json`，唔係裝置 `hardware.json`）、
  `msc` 口徑、`kind:"usb"` 名稱／路由、§4.5 現況、§5、§6 未做清單全部對齊程式。
- 6 份 project `system.json` 的「現時 interim kind:…,usb:0」註解（與檔案本身矛盾）已清；
  `hardware.json` 根／USB `_comment` 由 `ncportgen` 產生器改正（並重跑 sync）。
- `status.md`：日期、網路（DHCP）、HTTP :80、10 條 broken link；`config-layers.md` route
  `usb0`／USB schema／kind 清單；`line-identity.md`／`net-lines.md` 的名稱口徑；
  `buffers.svg` 的 `usb.rx` 4,192→4,128、`boot*.svg` 的 `usb.msc` → `link.msc`（改腳本重生成）。
- `tools/README.md`：hosttest 由 9 支補到 11 支（加 `run_all.sh`）、`board.py new --ws` → `--wifi`、
  補 `make.py projects`／`check`、`board.py resolve`／`projects`、`ncportgen`／
  `sync_hardware_presets.py`／圖表腳本，P4 靜態 IP 改 DHCP。`build_and_flash.md`
  疑難排解的 `rm firmware/sdkconfig` 改成 `make.py build … --fresh`；`testing.md` 的 OTA
  路徑 `firmware/build/esp_net_core.bin` 改成 `artifacts/<board>.bin`。

**仍然未做（下一輪）**

1. `hardware.json` 的 `USB` 段 typed 解析（要做 `nc_usb_port_cfg_t` ＋ `apply_usb()`，並把
   personality 由 build-time 移到 runtime decision）；`sys/usb` register／claim。
2. 多 USB 通道（P4 三條 PHY 只開一條）；`usb.rx`／`cdc_log` 仍全局。
3. `board.py`：`projects` 分不出 `default:true` 同同名慣例、`hw:`／`pj:` 前綴係 no-op、
   alias 撞名只查「硬體＋default project」、doctor 對新 build slug 的新鮮度判斷、
   `tools/esp_webmaster/recover.py` 的 board→image MAP 未跟新板（S3 16MB 有兩塊板）。
4. 舊筆記仍在用已消失的路徑（今次未動，屬舊債）：`03_notes/core-isolation.md`／`cross-core.md`
   的 `firmware/components/sys/transport/`、`p4-usb.md:219`、`usb-config.md:539` 的
   `ports/_profiles/usb-*.defaults`、`memory-budget.md`／`memory-vs-micropython.md` 的
   `firmware/build/*.map`。

**實機驗證（2026-11-17 10:30–11:05，兩塊板直接插喺 Windows 主機）**

WSL 見唔到 COM（WSL2 冇 usbipd → `/dev/ttyACM*` 全空），所以燒錄／讀 log 改用 Windows 的
`python -m esptool`（5.3.1）＋ pyserial；build 仍然喺 WSL。全程冇用 `make.py flash`
（它走 WSL，見唔到埠）。

| 板 | 燒咗 | 結果 |
|---|---|---|
| ESP32-S3 16 MB flash / 8 MB PSRAM（COM30，303A:1001 USJ） | `ESP32-S3-N16R8`（console=USJ） | ✅ 完整開機；`USB：mode=jtag｜link=usb0（kind=usb_jtag）｜log=1｜msc=0` → `{kind:"usb",index:0}` 依 `wiring.json` 的 `usj-fs` 解析成 **usb_jtag**（未修之前會變 `cdc`）|
| 同上 | `ESP32-S3-Mini-Nano`（`phy=otg-fs`、console=none） | ⚠️ 燒完**完全冇 USB 枚舉**（主機連 303A 都沒有）。呢塊板靠 RTS 復位唔生效，`--after hard-reset` 會留住 ROM download mode；要 `--after watchdog-reset` 才真開機。OTG 版冇 log 可睇 → 待查（要用 BOOT 鍵入 download mode 再燒 USJ console 版） |
| ESP32-P4（COM34 = CH343 UART0；ECO7 / **rev v3.2**；32 MB flash；周邊係 **ESP32-C5**） | `P4-ETH` / `P4-FireBeetle2` 原版 | ❌ `requires chip revision in range [v0.0 - v1.99]`；`--force` 燒得入但 **boot loop**（ROM `esp32p4-eco7` 反覆重啟）→ repo 兩個 P4 板都寫死 pre-rev3 配方 |
| 同上 | `P4-FireBeetle2` 暫時改成 `SELECTS_REV_LESS_V3=n` + `REV_MIN_301=y` | ✅ 開機到 B10（UART0 有完整 log）；`USB：mode=jtag｜link=usb0（kind=usb_jtag）｜msc=0` 一樣正確。但這塊板只引出 OTG／CH343，USJ 冇接線 → 主機見唔到 USJ。`esp_hosted` 期望 C6（0x0d）、實板係 C5（0x17）→ Wi-Fi 失敗 |

順手修：`P4-FireBeetle2` 的 `system.json` 有個**放錯層**的 `"hostname": ""`（`hostname` 屬於
`network.json`）→ 開機 log 一直 `unk=1`；已移除。`console` 與 `wiring.json` 的暫時改動已
`git checkout` 還原。

### 7.78 2026-11-17 下午（實機第二輪：S3 OTG 打通；新板 Waveshare P4-WIFI6-DB＋rev3 碎片；ESP32-C5 hosted Wi-Fi 打通）

兩塊板都直接插在 Windows 主機（WSL2 冇 usbipd，COM 只有 Windows 見得到）：**COM30＝S3
16 MB flash／8 MB octal PSRAM**（MAC `d8:85:ac:d4:2f:2c`）、**COM34＝CH343 UART0＝Waveshare
ESP32-P4-WIFI6-DB**（P4 rev **v3.2**／ECO7、32 MB flash `gd`、32 MB hex PSRAM@200 MHz、
MAC `80:f1:b2:d7:ba:f8`、協處理器 **ESP32-C5** silicon `0x17`）。build 在 WSL，燒錄／讀 log
在 Windows（`python -m esptool` 5.x ＋ pyserial；`make.py flash` 走 WSL 見唔到埠）。

**新板 `ports/hardware/P4/ESP32-P4-WIFI6-DB/` ＋ `ports/project/P4/ESP32-P4-WIFI6-DB/`**

- 硬體事實：32 MB flash（40 MHz／DIO）、hex PSRAM 200 MHz、**CPU 400 MHz**、console 走
  UART0（CH343）、USB OTG **HS**（4-pin 排針，UTMI pin 49/50）、板載 C5 走 ESP-Hosted／
  SDIO、冇 Ethernet PHY（`wiring.json` 留 ETH 但 `enable=0`，同 FireBeetle2 同一手法）。
- 32 MB 分割表另寫一份（`ota_0 @0x20000`、`ota_1 @0x420000`、`data @0x820000 size 0x1700000`），
  開機 log 見 `data 分區 size 23552 KB`。
- aliases：`P4-WIFI6-DB`／`ESP32-P4-C5`／`P4-C5`（`ESP32-P4-C6` 仍然屬 FireBeetle2）。
- 實測：階段全 `ok`（當時號碼由 B07 起跳、冇 B06；2026-11-17 已重編成連號 B01–B11，
  見 §7.80）、`hwcheck claims: []`、
  `TinyUSB Driver installed on port 1`（HS）、
  `links[].msc=0 → /data 留在 APP`。

**新能力碎片 `p4-rev3` / `p4-pre-rev3`（晶片版本也是硬體事實）**

- IDF：P4 `rev<3.0` 與 `rev>=3.0` **互斥**（`esp_hw_support/port/esp32p4/Kconfig.hw_support`
  第一行就寫死「mutually exclusive」），同一份映像只支援一邊。所以「這塊板是 rev3 還是
  pre-rev3」跟 eth／wifi 一樣用碎片表達，不再每塊板各抄兩行。
- `p4-rev3` ＝ `SELECTS_REV_LESS_V3=n` ＋ `REV_MIN_300`（揀 300 而非 IDF 預設 301：碎片要
  涵蓋所有 rev3 樣品，v3.0 不應被預設值擋）。`p4-pre-rev3` ＝ `SELECTS_REV_LESS_V3=y` ＋
  `REV_MIN_0`（pre-rev3 樣品 rev 五花八門，收緊冇好處）。
- `ESP32-P4-ETH`／`ESP32-P4-FireBeetle2` 改掛 `p4-pre-rev3`，原本手抄那兩行換成指向碎片的
  註解；Waveshare 板掛 `p4-rev3`。實測 log：`efuse_init: Min chip rev v3.0 / Max v3.99 /
  Chip rev v3.2`。

**修正①：rev3 上 `cpu_mhz:360` 根本不可達**

IDF `esp_system/port/soc/esp32p4/Kconfig.cpu` 的 `ESP_DEFAULT_CPU_FREQ_MHZ_360`
**depends on `ESP32P4_SELECTS_REV_LESS_V3`**；掛了 `p4-rev3` 之後只剩 400 可選，board.json
寫 360 會被 Kconfig 靜靜改回 400（實測 `cpu freq: 400000000 Hz`，同 board.json 唔符）。
新板改成 400、sdkconfig 亦改 `MHZ_400`。ETH／FireBeetle2 是 pre-rev3，360 保留。

**修正②：ESP-Hosted 的 SDIO 腳位放錯層 → C5 握手失敗**

- 症狀：`sdmmc_init_ocr: send_op_cond (1) returned 0x107` → `eh_sdio: card init failed` →
  `bring-up timed out (5000ms)` → `Wi-Fi 初始化失敗 ESP_FAIL`。**唔係**「C5 冇 slave
  firmware」，係 CMD5 冇人應。
- 根因：`wifi-hosted-*` 碎片裡寫了 `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y`。這符號是
  esp_hosted 的**板子符號**，一開就決定 CLK/CMD/D0..D3 ＋ reset 整組腳位；配 C5 時它給
  官方 P4-Function-EV-Board 子板的 **33/4/20/23/21/22＋53**，而 Waveshare 實板是
  **18/19/14/15/16/17＋54**（該公司文件「ESP32-C5 ESP-Hosted 通信 SDIO」一行）。
- 修正：**碎片只留 slave target／緩衝，板子符號搬到各板 `sdkconfig.defaults`**。新板用
  `CONFIG_ESP_HOSTED_P4X_C5_DEV_BOARD_FUNC_BOARD=y`（esp_hosted 對 P4+C5 的預設值，腳位
  剛好逐隻吻合實板）。FireBeetle2 保留原本的 func-board 符號（外掛 C6 未上板驗，註解已標）。
- 修好之後：`eh_sdio: SDIO 4-bit 40000 kHz CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54`
  → `Card init success` → `slave chip id: 0x17 (esp32c5)` →
  `esp-hosted fw versions: host=3.1.0 coprocessor=2.12.9`。
- **Wi-Fi 真的通了**：STA 掃描經 SDIO 回報 8 個真實 AP（`MB9_IT`／`TVBWiFi`／`VIPWiFi`…，
  2.4 G 與 5 G 都有，ch=1／161）→ RPC 數據路徑 OK。連線失敗只是因為種子 config 的 SSID
  `NETCORE-TEST` 不存在（`reason=201 NO_AP_FOUND`），唔係鏈路問題。

**S3 OTG 實測（`ESP32-S3-Mini-Nano`：console=none、PHY 走 OTG-FS）**

- 之前「燒完完全冇 USB」的真因**唔係** OTG 有問題，而是板上的舊 `/data` 設定：
  `kind=usb_jtag` ＋ 這份映像 `CONFIG_ESP_CONSOLE_NONE=y` → 開機早段就 abort／reboot loop，
  主機連 `303A` 都見唔到。清掉 `data` 分割區（`0x820000` 起 `0x700000`）＋ NVS 之後即刻正常。
- 正常之後：主機枚舉 **`VID_303A&PID_4003`** → CDC `COM31`（TinyUSB 出 log）＋ MSC 磁碟區
  **`NC-2F2C`**，log `碟交給 host: ESP_OK`。即 S3 的 OTG（FSLS PHY）在 console=none 建置下
  **完全可用**。
- 三個坑（已寫入 `usb-config.md`）：① 燒錄要喺 **build dir** 跑 `write-flash "@flash_args"`
  （`artifacts/<board>/` 那層的 `flash_args` 少了 `bootloader/` 前綴）；② 用 USJ 讀 log 一開埠
  DTR/RTS 就拉重置線 → 想乾淨抓 boot log 要用 `--after watchdog-reset`／唔碰 DTR/RTS；
  ③ 燒 OTG 版會令 USJ 掉埠（CDC 重新枚舉），要重開埠搶 ROM 下載窗口。
- **用詞修正（同日）**：早前講「USJ 的 PHY 被 OTG 搶」是舊模型（`usb_jtag`／`cdc` 可以在
  runtime 揀）留下的語感。現在 `wiring.json` 的 `phy` 是 build-time 事實、`{kind:"usb"}`
  自動跟、B03 寫入 ctx —— **一份映像只有一個 USB driver 會 init，不存在爭奪**。剩下的是
  寄存器事實：`RTCCNTL.usb_conf` 的 `sw_usb_phy_sel` 由兩個 driver 各自在 init 時寫
  （OTG→1、USJ→0，見 `usb_wrap_ll_phy_enable_external()`／
  `usb_serial_jtag_ll_phy_enable_external()`），且它在 RTC domain、**軟重啟不清**，
  所以 USJ 分支每次都明確寫一次（我們走 LL、冇 `usb_serial_jtag_driver_install()`）。
  完整表見 `docs/03_notes/usb-config.md`。

**S3 再燒一次（同日下午，實測流程）**

- 機上跑 `app=3450d85a`（舊映像），source 的新 build 係 `3d3e59af` → 需要重燒。
- 流程：按 BOOT＋RESET 入 ROM download（`303A:1001`，COM30）→ 由 **build dir** 跑
  `esptool … --before no-reset write-flash "@flash_args"`（1135648 B、`Hash of data verified`）。
- ⚠ **燒完不會自己離開 ROM download**：`--after hard-reset`、以及直接拉 RTS／EN、
  esptool 自己的 hard reset 都試過 → 依然停在 `303A:1001`（ROM 回 `read-mac` 有回應，
  NC4 冇回應 ＝ 確認係 ROM 唔係 app）。download 選擇係 strapping latch（RTC domain），
  軟／EN 重啟都不會重新採樣。**正解＝`--after watchdog-reset`**（`tools/flash.sh` 已經
  这样做，今次只是再次確認），RTC WDT 一 reset 就跳返 app。
- 驗證：`303A:4003` 重新出現 → CDC `COM31` 上 NC4 `status sys` 回
  `app=3d3e59af`、`up_s=13`、`chip=esp32s3`、`idf=v6.0.3`；MSC 磁碟區 `NC-2F2C`
  （7,237,632 B）照樣出現 → 新映像確認上機。

**兩個文件／工具不一致（今輪發現，未修）**

1. `main.c` 檔頭、`docs/02_guides/build_and_flash.md`、`tools/flash.sh` 都寫「CDC 收字串
   `DFU` → 重啟入 download mode」的逃生門，但 firmware **冇實作**（全 repo 的 `DFU`
   只出現在註解／文件）。現時唯一入 download mode 的方法係實體 BOOT＋RESET。
2. WSL 的 git 會把整個工作區看成 modified（351 檔、+67736/−67736 ＝純 CRLF/LF 差異），
   所以 **WSL 建置出來的 app 版本字串永遠帶 `-dirty`**（例如 `bfd1ded-dirty`）。
   commit 在 Windows 側做，內容無影響；但版本字串不能當「工作區乾淨」的證據。

**仍未做（今輪新增）**

1. `board.py check` 未擋「兩個互斥 rev 碎片同時掛」（`p4-rev3` ＋ `p4-pre-rev3` 寫相反值，
   Kconfig 檔尾者贏 → 隨 profiles 次序漂移）。
2. `board.py check` 未驗 `board.json` 的 `cpu_mhz` 是否**真的可達**（今次 360→400 的靜默不符
   就是因為 Kconfig 只管得到自己）。
3. ESP-Hosted 的板子符號目前只能手寫在各板 `sdkconfig.defaults`，`board.json` 冇對應欄位 →
   `check` 驗唔到「這塊板的 SDIO 腳位同 esp_hosted 選的一致」。
4. 映像與裝置上舊 `/data` 設定的一致性冇稽核（`kind=usb_jtag` ＋ `CONSOLE_NONE` 可以靜靜令
   USB 全滅，S3 就是這樣「假死」的）。
5. 板上 C5 的 slave firmware 落後（`coprocessor=2.12.9` vs host `3.1.0`，可用但建議升級）；
   esp_hosted 2.6+ 有 host-performed slave OTA，未接入。
6. FireBeetle2 rev1.0 專屬的 `PM_SLEEP_CLK_ICG_ENABLE=n` 補丁未確認 rev3 是否需要 ——
   Waveshare rev3 板冇加，開機正常。
7. 逃生門「對 CDC 送 `DFU` 字串 → 重啟入 download mode」**未實作**（`main.c` 檔頭、
   `build_and_flash.md`、`tools/flash.sh` 都咁寫）。做法應該係 CDC RX 見到 `DFU` →
   `SET_PERI_REG_MASK(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)` ＋ `esp_restart()`
   （RTC domain 保留，正好同 watchdog-reset 同一條路）。未做之前，S3 跑 OTG 版要重燒
   只能實體 BOOT＋RESET。

### 7.79 2026-11-17 傍晚（第三輪：功能面深入驗證 —— S3 存儲／NC4／日誌、P4 復位與 flash 回讀）

兩塊板照舊直接插主機（S3＝COM31 `303A:4003`，P4＝COM34 CH343）。今輪唔再燒錄，只驗行為。

**S3：MSC／FAT 與主機的「一個主人」模型**

| 驗證 | 結果 |
|---|---|
| 磁碟區 | `NC-2F2C`（FAT）7,237,632 B，total／free 同裝置 `status storage` 完全一致 |
| 內容 | `config/{system,hardware,network,tasks}.json` ＋ `system.json.bak`、`pixel/effects`；`system.json` 內 `cID:"2F2C"` → 磁碟標籤 `NC-2F2C` 由 `cID` 衍生（實測見下） |
| 裝置讀主機寫的檔 | `FILE_QUERY /config/system.json` → `exists=1 size=1351 sha256=91ebd45a…`，同主機 `Get-FileHash` **逐字節相同**（唯讀視圖真係讀 FAT，唔係快取假象）|
| claim 守衛 | `claim` → `file_mode=1 mode=app`；此時主機寫 D: **0.03 s 內失敗**（"裝置未就緒"），確認主機唔會偷偷寫爛 FAT |
| release | `release` → `file_mode=0 mode=usb`，約 4 s 後碟返嚟，`free` 由 4,046,848 回到 7,192,576（claim 期間的失效已復原）|
| 寫入吞吐 | 1 MB 花 **14.2 s（≈72 KB/s）**、3 MB 花 42.8 s（同速率）；資料 round-trip hash 一致。讀取數字（0.01 s）係 host cache，不可信 |
| 併發 | 3 MB 寫入期間同時串流 CDC 日誌：心跳最大 `dt=1.0529 s`（比 1 s 多 53 ms，冇出現 ~900 ms 停頓），heap 全程 169,995 不變 |
| `fmt` | `-99` ＝ 未嘗試（`main.c:469` 的 sentinel，唔係 bug —— 這塊板沿用既有 FAT）|

**S3：配置回環（真·由 FAT 設定驅動開機）**

改 `D:\config\system.json` 的 `cID` `"2F2C"`→`"2F2D"`（sha 由 `91ebd45a…` 變 `1087d1a0…`，
`Write-VolumeCache` 後裝置唯讀視圖即見到新 sha）→ NC4 `REBOOT (0x100F)` →
開機後 `status sys` 回 **`cid=12077`**（＝0x2F2D），磁碟標籤同時變 **`NC-2F2D`** →
還原原檔再重啟 → `cid=12076`、標籤 `NC-2F2C`、sha 回到 `91ebd45a…`。即：主機經 MSC 寫的
config 真係主導下一次開機，而且可逆。

**S3：NC4 與日誌（console=none 之下的唯一診斷管道）**

- `status` 全部 keys 正常：`sys`／`storage`／`log`／`mem`／`provides`／`tasks`／`ncm`／`cfg`；
  `log` 見 `cap=1 run=1`、`produced=[34,4]`、`sent=38`、`dropped=578`（冇人讀時正常丟棄）。
- `nc4_log.py` 串流 8 s：1 Hz 心跳、`dt` 抖動 ±100 µs、`heap=169995` 穩定。
- `REBOOT`：`up_s` 619→12、`rst` 1→3、USB 重新枚舉回 `303A:4003`、`app` hash 不變
  （即軟重啟後 OTG 棧照樣起返，PHY mux 由 driver 重新寫同一個值 —— 同 §7.78 的說明一致）。

**P4：三次復位 + flash 回讀**

- 用 pyserial 拉 RTS（EN）連續復位 3 次，每次都完整開到 **11 個階段全 `ok`**
  （當時號碼由 B07 起跳、冇 B06；2026-11-17 已重編成連號 B01–B11，見 §7.80）、
  `eh_sdio: Card init success`、
  `slave chip id 0x17 (esp32c5)`、SDIO `CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54`、
  `cpu freq 400000000 Hz` → 開機確定性 OK。
- `read-flash 0x20000 1038640` 回讀 app → sha256 `5472ed85…` 同
  `artifacts/build/P4_ESP32-P4-WIFI6-DB/esp_net_core.bin` **完全相同**（裝置內容＝建置產物）。
- 回讀分割表：`nvs 0x9000/0x6000`、`otadata 0xf000/0x2000`、`phy_init 0x11000/0x1000`、
  `ota_0 0x20000/0x400000`、`ota_1 0x420000/0x400000`、`data 0x820000/0x1700000`（23 MB），
  合計 31.1 MB ≤ 32 MB。

**P4 這輪做唔到的（缺硬件接線／憑證，唔係程式問題）**

- P4 的 `system.json` 只有 `usb0` link（OTG HS），**冇 uart0 link** → CH343 那個 COM34 只係
  console（單向 log），送唔到 NC4 指令；而 4-pin OTG HS 排針未接主機 → 主機見唔到 CDC／MSC。
- 所以 P4 的 NC4／MSC／網絡連線測試全部做唔到。要繼續驗，需要其中一樣：
  ① 4-pin OTG 線插主機（之後可以照 S3 同一套流程驗 CDC／MSC／配置回環）；
  ② 一個真實 AP 的 SSID／密碼（現時種子 SSID `NETCORE-TEST` 不存在，只驗到掃描）。

### 7.80 2026-11-17（B 號碼收返連號：舊編號 07–12 → 06–11；程式、文件、四張圖、實機一齊更新）

**背景：空號比連號貴**

2026-11-14 移除第六階段（pool）之後，刻意留空號（05 → 07），理由寫在
[naming.md §7.3](../03_notes/naming.md)：ID 是契約，唔為靚號碼重編。實際成本是每次講開機
（log、四張圖、每份文件）都要解釋一次「為何冇 B06」；而 §7.71 那輪寫圖時其實已經自然用
連號講（B06 declare、B07 commit…），兩種講法並存更亂。今日趁階段 ID 未有外部依賴
（未公開、log 即時看、冇存檔比對），一次過將後面六個階段前移一號。

**改動（純改名，行為零變化）**

- `firmware/main/main.c` `stages[]`：舊編號 07–12 那六行 → `B06…B11`。`needs`／`gives`／
  `ph_*`／次序全部唔動，所以 boot 時間、能力位帳、失敗語義一模一樣。
- `firmware/main/boot.h`：命名空間註解改成連號講法；`nc_boot_stage_t.id` 例子改 `"B06"`。
  順手改正一個舊錯：註解寫「B03 ＝ storage+cfg」，其實 storage+cfg 一直係 **B02**。
- 四支生成器重跑：`tools/{boot_diagram,boot_cores_diagram,core_timeline,net_arch_diagram}.py`
  → `docs/00_pm/{boot,boot-cores,core-timeline,network-lines}.svg` 四張一齊更新
  （每張的 B-token 都重新出過，唔係手改 SVG）。
- `tools/id_check.py` 的階段清單；13 份 docs；4 份 ports 文件（含 `sdkconfig.defaults` 註解）。
- 合共 **165 個 B-token** 改號。做法是單次 regex 掃換（`{7:6,8:7,9:8,10:9,11:10,12:11}`）：
  唔可以逐個 `sub`（先 B07→B06 再 B08→B07…），否則會連鎖塌落去 —— 第一次就踩過，全部
  `git checkout --` 重來。
- 規矩改：之後階段一有增減就順移，**唔好再留空號**。

**驗證**

- `tools/id_check.py`：B **11 個階段**、20 個 ID token 全部指得到 code ✓。第一次跑被
  status §7.78／§7.79 的「歷史上跳到 12」字樣叫停（歷史記載都算 ID token）→ 改寫成
  「號碼由 B07 起跳」的講法，唔再引用已消失的 token。
- `tools/hosttest/run_all.sh`：11 個 suite 全過。`tools/sync_hardware_presets.py --check`：
  8 塊板全部已同步。
- `make.py build --all --fresh --no-doctor`（WSL）：8 塊板全部建置成功；S3 Mini-Nano
  映像 1.1 MB（分區剩 28%）。
- **P4-WIFI6-DB（COM34）**：esptool 重燒後 log 見 **B01–B11 連號全 `ok`**，摘要
  「11 階：0 失敗、0 跳過、0 可選失敗」；`netcore: cid=0xBAF8（config）… （NVS，B06 套到
  decoder）` 已經用新號；SDIO 4-bit／C5 `0x17`／`fw=0x00020c09` 照樣 init 成功。
- **S3 Mini-Nano（COM31）**：用 **NC4 OTA**（[tools/ota.py](../../tools/ota.py)，0x22xx）
  推新映像 —— 唔使 download mode、唔使按鍵：1,135,648 B 以 **207 KB/s** 寫完、
  `END ok（驗證通過）`、`APPLY` 後自動重啟。`status sys` 見 `app` hash 由 `3d3e59af` →
  **`a5564178`**、`storage.boot.fw_new: 1`；重啟後 log 見 **B09 usb／B10 channels**
  （舊 B10／B11 前移後的新號）、`boot 完成：已具備 0x03ff`（能力位帳不變）。
  - 註：S3 是 `console=none`，B01–B08 全部發生在 USB 列舉之前（B09 才裝 TinyUSB），
    CDC 未接上，那幾行一定收唔到（log 環已 drop）——S3 只看得到 B09 之後。要睇全表
    就睇 P4 那條 log（P4 有 UART0 console）。
- `python tools/board.py check`：0 個錯誤（5 個「冇 driver.json」警告同之前一樣）。

### 7.81 2026-11-17 晚上（S3 接回 macOS：多輪 USB NC4 OTA、`BEGIN no reply` 假故障、`NC_PLAN_CLS_BULK` 一直是 INTERNAL）

承接 handoff 2026-11-17 的三個待辦。S3 Mini-Nano（原生 USB）接回 macOS，
用 [tools/ota.py](../../tools/ota.py) 直接 OTA（唔使 download mode、唔使按鍵）。

**① test 映像 OTA 的 `BEGIN no reply`：主機等得不夠久，唔係狀態機壞**

- 重現：release（app `8cbaf00f`）跑住，推 test 映像 1,224,272 B → caps 正常、BEGIN
  等滿 5.0 s → `no reply`。
- 但 BEGIN 其實已開始：之後 `0x221A STATE` 回 **`writing=1`、target `ota_1`**，
  `0x2218 PROGRESS` 回 `image_size=1,225,296`、`written=0`。係 ACK 遲到，唔係冇做。
- 真兇：韌體 `on_begin` 的 `esp_ota_begin()` 會**先抹整個目標分區**才回 ACK；
  1.2 MB 映像實測 **5.17 s**（≈237 KB/s、4 KB sector 逐個抹）。工具舊版硬編
  `timeout=5.0 s` —— release 映像 1,137,872 B 剛好 4.5–4.8 s「踩線過關」，test 映像
  只大 7.6% 就跌出窗口，所以症狀睇落好似「只有 test 映像 OTA 壞」。
- 修法：`tools/ota.py` / `tools/ota_tcp.py` 的 BEGIN 等待改由映像大小估
  （`10 s + size/100 KB`；1.2 MB → 22 s）；開始前先查 STATE（見到上次留下的
  `writing` 就叫用家 `--abort`）；新增 `--abort` 單獨清 session；ACK 遲到但 STATE
  已在 `writing`／`verified` → 當作已開始繼續。
- 實測（同一塊板、同一條 USB，三次全部 END ok、APPLY 後自動重啟）：

| # | 方向 | 大小 | BEGIN | WRITE | 之後 app hash |
|---|---|---|---|---|---|
| 1 | release → test | 1,224,272 B | **5.1 s** | 6.1 s（194.5 KB/s） | `b3bcfdd7` |
| 2 | test → test（修正版） | 1,224,432 B | 4.8 s | 5.6 s（213.4 KB/s） | `6abf3963` |
| 3 | test → release | 1,138,032 B | 4.5 s | 5.3 s（209.7 KB/s） | `5cfc874e` |

**② 更大的發現：`NC_PLAN_CLS_BULK` 由 IDF 6 起一直是 INTERNAL**

- 現象：test 映像 `status mem` → `plan.ok=false`、`ncm/ncm.arena`（524,288 B）`ok:false`；
  `status tasks` → `ncm started=0`；`status ncm` → `arena=0`。即 handoff 說
  「用 test image 驗 ncm 任務」在板上**由頭到尾冇起過**。
- 追查：用真的 compile command 預處理（`-E -dM`）見到
  `#define NC_PLAN_CLS_BULK NC_PLAN_INTERNAL`。原因：`main.c` 第 44 行
  `#if CONFIG_SPIRAM` 在**第一個 `#include` 之前**，而 ESP-IDF 6 唔再隱含
  `-include sdkconfig.h`（v5 時代嘅行為）→ `CONFIG_SPIRAM` 當 0 → BULK 永遠 INTERNAL。
- 後果：S3（8 MB PSRAM）所有 BULK 宣告（2×2,615 B 回覆幀）落內部 RAM；
  **512 KB ncm arena 向內部 RAM 要 → 必然失敗** → ncm 任務唔啟動。編譯零 warning，
  只有 `status mem` 的 plan 帳睇得到。
- 修法：`main.c` 最前面 `#include "sdkconfig.h"`（附註解講清楚原因）；順手令 plan 帳
  可診斷：`status mem.plan` 新增 `avail{dma,int,psram}`、`carve`（bitmask）、每項 `cls`。
- 修正後（同一塊板、test 映像）：

| 指標 | 修正前 | 修正後 |
|---|---|---|
| plan | `ok=false, fail=1` | `ok=true, fail=0` |
| `avail` | （冇印） | dma 38,912／int 38,912／psram 8,257,536 |
| `bytes` | int 10,894／psram 0 | int 5,664／psram 529,518 |
| `nc4.reply0/1` | INTERNAL | **psram** |
| `ncm.arena` | fail | ok（524,288 B） |
| `ncm` 任務 | started=0、arena=0 | **started=1、arena=524,288、loop 跑（frames 2,469）** |
| release 內部 heap | 169,035（舊 WSL 映像） | **174,015**（回覆幀搬去 PSRAM，+4,980） |

**③ `resources.json` seed（handoff 的「待下一次 OTA 驗證」）：兩次 OTA 後都係 miss=0**

- 舊 release（`8cbaf00f`）：`cfg.res.miss=1`。
- 第一次 OTA（test，含 `k_min_resources`）開機後：`miss=0`，
  `/Volumes/NC-2F2C/config/resources.json` = `{"version":1,"resources":[]}`（38 B）。
- 之後兩次 OTA（test→test、test→release）都保留（seed 只補缺檔）：release 亦
  `res.miss=0`、`def/inv/unk` 全 0。
- `ncm` key：release = `{}`（ncm 只在 `NC_ENABLE_TESTS` 建置）；test = 完整
  `{arena,used,live,frames,backend,period_ms,task_core,modules}`。與 handoff 預期一致。
- 補：`resources.json` 的 **project preset 現在真的會嵌入 firmware**（`main/CMakeLists`
  的 `foreach(_cfg system network tasks resources)` ＋ main.c 的
  `#if NETCORE_HAS_PRESET_RESOURCES`；之前 ports/project 下的 resources.json 是死檔）。
  刪掉板上的 resources.json 再開機，實測 seed 出 **519 B** 的 project preset
  （帶 `_comment` 說明；之前只有 38 B 骨架）。`make.py check` 亦把 resources.json
  加入已知層清單（13 → **5** 個警告，餘下 5 個是本來就有的「冇 driver.json」）。

**④ 主機測試（macOS）：12 支全過**

- `tools/hosttest/run_all.sh`：**12/12 OK**（之前 macOS 有 2 支失敗）。
- `mod_test`：`tools/hosttest/mod_test/include/dirent.h` 是 MSVC shim，但非 Windows
  一樣被 `-I` 搶先找到而內容係空 → `DIR` undeclared。加 `#include_next <dirent.h>`
  交返系統 header。
- `cfgstruct_test`：① macOS 的 ld64 對未定義 weak function 仍然報 undefined symbol
  → `stubs.c` 補 `nc_port_hw_defaults()`（回 NULL，語義＝冇 build-time 生成檔）；
  ② **補 resources 層測試**（handoff §5）：缺檔／正常 5 條／未知 type／名太長／
  冇 provider／負 bytes／version 不符／非陣列／空陣列／超 16 條／未知鍵，外加真的
  project preset `resources.json`；178/178 通過。
- 其餘 10 支（cfg、ncd、task、router、link、hub、plan、buf、portgen、ncm）本來就過。

**⑤ 未做**

- `fps` 由效果移除、新增 `pixel` 消費者 task（handoff 第 3 項）——未開始。
- `resources.json` 的 `provider` 對 `nc_task_started()` 的檢查已存在（`nc_res_merge`），
  但 `pixel` 未出現前表保持空；現時板上正是空表（符合 handoff 第 4 項的先後次序）。
- 本輪只驗到「ncm 任務起得返、arena 拿得到」；`.nct` / `.nce` 真正掃描→declare→attach
  仍未有樣本檔上板，留待 pixel／effect 模組可用後一併驗。

### 7.82 2026-11-17 深夜（外部任務真的跑起來：WAMR 引擎、任務模組 ABI v1、列印＋GPIO 驗證）

承接 §7.81 的下一步：**唔測效果，先測「任務管理器能唔能夠支援外部任務」**。
目標係一個由 `/data/tasks/*.nct` 載入、經執行引擎跑、可以印字同讀／反轉 GPIO 的
外部任務，並且用「驗證列印」收貨。

**① 接了什麼**

| 層 | 位置 | 內容 |
|---|---|---|
| 引擎 | `test_mod_tasks/nc_mod_engine.c` | WAMR（interp）backend：init／load／render／unload／deinit；大 allocation 去 PSRAM、細塊留 internal（custom allocator, 16-byte 對齊） |
| 介面 | `nc_mod.h` | `nc_mod_backend_t` 加可選 `load`／`unload` hook；loader 喺 payload 驗證（header／manifest／CRC）之後才叫引擎實例化 |
| ABI | `nc_mod_abi.h` | **任務模組 v1**：export `task_tick`（必需）／`task_init`（可選）；import 白名單 6 個：`nc_print`、`nc_print_i32`、`nc_print2`、`nc_gpio_read`、`nc_gpio_write`、`nc_gpio_toggle` |
| 安全 | 同上 | GPIO 只准安全腳位：0／3／45／46（strap）、7／8（本板 UART）、19／20（USB）、26–37（SPI flash／octal PSRAM）、43／44（UART0）一律拒絕；其餘 0–48 開放 |
| 排程 | `main.c` | engine init + `nc_mod_load_all()` 喺一次性 8 KB `ncm.boot` task 做（main task 只有 3 KB 棧，WAMR 會 stack overflow）；ncm loop 棧 4 KB → 8 KB |
| 目錄 | `main.c` seed | 加 `/data/tasks/`（同 `/data/pixel/effects/` 一樣係模組註冊表） |

`idf_component.yml` 加 `espressif/wasm-micro-runtime ^2.4.0`（rule: `target == esp32s3`）。
WAMR 2.4.0 對 IDF 6／picolibc 有 3 個位編唔過／跑唔起，補丁放
`tools/patch_wamr.py`，由 `firmware/CMakeLists.txt` 喺 `project()` 之後自動套
（`managed_components/` 唔入 git；腳本 idempotent，所以上游每次重新抓都補得返）：

| 補丁 | 原因 |
|---|---|
| `espidf_file.c`：`#include <sys/stat.h>`＋補 `S_IS*` 巨集＋宣告 `renameat` | IDF 6 標頭收緊、picolibc 冇呢啲；platform 係 glob 收檔，關 WASI 都照編 |
| `espidf_platform.c`：`#include <sys/stat.h>` | `fstatat()` shim 的參數用 `struct stat` |
| `espidf_thread.c`：`os_self_thread()` 改 `xTaskGetCurrentTaskHandle()` | IDF 的 `pthread_self()` 只認 `pthread_create()` 開的 thread，我哋由 FreeRTOS task 呼叫 → 直接 assert |
| `espidf_memmap.c`：`#include "esp_heap_caps.h"`＋`MALLOC_CAP_EXEC` fallback | 冇 PSRAM 的 S3 板（Matrix／SuperMini）行唔到 dual-bus 分支，而 IDF 6 個 macro 要 `CONFIG_HEAP_HAS_EXEC_HEAP` 才有 → 編唔過 |

**② 樣本模組（`tools/ncm_samples/`）**

`hello_task.wat` → `wat2wasm` → `tools/ncm.py pack --kind task --fps 1 …` →
`out/hello_task.nct`（871 B：payload 259 B wasm、meta 540 B、budget 20 ms）。
`task_tick` 每 tick 叫 `nc_print2("tick pin21 read=", gpio_read(21), " toggle=", gpio_toggle(21))`。
上板方式：複製去 MSC 的 `/tasks/` → reboot（開機掃描時載入）。

**③ 驗證（S3 Mini-Nano，USB NC4；映像 app `c066da30`，tests）**

```
$ tools/nc4.py status ncm
{"arena":524288,"used":259,"live":"hello_task","frames":…,"backend":"wamr",
 "period_ms":1000,"modules":[{"name":"hello_task","fmt":"wasm","ok":1,"bad":0,
 "kind":"task","fps":1,"budget_us":20000,…}]}

$ tools/nc4.py status tasks     # 外部模組以「租戶」登記，唔係服務
{"name":"hello_task","kind":"normal","level":4,"started":1,"runtime":1}

CDC log（1 Hz，讀 GPIO21＋反轉 GPIO21）：
  ncm.wamr: [hello_task] tick pin21 read=1  toggle=0
  ncm.wamr: [hello_task] tick pin21 read=0  toggle=1
  …交替
```

即係：檔案系統 → 掃描 → 建表 → attach → WAMR 實例化 → 定時 tick →
host import（列印／讀 GPIO／反轉 GPIO）全條鏈都通。`ok:1` 同 `backend:"wamr"`
亦令 `status` 由「登記得到、跑不到」變成真跑。

**④ 三個「只有上板才見到」的坑（全部已修）**

1. **WAMR 會 qsort 你俾佢的 NativeSymbol 陣列**：放喺 flash rodata（`const`）→
   `Cache error: Dbus write to cache rejected`，840 ms 就 panic reboot。
   backtrace 指到 `wasm_native.c: register_natives`。改成 RAM 陣列。
2. **`os_self_thread()` → `pthread_self()` assert**：`wasm_runtime_call_wasm()`
   每次都會 `wasm_exec_env_set_thread_info()`，而 IDF 的 `pthread_self()` 對非
   pthread thread 直接 assert（`pthread.c:583`）。用第三個補丁修。
3. **WAMR 由 flash 執行程式碼 ↔ OTA 抹 flash 撞 task watchdog**：
   1.3 MB 映像 OTA 期間模組照 tick（1 Hz）→ BEGIN 的同步 erase（≈5 s）／WRITE
   同另一核 park 撞 → **rst=6（TWDT）**、OTA 中斷（裝置 reboot 返舊 slot）。
   修法：`nc_ota_action_paused()`（BEGIN 一入嚟就 true，END／ABORT 清），
   engine 喺 session 期間跳過 tick。實測之後 1.3 MB OTA 帶住模組跑都成功
   （BEGIN 4.8 s、WRITE 6.0 s 215 KB/s、END ok、rst=3）。

另外 `tools/ncm.py` 有個真 bug：meta 字串用「char 截斷再 encode」，中文字
（3 bytes）會超標，令 meta 區塊由 540 B 脹到 561 B、之後全部欄位偏移。
改成 byte-level `trunc_utf8()` 並喺 `build_meta()` 斷言長度。

**⑤ 代價（S3、tests 映像）**

| 項 | 數 |
|---|---|
| flash | 1,224,432 → **~1,315,000 B**（WAMR interp＋引擎，+92 KB） |
| 內部 heap（boot 後） | **~15 KB**（WAMR runtime 結構＋每 module slot 細；linear memory 去 PSRAM） |
| PSRAM | arena 512 KB＋linear memory 64 KB／模組＋WAMR 大 allocation |
| 每 tick 成本 | task_tick＋5 個 native（實測無 strike、budget 20 ms 用唔到 1%） |

**⑥ 未做（下一步）**

- manifest 的 `cmd` 表 → 綁 guest export（模組自訂 NC4 指令）未接：而家模組只有
  `task_tick` 週期執行，不能由主機叫函數。
- AOT（`fmt=aot`／`wamrc`）未接（P3）；而家只收 wasm。
- `resources.json` 的 provides/requires 時序：B06 declare 早過 B08 attach，
  所以模組自己 provide 的資源會喺 merge 時被標「provider 未啟動」。要嘛延後
  merge，要嘛 provider 檢查放寬到「今次開機已掃到」。pixel 任務出現前一張
  空表仍然係正確次序。
- 每核一個引擎實例（P5）未做。
- `make.py build --all`：8 塊板全部建置成功（C3／P4 release 唔受影響；WAMR 只在
  S3 的依賴圖，release 唔 link）。
- `hello_task` 的 GPIO21 只係示範腳位；board 冇外接嘢，read/toggle 用
  `GPIO_MODE_INPUT_OUTPUT` 讀返自己（輸出同時保留輸入感測）——如果唔用
  INPUT_OUTPUT，`gpio_get_level()` 喺純輸出腳永遠讀 0，會令「反轉」變成
  單方向（實測踩過）。

**⑦ 診斷提示（重要，寫給下一次）**

`console=none`＋OTG 之下，B01–B08（USB 之前）的 log 同 panic backtrace 一律收
唔到，WAMR 開機 crash 的症狀只係「USB 唔見咗」。懷疑有開機 crash 時，暫時把
`ports/hardware/S3/ESP32-S3-Mini-Nano/sdkconfig.defaults` 的
`CONFIG_ESP_CONSOLE_NONE=y` 換成 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`，
由 ROM download mode watchdog-reset 冷開機，就可以用 USJ console 拿 backtrace；
修好後還原。

### 7.83 2026-11-17 深夜（收納定案：L0 外掛 → L1 AOT → L2 原生 → L3 硬件；首次 wasm vs C 實測 14.8×）

**① 定案（模組收納流程）**

- 新功能／測試**先做外掛模組**（通用 `.nct`，L0 wasm 直譯），喺 tests 固件驗證。
- 測試冊驗收通過後，按「量到嘅性能需要」**分層收納**：
  **L0** 外掛 wasm（通用、≈15× C）→ **L1** 外掛 AOT（同容器 `fmt=aot`、per-arch、預期 2–4× C）
  → **L2** 原生 C 固件組件（熱路徑／驅動／timing）→ **L3** 硬件加速（DMA／PPA／RMT／IRAM）。
- 每升一層要引用量測；保留上一層作 golden reference／回退；AOT 仍然受 import 白名單
  （唔算「加能力」）；原生能力只可以經 **OTA 固件**加，唔可以由 FAT 載入 native code。
- 文件：[module-adoption.md](../03_notes/module-adoption.md)、
  [test-register.md](test-register.md)（測試冊；收納入固件前一定要有一行）。

**② 首次同 workload 對照（S3 Mini-Nano、240 MHz、tests 固件）**

workload：50,000 次 LCG（`v = v*1664525 + 1013904223`，同一個 checksum）：

| 形態 | 實測（5 次） | 備註 |
|---|---|---|
| **L2 原生 C**：新測試指令 `0xF050 CPU_BURN` | **1,677–1,687 µs**（均值 1,683） | `__asm__` barrier 防優化；~8 cycles/iter |
| **L0 wasm 直譯**：`tools/ncm_samples/bench_task.wat` | **24,868–24,910 µs**（均值 24,887） | tick 內用新 import `nc_now_us()` 自量 |
| **倍數** | **≈14.8×** | 同一塊板、同一時段；兩個 checksum 都係 `1919499729` |

- 有一個 335 ms outlier（USB 擷取期間；不計入均值）。純 ALU workload，locals 喺
  internal exec stack，所以呢個倍數主要係**直譯器 overhead**，唔關 PSRAM 事。
- 順帶新增：`nc_now_us()` import（白名單第 7 個，= MPY `time.ticks_us()`）、
  `0xF050 CPU_BURN`（native 對照工具）、`bench_task` 樣本。

**③ 解讀**

- L0 適合「每 tick 唔爆 budget 的邏輯」；如果 tick 用咗 >30% budget → 升 L1 AOT；
  AOT 仍然 >30% → 升 L2 原生 C。實際門檻留待 AOT 數字出爐再校準。
- **AOT 未接**（`CONFIG_WAMR_ENABLE_AOT=n`；engine 明確拒 AOT）。P3 要：`wamrc`
  編 Xtensa、S3 PSRAM dual-bus 執行路徑、量 L1 倍數。AOT 唔等於原生 C
  （仍有 wasm 記憶體模型＋bounds check），「真·原生」只有 L2。
- 純 ALU 之外（大 framebuffer／streaming）要另外量 PSRAM 成本——緩衝管理器只
  解決 host 側搬資料，唔解決 guest linear memory 喺 PSRAM。

**④ 下一步**

1. capability provider 重構（`nc_cap_gpio`／`nc_cap_time` 先；之後 `nc_cap_led`）；
2. `nc_cap_led`＋`nc_led_show/info`，LED pattern 任務 L0 → driver 留 L2；
3. AOT spike（P3）；
4. per-module loop／prio／core（manifest 已有欄位，loader 只 attach 未調度）；
5. 測試冊逐項更新（T-NCM-008 fuzz、T-NCM-009 AOT、T-NCM-010 LED…）。

### 7.84 2026-11-17 深夜（定案：終極目標 = L3 硬件加速；L1.5 native ELF 可選但失去沙箱）

**① 討論定案（記錄）**

- viper／AOT／native ELF 的分別：
  - `@micropython.viper` = **runtime native emitter**（function 定義時即場編機器碼、放 IRAM、
    原始 pointer、冇 bounds check）→ 快，但只可信自己 code，**唔係 AOT**；
  - **L1 AOT** = 離線編機器碼，同一速度級（預期 2–4× C），但**仍然有沙箱**；
  - **L1.5 native ELF** = ~1× C，但**失去沙箱**（同 firmware 同一 address space、
    冇 MMU 隔離、crash = reboot、開機自動載入會 boot loop）。
- 「失去沙箱」的意思已寫入 [module-adoption §2.5](../03_notes/module-adoption.md)：
  記憶體隔離／能力白名單／失敗隔離三層全部冇；所以只可以俾自己寫／簽名 module。
- 官方工具：Espressif 有
  [`espressif/elf_loader`](https://components.espressif.com/components/espressif/elf_loader)
  （esp-iot-solution），支援 **S3/P4 在 PSRAM 執行 ELF／`.so`**、`dlopen/dlsym`、
  custom symbol table；所以「自己寫 OS 級動態連結器」唔係唯一路（亦唔係首選）。
  我們仍要做：收窄 symbol 白名單、ABI version、簽名／allowlist、boot safe mode、
  任務管理器整合、量 PSRAM 取指速度。估算 PoC 由幾星期降到幾日，但本質風險不變。

**② 終極目標文件**

新增 [goal-hardware-acceleration.md](goal-hardware-acceleration.md)：
- 「無敵」操作定義：**加功能唔加 CPU；加解像度唔加 jitter**；
- 三級定義（L1 可更新／L2 可收納／L3 硬件化）＋每級驗收；
- S3／P4 硬件能力表（IDF v6.0.3 `soc_caps.h` 實查）：P4 有 PPA／DMA2D／JPEG／ISP／CAM，
  S3 有 RMT／LCD_CAM／I2S／ADC-DMA／GDMA；兩者都有 AES/SHA；
- 5 個目標場景（LED 矩陣／相機 pipeline／LCD／音頻／密碼校驗）；
- 里程碑 **M1–M5**：M1 已完成（L1 外掛＋14.8× 量測）→ M2 AOT → M3 L2 收納 →
  M4 第一條 L3 pipeline（S3 RMT LED 或 P4 CAM→ISP→PPA→JPEG）→ M5「無敵」驗收；
- 量測方法、前置風險（接線／driver 成熟度／PSRAM 帶寬／信任邊界／fallback）。

**③ 測試冊新增**

T-NCM-013（L1.5 native ELF 可選 PoC）、T-NCM-014（S3 RMT LED L3）、
T-NCM-015（P4 CAM→ISP→PPA→JPEG）、T-NCM-016（LCD/顯示）。

**④ 下一步（照里程碑）**

1. **M2 AOT spike**（P3）：`wamrc`→Xtensa AOT→S3 PSRAM dual-bus 執行，量 L1；
2. capability provider 重構（`nc_cap_gpio/time`）＋`nc_cap_led`；
3. **M4 第一條 L3：S3 RMT LED matrix**（fps／CPU before-after）；
4. L1.5 native ELF 只作可選 PoC（簽名＋ABI 白名單齊備先做）。

### 7.85 2026-11-17 深夜（項目／工具重組：功能測試外掛化、效能測試留 dev、任務獨立編譯工具）

**① 固件目錄重組（目標：測試唔再混在固件）**

| 前 | 後 | 說明 |
|---|---|---|
| `components/tasks/test_mod_tasks` | **`components/host/ncm`** | 外掛宿主（loader＋WAMR）升為獨立 component；新 `NC_ENABLE_NCM` 開關（tests 自動開，release 預設 OFF） |
| `components/action/test_action` | **`components/dev/test_action`** | 開發／量測指令（0xF0xx） |
| `components/tasks/test_bench_tasks` | **`components/dev/test_bench_tasks`** | 效能測試 → 留 dev |
| `components/tasks/test_rgb_led_matrix_tasks` | **`components/dev/test_rgb_led_matrix_tasks`** | 效能／視覺測試 → 留 dev |
| `components/tasks/test_tcp_tasks` | **`components/dev/test_tcp_tasks`** | 吞吐效能 → 留 dev |
| `components/tasks/test_hello_tasks` | **刪除** | 功能測試改用外掛 `hello_task.nct` |
| `components/tasks/test_probe_tasks` | **刪除** | 測 host hub 內部 → hosttest `hub_test`＋`0xF003 HUB_BENCH` 覆蓋 |
| `main/idf_component.yml` 的 WAMR 依賴 | **`components/host/ncm/idf_component.yml`** | release 建置唔再 fetch／build WAMR（clean checkout 驗證：release lock 冇 WAMR；tests 才會 resolve） |

- `firmware/CMakeLists.txt`：`NC_ENABLE_TESTS` → `components/dev`；`NC_ENABLE_NCM` →
  `components/host/ncm`；WAMR patch hook 加 `EXISTS` 檢查。
- 注意（踩過）：ncm 一度放 `components/ncm`（第一層）→ IDF 自動當 component，
  C3/P4 因 WAMR rule 只限 S3 而 resolve 失敗；**任何 optional 組件要放第二層**
  （`components/<bucket>/name`）。
- `main.c`：移除 hello／probe include 同註冊；保留 bench／rgb／tcp 效能任務。

**② 任務獨立編譯工具**

- 新增 [tools/ncm_build.py](../../tools/ncm_build.py)：WAT／WASM（C 預留）→ `.nct`，
  讀 `<name>.build.json`（fps／budget／mem…）＋`<name>.meta.json`；`--list`／`--out`／
  `--clean`／`--device`；取代 `tools/ncm_samples/build.sh`。
- 樣本加 `api_cost.wat`（API 成本量測）；`hello_task`／`bench_task`／`api_cost`
  全部由工具建出（887／872／519 B）。
- README／tools/README／build_and_flash／testing／samples README 已同步；
  ncm 相關文件路徑全部改到 `components/host/ncm`。

**③ API 呼叫成本實測（回答「API 會唔會影響性能」）**

同一個 wasm task、N=2000，量四樣（空迴圈做底，扣掉迴圈本身）：

| 項目 | 實測（多次中位數） | per-call |
|---|---|---|
| 空迴圈（increment only） | 725 µs / 2000 | 0.36 µs / iter |
| `nc_now_us()` | 4,805 µs | **≈2.04 µs / call** |
| `nc_gpio_read(21)` | 5,440 µs | **≈2.36 µs / call** |
| LCG 算術（對照 bench） | 993 µs | 0.50 µs / iter（對得上 50k=24.9 ms） |

結論：**API 成本是「每次呼叫」固定開銷（≈2 µs），唔會拖慢模組自身運算**。
每 tick／每 frame 叫幾次（例如 10 次 = 20 µs/20 ms = 0.1% budget）完全可忽略；
**每 pixel／每 element 叫就係災難**（2000 次 ≈ 4–5 ms）→ 要 batching：一次交
buffer pointer＋length，host 一次過處理。

**④ 驗證**

- `make.py build --all`：**8 塊板全過**（C3 538 KB、P4 全部、S3 全部）。
- S3 tests 映像 1.31 MB（WAMR 只在 NCM）、release 1.14 MB（clean resolve 冇 WAMR）。
- 板子：`hello_task` live、`backend:"wamr"`、`ok:1`；api_cost 量測完已還原。
- 下一步：M2 AOT spike（P3）→ capability provider → M4 S3 RMT LED pipeline。

### 7.86 2026-11-17（外掛任務移出 firmware 樹：`modules/` ＋工具預設路徑）

**定案：外掛任務源碼唔屬於 firmware 專案。**

- 路徑重定義：`tools/ncm_samples/<name>.wat` → **`modules/<name>/`**（一個任務一個資料夾：
  `<name>.wat`／`build.json`／`meta.json`）。輸出 → `modules/out/<name>.nct`（gitignore）。
- `tools/ncm_build.py`：預設讀 repo 根 `modules/`；新增 `--dir` 指向**外部任務目錄**
  （外掛任務可以住在另一個 repo／專案，只帶工具去編譯）；`--device` 直接裝到
  MSC `/tasks/`。現有 3 個任務（hello_task／bench_task／api_cost）已搬好、重編通過
  （887／872／519 B）。
- firmware 樹（`firmware/`）而家只有宿主與驅動；`modules/` 係可搬走的資料/機器碼。
- 相關文件同步：README 目錄樹、tools/README、build_and_flash、testing、
  test-register（T-NCM-002/003/017 連結、§3.1）、module-adoption、ncm-contract、
  ncm-engine、handoff；新增 [modules/README.md](../../modules/README.md)。

**（附）指令動態插入的結論**

- 外掛**私有指令**可以「像模組一樣」在**載入時**插入（boot 或手動 reload），唔係
  任意 runtime hot-plug：`nc_action_init()` 仍然係開機統計→一次配置→排序→封存，
  外掛載入早於封存就加入，之後唯讀。
- 性能：dispatch 係排序陣列的二分搜尋（`nc_action_find`）→ 加 N 條指令只多
  log2(N) 次比較（幾十條 = 6–7 次）；**核心指令的處理成本完全唔變**。真正成本係
  「插件指令 → mediator → wasm 呼叫」的沙箱邊界（≈2 µs/call＋直譯成本），所以
  插件指令適合低頻控制／查詢，唔適合高頻 data path；慢指令要 async（回覆經 ncm
  任務）＋配額／timeout。

### 7.87 2026-11-18（命名規範：外掛任務 `modules/<role>/<name>`；action＝表格＋handler，唔加 `.nca`）

**① 外掛任務改名（嚴格命名，睇路徑就知係乜）**

```
modules/
├── tasks/                   → .nct（kind=task）
│   ├── test_<name>/         測試／量測（dev only，唔會收納入固件）
│   ├── demo_<name>/         示範
│   └── <name>/              真任務（production 候選）
├── effects/                 → .nce（kind=effect；預留）
└── out/                     編譯產物（gitignore）
```

- 現有 3 個模組改名：`hello_task` → **`modules/tasks/test_hello/`**、
  `bench_task` → **`test_bench/`**、`api_cost` → **`test_api_cost/`**；
  `<name>.wat`＋`build.json`＋`meta.json`；輸出 `modules/out/test_*.nct`
  （887／872／519 B），全部重編通過。
- `tools/ncm_build.py`：支援 role 目錄（`tasks`／`effects`），`--list` 顯示
  role／kind；輸出副檔名跟 kind（`.nct`／`.nce`）；`--dir` 外部專案；
  `build.json` 新增 `"cmds": [0x3xxx]`（外掛私有指令，vendor 區）。
- 文件：新增 [naming.md §7](../03_notes/naming.md)（外掛命名規則）；
  [modules/README.md](../../modules/README.md) 重寫；README／tools/README／
  build_and_flash／testing／test-register／handoff 全部同步。

**② `action` 的定義與 `.nca` 問題（結論）**

- `action` **唔係只有表格**：表格（`nc_action_t{cmd, handler, owner}`）只係索引，
  具體功能係各域自己寫的 C handler（`on_<動詞>`）。表喺開機統計→一次配置→排序→
  封存；`nc_action_find()` 二分搜尋。
- **唔加 `.nca`**：`.nct`／`.nce` 係可載入容器；指令 handler 係固件程式碼，
  唔可以／唔應該由外部載入（安全、實時、ABI）。外掛要加自己的指令，用
  `build.json` 的 `cmds` 宣告 **0x3000–0x3FFF vendor 區**，由 ncm 受控路由
  （未實作；配額＋timeout 一齊做）。
- [commands.md §1.1／§1.2](../01_protocol/commands.md) 已加入指令 ID 分區表
  （含外掛保留區）同「action＝表格＋handler」說明；rule 4 更新為
  「產品域 → `components/action/<域>_action`；測試 → `components/dev/test_action`」。

**③ 驗證**

- `tools/ncm_build.py --list`：3 個模組 role=tasks／kind=task；
- 全部重編 887／872／519 B；hosttest 12/12；`make.py check` 0 錯 5 舊警告。
- firmware 無改動（板子仍係 app 8007da05、`test_hello` 上板只係舊檔名；
  下次換模組時用新名 `test_hello.nct` 即可）。

### 7.88 2026-11-18（外掛模組命名收緊：leaf 尾綴＝角色；out/ 只放成品）

**定案**

- `modules/` 三個角色目錄：`tasks/`（`.nct`）、`actions/`（`.nct`，kind=normal，
  `cmds` 必須非空）、`effects/`（`.nce`）。
- **leaf 名一定要以角色結尾**：`<name>_tasks`／`<name>_actions`／`<name>_effects`；
  工具會驗證（flat 目錄則由後綴反推 role），唔啱直接報錯。
- **成品／中間產物分開**：`modules/out/` 只放最終 `.nct`／`.nce`（一入去就見到
  要收集嘅嘢）；`.wasm` 中間產物放 `modules/build/`。兩者都 gitignore。
- 現有模組改名：`test_hello_tasks`、`test_bench_tasks`、`test_api_cost_tasks`
  （資料夾／payload／容器同名）；重編 887／872／519 B。
- `modules/actions/README.md`、`modules/effects/README.md` 建立，寫明角色規則
  （actions 未實作路由前係保留位）。
- 工具：`--build-dir`（預設 `<root>/build`）、`--out`（預設 `<root>/out`）、
  `--clean` 清兩者、`--list` 顯示 role／kind；`--dir` 外部專案照樣可用；
  `build.json` 的 `cmds` 會傳 `--cmd`（非 0x3000–0x3FFF 警告）。

**驗證**

- `--list`：3 個任務 role=tasks／kind=task；`modules/out/` 只有 3 個 `.nct`，
  `modules/build/` 只有 `.wasm`。
- 黑板安裝 `test_hello_tasks.nct` → `status ncm`：`live test_hello_tasks`、`ok 1`
  （舊 `test_hello.nct` 已移除）。
- 文件同步：README／tools README／modules README／build_and_flash／testing／
  test-register（模組名同連結）／module-adoption／ncm-engine／handoff／naming §7。

### 7.89 2026-11-18（量測唔做任務模組；`tasks.json` 加 prio；資源／GPIO 現況）

**① 量測模組出 tasks → 入 `lab/`**

- 原則：**量測唔係任務**。host 內部量測用 dev 固件 native 指令（`0xF050` 等）；
  量測「外掛路徑」本身（interp／AOT、API 成本）係一次性實驗，放 `modules/lab/`。
- 改名：`test_bench_tasks` → `lab/bench_compare_lab`、`test_api_cost_tasks` →
  `lab/api_cost_lab`（leaf 尾綴 `_lab`；工具新增 lab role）；`tasks/` 只剩
  `test_hello_tasks`（smoke）。
- 量測數字已入測試冊（T-003／T-017），lab 模組量完可以移除、唔會收納。

**② `tasks.json` 新增 `prio`（可選）**

- schema：`{ "name", "level", "affinity": [c0,c1], "prio": 1..24 }`；
  `level` 管啟動次序／-1 停用、`affinity` 管跑邊核、`prio` 覆寫 loop 的 FreeRTOS
  優先級（冇寫 = 用 loop 宣告值）。
- 實作：`nc_cfg_types.h` 加 `has_prio/prio`；`nc_cfg_load.c` 解析；
  `nc_task.c` 記 `cfg_prio`、`prepare_loops()` 寫入 `rt->prio_override`、
  `nc_task_rt_start()` 用 override；status loop JSON 印**生效** prio。
- 治理不變：租戶（NORMAL／EFFECT）就算寫 7–15 都會被夾到 `NC_PRIO_OTHERS_MAX`（6）；
  SYSTEM 任務照 C 宣告、不受 tasks.json 影響。
- 各 project 的 `tasks.json` `_comment` 已補 prio 說明。

**③ 資源模型（回答「請投資源／提交什麼資源」）**

| 類型 | 宣告 | 處理 |
|---|---|---|
| 記憶體 | `nc_task_t.mem[]`／`declare()` | 開機 `nc_plan` 一次 carve（class／align／bytes）；`on_start` 收 `ptr/len/buf` |
| 硬體接線 claim | `nc_task_t.claims[]`（`pin:16`、`bus:spi1`、exclusive） | `audit()` 檢查獨占衝突；失敗的 claim 標記不算，衝突要人解決 |
| 對外 provide | `nc_task_t.provides[]`（buf／json／u32） | 登記到 provides 表；`STATUS_GET` 的 `provides` 可讀 |
| 外掛資源 | manifest `requires`／`provides` ＋`resources.json` | 掃描時合併；載入前 `nc_res_check()` 擋缺資源的模組 |

**④ GPIO 現況與缺口（重要）**

- 合約係「`hw_gpio` owner 唯一 writer」：其他人用 `nc_hw_send("gpio", CFG/SET)`、
  讀 `nc_hw_snapshot()`。
- **但外掛模組的 `nc_gpio_read/write/toggle` 直接 `gpio_config／gpio_set_level`**，
  繞過 owner：唔入 snapshot、冇 claim、狀態（pull／方向／默認電平）冇得明確指定，
  只靠 pin allowlist 擋危險腳位。→ 測試冊 T-NCM-019；
  建議方案（capability＋claim＋owner 統一寫入）寫入
  [hw-ownership §9](../03_notes/hw-ownership.md)。

**⑤ 驗證**

- hosttest 12/12；`make.py build --all` 8 板全過（prio 欄位不影響 release）。
- 上板：S3 Mini-Nano tests 映像（app `6b7ca0a4`）；`/data/config/tasks.json` 寫
  `{"name":"bench_a","level":4,"affinity":[0,1],"prio":3}` → 開機後
  `status tasks`：`bench_a` loop `core:1 layer:4 prio:3 running:1 b≈1891`（5 ms 週期），
  證實 override 生效；移除 `prio` 後回復預設 `prio:2 running:0`。
- 已還原板上 config（只留 `test_hello_tasks` level 4；缺的條目由管理器補 [0,0]），
  `status ncm`：`live test_hello_tasks`、`ok:1`。

### 7.90 2026-11-18（prio 收緊到 1–6；任務資源指南）

**① `tasks.json` 的 prio 上界＝6（唔可以搶核心保留區）**

- 原因：NC 核心（收／解／回）獨佔 **7–15**（[nc_prio.h](../../firmware/components/sys/task/include/nc_prio.h)）；
  租戶 config 如果可以寫 8，就會出現「偶爾慢」呢類極難查的搶佔。
- 實作（縱深防禦三層）：
  1. `nc_cfg_types.h` 定義 `NC_CFG_TASK_PRIO_MAX = 6`；
  2. `nc_cfg_load.c` 只接受 **1–6**，其餘（0、7、24…）當無效 → 用 loop 宣告值；
  3. `nc_task.c` 加 `_Static_assert(NC_CFG_TASK_PRIO_MAX == NC_PRIO_OTHERS_MAX)`；
     runtime `nc_task_rt_start()` 仍然會夾租戶 prio 到 ≤6（SYSTEM 例外）。
- 8 個 project 的 `tasks.json` `_comment` 改做「prio：可選，1–6…」。
- 上板驗（S3 tests，app `1b275806`）：`bench_a`：
  - `prio:3` → loop `prio:3 core1 running:1`；
  - `prio:10`（無效）→ 回落 loop 宣告值 `prio:2`；
  - `prio:5` → loop `prio:5`。
  已還原板上 config（bench 任務關閉、`test_hello_tasks` live ok:1）。

**② 新增任務資源指南**

[docs/03_notes/task-resources.md](../03_notes/task-resources.md)：四類資源
（記憶體 declare／硬體 claims／provides／外掛 requires＋resources.json）的
「怎樣宣告 → 怎樣配對 → 怎樣提交」、B06/B08 配對順序、GPIO owner 契約同
現況缺口（T-NCM-019）。hw-ownership §9 已有缺口說明。

### 7.91 2026-11-18（網絡緩衝預設審核：全部對齊量測最優；用戶唔需要調）

**① 審核結果：固件預設已經是量測最優，但有三處唔一致**

| 項目 | 之前 | 之後（跟量測） |
|---|---|---|
| CDC／USB | 4,128／1,024 chunk／4×256 | 不變（已最優） |
| UART | 1,024／4×256／driver ring 1,024×2 | 不變（已最優） |
| TCP | 16,410／2×8,205 | 不變（已最優） |
| UDP | 8,205／1×8,205 | 不變（已最優） |
| **WS（config 路徑）** | 16,384／2×8,192 | **32,768／2×16,384**（高頻 batch 建議） |
| **S3-Matrix project override** | tcp1 1×8,205、tcp2 1×8,192、udp 1×4,096 | **移除 override**，跟固件預設 |
| **legacy fallback**（`nc4_line_net`／`nc4_ws`／`nc4_udp`） | TCP/UDP 8,208＋2,048×4、WS 24 KB、UDP 16 KB | 對齊共同 profile（2×8,205／1×8,205／WS 16 KB／UDP 8,205） |

- 仍然保留：`NC4_MAX_BUSES 8`（P4-ETH 同時 5 條線，唔可以減）、plan 宣告 16 格、
  UART driver ring 1,024×2（TX 只係命令／回覆用）。
- 文件：[net-results §6](../03_notes/net-results-2026-11-15.md) 新增「固件預設＝最優」
  對照表＋「幾時仍然要寫 rx」清單。

**② 用戶需唔需要設定？→ 正常唔需要**

- 預設 = 量測最優；`links[].rx` 只係 escape hatch，project 唔應該重複寫（single source
  of truth 在固件）。
- 只有五種情況值得覆寫：①換線路參數（baud／MTU／socket buffer）；②大幀 profile
  （要過 4 KB 的 OTA chunk → CDC region ≥4,115）；③RAM 壓力（C3／無 PSRAM 同時多線
  → 收細 slots）；④未量過的新 transport（BLE／ESP-NOW／自訂）；⑤診斷／實驗（臨時
  加大 slots 量 burst，量完還原）。
- 判準：先量 `STATUS keys=net` 的 `max_dlen`／`slot_full`／`peak`／`cfg_src`；達標率
  ≥90% 就停。實測證明「加大緩衝」對吞吐冇幫助（TCP slots 1 KB→4 KB 一樣 490 KB/s）。

> **路徑註記（2026-11-18 稍後）**：§7.92–§7.94 寫的是當時的佈局（`webmaster/` 在 repo 根、
> 啟動腳本散在根目錄）。同日稍後已搬到 **`web_tools/`**（程式在 `web_tools/webmaster/`）——
> 睇 §7.95。以下保留原文，當作歷史。

### 7.92 2026-11-18（網頁工具提升一級：ESP WebMaster 搬到 repo 根，一鍵開）

- `tools/esp_webmaster/` → **`/webmaster/`**（repo 根）。理由：`tools/` 太多命令，
  網頁工作臺係「人用嘅入口」唔應該埋在工具堆；搬到上層之後路徑即係功能名。
- 一鍵啟動：
  - **`./web.sh`**（macOS/Linux）：自動建 `webmaster/.venv`＋裝 deps（`launch.py`）
    ＋開瀏覽器 → http://127.0.0.1:8010/；`./web.sh --sim` 免硬件、`--serial PORT` 直連；
  - **雙擊 `WebMaster.command`**（macOS）同一個流程，Terminal 會停住俾你睇 log。
- 文檔：root README 目錄樹＋「我要找的東西」加 WebMaster 入口；tools/README 頂部
  加「快速入口」（明確講已搬走）；webmaster/README 重寫啟動段；esp-webmaster-test
  guide 同 file-transfer／usb-config 的舊路徑更新。
- `.gitignore` 加 `.venv/`、`.pytest_cache/`（launch.py／pytest 產物）。
- 驗證：`webmaster/run.py --sim` 真係起得到（curl 200）；webmaster pytest 全過；
  `bash -n web.sh WebMaster.command` 通過。

### 7.93 2026-11-18（WebMaster 可讀 `.nct`／`.nce`／`.nca` 容器：header／manifest／meta）

- 新增 `webmaster/container.py`：純 Python NCM 容器解析（48 B header＋40 B manifest＋
  cmd table＋540 B meta），驗 magic／ABI／版本／區塊界線／payload CRC；未知副檔名
  （例如 `.nca`）照樣解析但提示「裝置只掃 `.nct`／`.nce`」。
- 新 API：`GET /api/container?path=…`（server 讀 FAT，回 JSON；壞檔仍然 200 +
  `valid:false` + 錯誤原因，UI 可顯示）。
- IDE 檔案樹：`.nct`／`.nce`／`.nca` 唔再當二進位檔下載，改開**容器檢視器**：
  - 檔頭（magic／flags／payload CRC／target）；manifest（kind／core／layer／prio／
    fps／px／mem／stack／budget／宣告指令）；meta（name／desc／usage／requires／
    provides）；驗證列表＋原始位元組前 96 B；
  - 容器 tab 停用儲存／格式化／驗證（唔會改壞 binary）；下載／刪除照常。
- `--sim` 模擬裝置：如果 `modules/out/` 有樣本，會 seed 一份 `/tasks/test_hello_tasks.nct`，
  免硬件都可以試容器檢視。
- 測試：`webmaster/tests/_ncm_sample.py` 造合法／壞容器；`test_container.py`（parser）
  ＋`test_server.py::test_container_view`（API）；**pytest 50 passed**。
- 驗證：重啟 server 後 `/api/container?path=/tasks/test_hello_tasks.nct` 回傳
  `valid:true`、kind=task、meta name／requires／provides（見下方板上實測）。

### 7.94 2026-11-18（WebMaster 一級入口：啟動腳本頁、Windows 啟動、格式化收 server）

**① Windows 啟動腳本（之前只有 macOS／Linux）**

- 新增 `WebMaster.bat`（雙擊即用）＋ `web.ps1`（等同 `web.sh` 的 Windows 版）：
  揀 Python（`py -3` → `python` → `python3`，用 `--version` 探測）→ 建
  `webmaster\.venv` → `pip install -r requirements.txt` → 起 server → 開瀏覽器。
- 三個踩過的坑（全部已修，寫進檔頭）：
  - **`.ps1` 冇 BOM 會被 PowerShell 5.1 當 ANSI 讀** → 中文註解亂碼令整個檔
    `Missing closing '}'`；`.ps1` 一律存 **UTF-8 with BOM**。
  - **`.bat` 係 cmd 用主控台代碼頁解** → UTF-8 中文會亂碼／爆語法；`.bat` 一律
    **純 ASCII**（中文訊息交由 `web.ps1` 輸出）。
  - **PS 5.1 傳 `-c "…"` 給原生程式時含引號的字串會被拆爛**（Python 回
    `SyntaxError: File "<string>"`）→ 探測改用 `--version`；`-c` 的內文唔可以
    有雙引號。
  - **cmd 唔接受 `cmd1 && ( 多行 ) || ( 多行 )`**（實測「此時不應有 (」→
    整個 launcher 直接死）→ 一律用單行 `&&` 或 `if errorlevel` ＋ `goto`。
  - `pip` 用 `--no-cache-dir`：本機 pip wheel 快取目錄可能被 ACL 鎖住（WinError 5），
    寫快取會令整個 install 失敗。
- 其他平台入口補齊：`build.sh`（Linux 建置）、`run_web.sh`（Linux 通用）、
  `make.bat`（Windows 建置）；`web.sh` 檔頭加平台對照。

**② UI 新增「啟動」頁（分類層級）**

- 左邊 rail 第一項 **🚀 啟動**；冇 hash 時它係預設頁（連上裝置才跳去「設備」）。
- 三層結構：**分類 → 平台 → 腳本卡片**。分類固定四層（空分類唔顯示）：
  網頁工作臺／建置-燒錄／板子-模擬器／AI-整合。
- 平台分段（Windows／macOS／Linux／通用）＋ **自動偵測**（UA）：自己平台的卡片高亮
  「你嘅平台」，其他平台變暗；可勾「只顯示我嘅平台」。`.sh` 顯示 **macOS／Linux**。
- 每張卡有：中文摘要、**可直接複製的指令**（`.\WebMaster.bat`、`./web.sh`，唔含本機
  絕對路徑）、提示、參數 chips、**睇內容**（只讀全文，唔會執行）。
- 後端 `webmaster/scripts.py`：分類／平台／摘要／參數全部有規則可循（檔名關鍵字、
  副檔名、`param()` 區塊、檔頭註解，支援 `#`／`REM`／`::`／`<# #>`，跳過 shebang／
  `@echo off`）。已知腳本用 `KNOWN` 表給中文文案（`.bat` 只能寫英文）。
- **安全**：`read()` 只准 repo 根直屬檔、副檔名白名單、拒絕 `../`、唔跟 symlink。
- API：`GET /api/scripts`、`GET /api/scripts/one?name=…`。

**③ JSON 格式化收歸 server**

- 新增 `webmaster/formatting.py` ＋ `POST /api/format`（唯一真相；UI 由它接手）。
- 壞 JSON **唔改內容**，回 `line`／`col`；非 `.json`、>1 MiB 直接拒絕。
- UI 的 `Ctrl/Cmd-Shift-F` 同「格式化」按鈕改走 API，toast 顯示
  「第 N 行，第 M 欄」與位元組變化。

**④ 其他**

- `test_flash.py` 兩個測試由 pytest `tmp_path` 改用 `workdir` fixture：
  Windows sandbox 會為 `tmp_path` 加「只俾建立者」ACL，`scandir` 會 WinError 5
  （之前一直有 2 個 error）。
- 測試：新增 `test_scripts.py`（分類／平台／參數／防穿越）、`test_scripts_api.py`
  （API＋repo 根唔存在）、`test_format.py`（輸出／壞 JSON 位置／大小上限／API）；
  **pytest 84 passed ＋ 1 skipped**（symlink 需要管理員權限，Windows 會 skip）。

**⑤ 驗證**

- Windows 實機：雙擊流程 `WebMaster.bat → web.ps1 → venv → deps → server` 全程通；
  `.\web.ps1 -Sim -Port 8019` 起得到，`/api/info` 200。
- 瀏覽器實測（ego-browser，127.0.0.1:8019）：啟動頁 4 個分類／7 張卡、平台徽章
  `Windows`／`macOS／Linux`、自動偵測＝Windows、`2/5` 適用數、複製指令、睇內容（771 B）。
- IDE：`/config/system.json` 壓成一行 → 格式化 → 11 行標準 JSON（66 B → 104 B）；
  壞 JSON 測試 → toast「第 2 行，第 8 欄：Expecting value」且內容不變。

### 7.95 2026-11-18（網頁工具收納：全部收進 `web_tools/`，根目錄只剩建置工具）

**① 目錄定案**

之前網頁工具「散落一地」：程式在 `webmaster/`、5 個啟動腳本散在 repo 根，
同與 web 無關的 `make.py`／`build.sh`／`make.bat` 混在一起。

```text
repo/
├─ make.py  build.sh  make.bat     建置（同網頁工具無關）
└─ web_tools/                       ← 網頁工具全部喺呢度
   ├─ README.md                    唯一入口文件（原本 webmaster/README.md）
   ├─ WebMaster.bat  web.ps1       Windows
   ├─ web.sh  run_web.sh           macOS／Linux
   ├─ WebMaster.command            macOS 雙擊
   └─ webmaster/                   server ＋ 前端（程式本體）
      ├─ run.py launch.py server.py scripts.py formatting.py container.py …
      ├─ static/  tests/  requirements.txt
      └─ .venv/  .simfs/           生成物（gitignore）
```

- 全部用 `git mv`（歷史保留）；`.venv` 先刪（venv 唔可以搬，會留絕對路徑）再由
  `web.ps1` 重建。
- **根目錄唔留跳板**（用戶決定）：要雙擊就入 `web_tools/`；文件與 UI 都講清楚路徑。
- 好處：`git status`／新人都只會見到一條界線（建置 vs 網頁工具），
  唔會再問「web.ps1 同 make.py 有咩關係」。

**② 路徑假設修正（搬移唔止係 `git mv`）**

| 位置 | 之前 | 之後 |
|---|---|---|
| `server.py` REPO_ROOT | `BASE_DIR.parent`（=webmaster 上一層） | `BASE_DIR.parent.parent`（repo 根） |
| 啟動腳本掃描根 | 只有 repo 根 | **兩條**：`<repo>/web_tools` ＋ `<repo>`（新增 `script_roots()`） |
| `sim.py` 找 `modules/out` 樣本 | `parents[1]` | `parents[2]`（多了一層） |
| `tests/test_container.py` | `parents[2]` | `parents[3]` |
| `scripts.py` | 單根 `discover(root)`／`read(root, name)` | `discover(roots)` 多根；`rel` 由 repo 根起算（`web_tools/web.sh`、`make.bat`） |
| `read()` 防穿越 | 只准單檔名 | 准 `<root>/<檔名>` 或 `<root>/<一層>/<檔名>`，仍然拒絕 `..`／絕對路徑／symlink |
| API | `?name=web.sh` | `?rel=web_tools/web.sh`（亦可 `?name=` 由 server 找；兩者皆無 → 400） |
| UI 命令 | `.\WebMaster.bat` | `.\web_tools\WebMaster.bat`（由 repo 根起算，貼落終端就用得） |
| UI 卡片 | 只有平台徽章 | 多一個**位置徽章** `web_tools/`（repo 根的腳本唔顯示） |

**③ 文件同步**

- `web_tools/README.md`（原 `webmaster/README.md`）：加「呢個資料夾有咩」目錄樹、
  啟動表改為「`web_tools/` 內 vs repo 根」、API 改 `?rel=`。
- 根 `README.md`：目錄樹、啟動對照表全部改成 `web_tools/…`；「我要找的東西」加
  「網頁工具的文件／API／全部程式 → web_tools/README.md（唯一入口）」。
- `tools/README.md` 的「快速入口」、`docs/README.md`、`docs/03_notes/file-transfer.md`、
  `docs/03_notes/usb-config.md`、`docs/02_guides/esp-webmaster-test.md` 全部更新。
- §7.92–7.94 保留原本寫法（係當時的事實），但加咗「路徑已於 §7.95 搬到 web_tools/」註記。

**④ 測試與驗證**

- 測試改成真實佈局（`web_tools/` ＋ repo 根）：`test_scripts.py` 加
  「`rel` 由 repo 根起算」測試；`test_scripts_api.py` 重寫（多根、`?rel=`、
  10 種穿越全部 404、冇目標 → 400、repo 根唔存在 → 空清單而唔係 500）。
- **pytest 90 passed ＋ 1 skipped**。
- 語法：`web.sh`／`run_web.sh`／`WebMaster.command` `bash -n` 通過；
  `web.ps1` PS 5.1 parse OK、UTF-8 BOM 保持；`WebMaster.bat` 純 ASCII。
- 實機：由 repo 根跑 `web_tools\WebMaster.bat --port 8010` → 自動建 venv、裝 deps、
  起 server、開瀏覽器，`/api/info` 200。
- 瀏覽器：`/api/scripts` 回 7 個腳本、`roots = web_tools, esp_Net-Core`、
  `rel`／`location` 正確；啟動頁卡片顯示 `web_tools/` 徽章、指令係 `.\web_tools\web.ps1`。

### 7.96 2026-11-18（WebMaster 設定檔：基礎數值集中，port 被佔用自動換）

**① 問題**

`WebMaster.bat` 雙擊時報「Port 8010 已被佔用（可能係另一個 WebMaster）」並 exit 6，
要人手換埠。根因唔止係「仲有個 server 開住」，而係**四方各有預設**：
`web.ps1` 寫死 8010／`run.py` 又一個 8010／`web.sh` 寫死 `--open`／
沒有地方講「預設連 sim 定唔連」。改一個地方，其餘三個唔會跟。

**② 定案：`web_tools/settings.json`（＋`settings.local.json`）**

- **位置：`web_tools/`——同啟動器同層**（唔係收喺 `webmaster/` 入面）。改 port／預設裝置
  係「打開工具」那一層的事，打開 `web_tools/` 就見到；程式目錄只有**預設池**
  （`settings.py` 的 `DEFAULTS`），冇填的 key 就落返預設。
- 檔：`settings.json`（團隊預設，入 git）／`settings.local.json`（本機覆寫，`web_tools/.gitignore`）。
- **可以只寫想改的 key**（深層合併）：`{"server":{"port":8020}}` 就夠。
- **優先次序：命令列 > settings.local.json > settings.json > 內建預設池**；
  亦支援 `ESPWM_PORT`／`ESPWM_HOST`／`ESPWM_AUTOCONNECT`／`ESPWM_LOG_LEVEL`。
- 內容（`web_tools/webmaster/settings.py` 有完整 schema＋範圍）：

| 段 | key | 預設 | 說明 |
|---|---|---|---|
| server | `host` / `port` | 127.0.0.1 / 8010 | 綁定 |
| server | `auto_port` / `auto_port_scan` | true / 20 | **被佔用就自動揀下一個** |
| server | `open_browser` / `log_level` | true / info | |
| device | `autoconnect` | `"sim"` | `"sim"`／`"COM13"`／`null`（UI 自己揀） |
| device | `baud` / `sim_dir` | 115200 / null | |
| flash | `esptool_chip/baud/before/offset` | `""`／460800／usb_reset／0x0 | 固件頁預設 |
| ui | `default_view` | launch | 冇 hash 時開邊頁 |

- **launcher 唔再寫死任何值**：`web.ps1`／`web.sh` 只把你明確俾的參數轉交 `run.py`；
  `web.ps1` 先用 `run.py --print-config` 印一行「設定檔：port=… autoconnect=…」再啟動。
  （所以「改咗設定但仍然報 8010」呢類問題唔會再出現。）
- **壞值要大聲**：型別錯／超出範圍／枚舉錯 → 報錯並指出 key 同來源檔
  （`server.port` 收到 `"8010"` 或 `true` 都會即刻擋）；未知 key 只警告，
  `_comment` 之類靜靜容許。
- 新指令：`run.py --print-config`（睇實際值＋來源，唔起 server）、
  `--write-config [--force]`（產生帶註解的 settings.json）、`--auto-port/--no-auto-port`、
  `--no-open`、`--log-level`。
- 新 API：`GET /api/config`（唯讀，實際生效值＋邊個檔提供＋local 有冇存在）。
- UI「啟動」頁新增**目前設定**卡（11 個基礎值一眼睇完，可 ⟳ 重讀）。

**③ 行為對齊（避免改差）**

`device.autoconnect` 預設 `"sim"`：雙擊 `WebMaster.bat` 仍然「一開就有畫面（模擬裝置）」，
同之前一樣；要真板就 `-Serial COM13`（CLI 永遠贏）或改設定檔。

**④ 驗證**

- 實測自動換埠：先佔 8010 開一個 → 第二個自動去 8011；再開第三個 → 8012
  （log 有 `Port 8010 已被佔用 → 改用 8011`）。
- 設定檔：`--print-config` 正確顯示來源；`settings.local.json` 覆寫生效；
  環境變數覆寫生效；壞 JSON 報「第 N 行第 M 欄」；`"port": "8010"`／`"port": true`
  都被擋並指出 `server.port`。
- 測試：新增 `tests/test_settings.py`（18 支）——優先次序、壞值、bool≠int、
  未知 key 只警告、`write_default` 唔覆蓋、repo 內 settings.json 一定讀得通、
  `port_in_use`／`find_free_port`、`run.py` 的 CLI 預設一律 None。
  **pytest 108 passed ＋ 1 skipped**。
- UI：瀏覽器實測設定卡 11 項正確（`settings.json` 為來源、auto_port=開、autoconnect=sim）。


### 7.97 2026-11-18（WebMaster 真板優先：`autoconnect: "auto"`，唔再偷偷開模擬）

**① 問題**

`autoconnect` 預設 `"sim"`，所以每次開機都自動連一個**模擬裝置**；用戶插住真板
（COM20，`VID:PID=303A:1001` Espressif）卻見到 device strip 寫住「sim · sim」，
要自己斷線、揀 port、再連線。而 device 下拉更出現 `sim · sim` 這種重複標籤。

**② 定案：真板優先，搵唔到就唔連**

- `device.autoconnect` 預設改為 **`"auto"`**：
  - 掃 `serial.tools.list_ports`，只認**有 VID 的 USB 序列**，白名單
    `0x303A`(Espressif 原生 USB)／`0x10C4`(CP210x)／`0x1A86`(CH34x)／`0x0403`(FTDI)／
    `0x1B4F`／`0x2341`；
  - **排除**：Intel AMT SOL、藍牙 SPP（就算有 VID 都排除）、冇 VID 的虛擬／PCI 埠；
  - Espressif 原生 USB 優先；`prefer_port` 可以在 auto 時指定優先埠（但**唔會**讓
    非板埠過關）。
  - **搵唔到板 = 唔連**，log 印「未偵測到板子（USB 序列）」，UI 仍可自己揀 port。
    **唔會** fallback 去模擬。
- 新增 `settings.device.prefer_port`。
- 判斷邏輯只有一份：`webmaster/link.py` 的 `is_likely_board()`／`find_board_port()`；
  `/api/ports` 回 `board`／`suggested` 給 UI，前端唔再自己寫一套 heuristic。

**③ UI 唔再撈亂真板同模擬**

| 之前 | 之後 |
|---|---|
| device 下拉：`sim · sim` | `COM20` ／「模擬裝置（無硬件）」 |
| port 下拉：COM3（Intel AMT）排第一 | `★ COM20` 排第一並預設選中（★ = 疑似板） |
| 按鈕：模擬裝置 | 按鈕：**模擬（無硬件）**（tooltip 講明唔會碰真板） |
| 冇板時靜靜咩都唔做 | log 講「未偵測到板子」，UI 照樣可揀埠 |

**④ 驗證（真板：S3 Mini-Nano／COM20）**

- 開 `web_tools\WebMaster.bat` → log「自動偵測到板子：COM20」→ `/api/info`
  `connected:true sim:false port:COM20`。
- `/api/status?keys=sys,storage`：`chip=esp32s3`、`fw=0.1.2`、`app=08d1d1ac`、
  `heap=68,548`、`storage total=872,448 free=798,720`、`mode=app`。
- `/api/manifest`：10 個檔（`/config/*.json`、`/config/zero.txt` 等），真實 FAT。
- 終端有真板 log：`I (32486) netcore: t=… dt=1000002 us heap=67844`。
- `/api/ports`：`COM20 board=true suggested=COM20`；COM3／COM32／COM33 `board=false`。
- UI：device 下拉 `COM20`、port 下拉 `★ COM20` 排第一且被選中、ticket
  `esp32s3 / COM20 · app`、設備頁顯示真 chip／fw／heap／storage。
- 測試：新增 `tests/test_link_ports.py`（18 支）——VID 白名單、Intel AMT／藍牙排除、
  「冇 VID 唔算板」、「有 VID 但寫明藍牙都唔算」、`prefer_port` 指向系統埠時要忽略、
  Espressif 優先。**pytest 126 passed ＋ 1 skipped**。


### 7.98 2026-11-18（Windows cp950 崩潰 + 自動連線失敗要出聲）

**① `UnicodeEncodeError` 會整個程式收場（唔止亂碼）**

繁中 Windows 的 Python 用主控台代碼頁（`cp950`／Big5）輸出。只要字串有一隻
Big5 沒有的字，`print()` 就 raise `UnicodeEncodeError` —— **整個 process traceback**，
唔係「顯示亂碼」咁簡單。實測爆點：

```
run.py --print-config
  print(json.dumps(...))        # effective 裡面有 settings.json 的 _comment
  UnicodeEncodeError: 'cp950' codec can't encode character '\u55ba'
```

`\u55ba` = **「喺」**（註解文字有「插住嘅真板」「搵唔到就唔連」）。我之前的測試
全部帶了 `PYTHONIOENCODING=utf-8` 所以冇暴露——**用戶環境冇設就一定死**。

- 修 1：新增 `webmaster/console.py`（`setup()`），同 `make.py` 一樣**只放寬
  `errors="replace"`、唔改 encoding**（改 utf-8 會令 cp950 主控台整版變亂碼）。
  `run.py`／`launch.py` 一 import 就呼叫，**喺任何 print 之前**。
- 修 2：新增 `settings.public()`，把 `_comment*` 這類說明欄位從
  `--print-config` 同 `/api/config` 的輸出剔除——註解唔係設定值，少印就少一個爆點。
- 測試：`tests/test_console.py`（6 支）——先證明 cp950 stream 真係會爆，
  再驗證 `setup()` 之後「怪字變 `?`、正常中文照出」、`public()` 剝註解、
  `run.main(["--print-config"])` 在 cp950 stream 下 rc=0 且輸出無 `_comment`。

**② 自動連線失敗唔可以靜靜 `connected:false`**

場景：已經有個 WebMaster 開住 COM20，第二個實例（自動換 port 到 8012）再去開
同一個序列埠 → 開埠失敗。之前只有一行 `autoconnect 失敗：…` 印喺 console，
**UI 完全唔知**，用戶見到「未連線」會以為板壞。

- `server.py`：`State.autoconnect_error` + `_friendly_connect_error()`，把
  `Access is denied`／`busy`／`PermissionError` 翻譯成
  「個埠好似被另一個程式佔用（另一個 WebMaster／Arduino 監控／PuTTY？）」。
- `/api/info` 回 `autoconnectError`。
- UI：開機讀到就 `logLine` + `toast`（12 秒），唔會靜靜。
- 測試：`tests/test_connect_errors.py`（6 支）——佔用／timeout／未知錯誤的文案，
  以及 `/api/info` 在「連唔到」「冇設定」「連得到」三種情況的值。

**③ 驗證**

- 無 `PYTHONIOENCODING` 下 `python run.py --print-config` rc=0、純設定值。
- 無 `PYTHONIOENCODING` 下跑 `web.ps1`：`設定檔（web_tools\settings.json）：port=8011
  … autoconnect=auto` 正常印出。
- 真板（使用者把 `server.port` 設成 **8011**）：`connected:true sim:false port:COM20`、
  `chip=esp32s3 fw=0.1.2 heap=68,132`、`storage 798,720/872,448`。
- 第二個實例：`autoconnectError=連唔到 COM20：個埠好似被另一個程式佔用…`。
- **pytest 138 passed ＋ 1 skipped**。


### 7.99 2026-11-18（把自動偵測講出來：掃一次、唔逐個開埠、跳過要講原因）

**① 用戶問「係唔係一開就掃描全部，然後自動識別？」**

係——但要講清楚**掃幾多、做咩**：

- `serial.tools.list_ports.comports()` 讀的是**中繼資料**（埠名／description／
  VID-PID／序列號），Windows 上問 SetupAPI／registry，**幾十毫秒、一次性**。
- **唔會逐個埠開一次試**（開埠會打斷板上程式，亦可能搶走 Arduino 監控／PuTTY
  正在用的連線）。真正開埠只有最後揀中的那一個。
- 判斷＝有 VID 的 USB 序列＋VID 白名單（303A／10C4／1A86／0403／1B4F／2341），
  排除 Intel AMT SOL、藍牙 SPP、冇 VID 的虛擬埠；Espressif 原生 USB 優先。

**② 但之前 log 只講一句「自動偵測到板子：COM20」**

用戶唔知跳過了幾多個、為咩跳過。改成有結構的說明：

```
[esp_webmaster] 掃描序列埠：共 4 個 -> 疑似板 1 個（只讀 USB 中繼資料，唔會逐個開埠試）
[esp_webmaster]    選中 COM20（Espressif 303A:1001）
[esp_webmaster]    跳過：COM3=Intel AMT SOL、COM32=藍牙序列埠、COM33=藍牙序列埠
```

- `link.scan_ports()` 回結構化結果（total／boards／chosen／others／skipped＋原因），
  `link.format_scan()` 負責出人話；插兩塊板會多一行提示 `--serial COM21`。
- 新增 `run.py --list-ports`：只掃描並解釋，**唔起 server**。
- 用 ASCII 符號（`->`）唔用 `✓`／`·`：cp950 冇 `✓`（U+2713），一印就爆。

**③ 重定向到檔案改寫 UTF-8**

`console.setup()` 分兩種情況：

| 情況 | encoding | errors |
|---|---|---|
| 真主控台（tty） | 保持系統碼頁 cp950 | replace |
| 被 pipe／重定向到檔案 | **UTF-8** | replace |

所以 `WebMaster.bat > boot.log` 的日誌用任何編輯器都讀得明（原本寫 cp950 會亂碼）；
主控台仍然用 cp950（硬改 utf-8 會令整版中文變亂碼）。

**④ 驗證**

- `python run.py --list-ports` → 上面三行、rc=0、無 `PYTHONIOENCODING` 亦唔爆。
- 重定向到檔案後以 UTF-8 讀 → 中文完整正確。
- 所有新訊息用程式檢查過**全部可編成 cp950**。
- 真板（8011）：`connected:true sim:false port:COM20`、`autoconnectError=null`。
- **pytest 140 passed ＋ 1 skipped**（`test_console.py` 增到 8 支，涵蓋
  tty＝cp950／重定向＝UTF-8 兩條路）。


### 7.100 2026-11-18（前端拆模組；刪「啟動」頁；JS／Python 分工講清楚）

**① 刪 `#launch`（承認理解錯）**

原本把「快速啓動網頁腳本放進去瀏覽器」理解成「在 UI 裡列出 repo 的啟動腳本」，
做成 rail 第一項的「啟動」頁。實際用起來係多餘的——**你都已經打開工具了，
站內再教一次啟動有咩用**。已刪：

- HTML：rail 按鈕、`<section data-view="launch">`、對應 CSS（約 100 行）
- JS：`launch*`／`loadScripts`／`loadServerConfig`／`copyText` 全部移除
- `settings.ui.default_view` 唔再接受 `"launch"`（改成 `device|ide|drive|flash|monitor`，預設 `device`）
- 鍵盤快捷鍵改回 `1`–`5`（設備／IDE／隨身碟／固件／指令）
- **`/api/scripts` 保留**（AI／CLI 用），只係冇 UI。

**② 前端拆成 12 個模組（`static/js/`）**

本來 2,560 行單一 `app.js`，17 個區塊撈在一起。現在：

```
00-core(狀態/工具/api) 10-connection 20-terminal 30-status 40-files
50-drive 60-flash 70-console 75-shell(switchView+bindUI)
80-ide 85-container 99-app(init)
```

- **傳統 `<script>` 按檔名次序載入**，唔用 ES module、唔需要打包／build step；
  `index.html` 換成 12 個 `<script src>`，舊 `app.js` 刪除。
- 切割用行號機械式進行（唔手抄），每檔都有 `"use strict"`，`node --check` 全過。
- 新增 `tests/test_frontend.py`（8 支）：模組清單／載入次序（core 首、app 末）／
  每檔 `use strict`／`node --check`／**JS 的 `$("#id")` 必須存在於 index.html**／
  `#launch` 冇殘留／`api()` 路徑都係 `/api` 開頭。
  （拆檔最易中的錯就係「HTML 漏個 id」，瀏覽器只會 `Cannot read properties of null`。）

**③ JS／Python 分工（回答「會不會影響我 JS 的讀取」）**

| 層 | 做咩 | 平台差異 |
|---|---|---|
| 瀏覽器 JS | 只做畫面與互動 | 冇（瀏覽器本身跨平台） |
| server Python | 序列埠、檔案系統、`settings.json`、esptool、NC4 | 都集中在 `link.py`／`flash.py`；`serial.tools.list_ports` 自己處理 Win／macOS／Linux |

- 瀏覽器**冇權**讀序列埠或檔案系統，所以「讀 port／讀檔」只能在 server；
  JS 只接 JSON。
- 「這是不是板子」的判斷**只有一份**（`link.is_likely_board()`），經
  `GET /api/ports` 的 `board`／`suggested` 傳給前端；之前曾在 `app.js` 另寫一套，
  已收回。所以改 JS 不會跟後端打架。

**④ 驗證**

- 真板 8011：`connected:true sim:false port:COM20`；12 個模組全部 200；舊
  `/static/app.js` 已 404。
- 瀏覽器：rail 只剩 5 項、無 `#launch`；`window.*` 的 14 個代表函式全部係 function；
  逐頁切換（device／ide／drive／flash／monitor）都 render 正常。
- IDE 實測開真板檔案：`/config/system.json`（2,120 B、`JSON ✓`、12 個樹節點）。
- 指令頁實測真板：`0x1101` → `0x1102` 94 bytes。
- **pytest 148 passed ＋ 1 skipped**（新增 `test_frontend.py` 8 支）。


### 7.101 2026-11-18（第三條燒錄路徑：esptool-js；環境自檢＋一鍵裝 ESP-IDF）

**① 為什麼加第三條路，而不是取代第二條**

用戶問「可否用 esptool-js 燒錄，其後全靠 API」。技術上可以，但它**不會消滅**
「序列埠只能有一個主人」這個約束，只是把埠的工作從 server 搬到瀏覽器
（見 [`esptool-js` README](https://github.com/espressif/esptool-js)：建基於 Web Serial，
Chrome／Edge 89+，需 user gesture 選埠）。所以定位成**可選的第三條路**，三條並存：

| | ① NC4 OTA | ② server esptool | ③ 頁面燒錄（esptool-js） |
|---|---|---|---|
| 要放開埠 | 唔需要 | server 自動 | 手動交接 |
| AI 可燒 | ✅ | ✅ | ❌ |
| 適用 | S3／C3 日常 | P4／救磚／首燒 | 零安裝、Chrome |

**② 序列埠交接（新的狀態機）**

- `State.handover = {port, at, device, sim}`；交接期間 server **唔會**自己重開埠。
- `POST /api/port/release`（關 link、記錄交接）→ 瀏覽器 Web Serial 燒 →
  `POST /api/port/reclaim`（重連，模擬裝置亦處理）。
- `GET /api/port/state` 回 `{handover, port, ageSec, stale(>10 分鐘), browserFlash}`。
- 交接期間其他 API 回 **409 並講明原因**（原本會變成「未連線」這種誤導訊息）。
- 實測（真板 S3／COM20）：release 後**另一個 process 真的開得到 COM20**
  （證明埠真空出來）→ reclaim 後 `connected:true`。
- 設定 `flash.browser_flash`（預設 true）可整張卡停用。

**③ esptool-js vendored 而非 npm**

- `static/vendor/esptool-js/`：`bundle.js`（308.5 KB，pako 已內聯）、`LICENSE.txt`
  （Apache-2.0，vendor 必須保留）、`VENDOR.md`（版本／來源／怎樣重建）。
- `bundle.js` 是 **ES module**，本專案其餘是傳統 script → 加 `loader.js` 薄 shim
  把 export 掛上 `window.esptooljs` 並發 `esptooljs-ready` 事件。
- 要改行為就在 `static/js/65-browser-flash.js` 外面包（進度／錯誤中文化／交接／自檢），
  唔好改 minified bundle。

**④ 環境自檢＋安裝（`env.py`）**

- 11 項工具：python／pip／deps／esptool／git／cmake／ninja／node／**idf**／
  **idf_toolchain**／serial_driver；每項有 `status`／`version`／`impact`／`fix`。
- **五種能力分開講**（用戶最常混淆「燒到」與「build 到」）：
  `run_server`／`flash_nc4_ota`／`flash_esptool`／`flash_browser`／`build_firmware`。
  本機實測：`build_firmware=false`（冇 IDF），其餘四項 true。
- `cmake 4.x` 會附註「部分 IDF 版本要 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`」。
- API：`GET /api/env`、`POST /api/env/install`（**confirm 必填**）、
  `GET /api/env/install?since=N`（逐次讀新行，唔阻塞）。
- 安全：**唔會覆蓋非空又唔係 IDF repo 的資料夾**（回 `rc=-2` 並保留檔案）；
  目標晶片名驗證；命令以 list 傳 `Popen` 不經 shell；一次只准一個工作。
- 開機自動檢查一次（可用 `--no-env-check` 關），缺的項目印一句並指向 UI／API。

**⑤ UI**

- 新增 rail **「環境」**（第 6 項，鍵盤 `6`）：能力 grid、11 項工具表、
  安裝區（項目／晶片／路徑）＋即時日誌。
- 固件頁加第 4 卡「頁面燒錄」：三步按鈕、offset／baud／mode／freq／size／eraseAll、
  進度條、日誌、`?` 自檢（Web Serial／esptool-js／secure context）。
- 前端模組由 12 → **14**（+`65-browser-flash.js`、+`90-env.js`）。

**⑥ 驗證**

- pytest **174 passed ＋ 1 skipped**（+`test_port_handover.py` 8 支、`test_env.py` 18 支）。
- 真板：三條路徑能力全 true；release→外部程式開得到 COM20→reclaim 後重連；
  交接期間 `/api/status` 回 409 附「交畀瀏覽器」說明。
- 瀏覽器：`window.esptooljs` 有 22 個 export；`navigator.serial` 存在；
  環境頁顯示 11 行、能力 5 格；`#launch` 頁已不存在。
- 開機日誌：`環境：run_server=v flash_nc4_ota=v flash_esptool=v flash_browser=v build_firmware=x`。


### 7.102 2026-11-18（AI 也能發起頁面燒錄：佇列＋人在最後按一下）

**① 先釐清問題**

用戶問「頁面燒錄為什麼 AI 不能操作」。準確講法：

- **AI 本來就燒得到**（① NC4 OTA／② server esptool 兩條都係 REST）。
- ③ 頁面燒錄之所以「AI 不能」，唯一原因是 **Web Serial 的
  `navigator.serial.requestPort()` 必須有 user gesture**——這是瀏覽器安全模型，
  任何網頁都繞不過（不是本專案的限制）。
- 因此正確做法不是「讓 AI 假裝人手勢」，而是 **AI 下指令、人按一下、結果回 AI**。

**② 新增：燒錄請求佇列**

| API | 用途 |
|---|---|
| `GET /api/flash/paths` | 三條路徑的可用性＋`aiCanInvoke`＋`needsPortHandover`＋`humanGestureRequired` |
| `POST /api/flash/request` | AI 上傳 `.bin` 排隊（`offset`／`mode`／`freq`／`size`／`eraseAll`／`note`） |
| `GET /api/flash/request` | 目前 pending ＋ 最近 5 條歷史 |
| `GET /api/flash/request/{id}/image` | 瀏覽器下載映像去燒 |
| `POST .../claim`、`.../result`、`.../cancel` | UI 接手／回報（`ok`／`error`）／拒絕 |

- 狀態機：`pending → claimed → done|failed`（或 `cancelled`）；
  **同一時間只准一個 pending**，第二個回 409。
- 映像存 `webmaster/.flashq/`（gitignore、`ESPWM_FLASH_DIR` 可覆寫），
  **燒完／取消即刪**；上限 8 MiB；offset 驗證非負整數。
- UI：WS `flash_request` → 固件頁彈「AI 請求燒錄」橫幅＋toast；
  按「執行」＝claim → release 埠 → `requestPort()`（人手勢）→ `writeFlash`
  → reclaim → `result(ok)`。開頁時亦會 `GET` 一次，唔會漏掉離線期間的請求。
- 前端模組 14 → **15**（+`66-flash-request.js`）。

**③ 驗證**

- pytest **188 passed ＋ 1 skipped**（+`test_flash_request.py` 14 支：佇列／映像回讀／
  claim 兩次 409／result 成功失敗／取消／上限／空檔／壞 offset／未知 id）。
- 實機流程：`POST /api/flash/request`（X-Actor: ai，1 KiB 測試映像）→
  `GET` 見到 `pending / by=ai` → 重新載入 UI 自動彈出橫幅（截圖）＋toast
  「AI 請求燒錄（1.0 KiB）」→ 按「拒絕」→ `history: cancelled`、pending 清空。
- 員測試殘留：測試改為 monkeypatch `server.FLASH_DIR`，唔會再喺 repo 留 `.flashq/`。

**④ 定位（唔取代 ②）**

頁面燒錄仍然係「server 側冇 esptool／想用瀏覽器埠」時的備選。AI 用 ② 更快更穩；
③ 的價值是**唔需要 Python 也能燒**，以及可以由 AI 發起、人在最後確認。


### 7.103 2026-11-18（IDF 版本化安裝：v6.1.0 根本唔存在；Windows 官方支援）

**① 查證結果（唔靠記憶）**

| 問題 | 查證 | 結論 |
|---|---|---|
| IDF 有 Windows 版？ | `esp-idf/v6.1.1/docs/en/get-started/windows-setup.rst` HTTP 200；`install.ps1` 存在 | **有，官方支援** |
| `v6.1.0` tag 存在？ | GitHub API `git/ref/tags/v6.1.0` → **404**；`v6.1`、`v6.1.1` 存在 | **冇 `.0`**（慣例 `v6.1`＋`v6.1.1`） |
| Python 版本？ | EIM 先決條件頁：支援 **3.10–3.14**，「3.14 已在 Windows／macOS／Linux 完整支援」 | 本機 3.14.6 ✓ |
| 可裝進 server 的 venv？ | `install.ps1` 呼叫 `idf_tools.py install-python-env`，在 `%USERPROFILE%\.espressif\python_env\` **自建**環境 | **唔可以亦唔需要** |

**② 實作：版本化安裝**

- `env.fetch_idf_versions()`：GitHub tags，**快取 1 小時**，離線時回舊快取＋`error`（唔 raise）。
- `env.stable_versions()`：濾走 `-dev`／`-rc`／`-beta`／`-alpha`。
- `env.normalize_version()`：`latest`／精確 tag／補 `v`／**去掉 `.0`**／同系列取最新修補／
  未知版本只警告（回原文照試）。回 `(tag, 說明)`，說明會寫入安裝日誌。
- `clone_command(dir, version)` 加 `--branch <tag>`。
- `Installer.start(…, version=)`；已存在 repo **唔會**自動切版本（會提示）。
- API：`GET /api/env/idf/versions`（`force`／`prerelease`）、`POST /api/env/install` 收 `version`。
- UI：環境頁加**版本下拉**（實時 41 個 tag）＋「↻ 版本」重新拉；記住上次選擇；
  確認對話框會顯示將要裝的版本。

**③ 驗證**

- `GET /api/env/idf/versions` → `latest=v6.1.1`、40 個版本、`v6.1.0` **不在**清單、
  `v6.1`／`v6.1.1` 在。
- `normalize_version("6.1.0")` → `("v6.1", "6.1.0 唔存在（ESP-IDF 冇 .0 tag）→ 用 v6.1")`。
- 瀏覽器：下拉 41 項、首項「最新正式版（v6.1.1）」、`v6.1.0` 唔存在（與現實一致）。
- pytest **201 passed ＋ 1 skipped**（+`test_idf_version.py` 13 支）。


### 7.104 2026-11-18（開機自動補環境；S／C／P 多晶片安裝）

**① 需求**

「開機時幫我檢查，有問題就順便裝」＋「會裝主流型號：S 系列、C 系列、P 系列」。

**② 自動補齊政策（`env.auto_install`）**

| 模式 | 行為 |
|---|---|
| `off` | 只檢查／只報告 |
| **`safe`（預設）** | 自動補 **deps／esptool**；**唔會**自動裝 1–2 GB 的 IDF |
| `all` | 連 **IDF** 都自動裝（背景 thread，唔阻塞開機） |

- 為什麼預設唔係 `all`：IDF 1–2 GB／10–30 分鐘，開機偷偷跑會令人以為「卡住」。
  只缺 IDF 時會明確講「唔影響燒錄」，唔會嚇人。
- **失敗冷卻 6 小時**（`.env-auto.json`，gitignore）：唔會每次開機都重試失敗項。
- 背景執行：`env.auto_plan()` 決策 → `env.INSTALLER.start()` → 逐項等完成 →
  `env.auto_mark()` 記結果。UI「環境」頁可即時睇同一份日誌。

**③ 多晶片：S／C／P**

- `env.KNOWN_TARGETS`：esp32／s2／s3／c2／c3／c6／h2／p4（8 款主流）。
- `env.DEFAULT_TARGETS = s3／c3／c6／p4`（對應本 repo 實際有板子的三條主線）。
- `env.TARGET_PRESETS`：`scp_mainstream`（s3/c3/c6/p4）／`s_only`／`c_only`／`p_only`／`all`。
- `env.normalize_targets()`：接受 preset 名／逗號字串／陣列；驗證晶片名、去重、
  `all` 只可單獨用；不認識會列出可用值。
- API `POST /api/env/install` 的 `target` 三種形式都收（UI 傳逗號、AI 可傳陣列/preset）。
- UI：8 個勾選 chip ＋「S＋C＋P 主流／全選／清空」＋即時提示（`4 個：s3／c3／c6／p4`）。

**④ 驗證**

- `auto_plan` 三種模式（本機實測：deps/esptool 齊 → `off`/`safe` 都唔裝；
  `all` 只排 IDF）。失敗冷卻、標記讀寫、離線不崩 都有測試。
- `normalize_targets`：`scp_mainstream` → `esp32s3,esp32c3,esp32c6,esp32p4`；
  `esp32x9` → 報錯並列出可用值；`all,esp32s3` → 報錯；重複去重。
- 實機命令：`install.ps1 esp32s3,esp32c3,esp32c6,esp32p4`。
- 開機日誌（safe + 只缺 IDF）：
  `只缺 ESP-IDF（唔影響燒錄）；safe 模式唔會自動裝 1–2 GB。要開機自動裝：設 env.auto_install="all"`。
- 順手修：`settings.write_default()` 產生的 JSON 之前因為註解內有未轉義 `"` 而是壞 JSON
  （`test_write_default…` 抓到），已改成不含雙引號的寫法。
- **pytest 220 passed ＋ 1 skipped**（+`test_env_targets.py` 19 支）。


### 7.105 2026-11-18（更正：esp32s31 是真的；晶片清單改為「以 IDF 為準」）

**① 我的錯**

上一輪把 `esp32s31` 當成手誤而拒絕，令開機自動安裝失敗：

```
[esp_webmaster] 自動補 idf 出錯：唔認識嘅晶片：esp32s31；可用：esp32, esp32s2, esp32s3, …
```

查證：**IDF v6.1.1 的 `components/soc/esp32s31` 確實存在**
（同批仲有 `esp32c5`／`esp32c61`／`esp32h4`／`esp32h21`）。所以硬編碼清單＋拒絕未知
是錯的設計。

**② 改為「以 IDF 為準」的三層清單**

| 順序 | 來源 | 說明 |
|---|---|---|
| 1 | 已安裝 IDF 的 `components/soc/esp32*` | 最快最準，不用 subprocess |
| 2 | GitHub API `components/soc` | 未安裝時用，快取 1 小時 |
| 3 | 內建清單 | 離線 fallback（已補 s31／c5／c61／h4／h21） |

- `targets_known()` 回 (清單, 來源)；`report()` 多回 `targets.live`／`source`。
- **UI chip 動態產生**（實測 13 款），新晶片自動出現，前端不用改。
- 預設勾選：UI 上次選擇 → 設定檔 `env.idf_targets` → 內建預設（s3/c3/c6/p4）。
- 嚴格模式（API）只拒絕**明顯打錯**的名字並列出可用值；
  **寬鬆模式（設定檔／開機自動安裝）保留未知名字並警告**，交 IDF 判斷。

**③ 安裝位置：自動揀可寫的地方**

實測本環境（沙箱）venv python **唔可以寫** `C:\Users\<user>\esp`
（PowerShell 可以、venv python 唔可以 → `git clone` 直接 `Permission denied`, rc=128）。

- 候選：`ESPWM_IDF_DIR`／`IDF_INSTALL_DIR` → `~/esp` → `%LOCALAPPDATA%\esp` →
  `~/.espressif` → `%TEMP%/esp`；每個都**實際試寫**才算可用。
- 全部唔可寫 → **唔會**白白跑 1–2 GB，開機直接講：
  「所有預設位置都唔可寫…請喺 settings.json 設 env.idf_dir」。
- 環境類失敗（權限／缺 git）**不記 6 小時冷卻**（`Installer.env_error`），修好重開即再試。

**④ 順手修的兩個真 bug**

- `settings.DEFAULTS` 之前**冇 `env` 段**（只加在 `settings.json`）→ 若用戶的設定檔缺
  `env`，`conf["env"]` 會 KeyError。已補。
- `DEFAULTS` 的 `auto_install` 之前係 `safe`，同用戶期望（開機就裝）不符 → 改 **`all`**。

**⑤ 驗證**

- `GET /api/env` → `targets.live` 13 款（含 `esp32s31`）、`source=GitHub esp-idf`、
  `configured=[esp32s3,esp32s31,esp32c3,esp32c6,esp32p4]`、`auto.mode=all`。
- UI：13 個 chip、S31 **已勾選**、來源與模式顯示「開機自動裝（含 IDF）」。
- 開機日誌（本沙箱冇可寫位置）：清楚列出候選並指示設 `env.idf_dir`。
- `git ls-remote` 確認 `v6.1.1` tag 存在、`git clone --depth 1` 1.5 秒可連（network OK）。
- **pytest 231 passed ＋ 1 skipped**（+`test_idf_new_chips.py` 11 支）。


### 7.106 2026-11-18（版本新鮮度；自動安裝「為何冇做」看得到；UI 一鍵設 idf_dir）

**① 用戶反映：esptool 冇幫我更新；ESP-IDF 冇幫我安裝**

查證：

- venv 的 esptool = **5.6.0**，PyPI 最新亦係 **5.6.0** → **已經係最新**。
  但 UI 只顯示「OK v5.6.0」，冇講「已是最新」，所以睇落好似冇做嘢。
- ESP-IDF：`auto_install=all`、`auto_plan` 有排 idf，但**被 blocked**——
  因為對 server 這個 python 而言，**4 個候選位置全部唔可寫**
  （`~/esp`、`%LOCALAPPDATA%\esp`、`~/.espressif`、`%TEMP%\esp` 全部 `x`）。
  之前只喺 console 印一句，UI 完全睇唔到 → 用戶只會見到「缺」而不知原因。

**② 版本新鮮度（PyPI，快取 6 小時）**

- `env.pypi_latest()`／`installed_version()`／`updates()`：esptool／fastapi／uvicorn／pyserial。
- 顯示 `已是最新` / `5.3.1 → 5.6.0（可更新）` / `未知（離線？）`。
- `install_command("esptool")` 改為 `pip install --upgrade esptool`。
- `auto_plan`：esptool 有新版本也會排自動更新（safe／all）。

**③ 自動安裝狀態上 UI**

- `/api/env` 新增 `auto.plan`（按模式會做咩，含 `blocked`＋`hint`）與
  `auto.last`（上次結果），並在 `tools[]` 加 `installed/latest/outdated/updateStatus`。
- UI 新卡「開機自動安裝」：模式 chip、今次會做咩、上次結果、套件版本。
- 工具表每行加動作按鈕：缺的→「安裝」、可更新的→「更新」，一按帶到安裝卡並預選。

**④ 一鍵解決「冇可寫位置」**

- `env.writable_candidates()`：逐個候選**實測**寫入，回 `[{path, writable}]`。
- UI「用可寫位置」→ 填第一個真正可寫（冇就明講要手動填）。
- 新 API `POST /api/env/config`：把 `idf_dir`／`idf_version`／`idf_targets`
  寫入 **`settings.local.json`**（唔會改團隊的 `settings.json`），先用 schema 驗證。
- UI「儲存到本機設定」→ 一按即寫，重開生效。

**⑤ 驗證**

- `/api/env`：`updates` 四個套件全部「已是最新」；`auto.plan=[{what:idf, blocked:true, hint:…}]`；
  `installDirCandidates` 四個全部 `writable:false`（本環境實況）。
- UI：卡顯示「❌ 唔可以自動補 idf…」＋完整 hint；ESP-IDF 行有「安裝」；
  esptool 行顯示「已是最新」；按「用可寫位置」會提示「冇一個候選可寫」。
- **pytest 237 passed ＋ 1 skipped**（+`test_env_config_api.py` 6 支）。

**⑥ 給用戶的結論**

- esptool 唔需要更新（已 5.6.0 = 最新）。
- IDF 未裝係**權限**：呢部機由本工具（受沙箱限制）開的 server 寫唔到任何候選位置。
  由 Explorer 雙擊 `web_tools\WebMaster.bat` 開的 server 通常可以；
  若仍然唔得，用 UI「用可寫位置」／填一個可寫路徑再「儲存到本機設定」。


### 7.107 2026-11-18（真的裝到了：repo 內兜底 + IDF_TOOLS_PATH；沙箱寫入限制的說明）

**① 用戶回報**

按「開始安裝」後仍然失敗：

```
$ git clone ... C:\Users\bl24847\esp\esp-idf
fatal: could not create work tree dir '...': Permission denied
```

**② 診斷（唔係 ACL 壞）**

- `icacls C:\Users\bl24847\esp` 顯示 `TVB-OA\BL24847:(OI)(CI)(F)` 正常，**冇 deny ACE**。
- 用 server 的 venv python 實測：家目錄、`%LOCALAPPDATA%`、`~/.espressif`、`%TEMP%`
  **全部 PermissionError**；但 **workspace 內可以寫**。
- 結論：係 **DSH 沙箱對「workspace 以外寫入」的預期限制**（`diagnose-windows-sandbox-acl`
  技能明確講：write outside workspace 屬 expected denial，唔應該改 ACL）。
  我啟動的 server 係沙箱的子進程，所以寫唔到 `~/esp`；用戶由 Explorer 自己開的 server 唔受限。

**③ 修正：加一個「一定寫得入」的候選＋把工具鏈也搬過去**

- 候選加入 **`<repo>/.idf/esp-idf`**（workspace 內；已加 `/.idf/` 到 `.gitignore`）。
  `writable_candidates()` 實測後選中它 → `auto_plan` 不再是 `blocked`。
- **`IDF_TOOLS_PATH=<idf_dir>/../tools`**：`install.ps1` → `idf_tools.py` 默認把工具鏈
  裝到 `~/.espressif`（同樣被擋），所以安裝時用 `extra_env` 指去 `<repo>/.idf/tools`。
- 安裝完成訊息會印出帶 `IDF_TOOLS_PATH` 的 export 指令（PowerShell／bash）。

**④ 實測**

- 重開後 `auto.plan` = `idf`（唔再 blocked），自動安裝即刻開始：
  `git clone --recursive --depth 1 --branch v6.1.1 … → .idf/esp-idf`
  進行中（已 267 MB／21,058 檔案；之後會跑 `install.ps1` 下載工具鏈）。
- `git clone` 前的 `warning: refs/tags/v6.1.1 … is not a commit!` 係 annotated tag
  ＋ `--depth 1` 的正常提示，之後會 `Note: switching to '<sha>'`，唔影響。
- pytest 仍 **237 passed ＋ 1 skipped**。


### 7.108 2026-11-18（ESP-IDF 裝好並驗證：v6.1.1，五種能力全綠）

**① 真正的失敗原因（第二層）**

第一輪失敗係 `git clone` 寫唔到家目錄（沙箱限制）。改用 repo 內 `<repo>/.idf` 之後，
clone 同**工具鏈（6.1 GB）都成功**，但最後一步 `install-python-env` 失敗：

```
WARNING: Building wheel for esptool failed: [WinError 5] 存取被拒。:
         'c:\users\bl24847\appdata\local\pip\cache\wheels\f2'
```

→ **pip 的 wheel 快取仍喺家目錄**。修法：安裝時一併指定
`PIP_CACHE_DIR=<tools>/pip-cache`、`TMP`／`TEMP=<tools>/tmp`。
（`IDF_TOOLS_PATH` 之前已指去 `<idf_dir>/../tools`；唔指就寫 `~/.espressif`。）

**② 續跑就成功**

`install.ps1` 係 idempotent：已 clone 會跳過、已裝工具鏈會 `Skipping … (already installed)`，
只重做 python env。結果 **rc=0**。

**③ 偵測與可用性（兩個真相）**

- IDF 裝喺非慣例位置（`<repo>/.idf/esp-idf`），所以 `_check_idf()` 要把
  `default_idf_dir()` 也列入候選；工具鏈偵測要加 `IDF_TOOLS_PATH` 同 `<idf_dir>/../tools`。
- `idf_toolchain` 仍是 `warn`（已安裝但未 export）。**`build_firmware` 改成
  「ok 或 warn」都算可用**（只要建置前 export／用 `idf_env()`），否則永遠紅。
- `_check_idf()` 改為**真跑 `idf.py --version`**（帶 UTF-8 環境），得到 `v6.1.1`。

**④ `export.ps1` 唔可靠（非互動環境）**

會用 `psutil` 偵測父 shell → `psutil.NoSuchProcess` → `Activation script failed`。
所以新增 `env.idf_env()`：自己砌 `IDF_PATH`／`IDF_TOOLS_PATH`／`IDF_PYTHON_ENV_PATH`／
`PATH`（python env Scripts＋各工具鏈 bin），檢查與建置都唔需要 export。
另外 `export.ps1` 需要 `-ExecutionPolicy Bypass`，提示已更正。

**⑤ `make.py` 識搵新位置**

`tools/ncbuild/env.py::idf_path_for()` 加 `ESPWM_IDF_DIR`、WebMaster 設定
（`settings.local.json` → `settings.json` 的 `env.idf_dir`）、`<repo>/.idf/esp-idf`。
實測 `idf_path_for("p4")` 回 `<repo>/.idf/esp-idf`。

**⑥ 結果**

```
ready = {run_server:true, flash_nc4_ota:true, flash_esptool:true,
         flash_browser:true, build_firmware:true}
idf = ok  v6.1.1        auto.plan = []（冇嘢要再裝）
```
體積：`.idf` 約 **6.1 GB**（5 個 target 的工具鏈）。UI 顯示「上次自動安裝 idf：成功」。
pytest **237 passed ＋ 1 skipped**。


### 7.109 2026-11-18（Windows 原生 vs WSL 的實測對照；順手令建置真的可用）

**① 為什麼要量**

用戶問「Windows 版同 WSL 係唔係效率差好多」。唔用印象答，直接量。

**② 量測結果（同一台機：Intel Core Ultra 7 265，20 核，NVMe，WSL2 Ubuntu）**

| 項目 | Windows 原生 | WSL2（ext4） | WSL2（/mnt/c） |
|---|---|---|---|
| 300 × `git --version`（**進程建立**） | **14,980 ms**（≈50 ms/次） | **254 ms**（≈0.85 ms/次） | — |
| 300 個小檔 建＋刪（**I/O**） | **180 ms** | **196 ms** | **1,583 ms** |
| ESP-IDF hello_world（esp32s3）清建置 | **27.7 s**（`set-target` 另計 19.1 s） | 未裝（需另裝 ~7 GB） | — |

**結論**：

- **進程建立**係最大結構差異：Windows **慢約 59 倍**（50 ms vs 0.85 ms）。
  Ninja 建置每個編譯單元都要 spawn，所以專案越大差距越明顯；
  但 hello_world 實測只係 27.7 s——**唔係「慢好多」**，係「慢幾成到一倍」。
- **小檔 I/O**：Windows 原生（180 ms）同 WSL ext4（196 ms）**差不多**；
  但 WSL 寫 `/mnt/c`（Windows 磁碟，經 9p）慢 **8 倍**。
  → 所以「WSL 快」只在**原始碼放喺 ext4 內**成立；放喺 `C:` 再用 WSL build 係最差組合。
- WSL 燒錄要 `usbipd-win` 轉發 COM 埠，額外一層脆弱點；本工具（COM＋UI）本身係 Windows。

**建議**：留喺 Windows 原生（已裝好、27.7 s 可接受）。若日後專案大到建置時間痛，
把原始碼搬入 WSL ext4 再 build（rsync 同步），唔好喺 `/mnt/c` 上面 build。

**③ 順手修好兩個「建置其實跑唔到」的問題（之前只驗到 `--version`）**

1. `ESP_IDF_VERSION` 冇設 → 新版 component manager 會
   `Version.coerce(None)` → TypeError，連 `set-target` 都失敗。
   `idf_env()` 已由 `version.txt`／`git describe` 補上（實測 6.1.1）。
2. gcc 中間檔寫 `%TEMP%`（受限環境寫唔到）→ CMake compiler check 報
   `<gcc> - broken`。`idf_env()` 已把 `TMP`／`TEMP` 指去 `<tools>/tmp`。

修完之後：`idf.py set-target esp32s3` rc=0（19.1 s）、`idf.py build` rc=0（27.7 s），
產出 `hello_world.bin` 同可直接用的 `flash_args`。**建置能力由「偵測 OK」升級為「真的建到」。**
pytest 仍 **237 passed ＋ 1 skipped**。


### 7.110 2026-11-18（I80 燈驅動真的跑得起了：pixel 後端 11 個 bug＋LED Textkit 6×20）

**背景**：`688c0bd feat(pixel)` 把 rgbmatrix 改寫成「環＋編碼器＋多後端」，
但**從來沒有 build 過**（commit 之後立刻是 webmaster 重構）。一 build 就连环爆：

- `main.c`：`nc_pixel.h` 的 include 在 `#ifdef NC_ENABLE_TESTS` 內，但 `nc_pixel_task()`
  在測試區之外呼叫 → release 全部編不過；`pixel` 亦唔在 main 的 PRIV_REQUIRES。
- `nc_pixel_task.c`：`s_nplan` 先用後宣告；`.chain` 欄位不存在；
  `CONFIG_NETCORE_PIXEL_I80_PINS` 係字串，直接塞入 `int[]` 初始化 → 編不過。
- `nc_pixel_backend_i80.c`：`d->codec[ln]`（codec 只是一個指標）→ 編不過。
- 後端緩衝按 `NC_PX_MAX_LEDS` 靜態配置：i80 1 MB＋direct 393 KB＋spi 66 KB，
  S3（無 PSRAM）連 link 都過不了。

修完編譯之後，真正跑起來又暴露 6 個**靜默**問題（全部已修，詳見
[pixel-renderer.md](../03_notes/pixel-renderer.md) §3／§6）：

| 問題 | 現象 |
|---|---|
| i80／spi 建好後端但沒有 `nc_px_backend_register()` | `0xF061` 只見 `mock` lane，`frames=0` |
| i80／spi 的 `d->q` 從來沒有被設定 | acquire 永遠 false（`starved` 一直加） |
| 生產者綁死環的第 0 格 | 第二幀之後寫舊格、commit 新格 → 畫面黑／跳格 |
| `nc_px_json()` 把 `"prd"` 塞在 `"lanes":[` 之後 | 回覆不是合法 JSON |
| 渲染器 fps 寫死 60（Kconfig 冇作用） | 文件說 450／實際 60 |
| I80 完成回呼「掃第一個未完成的槽」 | 槽回收重排後認錯格 → 提早還格（torn） |
| GPIO3 被當保留腳 | LED Textkit（GPIO1–5）起不到 I80（只准 GPIO3 例外） |

**硬體**：ESP32-S3-SuperMini ＋ **LED Textkit：6 條 WS2812 燈鏈、每條 20 顆**
（D0..D5 = GPIO1..GPIO6；D6/D7 = GPIO7/8、WR = 9、DC = 10 是 I80 必需的 dummy）。

**專案層（新）**：`ports/project/S3/ESP32-S3-SuperMini-LED-Textkit/`
（硬體仍然係 `S3/ESP32-S3-SuperMini`；project 只帶一份**只覆寫外接裝置**的
`sdkconfig.defaults`）。`firmware/CMakeLists.txt` 的 `SDKCONFIG_DEFAULTS` 次序變成
`firmware → hardware → project`；`tools/board.py check` 會擋 project 寫
flash／PSRAM／CPU／console／分區表／target 這類硬體事實。

**上板（測試映像 0x145dc0／1.33 MB、15% free；6 lan e × 20 顆）**：

```json
{"producer":{"ready":1,"fps_x10":600,"px":20},
 "render":{"started":1,"fps":300,"frames":834,"fresh":834,"fail":0,
           "enc_us":814,"tx_us":7,"frame_us":2058,
           "lanes":[{"be":"i80","lanes":6,"px":20,"seq":835,"fill":0},
                    {"be":"mock","lanes":1,"px":20,"seq":0,"fill":0}]}}
```

- `0xF061`：i80 lane 出現、`fill=0`（有被取走）、`seq` 持續增加、`fail=0`；
  生產者 60 fps；6 條 lane × 20 顆一次新格編碼 **814 µs**。
- `0xF060`：`zone1`／`solid`／`gain`／`demo off` 都生效；`0xF061` 兩份 JSON 合法。
- **肉眼驗證**：6 條燈鏈全部亮、每條 20 顆都有光（solid 30,30,30、gain 120）。
- 內存：I80 3 槽 × 1,640 B ＋ lane 暫存 1,640 B 全部 internal。
- release 映像（無 tests／無 0xF060 命令）：**0x11dbc0（1.16 MB，分區剩 26%）**。
- host：`tools/hosttest/pixel_test` **339 passed**（已加入 `run_all.sh`）。
- 燒錄注意：USB-Serial-JTAG 有一次卡在 download mode（`esptool` sync 失敗、
  TX path down）；**拔插 USB 就恢復**。之後用 `--before=usb-reset` 直燒 app 最穩。


### 7.111 2026-11-18（分層：pixel 拆出 `sys` → `sw`／`tasks`／`driver`）

**問題**：`components/sys/pixel` 一個 component 同時裝了環、編碼器、效果、
I80／SPI 硬體與任務生命週期 —— 驅動碼在 `sys/`、任務與硬體混在一起。
分層規則（[component-layout](../03_notes/component-layout.md) §1、
[driver-layer](../03_notes/driver-layer.md)）要求「driver 唔知任務；任務唔摸匯流排」。

**新佈局**：

| 層 | 路徑 | 內容 | 依賴 |
|---|---|---|---|
| `sw` | `components/sw/pixel/` | 環（`nc_pixel_q.c`）、編碼器（`nc_pixel_encode.c`）、內建效果（`nc_pixel_producer.c`） | 只有 esp_timer／log |
| `tasks` | `components/tasks/pixel_task/` | `nc_pixel_task.c`＋後端轉接（i80／spi／direct／mock） | task、mem、hw、core、log、pixel |
| `driver` | `components/driver/drv_i80/` | I80 bus／PCLK／DMA 槽／完成回呼；`nc_i80_drv_install/submit/slot_done` | esp_lcd、heap、log（**唔靠 nc_hw**） |
| `driver` | `components/driver/drv_spi_led/` | SPI 時脈式燈（WS2801／APA102） | esp_driver_spi、heap |

**邊界**：

- 腳位 claim／保留腳政策留在 `tasks/pixel_task`（`nc_hw` 屬 sys），driver 只驗數值範圍；
- driver 擁有 DMA 緩衝（`slot_buf(i)`），task 只填資料、submit、問 `slot_done`；
- `nc_pixel_backend_t.ring` 由 `bind_lanes()` 設定；driver 完全唔知「環」。

**驗證**：

- host：`pixel_test` **339 passed**（路徑已改為 `sw/pixel`＋`tasks/pixel_task`；
  `run.sh`／`run.bat` 同步）。
- 建置：Textkit tests 映像 **0x146f10**（1.33 MB，含 0xF062）、release **0x11e470**（1.17 MB），零 pixel 相關 warning；
  component 清單見到 `pixel`（sw）、`pixel_task`（tasks）、`drv_i80`、`drv_spi_led`。
- 上板（分層後重燒）：`0xF061` → `be=i80`、`lanes=6`、`px=20`、`fill=1`、
  `fail=0`、60 fps、`enc_us≈847`；6 條鏈 × 20 顆照常亮。
- `tools/hosttest/run_all.sh` 已加入 `pixel_test`。
- **2026-11-18 晚再拆任務**：效果與渲染唔止分 loop，係分**兩個任務**（將來 LCD／LED
  共用同一個 render 任務）：
  - `render`（`tasks/render_task/`）：`render` loop，300 fps，4,096 B／prio 6；
    宣告環、建後端、只做「環 → 編碼 → 硬件」；
  - `pixel`（`tasks/pixel_task/`）：`prd` loop，60 fps，3,072 B／prio 5；
    只做效果／Map → 環；環由 render 建立，pixel 按名（含 `prd`）找回；
  - `sw/pixel` 保持純計算（環、編碼、效果函數），亦負責環註冊表。
  上板：`status tasks` 見 `render@c1`、`prd@c1` 兩條 running；`0xF061` 生產者 60.0 fps、
  `be=i80`、`lanes=6`、`px=20`、`fail=0`、`enc_us≈842–958`。
- **節奏分權（2026-11-18 晚最終）**：
  - `pixel` 任務**不控 FPS**：環有空間就瘋狂填，填滿就每 1 ms 監控一次等 renderer 還格
    （實測 ~100 格／秒、環長期 3 格全滿；生產者 JSON `fps=0` 表示自由填環）；
  - `render` 任務由 **`system.json` 的 `frame_interval_ms`** 控制（預設 20 ms = 50 fps），
    `CONFIG_NETCORE_PIXEL_FPS` 只在拿不到設定時後備；
  - render loop 用 **esp_timer 週期信號量**（唔靠 1 ms tick、唔 busy-wait），
    所以 20 ms → **50.0 fps（late=0）**；
  - 上板：render `frames` 每秒 +50、環 `fill` 保持 1–3（常滿）；生產者自由填環
    （~100 格／秒，多數格內容相同）；`fail=0`、`enc_us≈716–1052`。
- **抖動預設關**（實驗功能）：`CONFIG_NETCORE_PIXEL_DITHER=0`、`DITHER_BITS=8`、
  producer demo 預設關；要試就 `0xF060 op=2`／`op=8` 或改 Kconfig。
- **舊設備要重 seed config**：`tasks.json` 多咗 `render` 條目，舊的 data 分區只有
  `pixel` → `render` 被補成 (0,0)（on_start 有跑、loop 唔開）。用
  `tools/nc4_file.py set '{"System":{"fs_format":1}}' 1` ＋ reboot 重建 FAT。
- 交叉建置：S3-Matrix tests（1 lane／64 顆）通過；S3-Mini-Nano release（pixel 未開，
  走 `#else` stub）零 warning；C3 release（`SOC_LCD_I80_SUPPORTED=0`，driver stub）通過。


### 7.112 2026-11-18（極限測試：16 × 600 × 50 FPS 在 S3 做唔到）

新增測試指令 **`0xF062 PXBENCH`**（測試映像；payload `[lanes][px lo][px hi][frames]`），
用同一份 codec 量「逐 lane 編碼＋多 lane OR 合成」的每幀成本，payload 用變動 pattern
避開「同上一顆就抄」的快取，scratch／輸出放 PSRAM（真實 backend 要 internal，數字保守）。
S3 @240 MHz、Textkit 映像實測：

| 配置 | µs／幀 | 上限 fps | 50 fps |
|---|---:|---:|---|
| **16 × 600** | **143,166** | **6.9** | ❌（差 7.2×） |
| 8 × 600 | 72,053 | 13.8 | ❌ |
| 16 × 300 | 55,076 | 18.1 | ❌ |
| 6 × 600 | 54,153 | 18.4 | ❌ |
| 8 × 256 | 20,140 | 49.6 | ❌（臨界） |
| 8 × 200 | 11,909 | 83.9 | ✅ |
| 6 × 256 | 15,099 | 66.2 | ✅ |
| 6 × 20（現況） | 998 | 1002 | ✅ |

**三個獨立瓶頸**：

1. S3 只有一個 I80／LCD_CAM（8 條資料線）→ 16 lane 需要兩個控制器（兩顆 MCU）或
   16-bit I80（P4）；
2. 8 lane × 600 px 一幀 = 43,400 B，3 個 DMA 槽 = 130 KB **internal**；S3 開機只剩
   ~20 KB → 連 8×600 都起不到 backend；
3. CPU：143 ms／幀 vs 20 ms → 差 7.2 倍。

**可行方向**（未實作）：P4 ＋ 16-bit I80／兩條 bus；兩核各編 8 lane；顏色 pattern
快取（文字通常幾種顏色，逐像素 memcpy＋OR）；或兩塊 S3 各 8 lane 同步。
測試指令留在測試映像：`python3 tools/nc4.py --port $P raw 0xF062 10580205`。


### 7.113 2026-11-18（I80 16-bit 支援；「20 KB」係測試映像，release 有 172 KB）

**16-bit I80**（用戶指出 8/16-bit 並行輸出應該同時間）：驅動已支援
`CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH=8|16`；16-bit 用 18 個腳位（D0..D15、WR、DC），
一個 PCLK 出兩個 byte（lane0–7 低位元組、lane8–15 高位元組），所以 **16 lane 的
DMA 傳輸時間同 8 lane 一樣**。S3 實測（6 lane 接同一塊 Textkit、其餘 dummy）：
I80 起得來、50 fps、`enc_us≈825`、`fail=0` → S3 硬件支援 16-bit i80。
Textkit 專案仍用 8-bit（6 lane），16-bit 係 driver 能力。

**「20 KB」之謎**：係測試映像，唔係 release：

| 映像 | h_int | h_dma |
|---|---:|---:|
| tests（NCM/WAMR＋bench＋test_action） | **20,551 B** | 12,763 B |
| release（NC_ENABLE_TESTS=OFF） | **172,663 B** | 164,875 B |

差 ~152 KB 主要係 NCM/WAMR 引擎、bench 任務與測試指令；正式映像冇呢啲。
用戶懷疑正確：之前用測試映像睇內存，低估了 release 的空間。

**但 16×600×50 仍然做唔到**：

- CPU：`0xF062` 量到 16×600 = **143 ms／幀（6.9 fps）**；16-bit 只加快傳輸，
  編碼仍然同 lane 數成正比；
- 記憶體：16-bit × 600 px 一幀 86,800 B，2 個 DMA 槽 173.6 KB > release
  DMA-capable 164.9 KB（差 ~9 KB）；1 槽＋scratch 放得落但冇 pipelining；
- 16-bit 已經解決「16 條一起出」的傳輸時間，剩低係 CPU 編碼同緩衝。

下一步（未做）：顏色 pattern 快取、兩核各編 8 lane、P4／雙 S3。


### 7.114 2026-11-18（編碼優化：波形 LUT＋direct-OR；16×600 由 6.9 → 18.6 fps）

對照舊 `mp_Net-Core/slave` 的原則（整數、固定數值域、bulk 掃 buffer、唔逐 pixel 呼叫）
重寫熱路徑：

- **波形 LUT**（256 × bit_cells×8 B，共用）：逐線材 byte 由 bit 迴圈變 memcpy；
- **8-bit／無抖動快路徑**：唔入 `dither_ch`；
- **`nc_px_codec_wave_frame_or()`**：每條 lane 直接 OR 入 I80 DMA 槽，
  **取消 43 KB lane scratch**（同時解決內部記憶體同一次讀寫）。

`0xF062`（測試映像、PSRAM 緩衝，保守值）：

| 配置 | 之前 | 之後 |
|---|---:|---:|
| 16 × 600 | 143 ms / 6.9 fps | **38.5 ms / 25.9 fps** |
| 8 × 600 | 72 ms / 13.8 fps | **19.2 ms / 52.0 fps** ✅ |
| 6 × 600 | 54 ms / 18.4 fps | **17.6 ms / 56.7 fps** ✅ |
| 16 × 300 | 55 ms / 18.1 fps | **18.9 ms / 52.9 fps** ✅ |
| 6 × 256 | 15 ms / 66 fps | **7.2 ms / 138 fps** |

上板 6×20：`enc_us` 由 ~825 降至 **514**、50 fps、`fail=0`。

**Packed bitstream＋8×8 轉置**（第三輪優化；16×600 拆分：pack 15.3 ms、merge 24.3 ms → 查表唔係瓶頸，merge／PSRAM 輸出寫入才係）：一條 lane 的波形壓成 bitstream
（每線材 byte 3 B），再 8 條 lane 一組合併；混合量由 691k 次 byte OR 降到
43k 次位元組，所以有上面嘅大躍進。I80 backend 已改用此路徑。

**結論**：**8×600 @ 50 fps ✅、16×300 @ 50 fps ✅、6×600 56.7 fps**（測試映像
PSRAM 數字，release internal 更快）。**16×600 @ 50 fps 仍然差**：packed 後 38.5 ms，
但 16-bit 一幀 86,800 B，2 個 DMA 槽 173.6 KB > release DMA-capable 164.9 KB
（差 8.7 KB）；單槽放得落但 encode 同 DMA 冇得重疊。下一步 P4／雙 S3／兩核各 8 lane。


### 7.115 2026-11-18（純 SRAM 量測：16×600 28.2 fps；瓶頸係 CPU 唔係記憶體）

`CONFIG_NETCORE_PXBENCH=y` 建 release 映像（無 NCM／測試任務），0xF062 的緩衝
全部 internal（回覆 `sram=1`、11/11）：

| 配置 | PSRAM | **純 SRAM** |
|---|---:|---:|
| 16 × 600 | 38.5 ms / 25.9 fps | **35.4 ms / 28.2 fps** |
| 8 × 600 | 19.2 ms / 52.0 | **17.8 ms / 56.2** ✅ |
| 16 × 300 | 18.9 ms / 52.9 | **17.7 ms / 56.4** ✅ |
| 6 × 600 | 17.6 ms / 56.7 | **16.1 ms / 62.0** ✅ |

**純 SRAM 只快 8–10%** → 剩低係 CPU（逐 pixel 映射＋合併），唔係 PSRAM 頻寬。
16×600 仍差 1.8×；下一步：專化 pack 熱路徑、bit-trick 8×8 轉置、兩核各 8 lane。
16-bit 雙緩衝 173.6 KB 仍然超 release DMA 164.9 KB（差 8.7 KB）。

另：今次全程用 **NC4 OTA**（無 esptool、無拔插），但 1.17 MB 用 187 s（6.1 KB/s），
比 CDC OTA 的 ~200 KB/s 慢很多，下一步要查 USJ/OTA chunk 節奏。


### 7.116 2026-11-19（OTA 6.2 KB/s 的真相：USJ 每次只讀 64 B ＋ 10 ms 輪詢）

用戶問「傳遞慢就傳少啲再解碼」。先量再改：

- **壓縮比**：app 1,174,064 B → zlib -9 **770,639 B（65.6%）**；即係就算做
  「壓縮→裝置解壓」，OTA 187 s 只會變 ~123 s（1.5×），而且要加解壓庫（IDF 冇內建）。
- **Pipelining 無用**：`tools/ota.py --window 4`（一次送 4 個 WRITE、再一次收 ACK）
  仍然 **186 s / 6.2 KB/s** → 唔係 ACK round-trip 問題。
- **真正原因**：`STATUS keys=net` 顯示 `max_read=64` —— USB USJ 一個包只 64 B；
  而 PULL decode loop 每讀完一次（FIFO 空）就 `ulTaskNotifyTake(..., 10 ms)`，
  即 **64 B / 10 ms ≈ 6.4 KB/s**，同實測完全吻合。1.17 MB 要 18,645 次讀。
- **已改（未上板驗證）**：`nc4_bus.c` 的 PULL 等待改成「20 ms 內有過資料 → 1 ms 再讀，
  否則 10 ms」；理論上 64 B/1 ms ≈ **64 KB/s（~10×）**，空閒時仍然 10 ms。
  這次 OTA 安裝失敗（BEGIN 後 WRITE 中斷），裝置已回到舊 release（app 44e0a599），
  改動只在源碼，未生效。

**結論**：先修 link 輪詢（10×），再考慮壓縮（1.5×）；壓縮的 decoder 成本／收益
暫時唔划算。像素傳輸方面本身已經係最少：主機只送 canonical RGBW（4 B/px），
裝置端再 pack／轉置。


### 7.117 2026-11-19（照 FastLED 做：pack/merge 35.4→14.33 ms；16×600 達 69.7 fps CPU）

參考 FastLED `channel_driver_lcd_clockless` + `bit_spread_lut.hpp`：

1. merge 改用 **nibble spread LUT（16×u32）＋u32 OR**（nibble 的 bit j → 第 j 個
   output byte），取代逐 unit 8 次 shift/OR；
2. pack 加 8-bit／無抖動專化：直接由 RGBW 按色序抄 3 個 LUT，唔經 `lane_px`；
3. `drv_i80` DMA 槽改 **PSRAM + MALLOC_CAP_DMA + 64 B 對齊 + esp_cache_msync(C2M)**
   （FastLED 同款；S3 `SOC_PSRAM_DMA_CAPABLE=1`），失敗才退 internal。

`0xF062`（release、純 SRAM、sram=1）：

| 配置 | 之前 | **現在** |
|---|---:|---:|
| 16×600 | 35.4 ms / 28.2 fps | **14.33 ms / 69.7 fps**（pack 7.07＋merge 7.08） |
| 8×600 | 17.8 ms / 56.2 | **7.24 ms / 138 fps** |
| 16×300 | 17.7 ms / 56.4 | **7.18 ms / 139 fps** |
| 6×600 | 16.1 ms / 62.0 | **5.88 ms / 170 fps** |

**16×600×50 的新算式**：CPU 14.33 ms；DMA 17.36 ms；兩個 PSRAM DMA 槽 overlap
→ frame = max(14.33, 17.36) = **17.36 ms（57.6 fps 上限）**，50 fps 已經過關；
DMA 傳輸成為新瓶頸。板上 app `c0787583`，6×20 正常；16 lane 實物未上板驗證。
下一步實驗清單（IRAM、lib8tion、dither、RMT、chunked DMA、ABL）見
[pixel-experiments.md](../03_notes/pixel-experiments.md)。


### 7.118 2026-11-19（結構歸位：取消 `components/dev`；測試就係名字 `test_*`）

用戶指出 `firmware/components/dev/` 唔應該存在：測試已經用**名字**分（`test_*`／
`_action`／`_tasks`）、用**路徑**分層（`components/action`、`components/tasks`、
`modules/`），`dev/` 係第三個（用途）座標 —— 同一個東西兩個位置。

| 之前 | 之後 |
|---|---|
| `components/dev/test_action/` | `components/action/test_action/` |
| `components/dev/test_bench_tasks/` | `components/tasks/test_bench_tasks/` |
| `components/dev/test_tcp_tasks/` | `components/tasks/test_tcp_tasks/` |
| `components/dev/`（整個目錄） | **刪除** |

**規則（只有兩個軸）**

- **路徑＝層**：`action/`＝指令表、`tasks/`＝任務（`sys/ bus/ driver/ sw/` 同前）。
- **名字＝種類**：`*_action`、`*_tasks`；**`test_` 開頭＝測試**。
- `NC_ENABLE_TESTS` 只係「加唔加入編譯」嘅開關，唔再兼任目錄分類。

**CMake**

- `firmware/CMakeLists.txt`：ON 時 `file(GLOB components/*/test_* CONFIGURE_DEPENDS)`
  自動收（只收有 `CMakeLists.txt` 的目錄）；OFF 時一個都唔加入。新增測試元件
  只要放對層、叫對名，唔使改 CMake。
- `firmware/main/CMakeLists.txt`：測試標頭的 include 路徑跟住搬。
- 仍然係「optional 組件放第二層」：`components/action`、`components/tasks` 本身
  冇 `CMakeLists.txt`，IDF 唔會自動收，所以 OFF 時佢哋真係唔存在（同 ncm 同理）。

**文件同步**：README、[commands.md](../01_protocol/commands.md) §1、
[execution-rules.md](../03_notes/execution-rules.md) §6、[test-register.md](test-register.md)
§3、[core-isolation.md](../03_notes/core-isolation.md)、[module-adoption.md](../03_notes/module-adoption.md)、
[testing.md](../02_guides/testing.md)。[test-register.md](test-register.md) 亦順手改正
`test_rgb_led_matrix`（2026-11-18 已由 pixel 生產者／渲染器取代）。

**驗證**（S3／`ESP32-S3-N16R8`、IDF 6.1.1、`idf.py reconfigure` 兩種旗標各建一個乾淨 build dir）

| `NC_ENABLE_TESTS` | build components | `test_*` 元件 |
|---|---:|---|
| ON | 175 | `test_action`、`test_bench_tasks`、`test_tcp_tasks`（＋`ncm`，因為 tests 會開 NCM） |
| OFF | 170 | **0 個** —— 三個都唔喺 build 圖，OFF 時真係「唔存在」 |

即係搬完之後 gate 冇漏、亦冇多收。`tools/id_check.py` 亦照跑（指令 ID 全部指得到
code ✓）。驗證用的 build dir／component cache 已清、ON 那次為 ncm 抓落的
`espressif__wasm-micro-runtime`（gitignore）已刪，維持原本的樹。

> 順帶一提：`firmware/main/CMakeLists.txt` 的 `_nc_test_reqs`（原本 append
> `ncm`／`test_bench_tasks`／`test_tcp_tasks`）**從來冇被用過**（冇進
> `PRIV_REQUIRES`），係死變數 —— 今次冇動它，要清就另開一次。


### 7.119 2026-11-19（刪掉效能測試任務：bench_a/b/c、tcp_sink/src；指令保留）

用戶：「`test_bench_tasks` 這些不太重要的任務其實可以搬去模組那邊，因為已經不用了、
棄用了，指令可以保留。」→ 拍板**直接刪除**，唔搬 `modules/`、唔留存檔（git 歷史已有）。

| 刪除 | 位置（上一版） | 為什麼 |
|---|---|---|
| `test_bench_tasks`（bench_a/b/c） | `components/tasks/test_bench_tasks/` | 註冊時預設 affinity `(0,0)`，**從來冇跑過**；native 基準改由 `0xF050 CPU_BURN` 等指令提供 |
| `test_tcp_tasks`（sink 5000/5003、src 5001/5002） | `components/tasks/test_tcp_tasks/` | 同 `components/bus/bus_net/nc_net_raw.c`（`rawbench`，:5000／:5001）重疊 |

**保留**：`components/action/test_action`（0xF0xx 55 條＋0xF050）—— 指令完全唔動。

**連帶改動**

- `main.c`：移除 `nc_bench.h`／`nc_test_tcp.h` include、`nc_test_tcp_start()` 呼叫、
  三條 `nc_task_register(nc_bench_task_*)`。`NC_ENABLE_TESTS` 區塊現只剩 ncm 註冊。
- `main/CMakeLists.txt`：移除兩個測試 include path；順手清掉 `_nc_test_reqs`
  （§7.118 提到的死變數）。
- `tools/bench/bench_all.py`：A 段（純 TCP）抽成 `section_a()` 並加 `--skip-raw`
  —— `:5002`／`:5003` 已經冇 listener，要用 rawbench（只有 :5000／:5001）。
- 文件：test-register、execution-rules §6、core-isolation（任務表與 L5 清單）、
  testing、disposition B7、execution-model、README、firmware/CMakeLists 註解。

**驗證**

- S3 `--tests` 全量 build 跑到 `nc_pixel_bench.c` 才停 —— **同今次改動無關**（見下面
  的既有問題）。`main.c` 與 `nc_test_action.c` 用同一份 `compile_commands.json` 的
  參數單獨編譯：**exit 0、0 error、0 warning**。
- `tools/id_check.py`：指令 ID 仍然全部指得到 code（模塊 35 → 33）。
- `bench_all.py` `py_compile` 通過。

> **順帶發現（既有問題，唔係今次改動造成）**：`components/sw/pixel/nc_pixel_bench.c`
> 無條件用 `CONFIG_NETCORE_PIXEL_FORMAT／PCLK_HZ／BIT_CELLS／T0H／T1H／ZONE_PX`，
> 但嗰幾個 Kconfig 只喺 `NETCORE_PIXEL_I80=y` 的板（現時只有 `S3/ESP32-S3-Matrix`
> 同 `S3/ESP32-S3-SuperMini-LED-Textkit` project）才會生成。其餘板（N16R8、
> Mini-Nano、SuperMini、P4、C3）一 build 就爆 —— 係 467af43（2026-11-19）把
> `nc_pixel_bench.c` 加進 pixel 組件 `SRCS` 時漏咗 guard。未修。


### 7.120 2026-11-19（上板重測編碼：真配置 19.44 ms，不是 14.33 ms）

之前 §7.117 的 14.33 ms 是 `sram=1`（11 個 bench 緩衝全在 internal）。這次用
SuperMini＋Textkit（project 預設 6 lane × 20 px、2.5 MHz、bit_cells=3；**實物接線
不是 6×20 —— 其中一條是 GPIO14 的 8×8 矩陣**）建 **tests 映像**
（`NC_ENABLE_TESTS=ON`，1,368,048 B），NC4 OTA 上板（218.5 s、6.1 KB/s），
用 `0xF062` 量同一塊板：

| lanes × px | `us_packed` | `us_pack` | `us_merge` | `sram` |
|---|---:|---:|---:|---:|
| 16 × 600 | **19,440–19,444 µs**（3 次） | 8,201 | 11,290 | **0（0/11）** |
| 8 × 600 | 10,162 | 4,254 | 5,999 | 0（0/11） |
| 6 × 600 | 9,109 | 3,021 | 5,995 | 0（0/11） |
| 16 × 300 | 8,881 | 3,716 | 5,504 | 0（0/11） |
| 6 × 20 | 234 | 97 | 135 | 0（6/11） |

**為何差 36%**：這塊板在真固件下 **internal 只剩 4,975 B**（`h_int`；ncm arena 在
PSRAM、WAMR／WiFi／任務已佔滿）。16×600 的工作集要 `2×43,400 + 8×5,400 + 2,400
≈ 132 KB` internal 才 `sram=1` —— 實際一個都配不到，全部落 PSRAM，所以
`us_merge` 由 7.08 → 11.29 ms（+60%）、`us_packed` 由 14.33 → **19.44 ms**。
舊的 release 固件 internal 也只有 ~64 KB（`h_int`），一樣做不到 `sram=1`。

**其他路徑（同一輪、16×600、PSRAM）**：`us_enc`（只編碼寫 scratch）49.2 ms、
`us_mix`（只 OR）74.8 ms、`us_direct`（direct-OR 全路徑）54.4 ms —— packed 在
PSRAM 下仍然是 direct-OR 的 **2.8×**，但已不是 internal 版的 3.8×。

**順帶更正**：§7.117 說「雙槽 173.6 KB > release DMA 164.9 KB 所以放不下」——
那是 *internal* DMA。`drv_i80` 的槽已經改 PSRAM（同一個 commit），這塊板
PSRAM 有 1.5 MB，3 槽 260 KB 完全放得下。**記憶體牆不成立**，真正的上限回到
CPU：`1/19.44 ms = 51.4 fps`（DMA 17.36 ms 由實測 `units=43,400 ÷ 2.5 MHz`
得出，仍比 CPU 低）。

**缺口**：真渲染任務在這塊板的 `tasks.json` 是 `enabled:0`（`render`／`pixel`
兩個 loop 都冇跑），所以 `0xF061` 的 `enc_us` 是 0。§7.117 的 514 µs（6×20）
要先把那兩個任務打開才量得到；本次只量到同尺寸的純編碼 234 µs（差 ~280 µs，
是 acquire／memset／submit 的固定開銷）。

**記憶體（同一塊板、tests 映像、開機後）**

| 項目 | 剩餘 |
|---|---:|
| internal SRAM（`h_int`） | **4,983 B** |
| DMA-capable（`h_dma`） | **3,343 B** |
| PSRAM（`h_ps`） | **1,514,152 B**（1.44 MiB／2 MiB） |
| 計劃期最大塊（`mem.plan.avail`） | int 31,744／psram 2,064,384 |

靜態佔用（map）：`.bss` 202,000 ＋ `.data` 47,799 ＋ `.iram0.text` 85,739 ＋
vectors 1,028 = **336,566 B（329 KB）**。最大幾塊 `.bss`：

| 大小 | 符號 | 誰 |
|---:|---|---|
| 17,664 | `s_res` | ncm（資源表） |
| 16,644 | `_cdcd_itf` | TinyUSB CDC |
| 16,410 | `s_parse_scratch` | nc4 parser |
| 11,656／6,400 | `s_mod_scan`／`s_mods` | ncm（模組掃描） |
| 9,120 | `s_slots` | 任務管理器 |
| 8,192×3＋4,104 | `s_bb_*`／`g_cnxMgr` | WiFi |
| 4,096×3 | `s_big$8`／`work$9`／`s_bench_pat` | test_action（只在 tests 映像） |

剩返嗰 5 KB 內部 heap 就是任務堆疊（HWM：ncm 7,588、TCP 5,200、UDP 5,136、
wifi 4,536、esp_timer 3,304…）＋ WiFi／lwIP 執行期緩衝。

**16×600 的記憶體需求**：3 個 DMA 槽 × 86,800 B = **260.4 KB PSRAM**（剩 1.44 MiB）
＋ packed scratch 43.2 KB → 放得下。**記憶體不是牆。**

**16×600 的 CPU（encode 19.44 實測 ＋ 其餘計算）**

| 項 | 值 | 依據 |
|---|---:|---|
| pack＋merge（PSRAM） | 19.44 ms | 實測 |
| memset 槽 86,800 B | 2.2–3.6 ms | quad PSRAM 80 MHz 上限 40 MB/s（2.17 ms）～實際 60–70% |
| `esp_cache_msync` 86,800 B | 0.1–0.2 ms | 估計 |
| **合計 CPU／幀** | **21.7–23.2 ms** | → **43–46 fps** |

DMA 仍然 17.36 ms（43,400 PCLK ÷ 2.5 MHz）→ 16×600 是 **CPU-bound**，
50 fps（20 ms）做不到。最直接的補法：**省掉 memset** —— 16-bit 時 group0
（lane 0–7）用 store、group1（lane 8–15）才 OR，整格唔需要先清零，
省 2.2–3.6 ms → 約 19.4–20 ms，剛好貼線。

> ⚠️ `CONFIG_NETCORE_PIXEL_I80_LANES` 是 `range 1 8`，所以 **真 backend 開不到
> 16 lane**（bench 不受此限）。要實測 16 lane 渲染，先要改 range 1..16。

### 7.121 2026-11-19（WiFi 預設改為不開：靜態 -54 KB、heap +128 KB；修 boot loop）

**改動**

- 4 塊 S3 板（SuperMini／Matrix／Mini-Nano／N16R8）的 `sdkconfig.defaults` 移除
  `CONFIG_NETCORE_WIFI=y` —— Kconfig 本身預設 `n`，係板子手寫開的。
- **開關放回 project 層**：要 WiFi 的 project 在自己的 `sdkconfig.defaults` 寫
  `CONFIG_NETCORE_WIFI=y`；`hardware/board.json` 的 `features.wifi` 只描述
  「晶片有沒有 radio」，不是開關。`tools/board.py --wifi` 不再寫 CONFIG；
  `ports/README.md` 同步（`_profiles/wifi.defaults` 已撤回）。
- **修 boot loop**：`nc_net_lines.c` 在 `CONFIG_NETCORE_WIFI=n` 時跳過 `iface=wifi`
  的線。之前照樣 bind socket，但 `esp_netif`／lwIP 係 `nc_wifi_init()` 初始化的 →
  `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` → 無限重啟
  （實測 12 秒 21 次）。修好之後開機正常。

**量測（SuperMini＋Textkit tests 映像）**

| | WiFi on | WiFi off |
|---|---:|---:|
| app image | 1,368,048 B | **841,440 B** |
| `.bss` | 202,000 | 182,752 |
| `.data` | 47,799 | 40,007 |
| IRAM | 85,739 | 58,599 |
| 靜態 SRAM | 336,566 | **282,386** |
| `h_int`（開機後） | 4,975 | **128,475–144,183** |
| `h_dma` | 3,335 | 120,687 |

`0xF062` 重測：16×600 **19,440 → 15,392 µs**（`sram=0` 但 9/11 塊 internal）；
16×300 **7,051 µs（11/11 internal，139 fps）**；8×600 7,657；6×20 210（11/11）。

**歸屬更正**：`s_bb_kf`／`s_bb_stream`／`s_bb_slot_mem`（8,192×3）係 **test_action**
的 byte-buffer 測試緩衝（8 個 1 KB slot ＋ 兩條 8 KB 流），唔係 WiFi。WiFi 係
`g_cnxMgr`（4,104）＋執行期緩衝（呢部分才是大頭，~100 KB internal）。

**新發現（未修）**

1. `0xF050` 重複註冊：`NC4_CMD_CPU_BURN`（nc_test_action.c:2489）同
   `NC4_CMD_TCP_SEND`（:4355）都用 0xF050 → 後者被丟棄（boot log 有 error）。
2. `0xF062` 16×600 第二次執行會觸發重置（第一次正常 15.39 ms；16×300／6×20 冇事）。
3. 兩次 OTA 槽內容一度不一致（bootloader 揀錯槽會見到舊的 assert）；已把兩個槽
   都刷成同一顆。

### 7.122 2026-11-19（渲染三招：store-first 免 memset、小幀槽回 internal、移除重複 msync）

**改動**

1. `nc_px_codec_pack_merge8_store()`：第一組 lane（0–7）用 **store 覆蓋**整格，
   之後的組才 OR → 每幀唔再 `memset` 整格（16×600 一幀 86,800 B，省 ~2–3.6 ms
   的 PSRAM 寫入）。`nc_pixel_backend_i80.c` 的 `encode()` 已改用它。
2. `drv_i80` 槽配置：**整組 ≤ 48 KB 先試 internal**（internal 唔經 data cache），
   大幀（16×600 一槽 86.8 KB）才落 PSRAM。之前一律 PSRAM，連 1,640 B 的小幀
   都要每幀 msync。
3. `nc_i80_drv_submit()` **移除自家的 `esp_cache_msync`** —— esp_lcd 的
   `panel_io_i80_tx_color()` 已經對 cache 管轄的緩衝做 C2M writeback，之前兩邊
   各做一次。

**實測（SuperMini＋Textkit、6 lane × 20 px、WiFi off、50 fps）**

| | 之前 | 之後 |
|---|---:|---:|
| `tx_us`（submit） | 74–76 µs | **9–13 µs** |
| `enc_us`（編碼） | 194–210 µs | 202–203 µs |
| `frame_us`／fps | 20,000 µs／50.0 | 20,000 µs／50.0 |
| `fail` | 0 | 0 |

映像 841,440 → **842,208 B**（`0xcd8e0`），分區仍剩 46%。

**備註**

- `pixel` 任務本來就係自由填（`fps=0`、`fps_x10=1000`、`render_us` 56 µs）；
  `render` 由 `system.json` 的 `frame_interval_ms` 控 50 fps —— 今次在實機確認
  兩者行為相符（`fill` 保持 2–3、`enc_us` 穩定）。
- `starved` 每秒 +51 是 **mock** lane（冇生產者）造成，不是 i80。
- 0xF061 的 `producer.frames` 與 0xF060 的 `frames`／`fps_x10` 對不上（前者 +2/s、
  後者 100 fps），計數命名要再清 —— 未修。
- 16×600 的**真渲染**仍未做：要先把 `CONFIG_NETCORE_PIXEL_I80_LANES` 由
  `range 1 8` 放寬到 16，並配 16-bit（18 個 dummy 腳）。

### 7.123 2026-11-19（16×600 極限實測：enc 18.4–20.1 ms、tx 1.16 ms、33 fps，然後 WDT 重啟）

**怎麼跑起來的**

- Kconfig 放寬：`NETCORE_PIXEL_I80_LANES` 由 `range 1 8` → `1 16`（8-bit 8、16-bit 16）。
- 新增 `-DNC_EXTRA_DEFAULTS=...`（`firmware/CMakeLists.txt`）：根 CMake 自己 set
  `SDKCONFIG_DEFAULTS`，之前命令列傳第四個 defaults 會被覆蓋 → 16×600 從來冇生效。
- 測試配置（`artifacts/16x600.defaults`，不改 ports）：LEDS=600、LANES=16、
  BUS_WIDTH=16、18 腳＝D1,2,4..18,21（避開 0/3/19/20/26–37/43/44/45/46）。
- 插槽仍有上限：DMA 3 槽、環 3 槽；16×600 一槽 86,800 B → 260.4 KB PSRAM。

**真機數字（SuperMini、16 lane × 600 px、16-bit、50 fps 目標）**

| 指標 | 值 |
|---|---:|
| `enc_us`（編碼） | **18,436–20,071 µs** |
| `tx_us`（提交＝esp_lcd 對 86,800 B 的 msync） | **1,157–1,187 µs** |
| `frame_us` | 37,853–39,581 µs（第 3 次量到 8–12 µs 是重啟後） |
| 實際幀率 | **~33–34 fps**（`render_frames` +33/34 每秒） |
| `fail` | 0 |

**穩定性：唔過關。** 跑幾秒之後會 **Task WDT panic 重啟**（每約 8 秒一次，
`up_s` 一直停在 8）。sdkconfig 證實：`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`、
`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y`、`PANIC=y` —— render loop
（19.6–21.2 ms／20 ms 週期）幾乎 100% 佔住 core 1，IDLE1 餓死 → 5 秒後 panic。

**同 §7.122 的 6×20 比較**：`enc_us` 由 203 µs（120 px）變成 18.4–20.1 ms
（9,600 px）＝約 1.9–2.1 µs/px，線性；`tx_us` 由 9–13 µs 變 1.16 ms，
全部是 86.8 KB 的 PSRAM msync（≈75 MB/s 等效）。

**下一步（未做）**

1. `pixel`（生產者）釘 core 0、`render` 留 core 1 —— 效果計算 ~1.7 ms 就不再同
   編碼爭同一核，core 1 有機會回到 <20 ms。
2. chunked submit（每 4 KB msync＋DMA）把 1.16 ms 攤到 DMA 期間。
3. 若仍超支，把 `frame_interval_ms` 提到 25（40 fps）先求穩定，再逐項優化。

### 7.124 2026-11-19（WiFi on/off 緩衝前後對比；能力閘補上 ETH）

**方法**：同一個 project、同一份 config，只用 `-DNC_EXTRA_DEFAULTS` 加／不加
`CONFIG_NETCORE_WIFI=y`，各自 build → flash → 量。兩邊都係 tests 映像、6×20。

| | WiFi off | WiFi on | 差 |
|---|---:|---:|---:|
| app image | **842,128 B** | **1,368,608 B** | +526,480 |
| `.bss` | 182,752 | 202,000 | +19,248 |
| `.data` | 40,007 | 47,799 | +7,792 |
| IRAM | 58,599 | 85,739 | +27,140 |
| 靜態合計 | 281,358 | 335,538 | +54,180 |
| `h_int`（開機後） | **120,255** | **10,063** | **−110,192** |
| `h_dma` | 112,467 | 2,275 | −110,192 |
| `h_ps` | 1,560,920 | 1,560,848 | ≈0 |
| plan `int`／`psram` | 5,280／526,903 | 5,280／526,903 | 0 |

- **plan 完全一樣**：WiFi 不在 plan 裡（driver 自己 malloc），所以「宣告制」看不到它。
- 靜態 54 KB 之外，**runtime 再多 ~56 KB**（110,192 − 54,180）：`status rt` 多出
  `wifi`（HWM 4,508）、`tcpip`（2,832）、`sys_evt`（2,040）、`espnow_rx`（3,160）
  等任務與驅動緩衝。
- **更正（同日）**：這 110 KB 是 **tests 映像**的數字，其中只有 **54 KB 是「編進來」
  的靜態成本**，另外 ~56 KB 是 **測試 hook**（`nc_test_action_espnow_boot()`，main.c
  B08 之後）自動開 ESP-NOW 造成的 —— 唔係 `enable:0` 的代價。
  真正規則：`network.json` 的 `wifi.enable=0` → `nc_wifi_boot()` 在建 netif 之前就
  return（`nc_wifi.c:615-619`）→ **0 B runtime**。所以：
  - **編進來**（`CONFIG_NETCORE_WIFI=y`）＝ 靜態 54 KB ＋ flash 526 KB，無論開唔開。
  - **開啓**（`enable=1` 且有名單）＝ 才付 runtime heap ＋ 任務 stack。

**能力閘補齊**：`nc_net_lines.c` 之前只擋 `iface=wifi`（未編入時）。現在連
`iface=eth` 都檢查 `hardware.json` 的 `ETH.enable`：`0`（或冇 ETH 段）→ 跳過並
警告，不再照 bind（冇 lwIP 會 `assert: Invalid mbox`）。這是「先確保有能力、
再設定」在 network 層的落實。

### 7.125 2026-11-19（指令表＋任務都改成「自己現身」；`main.c` 冇清單）

- **Action**：`NC_ACTION_TABLE_DEF` 產生 constructor → `nc_action_add()`；`main.c`
  只叫 `nc_action_init_all()`。登記表 8 格起、滿了配大（**冇固定上限**）。
- **Task**：`NC_TASK_AUTO_REG(tag, &task)` 同樣在 constructor 登記；`main.c` 的 6 個
  `nc_task_register()` 全部刪除。次序改由 `(level, order)` 決定
  （nc4 10／net 10／hw_gpio 10／render 10／pixel 20／ncm 30 ），唔受 link 次序影響。
- **必要條件**：`file_action`／`ota_action`／`test_action`／`hw_gpio`／`render_task`／
  `pixel_task` 都要 `WHOLE_ARCHIVE` —— 冇 symbol 被 main 引用，linker 會整份丟掉
  （實測：漏加時映像少 7 KB、任務 0 個）。
- **踩到的 bug**：`app_main` 原本有 `nc_task_reset()`，在 constructor 之後把登記清空
  → 開機「0 個任務、0 條通道」。已移除並在現場留註解。
- **驗證**（S3 6×20 測試映像）：任務次序 nc4→net→hw_gpio→render→pixel→ncm、
  `cmds` 85（0xF0xx 58）、50 fps、`enc_us` 191、`h_int` 120,247。
- **方向**：`modules/` 之後放所有測試／模組化功能；build 工具在特定模式下把
  `modules/actions/` 當成 `firmware/components/action/`（tasks 同理），因為兩者都
  自行登記，所以「放進去就生效」；另一條路是編成 `.nct/.nce/.nca` 由 ncm 載入。

### 7.126 2026-11-19（Matrix 板事實修正、Matrix-LED-Textkit project、上板驗證）

- **板子定義修正**：`ports/hardware/S3/ESP32-S3-Matrix` 之前寫「無 PSRAM」，實際是
  **ESP32-S3FH4R2：4 MB flash ＋ 2 MB PSRAM（quad 80 MHz）**。
  `board.json` 的 `psram` 已補、`sdkconfig.defaults` 由 `CONFIG_SPIRAM=n` 改成
  `SPIRAM=y／QUAD／80M`；notes 補上 IMU（QMI8658，I2C SDA=11／SCL=12，來自 vendor demo）。
  來源：Waveshare 文件、CircuitPython 板頁、`ESP32-S3-Matrix-Demo`（WS_Matrix.h：GPIO14、64 顆）。
- **新 project**：`ports/project/S3/ESP32-S3-Matrix-LED-Textkit`（alias `matrix-textkit`），
  hardware = `S3/ESP32-S3-Matrix`，預置沿用 64 顆／50 fps／render+pixel core1。
- **上板驗證**（Matrix 映像 `1e23943d`，USB CDC COM24）：
  `0xF061` → `lanes=[{be:"i80",lanes:1,width:8,px:64}]`、`fps=50`、`enc_us=348`、
  `frame_us=19,985`、`h_int=102,119`、`h_ps=1,560,896`。1 lane 64 顆的 units
  ＝4,808 B（比 6×20 的 1,640 B 多），所以 enc 348 µs 是預期，不是回歸。
- **注意**：Matrix 映像 `console=none`、log 走 TinyUSB CDC ＋ MSC（VID 303A:4003），
  燒錄後 USJ 的 COM 會消失、出現新的 CDC COM。

### 7.127 2026-11-19（行為分類重新設計：三平面 CORE／USER／DATA；參考 mp_Net-Core/slave）

- **背景**：`firmware/` 冇正式位置放效果器；`sw/pixel/nc_pixel_producer.c` 一個檔做效果數學、
  抖動、demo／時間基準、參數面。文件只有零散規則，冇一份講「邊種行為放邊」。
- **參考 MP**：`mp_Net-Core/slave` 全部係 user space（`.py`＋JSON），所以只需要一個軸
  （角色）；佢有四個習慣值得照抄 —— **資料先於程式**（畫波寫 JSON，寫唔出才寫 py）、
  **框架／內容分離**（`lib/sw/effect_core.py` vs `pixel/effects/`）、**計算／播送分離**
  （`pixel_task` vs `render.py`）、**檔案系統就是註冊表**（`effects.json`／`map`／`modes`／
  `registry.json`）。
- **定案：角色 × 平面**。C 有編譯邊界，要多一個軸：
  **CORE**（`firmware/components/`，特權、開機必需）／**USER**（repo 根 `modules/`，載入、
  沙箱）／**DATA**（裝置 `/data/`，值與宣告）。**USER 只准 effect／task／action／ui 四個
  角色**；driver／bus／sys／sw／host 永遠唔准（佢哋碰硬件、記憶體、載入器）。
- **效果器**：一個角色、**三種交付**（DATA program／CORE C／USER `.nce`，同一 canonical
  frame 契約）、**四層資料**（effect／map／mode／playlist）。**框架**（契約、registry、
  program interpreter、波表）住 `sw/pixel`；**內容**住 `components/effects/<域>_effects/`；
  **mapping／modes／playlist** 住 `/data/pixel/`。
- **落地**：[behavior-taxonomy.md](../03_notes/behavior-taxonomy.md) 重寫（設計階段，
  §8 有 Q1–Q10 待定）；`firmware/components/effects/README.md`、`component-layout`、
  `docs/README` 索引、`modules/README` 已更新。CORE-only 的 component 目錄暫時只有
  README，冇 CMakeLists（加了 IDF 就會當它是一個 component）。
- **未做（M1–M8）**：拆框架／內容 → 契約＋畫布由環 px 決定（取消 64 限）→ DATA program
  ＋interpreter → mapping＋scatter → modes＋playlist → `.nce` framebuffer → `pixel_action`
  （release）＋主機推送。
- **判準一句**：加一個效果應該係「加一個 JSON／丟一個 `.nce`」；要重燒 C 才出得到，
  就代表擺錯咗平面。

### 7.128 2026-11-20（「效果」拆名：引擎／實作／呼叫方；`sw/pixel/engine/`）

- **背景**：§7.127 定咗「框架住 `sw/pixel`、內容住 `components/effects/`」，但「effects」
  一個字同時指**工具（engine）**同**實作**，文件同目錄都混住 → 擺位判斷唔到落手。
- **定案（D11）**：三個名分開 —— **引擎**（工具：契約／registry／program interpreter／
  波表）住 `firmware/components/sw/pixel/engine/`（`nc_px_engine_*`）；**實作**住
  `firmware/components/effects/<域>_effects/`（`nc_effect_<name>.c`）；**呼叫方**住
  `tasks/`·`action/`·`host/`。規則一句：**`sw/` 只放工具**，任何「用工具做出嚟嘅嘢」
  （效果實作、task、action）都唔准住 `sw/`。
- **命名**：引擎 `nc_px_engine_*`（契約 `sw/pixel/include/nc_px_engine.h`、
  `engine/nc_px_engine_core.c`／`nc_px_engine_program.c`）；實作 `nc_effect_<name>.c`
  （域由 component 名講）。**原本的 `nc_fx_*` 全部唔用**（`fx` 意思唔清楚）。
- **目錄**：`sw/pixel/` 之下 `ring/`·`encode/`·`dither/`·`engine/`；`effects/` 同
  `tasks/`·`action/` 同層（唔係 `sw/` 子目錄）。
- **落地**：[behavior-taxonomy.md](../03_notes/behavior-taxonomy.md)（新增 §2.0 詞彙、
  D11、§3.3 改「引擎 vs 實作」、§6 命名、§9 M2–M3、§12 加第四條檢查）、`naming.md`、
  `component-layout.md`、`effects/README`、`modules/README`、`docs/README` 索引。**程式未改**。
- **下一步**：Q1／Q5／Q9 拍板 → M2 拆 `nc_pixel_producer`（835 行 → 引擎＋實作＋呼叫方）。

### 7.129 2026-11-20（M2 落地：引擎 `sw/pixel/engine/` vs 實作 `components/effects/pixel_effects/`）

- **做咗**：`nc_pixel_producer.c`（835 行：效果＋抖動＋demo＋setter 混在一起）拆成三份 ——
  **引擎**（`sw/pixel/engine/nc_px_engine_core.c`＋`nc_px_engine_tables.c`；契約
  `include/nc_px_engine.h`；`nc_px_engine_*`）、**實作**（`components/effects/pixel_effects/`
  `nc_effect_{zone1,breath,wave,solid,ramp}.c` ＋ `nc_effect_table.c`；`nc_effect_*`）、
  **呼叫方**（`tasks/pixel_task`：on_start 呼叫 `nc_pixel_effects_register()`）。
- **契約**：效果收到 `nc_px_engine_in_t { px, w, h, zone, now_us, params }`，只寫
  `uint16_t rgb[px*3]`（8.8 顯示值畫布）。量化／抖動／W 通道由引擎做 —— 效果唔知道
  位數、抖動、lane、環、task。
- **驗收**：新增 `tools/hosttest/fx_test`（15 個案例 × 15 幀，FNV-1a payload hash）。
  **重構前後 15/15 hash 完全一樣**（`golden.txt` 係重構前影的）。`run_all.sh` 15 支測試中
  14 OK；`task_test` 3 個失敗係 **HEAD 已經有**（clean worktree 一樣 fail），唔關今次。
- **建置**：**8 塊板全過**（C3／P4×3／S3×4，另加 `matrix-textkit` project）；新檔案 0 warning
  （C3／Matrix 各 2 個 `nc4_line_net.c` unused 係舊有）。Mini-Nano 之前**建唔到**
  （`nc_pixel_bench.c` 冇 guard，`CONFIG_NETCORE_PIXEL_*` 未定義）→ 已加
  `#if CONFIG_NETCORE_PIXEL`（HEAD 一樣壞，屬順手修）。
- **未做（M2b／M3）**：demo＋0xF060 參數面仍住引擎（要改狀態通道才搬得去 task）；
  `ring/`·`encode/` 子目錄化（純分類）；畫布由環 px 決定（取消 8×8／64 px 寫死）。
- **判準**：加一個效果而家係「`effects/pixel_effects/` 加一個 `.c` ＋ 表加一行」——
  引擎、task、環、driver 一律唔使改。

### 7.130 2026-11-20（DATA 平面命名定案＋`fx` 全面退役；文件先行）

- **背景**：設計「用戶空間 `/data/pixel/` 要放什麼」時，我提議 `nc_px_fx_t`，被 §7.128 的
  「`nc_fx_*` 全部唔用」擋住 → 一次過把命名、檔案佈局、結構體規則寫入文件。**程式未改**
  （除新增量測工具）。
- **命名定案**：
  - `fx`／`nc_fx_*` **全面退役**：code、文件、測試目錄、CLI 一律唔再用。舊的
    `tools/hosttest/fx_test/` ＋ `fx_api.h`（M2 遺留）→ 已改名 `pxEngine_test/` ＋
    `nc_px_engine_test.h`（§7.131）。
  - **DATA 平面**的結構體：`nc_px_asset_t`（一個效果資產：`name`／`kind`／`prog`｜`nce`）、
    `nc_px_seg_t`（波形段：`type`／`f`／`l_max`／`l_lim`／`phi`／`end_f`）、
    `nc_px_asset_tbl_t`（掃描結果）；四層其餘 `nc_px_map_t`／`nc_px_mode_t`／`nc_px_show_t`。
    讀取器住 `sw/pixel/asset/`（工具），header `include/nc_px_asset.h`。
  - **三個名唔可以共用**：引擎 `nc_px_engine_*`／實作 `nc_effect_<name>`／資產 `nc_px_asset_*`
    （＋呼叫方：`tasks`／`action`／`host`）。見 behavior-taxonomy.md §2.0。
- **佈局定案**：`effects/` 同時收三種載體 —— `effects.json`（一檔多效果，快速改波）、
  `*.json`（一檔一效果，複雜結構）、`*.nce`（外掛）。三者掃入**同一張資產表**；
  key = `name`；掃描順序 `effects.json` → `*.json`（檔名排序）→ `*.nce`；撞名先到者勝＋log；
  `id` 只作 alias。**掃到就算**，用不用由 `registry.json` 的 `list` 決定。
- **結構體優先**：JSON 只在載入時讀一次（`nc_px_asset_scan()`），熱路徑只行 struct，
  唔再碰 cJSON／字串；壞檔／缺字段／超上限 = 跳過＋log＋保留 last-good。
- **出廠空骨架**：`registry.json` = `{"version":1,"auto_play":false,"list":[]}`，其餘目錄空
  （冇預設內容）；firmware 只讀，`ASSET_RELOAD` 才重掃。
- **數值域**：program 段保留 MP 的 **12-bit（0..4095）**概念；引擎提供
  `nc_px_engine_sin_q12()`（多項式，無表）；入 8.8 畫布時 `<<4`。
- **量測（新工具 `tools/hosttest/sin_bench/`，唔入 `run_all.sh`；host x86-64，MSVC／gcc 皆可）**：
  10,000 次正弦 = 查表 **0.0020 ms**／多項式 **0.0117 ms**（每幀 192 次 ≈ 0.04／0.22 µs，
  對比 16.7 ms 幀預算可忽略）。精度（1 LSB of 4095）：1024 點表 max 24.8、4096 點表 5.95、
  多項式 3.62 → **多項式更準、省 2 KB 表、免 `powf`/`sinf`**（順帶修掉 golden 綁 host libm 的風險）。
- **未做（程式）**：`nc_px_asset.*`、四層 parser、`nc_px_engine_program`（interpreter）、
  `.nce` 執行後端（WAMR）。
- **影響文件**：`naming.md` §1／§2、`behavior-taxonomy.md` §2.0／§3.2、
  `config-layers.md` §5.1／§6。

### 7.131 2026-11-20（`fx_test` → `pxEngine_test`；`sin_q12` 落地；golden 交叉驗證）

- **改名（§7.130 的收尾）**：`tools/hosttest/fx_test/` → `tools/hosttest/pxEngine_test/`；
  `fx_test.c` → `pxEngine_test.c`；接縫 header `fx_api.h` → `nc_px_engine_test.h`
  （巨集 `fx_*` → `nc_px_test_*`、`FX_ZONES` → `NC_PX_TEST_ZONES`）。`git mv` 保留歷史；
  `run_all.sh`、`sw/pixel/CMakeLists.txt`、`effects/README`、文件全部跟改。**golden 內容未動**。
- **新增 `run.bat`**（Windows／MSVC，跟 `pixel_test` 的 cl/gcc 慣例）：建置 → 跑 → `fc` 對 golden；
  `--update` 可重影。
- **新增 `nc_px_engine_sin_q12()`**（`sw/pixel/engine/nc_px_engine_tables.c`）：0..65535 相位 →
  0..4095，拋物線＋二次修正，無表無浮點；`nc_px_engine.h` 已宣告，`!CONFIG_NETCORE_PIXEL`
  分支有 no-op stub。**現有 5 個效果未用它**（所以畫面不變）。
- **驗證**：
  - `pxEngine_test` 15 個 case 的 payload hash **同 golden 逐 byte 一樣**。
  - **MSVC（19.51 / UCRT）與 glibc 得到同一串 hash** → §7.130 提到的「golden 綁 host libm」
    實測**唔成立**（至少呢 15 個 case）：表值經 `lroundf` 後一致。兩邊都可以做閘門。
  - `sin_q12` 加咗純整數斷言（15 個取樣點、0..16384 單調、上下半週期對稱）＋ Python 獨立
    計一次期望值，兩邊一致；斷言成功唔印嘢，所以 golden 不受影響。
- **新增 `nc_px_asset.h`**（`sw/pixel/include/`）：資產層的 struct 定稿 —— `nc_px_seg_t`
  （段）、`nc_px_asset_t`（一個效果資產：name／kind／nseg／pixel_hint／seg[]）、
  `nc_px_asset_tbl_t`（`NC_PX_ASSET_MAX = 32`）；API `nc_px_asset_scan/clear/count/at/find/json`。
  `map`／`modes`／`registry.json` 三層**未定**（等 resolver 一齊做，Q2–Q4 仍然開）。`/W4` 乾淨。
- **未做**：`nc_px_asset_scan()` 實作（cJSON＋store FAT）、program interpreter、`.nce` 執行後端。



### 7.132 2026-11-20（`.nce` ABI 改名：`fx_*` → `nce_*`；PoC 重 build 並實跑）

- **背景**：§7.131 留了 ABI 的 `fx_init`／`fx_render`／`fx_param` 做例外；使用者要求
  「完全唔想留一點痕跡」→ **連 ABI 一起改**。
- **改名**：`NC_MOD_EXPORT_INIT/RENDER/PARAM` = `"nce_init"`／`"nce_render"`／
  `"nce_param"`（`nc_mod_abi.h`）。`nce` = `.nce` 的簡稱 ＝ 效果的**實作**（USER 平面），
  同 `nc_effect_*`（CORE C 實作）、`nc_px_engine_*`（工具）、`nc_px_asset_*`（DATA）四邊分開。
  （同日中途試過 `pxEffect_*`，最後定 `nce_*`：`px` 係工具字頭、`Effect` 又同 C 實作撞字。）
- **影響**：記憶體佈局（desc 28 B、param +32、data +96）**冇變**，不用改 offset；舊 `.nce`
  會因為缺 `nce_render` 被拒絕載入 —— 重 build 即可。header 已寫明。
- **PoC 重 build**：用 `wat2wasm`（wabt，臨時 npm 安裝）重建三支：`mod.wasm` 367→370 B、
  `mod2` 338→341 B、`modbad` 105→108 B；`host.mjs`、PoC `README`、
  `effect-language.md` §10 的實錄同步更新。
- **驗證（實跑 `node tools/poc/mod_poc/host.mjs`）**：
  - 基本：240 幀、`nce_init ok`、checksum `0x137be526`、scratch 240 次。
  - 熱替換：`--swap mod2.wasm --at 120` → 兩段 phase 各自有 checksum，中途不重啟。
  - 壞檔：`--bad modbad.wasm` → 拒絕載入、全程用 `mod.wasm`、畫面不中斷。
  - `wasm-objdump -x`：export 只有 `mod_abi_version`／`nce_init`／`nce_render`。
- **文件**：`naming.md` 由「例外（唔准改）」改成「冇例外」；`effect-language`／
  `ncm-contract`／`ncm-loader`／`behavior-taxonomy` M7 全部跟改。
- **未做**：板上（WAMR）未重驗 —— WAMR 執行後端本身未接，今次只到 PoC／host 層。

### 7.133 2026-11-20（DATA 平面讀取器落地：`nc_px_asset` 解析 ＋ 掃目錄；新 host 測試）

- **做咗**（`firmware/components/sw/pixel/asset/`）：
  - `nc_px_asset.c`：**純解析核心**（唔碰檔案系統）——JSON 文字 → `nc_px_asset_t`，
    支援 MP 的批次形式 `{"effects":[...]}` 同單一效果 `{...}`；未知鍵跳過（`step`／
    `spacing`／`reverse` 暫時唔用）、未知段型別／超範圍 = **整個資產拒絕**（寧願唔畫，
    好過畫錯）、撞名保留第一個（`dup`）、上限 `NC_PX_ASSET_MAX = 32`／`NC_PX_SEG_MAX = 16`。
    附 `nc_px_asset_json()` 狀態（`n`／`rej`／`dup`／items）。
  - `nc_px_asset_scan.c`：**唯一有 I/O 的殼**——掃 `effects.json` → `*.json`（檔名排序）
    → `*.nce`；`*.json` 冇寫 `name` 就用檔名；`*.nce` 只登記 kind=NCE（manifest 由 ncm 解）；
    壞檔／讀唔到／大過 16 KB 跳過＋log（計入 `rejected`），唔會令整張表失效；讀檔用臨時
    `malloc`（掃描係開機／換場才做），唔佔 16 KB BSS。
  - 手寫迷你 JSON 讀取器（無 cJSON、無 malloc、深度上限 8、食 UTF-8 BOM）：解析係最容易錯的
    一段，寧願自己控制有界行為，亦令**同一份 code 可以在 PC 上測**。
- **測試**：新 `tools/hosttest/pxAsset_test/`（`run.sh` ＋ `run.bat`；入 `run_all.sh`）。
  53 個檢查全綠：批次／單檔／檔名補名／壞 JSON／未知段／`pixel_n` 超範圍／撞名／上限／
  未知鍵／BOM／狀態 JSON／**掃目錄**（先後次序、檔名 hint、`.nce`、壞檔計數）。Windows 用
  `include/win/dirent.h` shim（`_findfirst`），POSIX 用真 `dirent.h`。
- **未做**：`nc_px_engine_program`（用段落表畫畫布）、`pixel_task` 接線（`nc_store_fs_base()`
  ＋ `/pixel/effects`）、`map`／`modes`／`registry.json` 三層、`.nce` manifest 取真名。
  全部未經 IDF 建置驗證（本機冇 IDF／板）。
### 7.134 2026-11-20（正弦算法對照：host 微基準 ＋ ESP32-S3 實機 render_us）

- **背景**：要決定「查表（現時）vs 多項式（MP）」邊個留。之前只有 host 數字；今次補實機。
- **工具**：`tools/hosttest/sin_bench/sin_bench.c`（5 種算法、min/median/p99、精度、記憶體）、
  `dev_probe.py`（經 NC4 0xF060 切效果／分區，讀 `render_us`；用完還原）。
- **Host（x86-64、MSVC /O2；10k 次 = 一幀 192 次的 52 幀）**：

  | 算法 | min ns/call | median | ms/10k | max 誤差（12-bit LSB） | 表 |
  |---|---|---|---|---|---|
  | lut_1024（現時） | 0.20 | 0.20 | 0.002 | 24.76 | 2 KB |
  | lut_4096 | 0.20 | 0.20 | 0.002 | 5.95 | 8 KB |
  | poly_q12（MP） | 1.13 | 1.20 | 0.011 | **3.62** | 0 |
  | cordic_q15 | 12.12 | 12.52 | 0.121 | **1.43** | 60 B |
  | sinf（libm） | 3.35 | 3.40 | 0.033 | — | — |

- **實機（ESP32-S3、fw 0.1.2、IDF 6.1.1、64 px、8-bit、無抖動）**，`render_us`（引擎量的
  effect＋抖動＋emit），每個效果 30–40 次取 median：

  | 效果 | sine/幀 | min µs | median | p90 |
  |---|---|---|---|---|
  | OFF | 0 | 53 | 55 | 59 |
  | SOLID | 0 | 53 | 56 | 60 |
  | ZONE1 | 0 | 54 | 56 | 61 |
  | RAMP | 0 | 56 | 57 | 60 |
  | BREATH | 8 | 59 | 60 | 63 |
  | WAVE | 72 | 87 | 87 | 88 |

- **分區掃描（驗線性）**：0／16／32／64 sine → 57／64／72／87 µs，每 8 px 一區 +3.75 µs。
- **斜率**：`(87 − 55) µs ÷ 72 ≈ 444 ns`／每個「sine ＋ wave 其餘數學」（240 MHz ≈ 107 cycles）。
  基線 55 µs 係抖動＋emit（64 px × 3 通道），同效果無關。
- **結論**：
  - 64 px 之下，兩種算法都係 noise（WAVE 全部 72 次 sine 只佔一幀 0.18%）；
  - 大畫布（16×600 = 9600 px）就唔同：一樣的 per-pixel 數學 ≈ 4.3 ms/幀（50 fps 預算 20 ms 的 21%），
    所以「sine 佔幾多」值得量清楚；
  - 精度同速度係反比：poly 3.62 LSB、cordic 1.43 LSB，但 poly 慢 5.6×、cordic 慢 60×（host）。
    折衷係 4K 表（5.95 LSB、8 KB、同 1024 一樣快）。
- **未做**：實機**逐次** sine 成本（要 flash 新固件；今次只量到「sine＋其餘數學」的合成斜率）。
  建議加一條 `0xF063 SINBENCH`（10k 次 × 4 種算法），一次 flash 就有齊。
### 7.135 2026-11-20（新方法：錨點表＋線性插值「混合正弦」；`nc_px_engine_sin_lin()`）

- **動機**：§7.134 的對照顯示兩個極端 —— 查表（快、表大、最近點誤差 12.4 LSB）同多項式
  （無表、慢 5.6×、誤差 3.7 LSB）。混合就係各取一半：**表提供精確錨點，錨點之間用一次式補**。
- **算法**（`nc_px_engine_sin_lin`，Q15 錨點 → 0..4095）：
  `i = (phase >> 8) & 255`、`f = phase & 255`、
  `v = t[i] + ((t[i+1] − t[i]) · f >> 8)`（Q15 有號），最後 `(v >> 4) + 2048`。
  誤差 ∝ (2π/n)²：n=64 → 2.5 LSB、n=128 → 0.69、n=256 → 0.23。
- **Host 量測**（同 §7.134 的機器／序列，10 萬次 × 120 批，min ns/call）：

  | 方法 | 表 | min ns | ms/10k | max 誤差（0..4095） |
  |---|---|---|---|---|
  | lut_1024 | 2 KB | 0.20 | 0.002 | 12.38 |
  | lut_4096 | 8 KB | 0.20 | 0.002 | 2.97 |
  | poly_q12（MP） | 0 | 1.13 | 0.011 | 3.74 |
  | **hyb64** | **130 B** | **0.49** | **0.005** | **2.53** |
  | **hyb128** | **258 B** | **0.48** | **0.005** | **0.69** |
  | **hyb256** | **514 B** | **0.46** | **0.005** | **0.23** |
  | cordic | 60 B | 12.0 | 0.120 | 0.66 |
  | sinf | — | 3.34 | 0.033 | — |

  ⇒ 混合（256）**比多項式快 2.4×、準 16×，表只 514 B**（1024 點表的 1/4）。
- **落地**：`nc_px_engine_sin_lin()` 加咗入引擎（表在 `tables_init` 建，514 B）；
  MP 多項式保留做 `nc_px_engine_sin_q12()`（對位 MP 用，行為不變）。
- **驗收**：`pxEngine_test` golden **15/15 逐 byte 不變**（新函式冇人用），另加 10 個取值、
  0..16384 單調、上下半週期對稱的斷言（成功唔出聲）——全綠。
- **未做**：板上逐次 ns/call（要 flash；今次只有 host 數字＋§7.134 的實機合成斜率）。

### 7.136 2026-11-20（program interpreter 落地：`nc_px_engine_program_*`；波形語義對齊 MP）

- **做咗**：`sw/pixel/engine/nc_px_engine_program.c` ＋ `include/nc_px_program.h` ——
  compile（段落 → 預算值）、value（幀 → 12-bit 等級）、phase（第 t 幀起點）、
  levels（一幀的空間分布）、canvas（12-bit → 8.8 畫布：gain／gamma／顏色）。
  純函數、有界、無 malloc；compile 之後熱路徑只剩陣列索引＋加減＋一次正弦（`sin_q12`）。
- **語義權威 = MP**（`mp_Net-Core/slave/lib/sw/PixelMathMethod.py` 的 compile／value_at、
  `effect_core.py` 的 `Effect.frame`），刻意保留：
  `step_phase = (65536·F)/10/fs`（**兩次整除的次序**）、`phi4 = phi<<4`、
  `l_range = l_max − l_lim`、`gap = fs/F`、`width = pulse % gap`、
  `g0 = ((t/speed)·step + offset) % total`、之後每顆 px `g += spacing % total`（單次減法取模）、
  reverse 由尾寫起。C 唯一偏離：`l_range` clamp ≥ 0、輸出 clamp 0..4095（壞資料唔會出負數）。
- **契約加嚴（`nc_px_asset.c`）**：
  - `pulse` 入 struct（`nc_px_seg_t`，缺省 **2047** = MP 預設）；以前 `pulse` 被當未知鍵跳過，
    `pulse_wave` 會靜靜畫錯。
  - `end_Time` 必須**嚴格遞增**（0 亦唔收），否則整個資產拒絕 —— MP 的波長 = 最後一段
    `end_Time`，唔遞增的 program 畫唔出連續波形。副作用：`{"type":"keep"}`（冇 end_Time）
    以前照收但實際畫唔到，現在會大聲拒絕；`pxAsset_test` 的 fixtures 已補。
- **驗證**：新 `tools/hosttest/pxProgram_test`（入 `run_all.sh`；`run.sh`＋`run.bat`）。
  **期望值全部由 Python 版跑出來**（唔係 C 自己解釋一次）：
  3 個 program 的 compile 欄位逐項比對、320／300／128 點波表 FNV-1a 指紋、段邊界／pulse 開關／
  繞回取樣、4 組 frame（speed 1／2 × reverse）、phase、canvas（gain／gamma／顏色）、7 個拒絕路徑。
- **順手修（測試漂移）**：`task_test` 自 e0b4617（2026-11-19「自行登記」）之後一直紅 3 項 ——
  測試仍當「未設定 = 照跑」，但現行契約係「未設定 = `[0,0]` = 唔跑（連 on_start 都唔跑）」。
  已把 fixture 改成明確配置 beta／gamma／delta，並把「未設定」的期望改成唔啟動。
  `bash tools/hosttest/run_all.sh` **16 支全綠**（新增 pxProgram_test 之後）。
- **未做**：`nc_effect_program`（把選中的資產畫上畫布的效果）＋ `pixel_task` 接線
  （`nc_store_fs_base()` ＋ `/pixel/effects`）、`registry.json`／`map`／`modes` 三層、
  `.nce` manifest 取真名。全部**未經 IDF 建置**（本機冇 IDF／板）。

### 7.137 2026-11-20（算法工具收斂：sin_bench 量出貨函數、0xF063 上板量測、精度閘門）

- **問題**：`sin_bench` 自己抄了一份多項式／混合算法。固件改咗而工具冇跟，量到的就係
  另一個算法；而且「實機逐次 sine 成本」一路未量到（§7.134 的「未做」）。
- **工具改法**（`tools/hosttest/sin_bench/`）：
  - `sin_bench.c` 直接 **link `sw/pixel/engine/nc_px_engine_tables.c`**，主行就是出貨中的
    `eng_sin`／`eng_sin_lin`／`eng_sin_q12`（★ 標記）；只保留對照組
    （`lut_4096`／`hyb64`／`hyb128`／`cordic`／`sinf`）。**工具冇得再走樣**。
  - 新增 `--quick`（REP=20）、`--json`（存檔／前後對比）；輸出多 rms 誤差與表大小。
- **量到一個真問題**：`nc_px_engine_sin_lin()` 的 Q15→12-bit 用**截斷**（`>>4`），
  平均偏 −0.5 LSB → 出貨版本的 max 誤差係 **1.23 LSB**。
  ⚠️ **更正 §7.135 的「0.23 LSB」**：那只係**插值誤差**，唔計 Q15→12-bit 這一步量化。
  改成四捨五入（`(v + 8) >> 4`，峰值 clamp 4095）→ **0.72 LSB**，速度不變。
  另外：直接 link 真函數之後，`sin_lin` 同 `sin_q12` 在 host **打成平手**（≈1.8 vs 1.8 ns；
  §7.135 的「快 2.4×」係工具自己抄一份時的數字）—— `sin_lin` 的價值在**精度 5×**
  （0.72 vs 3.74 LSB）同「唔使跟 MP 的公式」，唔係速度。
  `pxEngine_test` 的 10 個 `sin_lin` 期望值同步更新（`2048→2448`、`4096→2832`、
  `40960/57344→600`）；golden 不變（冇效果用 `sin_lin`）。
- **精度定義修正**：理想值要先夾到 **[0, 4095]**（12-bit 表示不到 4096；峰值 p=16384 的
  ideal = 4096，唔夾就永遠差 1 LSB）。host bench／`pxEngine_test`／板上 0xF063 三處用同一條定義。
- **新指令 `0xF063 SINBENCH`**（`test_action`，`CONFIG_NETCORE_PIXEL` 內）：
  payload `[iters:u16 LE]`（預設 50000，夾 100..500000）→
  `{"n","smp","q12":{"ns","err"},"lin":{…},"sin":{…}}`（`err` = milli-LSB）。
  相位用 LCG、3 次取最小。配 `dev_probe.py --sin`，一次 flash 就有齊實機數字。
- **精度閘門**：`pxEngine_test` 新增 `check_sin_accuracy()` —— 三個函數對 double 的
  全相位最大誤差（sin ≤ 14.5／sin_lin ≤ 1.0／q12 ≤ 4.0 LSB）。成功唔出聲，golden 不變；
  已驗證收緊界限會即刻紅（收緊到 0.1 → max 0.72 即刻報）。
- **文件**：`commands.md` 補 `0xF062`（之前漏了）＋ `0xF063`；`sin_bench/README` 重寫。
- **未做**：上板跑 `0xF063`（要 WSL/IDF flash；本機冇 IDF）—— 數字一有就補入 §7.134／§7.135。

### 7.138 2026-11-20 晚（上板：0xF063 實機 sine 數字；OTA 兩次＋release 驗證）

- **上板環境**：ESP32-S3-SuperMini ＋ project `ESP32-S3-SuperMini-LED-Textkit`
  （6 條 WS2812、GPIO1–6、每條 20 顆），IDF **v6.0.3**、macOS 本機 `~/esp/esp-idf`。
  ⚠️ 更正 §6「只有 WSL 能 build」：macOS 一樣 build 得（`source ~/esp/esp-idf/export.sh`
  之後 `python3 make.py build …`），所以本機可以 build → OTA 一條龍。
- **實機 sine（`0xF063 SINBENCH`，iters=50000，**65536 個相位全掃**）**：

  | 算法 | host ns | 實機 ns | ≈cycles@240 MHz | max 誤差（LSB） |
  |---|---:|---:|---:|---:|
  | `nc_px_engine_sin`（1024 點 Q15） | 1.29 | **88** | 21 | 13.31 |
  | `nc_px_engine_sin_lin`（256 錨點） | 1.83 | **146** | 35 | **0.72** |
  | `nc_px_engine_sin_q12`（MP 多項式） | 1.81 | **157** | 38 | 3.74 |

  ⇒ 板上 `sin_lin` 比多項式**快 7%**、準 **5.2×**；host 兩者打平（§7.137 已更正「快 2.4×」
  係工具抄算法時的數字，作廢）。1024 查表最快，但誤差 13.3 LSB。
- **量測陷阱（已修）**：0xF063 最初每 16 個相位取樣，`sin` 報 10.37 LSB，實際 13.31 ——
  峰值剛好落在取樣點之間。改成**全掃 65536 點**（回覆 `smp:65536`，三個算法共用一次
  double `sin()`）之後，實機數字同 host **逐位一致**（0.72／3.74／13.31）。
- **OTA 驗證（同一塊板、USB CDC，`tools/ota.py`）**：

  | # | 映像 | 大小 | BEGIN | WRITE | END | 重開後 |
  |---|---|---:|---:|---:|---|---|
  | 1 | tests | 838,944 B | 3.3 s | 132.4 s（6.2 KB/s） | ✅ verified | `app ad49f374`、`h_int 120,663` |
  | 2 | release | 678,448 B | 2.6 s | 108.3 s（6.1 KB/s） | ✅ verified | `app 7a4b7e37`、`h_int 219,347` |
  | 3 | release（0xF063 修正後，dirty 樹） | 678,448 B | 2.6 s | 108.3 s | ✅ verified | `app a96af8fc` |
  | 4 | release（commit `4fb33ea`，乾淨樹） | 678,448 B | 2.6 s | 106.9 s（6.2 KB/s） | ✅ verified | `app 63c2259f`、`git_dirty=false` |

  - 三次 `APPLY` 都自動重開；`storage.boot.fw_new=1` → 換韌體照設計格式化 `/data` 再 provision
    （所以 `/data` 回到 project 預置，config 的 sha 會變）。
  - 重開後任務表全部起得來：`nc4`（`decode0` run）、`net`、`hw_gpio`、`render`（`render` run）、
    `pixel`（`prd` run）；`prd` 計數 34,779（≈1,000/s）、`render` 1,582、`decode0` 3,164。
  - `0xF060`／`0xF061` 只存在於 tests 映像；release 映像冇（符合 `NC_ENABLE_TESTS` 契約）。
- **建置**：tests 819.3 KB（分區剩 47%）、release 662.5 KB（剩 57%）；WiFi 未編入
  （project 預設 `CONFIG_NETCORE_WIFI` 註解、runtime `network.json.wifi.enable=0`）。
- **未做**：P4-ETH（`10.10.1.50` 今次不在線）未上；OTA 速度 6 KB/s 明顯低於文件記錄的
  140 KB/s（未查是 macOS 序列埠還是 chunk 設定）。

### 7.139 2026-11-20 晚（DATA 平面第二～四層：map／modes／registry 的 C 解析器）

- **問題**：ESP 側一路只有第一層（effects.json 的 program）。mp 的四層結構
  （effect／mapping／mode／registry）在 ESP 完全冇對應，所以「讀陣列分組」同
  「委派（group×effect×write×range）」都做不到。
- **做咗（資料層，未接硬件）**：
  - 抽出共用迷你 JSON reader：`include/nc_px_json.h` ＋ `asset/nc_px_json.c`
    （`nc_px_js_open/eat/str/num/skip/key/bool`；`nc_px_asset.c` 改用同一支，**行為不變**）。
  - 新 `include/nc_px_layer.h` ＋ `asset/nc_px_layer.c`：
    - `nc_px_map_t`：groups（id／name／sel[]＝hw＋slice），hw 對應
      `ws2812/apa102/pca9685/pwm/uartMotor1`。
    - `nc_px_mode_t`：`mapping`／`play_loop`（-1＝無限）／`maxF`／
      `entry[] = {group, effect, write, range}`；write 收 `r/g/b/w/ww/rgb/rgbw/wwww`。
    - `nc_px_show_t`：`auto_play` ＋ `list[]`（registry.json）。
  - 解析規則同 asset 層一致：缺欄位＝預設；未知 write／壞 range／缺 group＝**跳過該項**
    （唔會靜默用錯通道）；壞 JSON＝整份拒絕；支援 UTF-8 BOM。
  - slice 解析 `nc_px_slice_parse()`：`:`／`0:16`／`16:`／`::-1`（stop=0 代表到尾、step=0 代表 1）。
- **驗證**：
  - 新 `tools/hosttest/pxLayer_test`（入 `run_all.sh`）—— golden 直接用 mp 的真檔
    （`map/matrix.json`、`modes/demo_eyes.json`、`modes/motor_sine.json`、`registry.json`）；
    驗 `matrix.full/eyes/rgb/0:16` ＋ `matrix.full/wave/g/16:32`、`play_loop=-1`、`maxF=500` 等；
    另有壞 JSON／未知 write／壞 range／缺欄位／slice／hw／write 的負面測試。全綠。
  - `tools/hosttest/run_all.sh` **17 支全綠**（新增 pxLayer_test）。
  - IDF v6.0.3 build 通過（`nc_px_json.c`／`nc_px_layer.c` 已入 CMakeLists）。
- **未做（下一階段）**：
  - 掃描／載入：`/data/pixel/{effects,map,modes}` ＋ `registry.json` 的掃描器同表（map／mode 表）。
  - **委派執行**：group 引用解析（`matrix.full`／`3.full`／`full`）→ 用實際 controller
    offsets 展開 `offs[]`；mode 播放逐 entry 建 generator、`write` 分派、`range` 子段 scatter。
  - 上板驗 `demo_eyes`（0:16 eyes/rgb ＋ 16:32 wave/g）。

### 7.140 2026-11-20 晚（委派執行：mode plan ＋ group→offsets ＋ 逐幀合成）

- **做咗**：`include/nc_px_show.h` ＋ `asset/nc_px_show.c`
  - `nc_px_show_plan(mode, map, assets, inventory)`：
    - group 引用解析：`matrix.full`／`3.full`／`full`（用 mode.mapping 做預設）
    - 群組 = 各 `sel` 的 slice 展開（`sel` 按硬體型別查 inventory 的 base/px）
    - mode entry 的 `range` 再套一次 Python slice（`0:16`／`16:`／`::-1` 反向暫不支援）
    - 每個委派項 → 一個 job（asset＋預編譯 prog＋write＋1–4 段 `[base,count]`）
  - `nc_px_show_frame(plan, step, dst)`：program interpreter 算 vals（`step/spacing/offset/speed/reverse`）
    → 按 `write` 分派（r/g/b/w/ww/rgb/rgbw/wwww）→ 寫 4 B/px（R,G,B,W）
- **資產層補欄位**：`nc_px_asset_t` 加 `step/spacing/offset/speed/reverse`（以前被當未知鍵跳過）；
  `nc_px_asset.c` 解析；`speed=0` 當 1。
- **驗證**：新 `tools/hosttest/pxShow_test`（入 `run_all.sh`）
  - 用 mp 真檔（effects.json＋matrix.json＋demo_eyes.json）＋假 inventory
    `ws2812 base0 px32`／`uartMotor1 base32 px1`
  - plan：2 jobs、`.px=33`；`job0 = eyes(rgb) 0..15`、`job1 = wave(g) 16..31`、冇跳過
  - 逐幀 step 20：job1 只寫 G 通道且有非零；job0 的 RGB 逐顆等於
    `program_value(ph + k*spacing) >> 4`（同 scatter 語義一致）
  - `motor_sine`（自訂段型別）→ `ESP_ERR_NOT_FOUND`（等 `.nce`，唔會亂畫）
  - `bash tools/hosttest/run_all.sh` **18 支全綠**
- **未做**：掃描器（`/data/pixel/{effects,map,modes}` ＋ registry）同 pixel_task 接線
  （把 plan 接去 render backend／真 controller offsets）。

### 7.141 2026-11-20 晚（掃描／登記：map／modes／registry ＋ `nc_px_layer_load_all()`）

- **做咗**：`include/nc_px_layer_scan.h` ＋ `asset/nc_px_layer_scan.c`
  - `nc_px_map_scan(dir)`：`map/*.json` → `nc_px_map_tbl_t`（檔名排序；id／name 重複跳過）
  - `nc_px_mode_scan(dir)`：`modes/*.json` → `nc_px_mode_tbl_t`（按 `id` 排序；重複跳過）
  - `nc_px_show_load(path)`：`registry.json` → `nc_px_show_t`
  - **`nc_px_layer_load_all(base)`**：一次掃齊四層（`base = …/pixel`）——
    effects → map → modes → registry，同 MP `pixel_task` 開機流程一樣；
    effects 失敗回 `ESP_ERR_NOT_FOUND`，其餘只 warn（唔會令整棵樹失效）
  - 查詢：`nc_px_map_find()`／`nc_px_mode_find()`（name 或數字 id）
- **驗證**：新 `tools/hosttest/pxScan_test`（入 `run_all.sh`）
  - 用 mp 真檔：6 effects（`uart_motor` 自訂型別被拒）、**3 mappings**（matrix／test／gundam）、
    2 modes（排序 demo_eyes→motor_sine）、registry `auto_play=false`
  - `load_all()` → `6 effects／3 maps／2 modes／list 0`
  - 重複 id → 只收第一個（rejected=1）；目錄唔存在 → `NOT_FOUND`
  - `bash tools/hosttest/run_all.sh` **19 支全綠**
- **未做**：pixel_task 接線（用真 controller 做 inventory、按 registry 播 plan）。

### 7.142 2026-11-20（架構決定：效果／map／mode 同 pixel_task 的分工）

**問題**：`pixel_task` 應該只做「canonical canvas → 總線緩衝」；咁「燈效好幾組」＋
map／mode 委派應該擺邊？效果任務同 map 分開定一齊？

**決定：map／mode／registry 係「效果任務」的資料，唔係獨立任務。**

```
┌─ 效果任務（producer，現時 pixel_task 的 prd loop）
│    四層：effects（六工具/.nce）＋ map（group→offsets）
│          ＋ modes/registry（排程）
│    每步：plan 逐 job 算 vals → scatter → canonical canvas（px×4，R,G,B,W 0-255）
│    換場／重載：clear tables → load_all → rebuild plan（同一任務內，原子）
└─ prd 環（既有，4 B/px，格式不變）
┌─ pixel_task／render（transport）
│    只做：canvas → 編碼（lane 合併／波形單元）→ 總線緩衝 → DMA
└─ 硬件
```

理由：

1. **同一 tick 要一致**：效果與 group 對應必須同一幀對齊；map 拆去另一個任務，
   換場時會出現「新 map + 舊效果」的半幀。
2. **map 冇獨立生命週期**：只在換場／重載時重算；每幀只係查表（微秒級）。
3. **介面已經係現成的**：canonical canvas 就係 prd 環的 4 B/px；transport 側
   完全唔需要知 map／mode／效果名。

**「燈效有好幾組」**：一個 mode 的 `map[]` 就係多組（`demo_eyes` = 0:16 eyes/rgb ＋
16:32 wave/g）；`nc_px_show_plan()` 已經支援 N 個 job。只有「各組要唔同更新率／
唔同核心」時才需要拆多個 producer（代價：每組一份 canvas 再合成）。

**什麼情況才拆**：

- `.nce` 需要 sandbox（WAMR）→ 把 `.nce` 執行放另一個任務，但**介面仍然是
  「交一塊 frame buffer」**；map／mode 留在 producer。
- 雙核：producer（core1）／transport（core0）—— 呢個已經係現時 render／pixel
  的分核方式，同 map 無關。

**接線步驟（下一階段）**：

1. producer 開機：`nc_px_store_fs_base()`＋`/pixel` → `nc_px_layer_load_all()`
2. inventory：由 backend 的 controller（型別＋px）推 `{hw, base, px}`
3. `nc_px_show_plan()`：registry/mode → plan（冇 mode 就 fallback 現有內建效果）
4. 每步：`nc_px_show_frame(plan, step, canvas)` → 寫 prd 環（格式不變）
5. 換場：reload → clear → load_all → rebuild（同一任務）

### 7.143 2026-11-20（配對／排版成本量測：map 值唔值獨立一層？）

工具：`tools/hosttest/pxMap_bench/`（host x86，同一份 C 實作；量 plan 建立、逐幀 scatter、
同「純效果計算」分開報）。

| 畫布 | jobs | plan（換場一次） | frame（每幀） | eval | scatter | ns/px |
|---:|---:|---:|---:|---:|---:|---:|
| 64 px | 1 | 0.5 µs | 0.75 µs | 0.39 | 0.36 | 11.7 |
| 64 px | 4 | 0.5 µs | 2.98 µs | 1.96 | 1.03 | 46.6 |
| 960 px | 1 | 4.4 µs | 5.59 µs | 0.49 | 5.10 | 5.8 |
| 960 px | 4 | 3.6 µs | 5.35 µs | 1.65 | 3.70 | 5.6 |
| 9,600 px | 1 | 26.5 µs | 33.30 µs | 0.38 | **32.92** | 3.5 |
| 9,600 px | 4 | 24.2 µs | 24.30 µs | 1.32 | **22.98** | 2.5 |

（4 jobs 反而平過 1 job：奇數 job 係 `wave(g)` 單通道，寫入少過 RGB。）

**判讀**：

1. **每幀成本 = scatter（配對）主導**：`eval` 只係每 job `pixel_hint` 個值（64 個），
   scatter 就係 px 次寫入；9,600 px ≈ 33 µs（x86）。換算 S3（240 MHz，約 8–10×）≈
   **0.26–0.33 ms/幀 → 50 FPS 預算 1.3–1.7%**。map 唔係瓶頸。
2. **plan（排版管理）只喺換場做一次**：9,600 px 26.5 µs（x86）→ 板上約 0.2 ms；
   同「每幀」冇關，唔需要獨立任務。
3. **記憶體**：`plan` 4,424 B（8 jobs × 預編譯 prog）、`map` 2,618 B、`asset table` 8,386 B
   —— 全部放 producer 的 static 就夠，唔使跨任務共享。
4. 所以 §7.142 的結論有數據支持：**effects／map／mode 留喺同一個效果任務**；
   `pixel_task`（encode）只認 canvas。

### 7.144 2026-11-20（真機極限數據：S3 逐值／scatter／plan）

命令：`0xF064 PROGBENCH`（tests 映像；payload `[px:u16][per_led:u8]`）；
工具：`python3 tools/hosttest/sin_bench/dev_probe.py --port <PORT> --prog --px 64,960,9600 [--per-led]`。
量測條件：板上正常任務全部跑住（NC4／pixel producer／render／net），命令喺 test_action
context 執行；數字係 3 次取最小（逐值）／500 或 100 幀平均（frame）。

**逐值成本（一個 LED 一個波形值，12-bit，S3 240 MHz）**

| 型別 | ns/值 |
|---|---:|
| `keep` | 266 |
| `starter` | 199 |
| `math_now` | **482** |
| `square_wave_now` | 485 |
| `pulse_wave` | 489 |
| `pulse` | 273 |

**整幀（values ＋ scatter，RGB 一個 job 覆蓋全鏈，canvas 喺 internal SRAM）**

| 燈鏈 px | 共享 64 值（vals 循環用） | 每 LED 一個值（極限） | 極限 ns/px |
|---:|---:|---:|---:|
| 64 | 48.7 µs | 48.7 µs | 760 |
| 960 | 242.4 µs | 352.8 µs | 368 |
| **9,600** | 2,148.5 µs | **2,329.4 µs** | **243** |

- **plan（換場）**：4–5 µs（真機；比 host 26 µs 仲快，因為 9600 px 個 case 只係 1 個 job）。
- **極限情境**：9,600 px、每顆 LED 都有自己一個波形值 → **2.33 ms/幀**
  ＝ 50 FPS 預算（20 ms）的 **11.6%**；仍有一粒完整 core 的時間做編碼／送燈。
- scatter 主導：9,600 px 由 64 值升到每 LED 一值只多 181 µs（+8%），其餘係 4 B/px 寫入。
- 對照 host x86：`math_now` 7.8 ns（host）vs 482 ns（S3）≈ 62×；所以 host 數字只可作相對比較。

### 7.145 2026-11-20（配對層對齊 mp `_SCATTER`；效果交「值緩衝」）

**問題**：我原本 `nc_px_show_frame()` 自己算 vals 再寫畫布，等於把「效果輸出」同
「配對」綁死；而且兩處語義同 mp 唔同：

| 寫法 | mp 行為 | 我原本（錯） |
|---|---|---|
| `r/g/b/w` | **只改該通道，其餘不動** | 四通道全寫（清走其他效果寫的通道） |
| `ww` | `o[2]=v&0xFF`、`o[3]=v>>8`（12-bit 拆兩個 byte） | 當單通道寫 W |
| `wwww` | 同一個 `v>>4` 寫晒四通道 | 只寫 W |

**修正**：拆出通用配對層
- `nc_px_pair_apply(job, vals, nv, dst)`：任何效果器（六工具／`.nce`／未來）交
  `uint16_t *vals + nv` 就配到；八種寫法逐條照 `pixel_layout._SCATTER`。
- `nc_px_job_vals(job, step, out, cap)`：六工具用（program → vals）；未來效果器自己
  出 buffer 就跳過呢個。
- `nc_px_show_frame()` = `job_vals()` ＋ `pair_apply()`（行為不變）。

**驗證**：`pxShow_test` 加 6 條配對語義測試（單通道保留其他通道、`ww` 拆 byte、
`wwww` 四通道、`rgb` 的三值對位、重疊寫保留、**外部 buffer 直配**）；`run_all.sh` 19 支全綠。

**延伸點（未做）**：未來彩色效果器可能交 RGB8／HSV buffer（`PixelMathMethod` 已有
hsv↔rgb bulk）。到時在 `nc_px_pair_apply` 前面加一層「buffer 格式 → 12-bit 值流」，
配對層本身唔使改。

### 7.146 2026-11-20（不變量落地：效果層永遠 12-bit，位深轉換／抖動只在最尾）

**規則**（用戶確認）：所有 LED 在效果狀態都係 **12-bit（0..4095）**，再按輸出需要轉換；
數據來源必然 12-bit，呢點係將來抖動的鋪墊。

**做咗**：

- `nc_px_pair_apply12(job, vals, nv, uint16_t *dst12)`：12-bit 原生配對，
  八種寫法語義同 8-bit 版一致，但**唔做 `>>4`**（`ww` 仍然照 MP 的 byte 打包）。
- `nc_px_canvas12_to_8(src, dst, nch, dither, err[4])`：
  - `dither=false` → 四捨五入
  - `dither=true` → 1D 誤差擴散（低 4 位帶去下一顆），平均亮度守得住
  - 未來 >8-bit 輸出（apa102 16-bit／pca9685 12-bit）可以直接食 12-bit，唔使轉
- `nc_px_pair_apply()`（8-bit）保留做「已經轉好」的管線用；12-bit 版係正路。

**驗證**：`pxShow_test` 加 5 條：12-bit 原值保存、wwww 四通道、12→8 四捨五入、
抖動 0/1 交錯且總和守恆、0xFFF → 255。`run_all.sh` 19 支全綠；IDF build 通過。

**成本備註**：12-bit 畫布 = px × 4 × 2 B；9,600 px = **76.8 KB**（internal SRAM 要預留；
或者分塊 256 px 一段處理，兼顧 cache）。8-bit 畫布只 38.4 KB。

### 7.147 2026-11-20（真機：12-bit 畫布 internal vs PSRAM、抖動時間、總線預算）

**PSRAM 事實**：SuperMini = ESP32-S3FH4R2，**2 MB PSRAM（quad @ 80 MHz）**，
`CONFIG_SPIRAM=y`。實測：**9,600 px 的 12-bit 畫布（76.8 KB）internal 分配失敗**，
自動落 PSRAM；960 px（7.68 KB）就 internal 冇問題。

**真機數字（0xF064，canvas 12-bit，S3 240 MHz，正常任務全跑）**

| px | canvas | pair_apply12 | 12→8（捨入） | 12→8（抖動） |
|---:|---|---:|---:|---:|
| 960 | internal | 260.6 µs | 211.5 µs | **358.0 µs** |
| 9,600 | **PSRAM** | **5.35 ms** | 3.25 ms | **4.67 ms** |

- 960 px：12-bit 全鏈（配對＋抖動）≈ **0.62 ms/幀** → 50 FPS 預算 3%。
- 9,600 px：光配對＋抖動就 **10 ms/幀**（50% 預算），因為 PSRAM 隨機寫慢 2×
  （internal 960 px 係 271 ns/px；PSRAM 9,600 px 係 557 ns/px）。
- **結論：12-bit 畫布要分塊**（例如 256 px = 2 KB，internal＋cache hot），
  配對→抖動→交 encode→下一塊；唔好一次過 76.8 KB 落 PSRAM。

**總線預算（抖動的本質：同一畫面送 M 次）**

ws2812：30 µs/LED/幀（24 bit × 1.25 µs）。視覺更新 50 FPS（20 ms）時：

| 每條 lane 的 LED | 可送子幀 M | 有效抖動位元 |
|---:|---:|---:|
| 20（現時 6×20） | 33 | 5 bit |
| 64 | 10 | 3.3 bit |
| 120 | 5 | 2.3 bit |
| 960 | 0.7 | 連一幀都唔夠（28.8 ms） |

- 用戶的理解正確：**抖動 = 一次過送多個子幀**，緩衝區大小不變（同一塊 canvas ＋
  4 個誤差累加器），但**總線時間 × M**，所以高 FPS／多子幀之下可支援 LED 數下降。
- 6×20 = 120 顆、6 條 lane 並行：每子幀 0.6 ms → 20 ms 內可送 **33 個子幀**，
  抖動空間非常充裕；反過來 960 px 單鏈連 50 FPS 都做唔到（28.8 ms/幀）。

### 7.148 2026-11-20 晚（EffectPerf 專案：純效果量測；64／960／4,096／9,600 px 真機）

**背景**：§7.147 量到 9,600 px 的 12-bit 畫布在滿載映像落 PSRAM，配對＋抖動 10 ms/幀；
下一步要分開「畫布本身」與「其他模組佔 internal」的影響，而且用戶要一個**只測 Effect**
的專案（不要基礎燈效、不要其他測試）。

**做咗**：

- 新 `CONFIG_NETCORE_EFFECT_BENCH`（release 映像也可用，**不需要 `NC_ENABLE_TESTS`**）＋
  `firmware/components/tasks/effect_bench/`：自己一條 `0xF065 EFBENCH`／`0xF066`，
  沒有 `0xF060–0xF064`、network、ncm、其他量測。
- 新 project **`ESP32-S3-Matrix-EffectPerf`**（`ports/project/S3/`）：render／pixel
  `level=-1`（不亮燈、不初始化 I80、不跑六個內建效果），tasks.json 只開
  `effect_bench`；開機 8 秒後自動跑 64 px／2,000 幀，其餘 px 由 `0xF065` 指定。
- 量測覆蓋：program 逐值／逐幀、8／12-bit 配對、12→8 捨入／抖動、
  internal／PSRAM／混合、分塊 64／256／1024、2／4 job。
- 工具 `tools/effect_bench.py`（`--via` WebMaster REST 或 `--port` 直接 CDC）；
  詳細設計／數字見 [effect-perf.md](effect-perf.md)。

**實機數字（S3 240 MHz、IDF 6.0.3、EffectPerf 映像、nc4 跑住）**

| px | 整條 12-bit internal | 整條 12-bit PSRAM | 混合（12p→8i） | 分塊 1024 | i12 佔 20 ms |
|---:|---:|---:|---:|---:|---:|
| 64（8×8） | **63.8 µs** | 63.8 µs | 63.8 µs | 63.9 µs | **0.32%** |
| 960 | 500.6 µs | 500.6 µs | 500.6 µs | 500.7 µs | 2.5% |
| 4,096 | 2.039 ms | 3.433 ms | 2.039 ms | 2.128 ms | 10.2%（p12 17.2%） |
| **9,600** | **4.738 ms** | **11.095 ms** | **8.701 ms** | **5.005 ms** | **23.7%（p12 55.5%）** |

- **64／960 px：internal 與 PSRAM 同速**（畫布 512 B／7.7 KB，cache 吸掉）；
  PSRAM 代價由 **4,096 px（32 KB）** 開始，9,600 px 的 12-bit 配對慢 **2.03×**。
- **分塊 1024 px** 把 9,600 px 由 11.095 → **5.005 ms**（只比全 internal 慢 5.6%，
  比整塊 PSRAM 快 2.22×）；256 px 是 5.836 ms，64 px 反而 9.556 ms（固定成本太高）。
  ⇒ §7.147 的「256 px 一塊」修正為 **1024 px 一塊**。
- **EffectPerf 隔離出 ~132 KB internal**（開機 heap 234.7 KB vs tests 映像 102.1 KB）：
  同一塊 9,600 px 12-bit 畫布（76.8 KB）在 EffectPerf **internal 分得到**，
  滿載映像分不到。所以「畫布落 PSRAM」很多時候是其他模組先佔了 internal，不是畫布太大。
- 8×8 的 63.8 µs 中 **33.7 µs（53%）是算 64 個效果值**；配對＋轉換只 30 µs。
  8×8 要再快，先減 `pixel_hint`／`eval`，換畫布放置冇用。

**踩到並修正**：

1. 分塊 scratch 用了畫布絕對 `base` → 9,600 px 寫出界（960 px 剛好還在 8 KB 內）；
   改成 scratch base=0，寫完再交回 `c8 + off*4`。
2. `vTaskDelay(1)` 餵不到 idle（decode 優先級高，每個 tick 都搶先）→ 量測幾秒後
   `task_wdt`（`rst=6`）；改 `vTaskDelay(2)`，數字不變。

**驗證**：`tools/effect_bench.py --px 64,960,4096,9600` 全部回 `0xF066`；
開機自測 `ebench` 14.4 s 完成（8 s 延遲＋6 s 量測）、`loop [ebench] 結束`、無 WDT；
OTA 6 次（`app` 最後 `51b88127`、image 670.9 KB），每次 `APPLY` 自動重開。

**未做**：encode／I80 端到端（EffectPerf 不開 render）；network／ncm 同時跑的
cache 污染；temporal／spatial 抖動差異；apa102 16-bit／pca9685 12-bit 直出。

### 7.149 2026-11-20 深夜（0xF067 PSRAMBENCH：大 buffer 放 PSRAM、純搬運頻寬＋混合讀取）

**背景**：§7.148 之後的結論是「大 canvas 放 PSRAM、1024 px 分塊入 internal」；
但之前只由 effect 管線反推，未直接量 PSRAM 的純 read／write／copy。今次補一條
**只做資料搬運**的指令，確認「大資料放 PSRAM、讀取盡量快、混合使用」的實際上限。

**做咗**：

- `0xF067 PSRAMBENCH`（回 `0xF068`，同一個 `CONFIG_NETCORE_EFFECT_BENCH` component）：
  `memset`／sequential read／`memcpy`、PSRAM cold read（每次 M2C）／writeback（C2M）、
  internal↔PSRAM 混合 copy、1/4/16/64 KB chunk 讀。工具 `tools/psram_bench.py`。
- OTA 第 7 次（`app 49ff3d64`、image 673.9 KB）；`status cmds` 仍只有 `0xF065`／`0xF067`，
  冇 `0xF060–64`、network、ncm。

**真機數字（S3 240 MHz、2 MB quad PSRAM 80 MHz、buffer 64 B 對齊）**

| 大小 | internal wr/rd/cp | PSRAM wr / wr_cold | PSRAM rd / rd_cold | PSRAM cp | 混合 i2p/p2i/p2p |
|---:|---:|---:|---:|---:|---:|
| 64 KB | 725 / 129 / 361 MiB/s | 14 / 13 | 24 / 26 | 9 | 14 / 24 / 9 |
| 256 KB | internal 不足 | 13 / 13 | 25 / 26 | 9 | 13 / 25 / 9 |
| 512 KB | internal 不足 | 13 / 13 | 25 / 26 | 9 | 13 / 25 / 9 |
| **1 MB** | internal 不足 | 13 / 13 | 25 / 26 | — | 13 / 25 / — |

- **PSRAM 順序讀 25–26 MiB/s、寫 13 MiB/s、copy 9 MiB/s**；1 MB 仍然同一水平線。
  internal copy 361 MiB/s → PSRAM copy **慢 ~40×**。
- **chunk 1/4/16/64 KB 純讀全部 ~25 MiB/s**：PSRAM 係 bandwidth-bound，
  分塊細唔會慢；1–4 KB 反而 cache footprint 更細。
- cold/hot 幾乎一樣（26 vs 25）——64 KB 已超過 L1，重讀都係 PSRAM。
- `esp_cache_msync`：256 KB–1 MB 約 **C2M 17 µs、M2C 12 µs**，相對搬運可忽略；
  64 KB 第一次 178 µs 是 warm-up。
- 換算：9,600 px 12-bit 畫布 76.8 KB → 讀 **3.0 ms**、寫 **5.6 ms**、copy **8.3 ms**，
  同 §7.148 的 effect `p12 11.1 ms`／`mix 8.7 ms`／分塊 5.0 ms 對得上。
  1 MB → 讀 40 ms、寫 77 ms、copy 111 ms：**大 buffer 只可以做冷資料，唔可以每幀全掃**。

**定案（用既有 `nc_buf`，唔另開 allocator/header）**：full canvas／asset arena 等冷 buffer
放 PSRAM（`where=P`）；1,024 px chunk（8 KB 12-bit scratch）＋8-bit 輸出留 internal（`where=S`）；
DMA 槽依 backend 契約（drv_i80 大幀已有 PSRAM 槽例外）。`0xF067` 只量 raw bandwidth，
正式 buffer 用 `nc_buf` 的 `slot_bytes × n`／`reserve`／`off`／`len`／`handover`。由 PSRAM 讀入 internal 用 1–4 KB chunk 就夠；
pair/dither 在 internal 做，只把最終結果寫 PSRAM（寫比讀貴一倍）。
詳見 [effect-perf.md §9](effect-perf.md#L268)。

### 7.150 2026-11-21 凌晨（0xF069 PIPEBENCH：9,600 個波→配對→12→8 的樽頸）

**背景**：§7.148/§7.149 的 effect bench 用 `pixel_hint=64`，只代表 8×8；
16×600 要「每顆 LED 一個波」時，生成器要跑 9,600 次（RGB 28,800 次）。
今次加 `0xF069 PIPEBENCH`（回 `0xF06A`），把 **gen／pair／conv** 分開量，
RGBW→I80 編碼／送燈唔計（render backend 已有）。

**真機（9,600 px、S3 240 MHz、12-bit canvas internal、frames=2）**

| 階段 | 1 值/px（wwww） | 3 值/px（rgb） |
|---|---:|---:|
| gen（nc_px_job_vals） | **5.155 ms** | **15.587 ms** |
| pair range（pair_apply12） | 4.790 ms | 4.894 ms |
| pair 連續 table | 5.302 ms | 5.303 ms |
| pair 打亂 table | **19.941 ms** | **19.939 ms** |
| conv 12→8 | 3.425 ms | 3.426 ms |
| **full（gen＋pair＋conv）** | **13.530 ms（67.7%）** | **24.167 ms（120.8%）** |
| 50 FPS 剩餘 | **6.47 ms** | **−4.17 ms** |

- 1 值/px：gen 38%、pair 35%、conv 25% —— 三段都重要。
- 3 值/px：**gen 65%**、pair 20%、conv 14% —— 生成器是主瓶頸。
- pair 的 W 與 WWWW 同價（4.77 vs 4.79 ms）：成本在 `k % nv`／range 迴圈，不是寫 4 通道。
- 連續 table 比 range 慢 11%；**打亂 table 4.2×**（19.9 ms）——表唔貴，非連續目的地才貴；
  配對器應該用 **runs（連續段）**，唔好逐顆 table。

**下一步（未做）**

1. **波表快取**（MP `effect_core.py` 的 `_WAVE_CACHE`）：`program` 的 total=256 預算成
   `uint16_t wave[256]`，逐幀只索引；生成可望由 5.16／15.59 ms 降到 <1 ms。
2. pair 在 `nv >= nled` 時省掉 `% nv`（每幀 9,600 次除法）。
3. 非連續 group 先排成 runs 或分塊，避免 19.9 ms random scatter。
4. `full_ps`（canvas 在 PSRAM）今次 `c12p` 配不到，未取得；之前 0xF065 的
   `p12=11.1 ms`／`mix=8.7 ms` 仍有效。

**驗證**：OTA 後 `app 618f7233`；`status cmds` 只有 `0xF065`／`0xF067`／`0xF069`，
冇其他測試。詳見 [effect-perf.md §10](effect-perf.md#L315)。

### 7.151 2026-11-21 凌晨（production 檔案驅動 Show：content_task ＋ white mix ＋ 0xF06B）

**背景**：EffectPerf（§7.148–§7.150）只量效果、唔點燈；production 8×8 Matrix 要的是
**內容驅動**的 show：`/data/pixel` 四層（`effects`／`map`／`modes`／`registry`）由 Web UI
上傳，開機載入，逐幀合成後送 64 顆 WS2812B，並且要「80% 白 ＋ 20% 效果」的 white mix；
Web UI 要用一條高頻指令預覽最近一幀。

**做咗**（只動 `content_task` ＋ layer parser ＋ project ＋ docs，EffectPerf／pixel_task／
render／編碼器全部唔改）：

- 新 component `firmware/components/tasks/pipeline_task/nc_pair_task.c`（CONFIG 開關、
  `WHOLE_ARCHIVE`＋constructor 自己登記，`main.c` 唔使改）：
  - 任務：`show`／loop `show`／NORMAL／level 4／order 21／stack 4096／core 1／
    prio `NC_PRIO_OTHERS_MAX-1`（5，低 render 一級）／layer 5／period 0。
  - on_start：找 render 的 prd 環 → `nc_store_fs_base()`＋`/pixel` →
    **先 mkdir `pixel/map`、`pixel/modes`**（FILE_BEGIN 唔會建父目錄，新機上傳會
    `err_write_fail`）→ `nc_px_layer_load_all()` → registry `list[0]` → `mode.mapping` →
    map → 只抄 mode 引用到的 asset → `nc_px_show_plan()`（inventory
    `{WS2812,0,px}`）→ canvas（internal heap，px×4）→ log 四層數量／mode／map／white／jobs。
  - 每幀：`memset(canvas,0)` → `nc_px_show_frame(plan, step, canvas)` → white mix →
    `nc_px_ring_begin_ex()` payload（4 B/px）→ commit → `step++`；環滿 `vTaskDelay(1)`
    且唔加 step。表頭 `src=BUILTIN`、`ts_us=esp_timer_get_time()`、`drops=環滿次數`。
  - 失敗 fallback：`/data` 未掛、四層缺失／解析失敗、冇 mode／map、plan 0 job、canvas
    配置失敗 → log warning 講原因 ＋ 每幀填黑（`NC_PX_F_CLEAR`）入環，**不 crash／不 boot loop**；
    修好檔案重開即用真 plan。
  - `0xF06B SHOWFRAME`（回 `0xF06C`，指令表在同一个 component）：最近一幀 4 B/px 以 hex
    回覆 `{"px":N,"seq":M,"hex":"..."}`（64 px = 512 hex）；`nodata`／`big`（px>512 或
    hex 塞唔落 2,600 B 回覆 JSON，約 px>315）。hex 直接寫入 decoder 的回覆幀緩衝，
    decode task 嘅 5 KB 棧唔會開大 buffer；快照 2,048 B static ＋ spinlock。
- `nc_px_mode_t` 加 `uint8_t white`；`nc_px_layer.c` parser 收 JSON `"white"` 或
  `"white_mix"`（0..100，缺欄位＝0，>100／負數／字串→0 但唔整份壞）→ 舊 JSON 完全相容。
  white mix 公式（四通道都做，整數）：`out = (out*(100-w) + 255*w) / 100`。
- 新 Kconfig `CONFIG_NETCORE_CONTENT`（depends `NETCORE_PIXEL`，default n）＋
  `firmware/CMakeLists.txt` 無條件加入 component（關閉時 .c 為空 object）。
- 新 project `ports/project/S3/ESP32-S3-Matrix-Show/`：`project.json`
  （hardware S3/ESP32-S3-Matrix、aliases show／matrix-show、seed 4 層）、
  `sdkconfig.defaults`（`NETCORE_CONTENT=y`、`PXBENCH=n`、不開 tests）、
  `data/config/tasks.json`（render level4 affinity[0,1]、show level4 affinity[0,1]、
  pixel level -1）、`data/config/{system,network,hardware,resources}.json` 抄
  Matrix-LED-Textkit；`data/pixel/`（主機端內容來源，唔 embed）：`colorwave` effect、
  `matrix` map、`white80`（white=80）／`color`（white=0）兩個 mode、registry
  `list=["white80","color"]`。
- 文件：[show-pipeline.md](show-pipeline.md)（資料流、white mix、0xF06B 協議、project、排錯）。

**驗證**

- `IDF_PATH=~/esp/esp-idf python3 make.py build ESP32-S3-Matrix --project
  ESP32-S3-Matrix-Show --fresh` **成功**：image **694.8 KB**（0xadb50，分區剩 55%）、33–35 s；
  `CONFIG_NETCORE_CONTENT=y`、PXBENCH／EFFECT_BENCH not set；ELF 有
  `_nc_task_reg_content`／`_nc_action_reg_nc_pair_task_table`／`nc_pair_task_table`。
  唯一警告是既有 `bus/bus_net/nc4_line_net.c` unused-function／variable（同本改動無關）。
- Host（x86，同一份 C 實作）用真 project 內容：`load_all` → effects=1／maps=1／modes=2／
  list=2；`white80`（white=80）→ plan jobs=1 skipped=0 px=64；200 幀後 R/G/B≥204、W=204、
  有動畫；`color` W=0。`pxLayer_test`／`pxShow_test`／`pxScan_test` 全綠（white 是新增欄位，
  舊 fixture 不變）；另驗 `white_mix` 別名、>100／負數／字串→0。
- **未上真機**：實際送燈、50 fps 節奏、Web UI poll 0xF06B 未驗（OTA 由 lead 做）。

**未做／限制**

1. 只有 registry `list[0]` 一個 mode；`auto_play` 輪播／切 mode 指令未做。
2. 內容是開機載入；改檔後要重開（reload 指令未做）。
3. `.nce` 效果未接合成（program interpreter 仍只有六種段型別）。
4. white mix 整幀統一；per-mode fade／per-frame 動畫未做。
5. 0xF06B 預覽受回覆 JSON 2,600 B 限制：>315 px 回 `err=big`（production 64 px 冇影響）；
   若日後要 512 px 預覽要加大 `NC4_REPLY_JSON_MAX` 或分幀。

### 7.152 2026-11-21（Show 真機：80% 白＋0xF06B live frame＋0xF06D 切 mode；Web 全流程）

**背景**：§7.151 的 Show firmware 未上真機；今次補：
① 真機 8×8 出 80% 白 frame；② `0xF06B` live frame；③ `0xF06D` runtime 切 mode；
④ WebMaster `/api/build` 一鍵建固件＋8×8 燈泡模擬器；⑤ 修第一次上板的 stack overflow。

**真機結果（ESP32-S3-Matrix、app `ea8475dc`、image 695.5 KB、render＋show 跑住）**

- `status tasks`：`show` level 4、started=1、loop `show` running=1；`render` 同樣 running；
  `pixel` level=-1（唔跑內建六色）；`status cmds` 只有 `0xF06B`（＋公共指令）。
- `0xF06B`（white80，white=80）：`px=64`、hex 256 B；min RGBA=[204,204,204,204]、
  max=[255,255,255,204]；64 顆全部 ≥ floor 204；W 全部 204。即 **80% 白＋20% 效果**
  真機正確。
- `0xF06D` payload `02` → `{"ok":1,"queued":2,"mode":"white80","white":80}`；
  下一幀 `0xF06B`：`mode="color"`、`white=0`、W=0，效果顏色（R/G/B 0..254）；
  切回 payload `01` → `mode="white80"`、`white=80`、W=204。
- **plan rebuild（apply_us）真機 226–251 µs**（1 job、64 px）；切 mode 係「plan 重建＋
  下一幀生成」，唔使重新解析四層檔案。
- WebMaster `/api/build` 實際建 `ESP32-S3-Matrix-Show`：2.29 s、artifact
  `artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-Show.bin`、size 711,440 B；再用
  `/api/build/artifact` + `/api/firmware` 一鍵 OTA 成功（8.87 s），frame 仍然正確。
- WebMaster pytest：292 passed；host tests 20 套全綠。

**第一次上板失敗（已修）**

- 第一次 OTA 後板子完全冇 USB：根因是 `content_task` 的 `on_start` 由 **main task（3,072 B 棧）**
  呼叫 `nc_px_layer_load_all()`；`nc_px_map_scan()` 的 local `nc_px_map_t` **2,618 B**
  ＋ `nc_px_mode_t` 568 B，直接 stack overflow → boot loop。
- 修法：
  1. `nc_px_layer_scan.c` 的 `nc_px_map_t m`／`nc_px_mode_t m` 改 **static**（掃描只在
     開機／換場做，單執行緒）；
  2. Show project `sdkconfig.defaults` 加 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`。
- 修好後用 **ROM download mode ＋ esptool 全片 flash**（bootloader／分割表／app）
  直接刷入，成功開機；之後 `0xF06B` 正常。**唔再靠 OTA 做未驗證 firmware 的迭代。**

**Web 全流程（已可用）**

- 固件頁 Build 卡：board／project 下拉（8 boards／12 projects，含 Matrix-Show）、
  `--tests`／`--fresh`、Build 按鈕、輸出 pre、artifact；Build 成功後可一鍵 OTA。
- `GET /api/build/info` 回 repo／IDF（`~/esp/esp-idf`）／boards／projects／lastBuild。
- mode 頁：8×8 燈泡 Canvas（圓形、glow）、`modeWhite` 0..100（預設 80）、
  「讀取裝置 frame」0xF06B、「套用到裝置」0xF06D（用目前 mode id）、
  Show 全流程 5 步（載入／pixel→預覽→Build→OTA→讀 frame）。
- 修 `_find_artifact`：之前 build log 內的 `artifacts/build/.../partition-table.bin`
  會被當成 artifact（size 3,072）；現在只認 `產物 …`／app `.bin`，排除
  partition-table／bootloader／ota_data。實測回 app 711,440 B。

**限制**

- 開機仍只選 registry `list[0]`；`auto_play` 輪播未做（手動 0xF06D 可以）。
- 改 `/pixel` 檔案後仍要重開（reload 未做）。
- `0xF06B` >315 px 會 `err=big`（64 px 冇影響）。
- white mix 是整幀統一；per-mode fade 未做。

### 7.153 2026-11-21（安全調整：80% 白太光 → 低亮度 breathe）

**現象**：§7.152 的 `white80`（white=80、8-bit 底 204）在 64 顆 WS2812B 上約 3 A，
LED 太光／受熱（用家反映「LED 甩了」）。production 8×8 不需要這個亮度。

**即時處理（唔重燒 firmware，因為 effect 係 `/pixel` 檔案）**：

- `effects/effects.json`：改成 `breathe`，`math_now`、`F=16`、`l_max=384`
  （12-bit → 8-bit 最高約 24/255，約 9%）、`end_Time=4096`（256 幀／5.1 s 一循環）。
- `modes/white80.json`：檔名保留但 mode 改名 `breathe`、**`white=0`**、
  `matrix.full × breathe × rgb`；`registry.list=["breathe"]`。
- `tools/show_upload.py --reboot` 上傳＋重開；`show` 任務重新載入，冇 OTA firmware。
- 真機 `0xF06B`：`mode=breathe`、`white=0`、max RGBA=[16,16,16,0]（連續讀 4 次
  11–18，呼吸中），遠低於 80% 白的 204。
- `tools/show_verify.py` 加 `--max-led`（例如 `--max-led 40`）同 `--white` 省略時
  自動用 frame 回報值，方便日後驗「唔太光」。

**注意**：唔好再把 `white` 設 80 同時 `l_max=4095`；要光一點應逐步加 `l_max`
（128 → 8-bit 8、256 → 16、384 → 24、512 → 32），每次用 `--max-led` 驗。

### 7.154 2026-11-21（任務改名 content_task；effect_bench 只入 EffectPerf；安全內容）

**改名**：`show_task` 嘅語意同 `render_task` 太接近（兩者都似「顯示」），production
生產者改名 **`content_task`**：

- component：`firmware/components/tasks/pipeline_task/`、`nc_pair_task.c`
- task／loop 名：**`content`**（`tasks.json` 同步；舊 `show` 名由 task_manager
  讀 config 時做 alias，OTA 舊 `/data/config/tasks.json` 不會令新任務自動關閉）
- Kconfig：`CONFIG_NETCORE_CONTENT`（原本 `NETCORE_SHOW`）
- 0xF06B／0xF06D 協議名保留 `SHOWFRAME`／`SHOWMODE`（它們是「顯示 frame／mode」，
  不是任務名）
- 真機未重新 OTA（板子短路，暫時不供電）；只做 build／ELF 驗證。

**測試元件隔離**：`effect_bench` 再不是 production component：

- `firmware/CMakeLists.txt` 只在 **`NC_PROJECT` 包含 `EffectPerf`** 時把
  `components/tasks/effect_bench` 加入 `EXTRA_COMPONENT_DIRS`。
- Show 映像：`CONFIG_NETCORE_CONTENT=y`、`CONFIG_NETCORE_EFFECT_BENCH` not set；
  ELF **冇** `nc_effect_bench_table`／`_nc_task_reg_effect_bench`／`_nc_action_reg_nc_effect_bench_table`。
- EffectPerf 映像：`NC_PROJECT=S3/ESP32-S3-Matrix-EffectPerf` → 仍然有
  `_nc_task_reg_effect_bench` 同 0xF065；build 676.6 KB 成功。
- `CONFIG` 未載入前（`project()` 之前）不能可靠讀 Kconfig，所以用 builder 已有的
  `NC_PROJECT` 環境變數判定；production 連目錄都唔掃。

**Build 驗證**：

- Show：`695.6 KB`（0xade70）、7 s；task symbol 只有 `_nc_task_reg_content`；
  冇 effect_bench。
- EffectPerf：`676.6 KB`（0xa9250）、39 s fresh；有 effect_bench。
- Host tests 20 套、WebMaster pytest 292 仍綠。

**安全內容**：維持 §7.153 的低亮度 `breathe`（`white=0`、`l_max=384`、8-bit 最高約 24）；
板子確認短路，未再上電、未 OTA、未讀 frame。等硬件修好後再驗改名後映像。

### 7.155 2026-11-21（content_task：auto_play＋0xF06F reload；Web reload；測試隔離完成）

- task 改名已完成：`content`（component `content_task`、`nc_pair_task.c`、
  `CONFIG_NETCORE_CONTENT`）；舊 `/data/config/tasks.json` 的 `show` 由 task_manager
  alias 遷移（exact `content` 優先，無才讀 `show`）。Show ELF 只有
  `_nc_task_reg_content`，冇 `_nc_task_reg_show`。
- `effect_bench` 只喺 `NC_PROJECT` 含 `EffectPerf` 時加入 `EXTRA_COMPONENT_DIRS`；
  Show ELF 冇 `nc_effect_bench_table`／`_nc_task_reg_effect_bench`／0xF065；
  EffectPerf fresh build 仍成功（676.7 KB、8 s incremental；fresh 39 s）。
- **auto_play**：content loop 每 `mode.maxF` 幀（0 → 500）按 registry list 次序換 mode；
  production list 只有 `breathe`，所以暫時不切。
- **0xF06F RELOAD → 0xF070**：decode task 排隊，content loop 下一幀邊界
  `nc_px_layer_load_all()`，按目前 mode 名重建 plan；改 `/pixel` 不用重開。
  Web UI 加「重新載入 /pixel」按鈕（0xF06F）。
- Show build 696.5 KB（0xae230）成功；task symbol 正確；未上電（板子短路）。
- Host tests 20 套全綠；WebMaster pytest 292 passed；docs §3.2/§3.3、commands 已更新。


### 7.156 2026-11-21（呼吸修正＋hash cache 評估）

**「到底有沒有呼吸？」** 之前板上的 `breathe` 是 **F=16、end_Time=4096**：
`step_phase = 65536*16/10/4096 = 25`，4096 幀內相位會超過 65536，波形在
4096 幀結尾跳回；短時間取樣只見 **11–16/255**，所以「有動但唔似完整呼吸」。
用家嘅懷疑正確。

已修正 `effects.json`：

```json
{"type":"math_now","F":10,"l_max":384,"l_lim":0,"phi":0,"end_Time":256}
```

- `step_phase = 65536*10/10/256 = 256`；256 幀 = 一個完整 0 → 23 → 0 循環。
- 50 fps 約 **5.12 s** 一次呼吸；8-bit 最亮 **23/255**（約 9%），安全。
- 因為板子短路，**未上傳到裝置**；只在本機／模擬器驗證。修好後
  `show_upload.py --reboot` 再上傳。
- 軟件模擬 GIF：`artifacts/breathe_preview.gif`（256 幀、20 ms/frame）；
  對應數值 `artifacts/breathe_frames.json`（min 0、max 23）。不是硬件輸出。

**Hash／cache 提案評估**

- 目前 mode 切換**不讀 JSON**：`content_apply_mode()` 只用 RAM 內的 mode／map／asset
  重建 plan；真機量到 **199–251 µs**（64 px）。所以 hash sidecar 對「切換 mode」
  幾乎冇幫助。
- Hash cache 真正有幫助的是 **開機／0xF06F reload**：可跳過 JSON parse。但
  ① 仍要讀檔算 hash（5 個小 JSON 很快）；② 仍要建 plan；③ 要處理 ABI／
  struct layout／endianness／px 格式／原子寫入／CRC。
- 若日後 9,600 px／大量 mode 令 boot/reload 變慢，建議做**binary cache**（不是
  只存 hash）：`pixel/.cache/manifest.json`（ABI、檔案 size/mtime/sha256、blob
  offset）＋ `pixel/.cache/show.bin`（maps／modes／assets／prebuilt plans 的固定
  layout，CRC）；載入時 stat 比對，變更才 parse＋重建，寫入用 `.part` rename。
- 對 64 px production，現時 plan rebuild 200 µs 不是瓶頸，暫不加入 cache；先保持
  「JSON → RAM → plan」單一路徑，減少 ABI 風險。

### 7.157 2026-11-21（Web 全流程實測：無板 sim、Build UI、OTA UI、白色跟 mode）

**目的**：確認「完全透過網頁控制」不再靠人手 curl。用 headless Chrome（CDP）在
`http://127.0.0.1:8012/`（`--sim`，無 Matrix 硬件）操作真實前端：

- `#mode`：按「載入裝置 pixel 樹」→ 1 effect／2 modes／2 mappings；
  選 `breathe` 後 slider 跟 mode 的 `white=0`（之前錯誤地停在 HTML 預設 80）。
- 8×8 預覽：未載入時是黑，不再用 white mix 把黑畫成白；低亮度顯示加
  `#modeGain`（×8，只影響預覽，chip 明示）。
- 按「播放」：6 次取樣 step 40→242，亮度 168→184→112→24→0→56，**真的在呼吸**。
- 按「讀取裝置 frame」：`0xF06B` → `0xF06C`，chip 顯示
  `seq 2 · 64 px · breathe · white 0% · apply 199 µs`；sim 回 512 hex。
- 按「套用到裝置」`0xF06D`、按「重新載入 /pixel」`0xF06F` 都成功。
- `#flash` Build 卡：board `ESP32-S3-Matrix` + project `ESP32-S3-Matrix-Show`，
  真的由 UI 建置成功（2.6–14.9 s），artifact 696.6 KiB；`BuildApi.ota()` 再
  成功 OTA 到 sim。`window.confirm` 需在 headless 測試中 override；正常桌面
  瀏覽器會出確認對話框。
- `python -m pytest`：**295 passed**（新增 `test_sim_show.py`＋前端 blank/gain/white/reload
  檢查）。Headless Chrome 截圖：`/tmp/webmaster-mode-gain.png`、
  `/tmp/webmaster-build-done.png`、`/tmp/webmaster-live-sim.png`（本機，不入 repo）。
- sim 的 seed 已加入 Show 四層（breathe、matrix、white80/color、registry），
  `loadTree` 會先 `POST /api/scan` 重建 manifest，無板都可預覽。
- 限制：sim 的 `0xF06B` 是軟件模型，不是真 LED；Matrix 硬件仍短路未供電。

### 7.158 2026-11-21（新硬件 ESP32-S3-Zero＋Zero-Show project；GPIO1）

**新硬件** `ports/hardware/S3/ESP32-S3-Zero/`（Waveshare ESP32-S3-Zero）：

- ESP32-S3FH4R2：4 MB flash、2 MB PSRAM quad 80 MHz、240 MHz；USB Type-C
  OTG FS（GPIO19/20）、console none／CDC。
- **LED 資料腳 = GPIO1**（用家指定，外部燈鏈）。官方板載 WS2812 是 GPIO21，
  本專案不用它。
- I80 設定：D0=GPIO1、D1..D7=38,39,40,41,42,47,48、WR=2、DC=4；
  dummies 會出現在排針，不要接。
- LED 鏈當 **64 顆（8×8）**；如果只接單顆，firmware 仍送 64 顆，第一顆正常，
  多出的資料會被鏈尾忽略。格式 ws2812／GRB／無 W。
- flash 4 MB、2 MB PSRAM 依 ESP32-S3FH4R2 規格；未逐項實測。

**新 project** `ports/project/S3/ESP32-S3-Zero-Show/`：

- `hardware: S3/ESP32-S3-Zero`，`default: true`，aliases `zero-show`／`zero`。
- 生產者 content_task（`/data/pixel` 四層）→ prd 環；render → I80 → GPIO1；
  pixel level=-1；不含 effect_bench／NC_ENABLE_TESTS。
- data/config 由 Matrix-Show 複製（tasks：render/content level4 [0,1]）；
  data/pixel 低亮度 breathe（F=10／end_Time=256、white=0、0–23/255）。
- `tools/board.py check`：**0 errors**；Web `/api/build/info` 已列出
  `ESP32-S3-Zero`／`ESP32-S3-Zero-Show`。

**Build**：

```bash
python3 make.py build ESP32-S3-Zero --project ESP32-S3-Zero-Show --fresh
→ ✔ 696.2 KB (0xae0b0)，33 s；artifacts/ESP32-S3-Zero.bin 712,880 B
→ CONFIG_NETCORE_CONTENT=y、PIXEL_I80_PINS="1,38,39,40,41,42,47,48,2,4"
→ ELF 有 `_nc_task_reg_content`，冇 effect_bench／0xF065
```

Web `/api/build` 也成功（14.07 s）。**板上未插 USB（`/api/ports` 冇新板）**，
所以未 flash、未驗真燈；插上後由 Web Build 卡（board Zero／project Zero-Show）
或 `tools/ota.py` 上傳。

### 7.159 2026-11-21（ESP32-S3-Zero 真板上板：GPIO1、64 px 呼吸、Web OTA）

**硬件**：USB 出現 `/dev/cu.usbmodem11201`（USB-Serial/JTAG，MAC `50:78:7d:18:8f:50`）。
esptool 確認 ESP32-S3 QFN56 v0.2、4 MB flash（XMC）、2 MB PSRAM。全片 flash
`artifacts/build/S3_ESP32-S3-Zero/flash_args` 成功；重開後 app CDC 變成
`/dev/cu.usbmodem50787D188F501`，WebMaster 認到 `esp_Net-Core`。

**Firmware**：`ESP32-S3-Zero-Show`、app `d07e166b`、image 712,880 B；
`CONFIG_NETCORE_CONTENT=y`、`PIXEL_I80_PINS="1,38,39,40,41,42,47,48,2,4"`、
`PIXEL_LEDS=64`；tasks：render／content level4、pixel -1。

**內容／真機 frame**：

- 第一次上傳遇到 `err_busy`（FAT 由 MSC host 掛住）；用 `0x1103
  {"System":{"file_mode":1}}` 接管 FAT 後 5 個 `/pixel` 檔全成功。
- `0xF06B`：`px=64`、`mode=breathe`、`white=0`、`apply_us=195`；
  連續取樣 max **21→14→0→20→15→0**，真機在呼吸（0–23 級）。
- 0xF06D 切 `color` → `white=0`、apply 278 µs；切回 breathe；0xF06F reload 成功，
  frame 仍 64 px／breathe／apply 349 µs。
- Web API OTA 同一 image：8.42 s、`applied=true`；重連後 app `d07e166b`、frame 正常。

**注意**：LED 鏈按 64 顆（8×8）設定；如實際只接單顆，第一顆仍正常，多出的資料
會被鏈尾忽略。板載 WS2812 是 GPIO21，本專案用 GPIO1 外部鏈。D1..D7／WR／DC
dummy 腳不要接東西。

### 7.160 2026-11-21（三任務：effect → pair → render；Zero 真機驗證）

用家要求把 producer 拆成三個任務；已完成並在 Zero 真機跑住：

| 任務 | 核心 | 職責 |
|---|---|---|
| **effect** | core0 | 只 evaluate program 波形值；`nc_pipe_plan_snapshot()` 取 plan 副本；輸出 fixed `nc_pipe_effect_frame_t`（8 job × 256 value、seq、plan_gen），50 fps |
| **pair** | core1 | 載 `/data/pixel`、建 plan、按 group/range/write 配對、white mix、寫 prd 環；0xF06B/0xF06D/0xF06F；auto_play |
| **render** | core1 | 現有 encode→I80→WS2812；只認 canvas |

介面 `firmware/components/tasks/pipeline_task/include/nc_pipeline.h`：

- `nc_pipe_plan_snapshot(plan, assets, cap, &gen)`：pair → effect；copy plan，asset 逐個
  copy 並 repoint，effect 不再持有 pair 的 asset 表 pointer。
- `nc_pipe_effect_copy(frame)`：effect → pair；mutex 保護的 fixed frame copy。
- plan mutex：pair 的 mode rebuild/reload 同 effect snapshot 互斥；effect frame 有
  `plan_gen`，pair 會丟棄換 plan 期間的舊 frame。
- pair 每幀用 `nc_px_pair_apply()`，**不再**呼叫 `nc_px_show_frame()`（後者仍是 host／
  舊 API 的合成入口）。

**建置／真機**：

- Zero image **714,192 B**、app **`25966230`**；`status tasks` 見到
  `render`／`pair`／`effect` 三條（pair level4 core1；effect level4 core0），pixel -1。
- `0xF06B`：64 px、breathe、white=0、apply_us **233**；連續 max
  **23→17→5→0→5**，呼吸正常。
- 0xF06D 切 color／切回、0xF06F reload 仍正常（pair 處理；effect snapshot 自動跟
  plan_gen）。
- Matrix-Show build 697.9 KB、EffectPerf build 676.7 KB 都成功；Show ELF 冇
  effect_bench／0xF065。
- Web UI `status` 會直接見到這三條任務；Build 卡仍可建 Zero-Show。
- 限制：effect frame 每 job cap 256 values（同舊 `nc_px_show_frame` 一致）；9,600 px
  的高密度值仍要之後放大 frame／分塊。white mix 仍是整幀。

### 7.161 2026-11-22（任務名清理＋三任務分段計時＋effect 極限 burn）

**任務清單**：Show 映像現在只有三個 production 任務，無 `pixel`／`content`／
`effect_bench`：

```text
render  level4 core1
pair    level4 core1
effect  level4 core0
```

- component 由 `content_task` 改名 **`pipeline_task`**；`pixel_task` 在
  `CONFIG_NETCORE_CONTENT` 之下不再登記（`#if PIXEL && !CONTENT`）；`effect_bench`
  移到 `firmware/components/bench/effect_bench`，只有 EffectPerf build 加入。
- `tasks.json`：`render`／`pair`〔core1〕＋`effect`〔core0〕。

**分段計時**（0xF071，Zero 真機、64 px、1 job、白 0）：

| 段 | avg | max | 說明 |
|---|---:|---:|---|
| effect evaluate | **46 µs** | 76 µs | 1 job × 64 值 |
| pair total | **34 µs** | 75 µs | apply＋white＋commit |
| ├ apply | 25 µs | 45 µs | `nc_px_pair_apply` |
| ├ white | 0 µs | 1 µs | white=0 快路 |
| └ commit | 8 µs | 31 µs | 寫 prd 環 |
| effect→pair latency | 434 µs | 443 µs | 50 fps 排程 |
| render encode | **372 µs** | — | 64 px WS2812 |
| render tx | 17 µs | — | I80 |
| render fps | 49.9 | — | frame 20.024 ms |

**極限 burn**（0xF073，3 秒；effect 不等 50 fps，pair/render 仍受環／50 fps 限制）：

| 段 | 結果 |
|---|---|
| effect | **45,646 幀 / ~4 s ≈ 11,400 fps**；avg **36 µs**、max 108 µs |
| pair | 354 幀（ring/render 限流）；apply avg26、commit avg5 |
| ring full | 2,933 |
| render | 1,236 幀、50.0 fps、enc 363 µs、tx 15 µs |

結論：64 px 的 effect/pair 只佔幾十 µs，**render 50 fps 才是主節拍**；effect 有
大量 headroom（burn 11k fps）。9,600 px 仍需另量，不能直接把這個比例外推。

**指令**：`0xF071` PIPESTATS→`0xF072`、`0xF073` PIPEBURN→`0xF074`、
`0xF075` PIPERESET→`0xF076`；Web 模式頁加「三任務計時」卡（讀 stats／3s burn／reset）。
CLI：`tools/pipeline_stats.py`。

**Build**：Zero 688.5 KB（app `41d90373` 之前；最新 stats build 705,056 B）、
Matrix-Show 689.0 KB、EffectPerf 676.8 KB；皆成功。

### 7.162 2026-11-22（極限定義：9,600 RGB、每個 LED 一個波、隨機配對）

用家澄清「極限」＝ **9,600 個 RGB LED，每個都有自己的波，完全混亂／隨機擺放，
再由配對任務放回 9,600 個輸出的位置**。生產 pipeline 目前 effect frame 每 job
cap 256 值，唔可以直接跑 28,800 值；因此用 **EffectPerf 0xF069（同一個 S3、
同一份 nc_px_job_vals／nc_px_pair_apply12）** 做極限 probe，量三階段 CPU：

設定：`px=9600`、`nv=28800`（3 值/LED，write rgb）、`frames=2`、
`table_rnd[k] = (k*7919) % 9600`（近似隨機 permutation）。

| 階段 | 時間 | 備註 |
|---|---:|---|
| gen（28,800 個 12-bit 值） | **15.600 ms** | 542 ns/值 |
| pair range（連續） | 4.909 ms | 511 ns/px |
| pair table（連續查表） | 5.301 ms | — |
| **pair random（隨機 destinations）** | **19.967 ms** | 2.08 µs/px；比 range 慢 4.07× |
| conv 12→8（捨入） | 3.424 ms | — |
| **full range** | **24.202 ms** | 121% of 20 ms；剩 −4.20 ms |
| full table | 23.948 ms | — |
| **full random** | **37.863 ms** | **189% of 20 ms；剩 −17.86 ms** |
| PSRAM 12-bit canvas | 量不到（c12p 配置失敗） | — |

判讀：

1. **光是隨機 pair 已經 19.967 ms > 50 fps 的 20 ms 預算**；即係就算
   effect 免費、render 免費，9,600 RGB 隨機映射都已經貼死 50 fps。
2. 加 gen 同 conv：**37.863 ms → 26.4 fps**；60 fps 更不可能。
3. 連續 range 的 full 是 24.202 ms，仍然超 50 fps 4.2 ms；所以「9,600 RGB 全隨機
   + 50 fps」在目前單核資料路徑上不可行。
4. 三個 production 任務在 9,600 之前要先改：
   - effect frame 由 256 值/job 擴到 28,800（或分塊／PSRAM 環）；
   - pair plan 由「最多 4 連續 ranges」扩展到 random offset table（19.2 KB），
     或先把 random 排成 runs；
   - render 9,600 顆 WS2812 單 lane 的物理時間約 **288 ms/幀（3.47 fps）**；
     要 50 fps 需要約 **15 條 800 kHz lane**（或更低 FPS／更少燈）。
5. 所以目前 64 px 的三任務數字（effect 46 µs、pair 34 µs、render 349 µs）
   不能外推到 9,600；極限是 **37.9 ms／26 fps（CPU）**，未計 1-lane 送燈。

**已恢復**：probe 後已 OTA 回 Zero-Show 三任務映像；`status tasks` 仍是
`render`／`pair`／`effect`，冇 pixel／content／effect_bench。

> **重要修正（見 §7.163）**：19.967 ms 是 **12-bit canvas** 的 random scatter；
> production pair 用 **8-bit canvas**，同一 9,600 RGB random 只需 **0.970 ms**；
> gen+pair8 = **16.571 ms**，仍可在 50 fps CPU 預算內。


### 7.163 2026-11-22（9,600 RGB random 優化：8-bit canvas 快 20×）

同一 EffectPerf probe 加了三種 8-bit 寫法（production pair 本身是 8-bit；
之前的 19.967 ms 是 **12-bit 76.8 KB canvas** 的 random RMW）：

| 寫法 | 時間（9,600 px RGB） | 相對 12-bit random |
|---|---:|---:|
| 12-bit random table | 19.967 ms | 1.00× |
| **8-bit random table** | **0.970 ms** | **20.6× 快** |
| 8-bit inverse（dest 順序寫、source 隨機讀） | 0.931 ms | 21.4× |
| 8-bit block-random（16 px 一段，近似多組別） | 0.967 ms | 20.7× |
| **gen + pair8 random（full8）** | **16.571 ms** | 50 fps 的 **82.9%** |

判讀：

1. **最大優化就是 production 已經用的 8-bit canvas**。後續用 `esp_ptr_internal()`／
   `esp_ptr_external_ram()` 證實：**12-bit canvas 落在 PSRAM**（`c12_int=0,
   c12_ps=1`），8-bit canvas 在 internal SRAM（`c8_int=1`）。20× 主要是
   **PSRAM 隨機寫入**，不是單純 4 次 16-bit store。詳見 §7.165。
2. 8-bit random 同 inverse／block 幾乎一樣（0.93–0.97 ms），所以「完全隨機」在
   8-bit 之下已經不是瓶頸；**gen 的 15.6 ms 才是主要成本**。
3. 9,600 RGB CPU full8 random **16.571 ms**：50 fps 剩 **3.43 ms**，可行但緊；
   60 fps（16.67 ms）只剩 ~0.1 ms，不可靠。
4. 真實「很多組別」用 block8（16 px/段）0.967 ms，與完全 random 差不多；group
   數量本身不是 8-bit 的主要問題。
5. 仍未計 render：9,600 顆 WS2812 單 lane 約 **288 ms/幀（3.47 fps）**；要 50 fps
   需要約 15 條 800 kHz lane。render encode CPU 亦要另量。
6. 下一步若要 production 9,600：
   - effect frame 由 256 值/job 擴到 28,800（PSRAM／分塊）；
   - pair plan 加 random offset table（19.2 KB）或 runs；
   - **保持 8-bit canvas，不要在中間建 12-bit random canvas**；
   - gen 用 wave table 快取（把 15.6 ms 壓低）才有 50 fps 餘量；
   - 多 lane 輸出。


### 7.164 2026-11-22（Wave table 重用：gen 15.6→2.0 ms；9,600 RGB full 4.41 ms）

**為什麼 12-bit／8-bit 差 20×**（2026-11-22 最終修正）：

- 用 `esp_ptr_internal()`／`esp_ptr_external_ram()` 量到：
  **12-bit canvas 在 PSRAM**（`c12_int=0, c12_ps=1`），8-bit canvas 在
  internal SRAM（`c8_int=1`）。
- 原因：9,600 RGB 的 vals（57.6 KB）＋ 8-bit canvas（38.4 KB）＋兩個 table
  （38.4 KB）已經佔用 internal；再要 76.8 KB 的 12-bit canvas 時 internal
  不足，fallback 去 PSRAM。
- **不是**「四個 channel 連續但分成四次 store」造成 20×；用 packed12
  （每顆燈兩個 32-bit store）量到 **20.600 ms**，幾乎一樣，證明 store 數不是主因。
- 真正主因：**PSRAM 的隨機寫入**。Sequential range 在 PSRAM 是 4.9 ms；
  完全 random 變成 19.97 ms；internal 的 8-bit random 只需 0.97 ms。
- 如果真的要 12-bit：保持 canvas 在 internal（減少同時配置／用 tile），
  或先做 partial 12-bit chunk；**不要**把整個 76.8 KB random canvas 放 PSRAM。

**一次預算、之後重用：wave table**：

- effect program 的總長度只有 256（或最多 4096）；把 `nc_px_engine_program_value()`
  的 0..total-1 結果預先算成 `uint16_t wave[total]`，plan 變更時建一次，
  之後每幀只做 index。
- 已實作在 `effect` 任務（`nc_effect_task.c`），64 px 真機：
  **effect avg 46 → 7 µs**（max 75 → 65 µs），burn 6 µs。
- 9,600 RGB probe（EffectPerf 加 cached wave / 8-bit full）：

| 項目 | 無 wave table | 有 wave table |
|---|---:|---:|
| gen 28,800 值 | 15.603 ms | **1.993 ms** |
| pair8 random | 0.970–1.010 ms | 0.970–1.010 ms |
| **full8 random** | **16.572 ms** | **4.408 ms** |
| 50 fps 預算 | 82.9% | **22.0%** |
| 60 fps | 99.4% | **26.4%** |

**其他可以一次過預算、重用**：

1. **permutation table／inverse**：9,600 × uint16（19.2 KB）；inverse8 0.931 ms，
   比 random8 0.970 ms 略快；12-bit 或 PSRAM 時更有價值。
2. **runs／block schedule**：真實多組別用 16 px block，0.968 ms，和完全 random 差不多。
3. **destination byte offsets**：`offs[k]<<2` 預先存，省每像素 shift。
4. **12→8 LUT** 4,096 entries（4 KB），但 8-bit pair 直接 `>>4` 已很便宜。
5. **wave table** 是最值錢：gen 的 7.8× 改善。
6. 不可重用／要每次做的：效果參數改變、mode reload、plan rebuild；wave table 要在
   plan_gen 改變時重建。

限制：這是 **CPU probe**，未計 render；9,600 單 lane 物理仍是 288 ms（3.47 fps）。
要 50 fps 仍要約 15 條 lane（或降低 FPS）。


### 7.165 2026-11-22（實測 c12 在 PSRAM；packed12 20.6 ms；修正 20× 解釋）

`0xF069` 新增 `esp_ptr_internal()`／`esp_ptr_external_ram()` 診斷，9,600 RGB：

```text
mem: c12_int=0, c12_ps=1, c8_int=1, c8_ps=0
pair_us: range=4.895ms, random12=19.968ms, packed12=20.600ms, random8=0.970ms
```

- **12-bit canvas 在 PSRAM**：76.8 KB 放不落 internal（vals 57.6 + c8 38.4 +
  tables 38.4 已佔用）；fallback PSRAM。
- **packed12 沒有改善**（20.600 ms）：每顆燈雖然由 4 次 16-bit 變 2 次 32-bit，
  但仍然是 PSRAM random write；證明「四個連續 channel 分四次 store」不是主因。
- 8-bit canvas 在 internal（38.4 KB），random8 0.970 ms；差距 ~20× 主要是
  **PSRAM vs internal**。
- 所以 production 保持 8-bit internal canvas；真要 12-bit，必須令 canvas 留在
  internal（減少並存 buffer／tile／分塊），不是換 store 寫法。
- Wave table 仍有效：gen 15.614 → **1.996 ms**；full8 random **4.409 ms**。

### 7.166 2026-11-22（固定 buffer 政策：HOT=internal、BULK=PSRAM，取消 fallback）

用家要求：不要「智能」按 free heap 動態選 internal／PSRAM；固定一套使用方法，
大資料直接 PSRAM。已改成 **build-time 固定政策**：

- **content/pair canvas 已硬編碼永遠 PSRAM**（任何 LED 數都同一條路徑），
  沒有 `NETCORE_PIPE_CANVAS_PSRAM` 分支。
- `CONFIG_NETCORE_PIXEL_PACK_PSRAM`：Show project 固定 `y`，I80 packed 波形
  所有 LED 數都用 PSRAM。
- 64 px Zero 也走 PSRAM；allocation 失敗就 task 失敗／log，不會偷偷改位置。
- `NETCORE_PIXEL_LEDS` range 由 2048 擴到 16384（9,600 必要）。
- wave table：固定 internal，plan 時建一次；OOM 才 fallback interpreter（極少數，
  屬錯誤路徑，不是正常性能路徑）。
- `nc_buf` 仍是唯一 buffer 工具；policy 只決定 class（S/P/DMA）。

效果：同一份 firmware 的 canvas／packed buffer 位置固定，latency 可預期；
9,600 要 PSRAM 就在 project sdkconfig 明確寫 y，不再靠 runtime 猜。

### 7.167 2026-11-22（固定 buffer policy 後全面重測；9,600 RGB 三次穩定）

**受測映像**：Zero-Show 固定 policy，app `431e1cb5`、image 705,584 B；
`status tasks` 只有 `render`／`pair`／`effect`。64 px `/pixel` breathe、white=0。

**64 px 三任務（0xF071，reset 後 steady）**：

| 段 | avg | max |
|---|---:|---:|
| effect | **6 µs** | 25 µs |
| pair | 49 µs | 79 µs |
| ├ apply | 33 µs | 45 µs |
| ├ white | 0 µs | 1 µs |
| └ commit | 15 µs | 35 µs |
| latency | 477 µs | 484 µs |
| render | 361 µs enc / 21 µs tx | — |
| render fps | 49.9 | frame 19.993 ms |

**64 px burn 2 秒**（0xF073）：

| 段 | 結果 |
|---|---|
| effect | **55,866 幀 / ~3 s ≈ 18,600 fps**；avg 6 µs、max 51 µs |
| pair | 411 幀（ring 限流）；apply 30 µs、commit 11 µs |
| full | 1,889 |
| render | 50.0 fps、enc 336 µs、tx 22 µs |

**9,600 RGB、28,800 值、random permutation（0xF069，3 次獨立 run）**：

| 項目 | run 1 | run 2 | run 3 |
|---|---:|---:|---:|
| gen（interpreter） | 15.614 ms | 15.6 ms | 15.6 ms |
| gen（wave table） | 1.996 ms | 1.99 ms | 1.99 ms |
| pair random12 | 19.968 ms | 19.967 ms | 19.968 ms |
| pair packed12 | 20.599 ms | 20.601 ms | 20.599 ms |
| pair random8 | **0.971 ms** | **0.971 ms** | **0.971 ms** |
| pair inverse8 | 930 µs | 930 µs | 930 µs |
| pair block8 | 967 µs | 967 µs | 967 µs |
| full random12 | 37.880 ms | 37.862 ms | 37.862 ms |
| full random8 | 16.586 ms | 16.595 ms | 16.591 ms |
| **full cached8** | **4.426 ms** | **4.425 ms** | **4.409 ms** |
| mem | c12 PSRAM／c8 internal | 同左 | 同左 |

- `c12_int=0, c12_ps=1, c8_int=1` 三次一致 → **固定位置、數據穩定**。
- full cached8 **4.41–4.43 ms**，50 fps 22%，60 fps 26.5%（CPU probe）。
- 9,600 單 lane WS2812 物理仍約 288 ms/幀；render 未計。
- production 未支援 9,600（effect frame cap 256、pair random table 未接）；
  以上是 EffectPerf fixed-policy probe，不是 production 三任務。

**功能重測**：0xF06D 切 color／breathe、0xF06F reload 成功（apply 225–231 µs），
frame 64 px 正常。

**Build／測試**：Matrix-Show 689.5 KB、EffectPerf 678.6 KB、Zero 705,584 B 全部成功；
host 20 套全綠、Web 296 passed。

### 7.168 2026-11-22（固定 policy 下 8-bit internal vs PSRAM；inverse/block 是關鍵）

用 `0xF069` 的 `--c8-psram` 明確固定 8-bit canvas 位置（不是 runtime fallback），
9,600 RGB／28,800 值、random permutation：

| 項目 | c8 internal | c8 PSRAM |
|---|---:|---:|
| random8 pair | **0.972 ms** | **10.647 ms** |
| inverse8 pair | 0.931 ms | **1.872 ms** |
| block8 pair（16 px） | 0.966 ms | **1.864 ms** |
| full8 cached（gen wave + random8） | **4.423 ms** | **14.060 ms** |
| **full8 inverse（gen wave + inverse8）** | **3.648 ms** | **4.586 ms** |
| full8 block | 3.689 ms | 4.583 ms |
| mem 診斷 | c8_int=1 | c8_ps=1 |

- 固定 PSRAM 不是世界末日：**唔好 random write**，用 destination 順序的
  inverse／block，full8 由 14.060 → **4.586 ms**。
- 64 px steady：effect **6 µs**、pair 49 µs（apply33／commit15）、render enc 361 µs、
  50.0 fps；burn 2 s：effect **55,866 幀／~18,600 fps**、avg6 µs。
- 12-bit PSRAM：random19.967、packed20.6（packed 冇用）；c12 固定 PSRAM，
  8-bit internal/PSRAM 分開量，不再靠 free heap。
- 9,600 三次 run 穩定：random12 19.967–19.968、random8(internal) 0.971、
  full cached8(internal) 4.409–4.426；mem 三次一致。
- 功能：0xF06D color/breathe、0xF06F reload 成功；app `431e1cb5`、image 705,584 B。
- Web 296 tests、host 20 套全綠。


### 7.169 2026-11-22（單一服務流程：任何 LED 數都經 PSRAM）

依用家要求改為**一條固定流程**，不再按 LED 數／free heap 選 internal 或 PSRAM：

- `content/pair` canvas：**永遠 PSRAM**（hardcode，無 Kconfig 分支）。
- Show 的 I80 `packed` 波形：**永遠 PSRAM**（Show project 固定
  `CONFIG_NETCORE_PIXEL_PACK_PSRAM=y`）。
- HOT internal 只保留 plan／asset／wave table／offset table，不再包括 64 px canvas。
- 64 px 與 9,600 px 走同一條 `effect → pair(PSRAM canvas) → render(PSRAM packed)`
  流程，只有 buffer 大小不同。

**64 px 全 PSRAM 真機重測**（app `d6837876`、image 705,568 B）：

| 段 | avg | max |
|---|---:|---:|
| effect | **8 µs** | 23 µs |
| pair | **35 µs** | 86 µs |
| ├ apply | 25 µs | 47 µs |
| ├ white | 1 µs | 3 µs |
| └ commit | 9 µs | 36 µs |
| render encode | 363 µs | — |
| render tx | 24 µs | — |
| render fps | 50.0 | — |

Burn 2 s：effect 55,656 幀（~18,500 fps）、avg6 µs；pair 257 幀（ring 限流）；
render 50 fps、enc362 µs。0xF06D/0xF06F 功能正常。

**9,600 RGB 同一 PSRAM 路徑**（EffectPerf probe，c8 PSRAM 固定）：

| 項目 | 時間 |
|---|---:|
| pair random8 | 10.647 ms |
| **pair inverse8** | **1.872 ms** |
| pair block8 | 1.864 ms |
| full cached random8 | 14.060 ms |
| **full cached inverse8** | **4.586 ms** |
| full cached block8 | 4.583 ms |
| 12-bit random12 | 19.967 ms |
| 12-bit packed12 | 20.599 ms |

結論：固定 PSRAM 之後，pair **必須用 destination 順序的 inverse／block**，不要
random write；9,600 的 CPU full 可由 14.06 ms 降到 **4.59 ms**，50 fps 仍可行。
12-bit packed 冇用，12-bit 要 tile／留在 PSRAM 但避免 random RMW。

### 7.170 2026-11-22（真實分組：500 組任意順序 ≈ 19 px/組）

用家澄清：真實排列通常分組，完全 random 只是極端。以 9,600 px／500 組計算，
每組約 **19.2 px**；組內連續、只有 500 個 group 的順序隨機。

用現有 `block8`（16 px 一組、600 組、group 順序 random）做保守 proxy：

| 配對 | c8 internal | c8 PSRAM |
|---|---:|---:|
| 16 px block（600 組） | **0.966 ms** | **1.864 ms** |
| 完全 random（9,600 destinations） | 0.972 ms | 10.647 ms |

- 500 組 ≈19 px，寫入次數同 16 px block 一樣係 9,600 次，但只有 **500 次 group
  跳轉**，不是 9,600 次 random jump；所以 pair 應該接近 block8 的 **0.97／1.86 ms**。
- full8（wave gen 1.99 ms + group pair）估 **3.0–4.6 ms**，50 fps 有空間。
- 如果用 per-pixel table：19.2 KB；用 runs table：500 × 6 B ≈ **3 KB**，而且
  每個 run 連續寫，PSRAM 友善。
- 所以 realistic 流程應該係 **runs table**，不是 9,600-entry random table；
  random table 只留作 worst-case probe。
- 現時 plan 仍限 8 mode entry／16 map group；500 組 production **未支援**，要先把
  plan 改成 run table（start/count/src）才能承接。
- 12-bit 不需要先建 12-bit canvas：pair 時 12→8 直接寫 8-bit；group 寫入順序化後，
  四次 16-bit 寫的影響遠小於 PSRAM random write。

### 7.171 2026-11-22（PSRAM 單一服務管線完整紀錄）

所有決策、實測、500 組可變長度 run 的 8-bit／12-bit 計算、table 設計、
render 物理限制、production 未完成項目，已整理成獨立頁：

→ **[psram-service-pipeline.md](psram-service-pipeline.md)**

重點：

- 所有 LED 數同一條 PSRAM 流程（canvas／packed 固定 PSRAM，無 runtime fallback）。
- 64 px production：effect 8 µs、pair 35 µs、render 363 µs、50 fps。
- 9,600 probe：8-bit full cached internal 4.4 ms、PSRAM inverse 4.59 ms；
  12-bit random 19.97 ms、packed 20.6 ms。
- 500 個可變長度連續 group：用 3 KB run table；估 8-bit pair 1.5–1.9 ms、
  full 3.5–3.9 ms；12-bit canvas full 10.4–10.7 ms（不建議）。
- 單 lane 9,600 物理 288 ms/幀；要約 15 lane 才 50 fps。

### 7.172 2026-11-22（排版配對：500 個組合的真相；設計定案）

**用家澄清兩件事（之前的理解錯）**：

1. 「500」是 **一個 map 裏面 500 個 group（組合）**，唔係 500 條 run。舊項目
   `mp_Net-Core/slave/pixel/map/*.json` 的 group 係一段**有序 `sel` 列表**：可交叉型別、
   反序（`15:10:-1`）、隔顆（`::2`）、單顆、重疊；展開 = 每顆的全域 pixel index。
   mode 的每一項 = group × effect × write × range。
2. **16×600 = 9,600 px 是所有測試的設計基準**（真實 show 檔 `tools/PC/show_test.pxld`
   係 37 slaves、**15,808 px**、12,444 幀、40 fps，每 slave 336／776／1,584 px）。
   排版配對要「越多組合越好」，但之前量到完全隨機的散落寫入不可行，所以要設計。

**排版檔案本身是自描述的**（PXLD v3 同一原則）：header 講總數、每個 slave entry 講
`{channel_start, channel_count, pixel_count, data_offset, data_length}`——切法係資料，
firmware 唔應該用固定常數假設。

**firmware 現在的失真（全部靜默截斷）**：

| 失真 | 位置 | 後果 |
|---|---|---|
| group 上限 16／每組 sel 8／mode 條目 8 | `nc_px_layer.h` | 第 17 組、第 9 段、第 9 條目消失 |
| 反序 step<0 | `nc_px_show.c: expand_slice()` | 整段變空 |
| 隔顆 step>1 | 同上（cap=4） | 600 顆只取 4 顆 |
| 每 job 4 段 | `NC_PX_JOB_RANGE_MAX` | 第 5 段起消失 |
| 值流 256 | `nc_pipe.h: NC_PIPE_VAL_MAX` | `pixel_n>256` 被切 |
| index 16-bit、base+count 連續假設 | `nc_px_job_range_t` | 65,535 上限；任意排列無法表達 |

**定案（設計寫入 [pixel-layout-pairing.md](pixel-layout-pairing.md)）**：

- 排版保持自描述（map／mode JSON 不變語義），容量由檔案決定：group 數、`sel` 段數、
  總 px、job 數、值流長度全部 data-driven；index 用 **32-bit**。
- plan 編譯成 **destination 升序的 run 表** `{dst:u32, src:u16, count:u16, job:u8}`：
  counting bucket（O(px+n)，唔用 qsort）＋ 掃一次合併連續段；重疊寫保持 entry 次序。
- pair 只順序掃 runs → PSRAM 順序寫（inverse8 實測 **1.872 ms**），唔再係 random 10.647 ms。
- run 數 ≈ `sel` 段數（500 組 × 1–3 段 ≈ 500–1,500），成本同「幾隨機」無關。
- 估 9,600 px：pair ≈1.9 ms、full（wave gen 2.0）≈3.9 ms → 50 fps 20%；15,808 px ≈3.1 ms。

**遷移（6 階段，逐步 host test 綠）**：map data-driven → 編譯器＋對拍舊 `scatter_offs`
→ pair 改讀 runs → job／值流 data-driven →（可選）PC 預編譯 `.pxlt` → 9,600／15,808 真機 probe。

**未做**：以上全部；`psram-service-pipeline.md` §6 已更正「500 條 run」的誤讀，
§2／§7 補上 16×600、37 slave／15,808 px 基準與 16 lane 的 18 ms（≈55 fps）數字。

### 7.173 2026-11-22（排版配對落地：data-driven map／mode ＋ dst 順序 run 表）

**依據**：[pixel-layout-pairing.md](pixel-layout-pairing.md) §7。把 §7.172 的設計做成代碼，
目標係「500 個 group × 16×600、任意 sel（反序／隔顆／重疊）」真正入到 production。

**做咗**：

| 層 | 改動 |
|---|---|
| map | `nc_px_map_t`／`nc_px_group_t` 改 pointer＋pool；新增 `nc_px_map_count()`／`nc_px_map_parse(pool)`（兩步、容量由檔案決定）；group／sel 唔再 16／8 上限 |
| mode | `nc_px_mode_t.entry` 改 pointer；`nc_px_mode_count()`／`nc_px_mode_parse(pool)`；條目唔再 8 上限 |
| inventory | `nc_px_hw_inv_t.base/px` 改 **uint32**（唔再 65,535 上限） |
| 新編譯器 | `nc_px_layout_build()`：group.sel → 有序全域 index（反序／隔顆／重疊照舊語義）→ range → **counting bucket 按 dst** → 合併 `{dst, src, count, job}` runs（O(px+n)，冇 qsort） |
| 配對 | `nc_px_layout_apply()`／`apply12()`：順序掃 runs 寫 canvas（PSRAM 友善）；舊 `nc_px_pair_apply` 保留 A/B |
| 值流 | `nc_px_aprog_t`：**按 asset 去重**（500 個 group 共用效果 → 唔重複算）；`nv = pixel_n`，取消 256 |
| pipeline | pair 建 layout、發佈 aprog；effect 每條值流一個 wave table／**雙 pool + ack**（pair 慢就等，唔撕裂）；pair 逐幀 `nc_px_layout_apply` |

**驗證**：

- 新 host test `tools/hosttest/pxLayout_test/`：
  - 單 group 連續鏈：新舊 path **逐 byte 相同**
  - 反序 `31:15:-1`：舊 path 整段丟棄 → 新 path 16 條 run（dst 16..31 ← src 15..0）
  - 隔顆 `::2`：舊 path 只取 4 顆 → 新 path 16 顆
  - 重疊寫：兩個 job 打同一批燈的 R／G，互不清走
  - **500 個 group、9,600 px、隨機段次序**：build **0.66–0.67 ms**、apply **0.03 ms**（host x86，3 次穩定）；
    run 覆蓋 9,600 顆、dst 非遞減、抽樣值正確
- `bash tools/hosttest/run_all.sh`：**21 套全綠**（新增 pxLayout_test）。
- 三塊 project 都建置成功：
  - Zero-Show：**688.7 KB**（0xac2a0，分區剩 55%）
  - Matrix-Show：**689.1 KB**（0xac470）
  - Matrix-EffectPerf：**679.1 KB**（0xa9c80）

**限制解除對照**（§7.172 的表）：

| 之前 | 現在 |
|---|---|
| group 16／sel 8／mode 條目 8 | 由檔案決定（pool；`NC_PX_GROUP_MAX` 只係壞檔健全性上限） |
| 反序整段丟棄 | 完整支援（dst 升序後可能拆成多條 run） |
| 隔顆只取 4 顆 | 完整支援 |
| 每 job 4 段 | 無段數限制（任意多 sel） |
| 值流 256 | `pixel_n` 無上限（pool 由 plan 計總長） |
| index 65,535／base+count 連續假設 | 32-bit per-pixel index，唔假設連續 |

**未做**：真機 probe（9,600／15,808 px，要上板）；PC 預編譯 `.pxlt`；
16 lane render（9,600 要 16 lane 才 ≈55 fps）；`pixel_n=9,600` 的 PSRAM 雙緩衝實測。

### 7.174 2026-11-22（記憶體修正：internal 靜態 62 KB → 0.8 KB；group id 16-bit）

**問題（用家指出）**：internal 總共只有 ~100 KB，唔可以一個任務食一半。
量 ELF 之後證實第一版 §7.173 有：

| 位置 | 大小 | 內容 |
|---|---:|---|
| `s_sets[2]` | 34,320 B | 兩份 aprog set（每份 32 × 536 B，含 16 段編譯 program） |
| `s_set` | 17,160 B | effect 的工作副本 |
| `s_content` | 10,644 B | pair 狀態（asset 表 8.4 KB ＋ 0xF06B 快照 2 KB） |
| **合計** | **~62 KB** | 佔 internal 六成 |

**修正**：

1. **aprog 集搬 PSRAM**：三份 set（2 發佈 ＋ 1 工作）改為 lazy `heap_caps_malloc(MALLOC_CAP_SPIRAM)`，
   用 claim flag 保證只配一次；真正留在 internal 的只有 wave table 同指標。
2. **asset 表（8.4 KB）、0xF06B 快照（2 KB）搬 PSRAM**：兩者都唔係每幀熱路徑
   （換場讀一次／主機 poll 才讀）。
3. **group id 由 uint8 → uint16**：500 組時 `id` 會超過 255，解析會回 `INVALID_ARG`
   （`nc_px_js_num(j, 255, …)`）；呢個是新增的 **JSON-level 測試**（500 組 map／mode 檔案
   → `nc_px_map_count`／`nc_px_map_parse` → `nc_px_layout_build`）抓出來的，唔是 struct 測試。

**修正後 ELF 實測（Zero-Show）**：

| symbol | 大小 |
|---|---:|
| `s_content`（pair） | **220 B** |
| `s_pub_frame` | 272 B |
| effect 指標／計數 | 280 B |
| `s_sets`／`s_set` | 4 B ＋ 4 B（指標） |
| **internal 靜態合計** | **~0.8 KB** |

- image：**678.5 KB**（0xa99e0，分區剩 56%），比修正前細 10 KB。
- wave table 仍在 internal heap（每效果 512 B；最多 8 KB），OOM 退 interpreter。
- 編譯暫存（~355 KB）只喺換場用，之後 free；runs／jobs／aprog／canvas／值流 pool 全部 PSRAM。

**驗證**：`run_all.sh` 21 套全綠；`pxLayout_test` 新增第 6 組
「500 組 map／mode JSON → parse → compile」，host **0.87 ms**（內含 JSON 解析）。
剩餘 internal（`h_int`）要上板量；目標是回到 64 px production 的 ~102 KB 以上。

**可選下一步**：runs 改 8 B（dst／src 用 uint16）放 internal（4–12 KB），省每幀 0.3–1 ms 的 PSRAM 讀取。

### 7.175 2026-11-22（真機驗證：layout 管線上板；8-bit／12-bit 數字）

**板**：Waveshare ESP32-S3-Zero（`/dev/cu.usbmodemB43A45AD52E41`，USB-Serial-JTAG）、
IDF 6.0.3、`app 637bc82c`（Zero-Show，三任務）。firmware 用 OTA 推（`tools/ota.py`，
114–135 KB/s）；esptool 直燒失敗（USB-JTAG 收唔到 reset）→ 本板之後用 OTA。

#### 7.175.1 上板先踩到：pair／effect 冇啟動

- 現象：`0xF071` 全 0、`0xF06B` 回 `nodata`；`status tasks` 見 `pair`／`effect`
  `started=0`、`loops=[]`。
- 原因：板上的 **`/config/tasks.json` 是舊 project 的檔案**（pair／effect
  `affinity:[0,0]`），開機時優先於 firmware 內嵌的 preset；`[0,0]` = 唔開 loop。
- 修正：用 WebMaster `PUT /api/file?path=/config/tasks.json` 上傳 project 版本
  （pair `[0,1]`、effect `[1,0]`、pixel `-1`）＋ `tools/show_upload.py` 上傳 `/pixel`，
  重開機後三任務正常。
- **運維注意**：換 project 時一定要同步 `/config/tasks.json`，否則「任務有登記、但唔跑」。

#### 7.175.2 Production（64 px、50 fps）

| 項目 | 數值 |
|---|---:|
| `h_int`（開機剩餘 internal） | **184,555 B（180 KB）** |
| `h_ps` | **1,998,656 B（1.9 MB）** |
| effect | avg **6 µs**、max 29 |
| pair | avg **42 µs**（apply 28／white 0／commit 12）、max 186 |
| effect→pair latency | 452 µs |
| render | **49.9 fps**、enc 357 µs、tx 16 µs |
| 0xF06B | 64 px、mode breathe、plan rebuild 4,845 µs |

對照 §7.169（舊 plan）：effect 8 µs、pair 35 µs、render 363 µs — 新 layout 只多 ~6 µs（64 px）。

#### 7.175.3 Layout 編譯／配對（0xF077，9,600 px，PSRAM canvas）

payload `[px:u16][groups:u16][variant:u8][reps:u8]`；variant 0 = 每組一段、1 = 每組兩段（跨區）。
`apply` 寫 8-bit canvas、`apply12` 寫 12-bit canvas（效果層仍然 12-bit）。

| groups | variant | runs | build | apply8 | apply12 |
|---:|---:|---:|---:|---:|---:|
| 100 | 0 | 100 | 32.4 ms | 3.17 ms | 5.39 ms |
| 500 | 0 | 500 | 39.2 ms | 3.73 ms | 5.59 ms |
| 1,000 | 0 | 1,000 | 48.0 ms | 4.17 ms | 5.82 ms |
| 100 | 1 | 200 | 32.2 ms | 3.28 ms | 5.48 ms |
| **500** | **1** | **1,000** | **40.0 ms** | **4.06 ms** | **5.87 ms** |
| 1,000 | 1 | 2,000 | 50.7 ms | 4.77 ms | 6.12 ms |

- **build 是換場一次**；optimize 前 500 組是 **65.4 ms**、1,000 組是 **162 ms**
  （每條目線性掃 group 名，O(entries×groups)，名字在 PSRAM）→ 加 name hash 後
  40／50.7 ms。固定成本 ~32 ms 是 PSRAM 上的暫存（counts／cursor／entries／runs ~350 KB）。
- **apply 每幀一次**：500 組 1,000 runs **3.7–4.1 ms**（8-bit）、**5.6–5.9 ms**（12-bit）。
  內圈已改「計數器回繞」避免 `% nv`（原本 3.7 ms，改完 4.1 ms → **唔係 modulo 瓶頸**，
  是 PSRAM 寫入頻寬：4 B/px × 9,600 = 38.4 KB，約 10–14 MB/s）。
- 12-bit 只慢 1.5–1.8×（唔再是 20×）：因為 dst 已經係升序，冇 random RMW。

#### 7.175.4 8-bit vs 12-bit（0xF069 重跑，同一顆板）

| 項目 | 數值 |
|---|---:|
| gen（interpreter／wave table） | 15.607 ms ／ **1.996 ms** |
| pair range12 | 4.883 ms |
| pair random12 | 19.967 ms |
| pair packed12 | 20.604 ms |
| pair random8（canvas internal） | 0.971 ms |
| pair inverse8／block8 | 0.930／0.968 ms |
| conv 12→8 | 3.425 ms |
| full range／random12／random8／cached8／inv8／blk8 | 24.174／37.878／16.629／**4.426**／3.647／3.689 ms |

結論同 §7.164–7.169 一致：**12-bit canvas 落 PSRAM + random 寫 = 20× 慢**；
dst 升序（runs／inverse）之後 9,600 px 的配對回到 **4 ms 級**。

#### 7.175.5 未做／可再省

- production pair 仍然係 `memset(canvas)` → apply → `memcpy` 去環：9,600 px 多 2 次 38.4 KB
  （約 4–6 ms）。可直接寫入環 payload + white mix 就地做。
- runs 16 B → 可壓到 8 B（dst／src uint16），減少 PSRAM 讀取。
- build 固定成本 ~32 ms：暫存改 uint16／分塊／部分放 internal。

### 7.176 2026-11-22（16×600 = 9,600 px 真機：production 三任務實測）

用家要求「測試 16×600，不是 64」。新增 project **`ESP32-S3-Zero-Show9600`**
（Same Zero board、GPIO1；`CONFIG_NETCORE_PIXEL_LEDS=9600`）＋ 501 組 map ＋ 三條 mode：

| mode | 內容 | 用途 |
|---|---|---|
| `full256` | 1 組全鏈 × `wave256`（pixel_n 256） | 典型 |
| `g500` | 500 組 × `wave256`（每組 18–22 px） | 真實排版 |
| `full9600` | 1 組全鏈 × `wave9600`（pixel_n 9,600、rgb 28,800 值） | 每顆一個波（極限） |

**上板先要修的（全部已修）**：

| 問題 | 修正 |
|---|---|
| `NC_PX_MAX_LEDS=2048` 擋住 9,600 → render 冇 lane（start_err=261） | 改 **16,384**；SPI codec 暫存獨立成 2,048（唔跟隨，避免 65 KB BSS） |
| prd 環 115 KB 放 internal → render `NO_MEM`（start_err=257） | 新 Kconfig **`NETCORE_PIXEL_RING_PSRAM`**（build-time 固定政策）；9600 project 開 y |
| mock 後端冇 bind 環（`m->q=NULL`）→ 永遠 starved | `bind_lanes()` 對 mock 呼叫 `nc_px_backend_mock_bind()` |
| pair 靠「名入面有 prd」揀環；i80 起唔到時餵錯環 | 新增 `nc_render_first_ring()`，pair 跟 render 實際綁到的那條 |
| map／mode 檔 >8 KB 讀唔到（`SCAN_FILE_MAX`） | `read_all()` 先問大小再配（上限 256 KB） |
| 上傳工具限制（16 組／8 條目／id 255、寫死 white80／color） | `tools/show_upload.py` 改成 data-driven（65535 組／條目、全部 modes/*.json） |
| 舊 mode 檔同名 id 衝突（9600 的 mode 與 64 px 的 mode 撞 id）→ 64 px 內容被拒 | 換 project 時要清 `/pixel/modes` 舊檔（已在流程記錄） |

**9,600 px production（render = mock，令 pair 可以 50 fps 跑）**：

| mode | effect | pair | ├ apply | └ commit | render |
|---|---:|---:|---:|---:|---:|
| full256 | **42 µs** | **9.69 ms** | 5.17 ms | 4.51 ms | 50.0 fps |
| g500（500 組） | **39 µs** | **9.65 ms** | 5.17 ms | 4.49 ms | 50.0 fps |
| full9600（28,800 值） | **365 µs** | **10.25 ms** | 5.88 ms | 4.37 ms | 50.0 fps |

- `h_int` **199,395 B**、`h_ps` **1,519,256 B**（環 115 KB ＋ canvas 38.4 KB 都在 PSRAM）。
- **9,600 px／50 fps 的 CPU 預算**：pair ≈9.7–10.3 ms（48–51%）＋ effect 0.04–0.37 ms，
  仍然在 20 ms 內；但 `apply`（5.2–5.9 ms）同 `commit`（4.4 ms）兩筆就佔了大半。
- 對照 §7.175：64 px 是 effect 5 µs、pair 43 µs — 放大 150× 後的比例合理
  （9,600/64 = 150；pair 43 µs × 150 ≈ 6.5 ms，實測 9.7 ms，多出的部分是 PSRAM 寫入）。

**Render（真 I80）在 9,600 px 未可行**：I80 packed 波形 = `px × lane_bytes × bit_cells × 8`
≈ **10 MB**（2 MB PSRAM 放唔落）→ i80 後端起唔到，所以今次用 mock 量 CPU。
16×600 要真出燈，須改 packing（streaming／tile／減少 lane 組緩衝）＋ 16 lane（每 lane 600 px，
物理 18 ms/幀 ≈55 fps）；單 lane 9,600 px 本身是 288 ms/幀（3.47 fps）。

**工具**：`tools/layout_bench.py`（0xF077）、`tools/show_upload.py`（已支援 >16 組）；
9600 project 在 `ports/project/S3/ESP32-S3-Zero-Show9600/`。

### 7.177 2026-11-22（拆掉固定上限；改成「先計算、由配置回答」）

用家指示：**唔好設定 `NC_PX_MAX_LEDS=2048` 呢類上限**；要先計算（因為板上有其他任務），
先令系統按實際配置＋實際記憶體回答。

**拆掉的上限**：

| 之前 | 之後 |
|---|---|
| `NC_PX_MAX_LEDS=16384`（ring／render／engine 都靠它擋） | **移除**；px 由 `CONFIG_NETCORE_PIXEL_LEDS` 決定，環放唔放得落由 region 大小／配置成敗回答 |
| `NC_PX_UNITS_MAX = NC_PX_MAX_LEDS×8×8`（i80 一幀 bytes 上限） | **移除**；只檢查 `units != 0`，真正的上限是配置出來的 DMA 槽 |
| SPI codec 暫存 `NC_PX_MAX_LEDS×4` | 已獨立成 2,048×4+16（SPI 只服務小鏈；I80 唔受影響） |
| 測試／bench 內的 clamp | 移除；bench 由 payload 決定 px |

**改成「先計算」：新增預算 log（由實際配置算，唔猜）**

- `i80_out`：`預算：px=… lanes=… bus=…-bit → packed X B × 8、DMA 槽 Y B × slots`
- `pair`：`pair 記憶體：canvas … 值流 … runs … jobs … aprog …`
- `0xF071` 把 pair 拆成 **clear／apply／commit**（以前 memset 混入 apply）。

**16×600 的計算（ws2812、3 bit-cells／bit、8-bit bus）**

| 項目 | 公式 | 9,600 px 單 lane | 600 px／lane（16 lane 的每條） |
|---|---|---:|---:|
| DMA 槽 | `(px×3×8×3 + 200) B` | **691.4 KB** | **43.4 KB** |
| DMA 總額（3 格） | ×3 | **2.07 MB（internal，不可能）** | **130 KB（internal，可以）** |
| packed 波形 | `px×3×3 B ×8` | 691.2 KB（PSRAM） | 43.2 KB（PSRAM） |
| prd 環 | `px×4×slots` | 115.2 KB（PSRAM） | 7.2 KB（PSRAM） |
| pair canvas | `px×4` | 38.4 KB（PSRAM） | 38.4 KB（全鏈合計） |

⇒ **16×600 應該設定成 16 lane × 600 px，不是 1 lane × 9,600 px**：DMA 由 2.07 MB 降到 130 KB，
才放得入 internal（實測開機剩 199 KB）。單 lane 9,600 px 唔只是慢，是 **DMA 記憶體上不可能**。

**9,600 px production 實測（`ESP32-S3-Zero-Show9600`，render=mock 令 pair 可 50 fps）**

| mode | effect | pair | ├ clear | ├ apply | └ commit | fps |
|---|---:|---:|---:|---:|---:|---:|
| full256（256 值） | 39–42 µs | 9.64 ms | 2.03 ms | 3.13 ms | 4.47 ms | 50.0 |
| g500（500 組） | 39 µs | 9.65–9.68 ms | 2.04 ms | 3.15 ms | 4.49 ms | 50.0 |
| full9600（9,600 值） | **1.14 ms**（log 實測；stats 平均 0.65 ms 含切換前） | 9.7–10.8 ms | 2.05 ms | 3.15 ms | 4.48 ms | 49.9 |

- `h_int` **199,331 B**（最大連續 126,976）、`h_ps` **1.52–1.69 MB**。
- pair 的三段：**clear 2.0 ms（38.4 KB memset）＋ apply 3.15 ms ＋ commit 4.47 ms**；
  即 9,600 px 的固定成本約 **9.6 ms（48% of 20 ms）**。
- 量測注意：切 mode 之後要等 3–4 s 再 `0xF075`／`0xF071`（layout rebuild ~40 ms ＋排程），
  否則會讀到切換中的 0 值。

**未做**：I80 packed／DMA 改成 tile（例如 600 px 一塊）令單 bus 可以出 9,600；
16 lane 的實際接線與 pin map；把 clear 併入 apply（直接寫環）省 2.0 + 4.47 ms。

### 7.178 2026-11-22（原則落地：無隱藏上限；超出能力要講清楚）

用家原則：**用戶要能按自己想法執行；冇隱藏神奇限制；超出能力就讓用戶知道發生什麼事。**

**做咗（診斷先行）**：

1. **移除固定上限**：`NC_PX_MAX_LEDS`、`NC_PX_UNITS_MAX` 全部刪；px／一幀 bytes 由設定同
   實際配置回答（§7.177）。
2. **新指令 `0xF079 PIXDIAG`（回 0xF07A）**：一次過答：
   `px / slots / ring_b / canvas_b / vals_b / runs_b / jobs / runs / naprog / skipped`
   ＋ **skip 原因分佈**（no_asset／no_group／bad_write／empty／asset_full／prog／oom）
   ＋ `free_int`／`free_ps` ＋ render 每條 lane（be／ring／px／seq／fill）。
   用戶睇一次就知「我要嘅 px 要幾多記憶體、而家有幾多、邊條 lane 冇起」。
3. **layout 跳過唔再靜靜**：七類原因逐項計數，pair log 會列出「effect 唔存在 X、group 唔存在 Y、…」。
4. **i80 配置失敗講數**：`packed 波形緩衝（X B × 8 = Y B）配置失敗（固定 PSRAM；free internal …、free PSRAM …）`。
5. **lane 停用講原因**：`lane N 停用：後端 i80 起唔到／環 i80.prd 未登記`。
6. `0xF071` 拆 `clear／apply／commit`，唔再把 memset 混入 apply。

**實機示例（同一個指令、兩個配置）**：

64 px（production）：`ring_b=768、canvas_b=256、free_int=184,427、free_ps=1,998,656、
lanes=[i80, mock]、fps=49.9`。

9,600 px（`Show9600`）：`px=9600、ring_b=115,200、canvas_b=38,400、free_int=199,251、
free_ps=1,519,256、lanes=[mock]` —— **i80 唔喺 lanes 入面**，即係「超出能力」，
而同一個畫面已經有 free_int／free_ps 可以對照。

**能力計算（16×600）**：DMA 槽 = `(px×3×8×3 + 200) B`。
1 lane × 9,600 px = **691.4 KB/槽 × 3 = 2.07 MB**（internal 不可能）；
16 lane × 600 px = **43.4 KB/槽 × 3 = 130 KB**（可以）。所以 16×600 要設定成
**16 lane × 600 px**，唔是 1 lane × 9,600 px；呢個結論而家可以由 PIXDIAG 直接驗證。

### 7.179 2026-11-22（用家澄清：9,600 一直係 16×600；只測效果層／配對層）

用家指正：**9,600 一直指 16 條線 × 每條 600 顆**，唔係「一條線 9,600 顆」。
分層責任：效果層只決定「有幾多像素」；硬件層才負責分開每條線。
每條線 **絕對不超過 600（上限 666.6）**，因為 600 是 50 FPS 留餘量的設定。
硬件層測試已完成、剛好滿足；現在只測**效果層＋配對層**。

**用語對照（之後文件／回覆統一用左邊）**

| 用家字眼 | 之前我寫的技術字眼 |
|---|---|
| 效果層 | effect task／nc_px_layout 之前的值流 |
| 配對層 | pair task／apply／commit |
| 硬件層 | render task／I80／lane |
| 一條線 | lane |
| 清畫布 | memset(canvas) |
| 交去硬件層 | commit（複製去環） |
| 內部記憶體 | internal（晶片內建，快、細） |
| 外掛記憶體 | PSRAM（慢少少、大，2 MB） |
| 直接搬運 | DMA（硬件自己搬，唔用 CPU） |
| 連續一段 | run |

**16×600 的效果層／配對層實測（9,600 個像素）**

| 層 | 工作 | 每格時間 |
|---|---|---:|
| 效果層 | 一個效果 256 個值 | **39 µs** |
| 效果層 | 每顆燈一個值（9,600 個） | **1.14 ms** |
| 配對層 | 清畫布 | 2.0 ms |
| 配對層 | 寫入 9,600 個位置（500 個組合都一樣） | 3.2 ms |
| 配對層 | 交去硬件層 | 4.5 ms |
| **效果＋配對** | | **9.7–10.8 ms（20 ms 的 48–54%）** |
| 硬件層（已驗證） | 16 條線 × 600 顆，每條 18 ms（≈55 FPS） | 同 CPU 重疊 |

### 7.180 2026-11-22（固定種子重測：完全隨機 vs 500 組；8 位 vs 12 位）

用家要求重測清楚（之前數字有出入，可能是隨機或測試問題）。做法：
`0xF077` 用**固定種子**排列（重跑會得到同一組落點），每組獨立跑 **3 次 × 每次 5 幀**，
報 min/median/max。三次差異 **< 0.3%**，所以之前的出入是「配置／狀態不同」，
不是隨機噪聲。

| 排列 | 段數 | 建立（換場一次） | 8 位畫布 | 12 位畫布 |
|---|---:|---:|---:|---:|
| **完全隨機**（每顆獨立落點） | 9,598 | **137.0 ms** | **12.28 ms** | **14.80 ms** |
| 500 組（每組兩段） | 1,000 | 40.0 ms | 4.06 ms | 5.85 ms |
| 500 組（每組一段） | 500 | 39.0 ms | 3.73 ms | 5.59 ms |

（9,600 像素、值流 64 個；min/median/max 差距 ≤ 0.3%）

**加上 production 的另外兩段（清畫布 2.0 ms、交去硬件層 4.5 ms）後的配對層總數**：

| 情況 | 清＋寫＋交 | 20 ms 預算 |
|---|---:|---:|
| 500 組、8 位 | 2.0 + 3.7 + 4.5 = **10.2 ms** | 51% |
| 完全隨機、8 位 | 2.0 + 12.3 + 4.5 = **18.8 ms** | **94%** |
| 完全隨機、12 位 | 4.0 + 14.8 + 3.4（轉 8 位）+ 4.5 ≈ **26.7 ms** | **>100%（不可行）** |

**判讀**：

1. 完全隨機不是「量不到」，是**每顆都要換一次段**（9,598 段 × 1 顆），
   寫入位址跳；8 位仍可（94%），12 位隨機已經超標。
2. 真實 500 組（1,000 段）只需 3.7–4.1 ms；**段數**才是配對層的成本來源，
   不是像素數。
3. 效果層：256 個值 39 µs；每顆一個值（9,600 個）1.14 ms。效果層本身不受排列影響。
4. 「12 位」是指配對層保留 12-bit 畫布（給未來 16-bit 燈）；效果層本身永遠輸出 12-bit。

**可重現**：`python3 tools/layout_bench.py --port <PORT> --all`（variant 0/1/2）。

### 7.181 2026-11-22（「換場一次」＝建立一次；組合通常播 5–10 分鐘）

用家澄清：「完全隨機」是指**一個完全隨機的組合**，但同一個組合通常會維持 5–10 分鐘。
問：之前講「換場一次」是否代表只需做一次、之後不用再做？——**是。**

**建立（組合編譯）何時做**：切 mode（0xF06D）、reload `/pixel`（0xF06F）、
自動播放換下一個組合（registry `maxF` 到）、重開機。同一組合播放期間**不會重建**。

**真機驗證（64 px，同一 mode 連續 20 秒）**：
`pair_n 203,382 → 204,408（+1,026 格，≈51 格／秒）`、`apply avg 26 µs 不變`、
`plan rebuild 4,855 µs（開機一次，之後不變）`。

**把建立成本攤到每格**（完全隨機 9,598 段 = 最壞情況；500 組 = 用家的隨機組合）：

| 情況 | 建立一次 | 攤到每格（5 分鐘 15,000 格） | 每格總計 | 20 ms 佔用 |
|---|---:|---:|---:|---:|
| 完全隨機（每顆獨立落點） | 137 ms | 0.009 ms | 18.8 ms | 94% |
| 500 組（1,000 段） | 40 ms | 0.003 ms | 10.2 ms | 51% |

⇒ 只要組合維持 5–10 分鐘，建立成本可以完全忽略；**每格只有清＋寫＋交**。

**用語更正**：「完全隨機組合」在我們的量測有兩個層次——
（a）500 個組合、組內連續、組間隨機（用家的情況）→ 1,000 段、3.7–4.1 ms；
（b）每顆燈都獨立一段（9,598 段）→ 12.3 ms，是刻意的最壞情況，不是日常。

### 7.182 2026-11-22（組合永久快取；10-bit 抖動計算）

**A. 組合快取（取代「攤分」）**

用家指正：唔可以用攤分邏輯——每個組合時間唔同，做同步時不可控。所以改成**永久儲存**：
同一組合只建立一次，寫成檔案；下次直接讀（時間固定，只取決於檔案大小）。

- 檔案：`/pixel/cache/<key>.pxlt`（**每個組合一個**；key = mode 內容 hash ＋ map 內容 hash
  ＋ px ＋ 格式版本，FNV/CRC 校驗）。
- 內容一改或 px 一改 → key 唔夾 → 自動重建並覆寫（唔會誤用舊組合）。
- 格式自描述：header（magic/ver/key/px/各表長度/crc）＋ 效果（名稱、值流長度、已編譯 program）
  ＋ 條目 ＋ 連續段；讀取時用 asset 名稱指回真實效果。
- 讀取失敗（檔案唔存在／版本／CRC／效果唔存在）→ 重建，唔會靜默用錯。

**實測（9,600 px = 16×600）**：

| 組合 | 無快取（重建） | 有快取（讀取） |
|---|---:|---:|
| 500 組（500 條目、500 段） | **550 ms**（含寫入快取） | **10.2 ms**（10,197 µs） |
| 單組（1 條目、1 段） | — | 3.6–4.1 ms |
| 64 px（1 條目） | — | 4.6 ms |

- 純建立（bench、無檔案寫入、無其他任務競爭）：500 組 39–40 ms。
- 所以「第一次 550 ms」主要不是建立，是 **寫入 FAT 快取**（待加分段量測確認）；
  但用家要的「下次直接讀」已達到：**10 ms，且與組合複雜度無關**。

**B. 10-bit 抖動（4 個子幀）計算**

目標 8-bit → 10-bit＝多 4 倍，用 4 個子幀（時間抖動）；每個子幀仍是 8-bit 畫布（大小不變）。

- 每條線送一個子幀：600 × 24 bit ÷ 800 kHz = **18 ms**；4 個子幀 = **72 ms／視覺幀 → 13.9 fps**。
- 要 50 fps（20 ms／視覺幀）：每子幀 ≤ 5 ms → 每條線 ≤ **166 顆**
  （公式：px ≤ 20 ms × 800 kHz ÷ (24 × 4)）。
- 9,600 像素若每條線 166 顆 → 需要 **約 58 條線**。
- CPU：效果層算一次（39 µs–1.14 ms）；配對層要寫 4 份（500 組：4 × 3.7 ≈ 14.8 ms，
  再加清畫布／交硬件層）。
- 緩衝：每個子幀 38.4 KB（不變）；一次過做 4 份 = 153.6 KB（PSRAM），或逐份重用。
- 環：4 格 = 153.6 KB（目前 3 格 = 115.2 KB）。

**結論**：4 子幀 10-bit、50 fps、每條線 600 顆，三者不可兼得；要揀
（a）降 fps 到 13.9、（b）每條線減到 ≤166 顆（多線）、或（c）改用空間抖動（不加時間、犧牲解像度）。

### 7.183 2026-11-22（完全隨機的代價；4 子幀抖動的逐幀數據）

同一支 `0xF077`、9,600 像素（16×600）、固定種子、每個配置 3 次 × 5 幀（min/median/max）。

**配對層寫入（不含清畫布／交硬件層）**

| 排列 | 段數 | 建立 | 8 位畫布 | 12 位畫布 |
|---|---:|---:|---:|---:|
| 基準：1 條連續鏈 | 1 | 38.2 ms | **2.98 ms** | 5.31 ms |
| 500 組（每組兩段） | 1,000 | 40.0 ms | **4.06 ms** | 5.85 ms |
| 完全隨機 | 9,598 | 137.1 ms | **12.29 ms** | 14.78 ms |

**相對倍數（8 位）**：

| 比較 | 倍數 |
|---|---:|
| 500 組 vs 基準 | **1.36×（+36%）** |
| 完全隨機 vs 500 組 | **3.03×（+203%）** |
| 完全隨機 vs 基準 | **4.13×（+313%）** |

12 位：500 組 1.10×、完全隨機 2.78×（相對基準）。

**加上清畫布（2.0 ms）＋交硬件層（4.5 ms）後的每格總數**：

| 排列 | 8 位 | 20 ms 佔用 | 12 位 | 20 ms 佔用 |
|---|---:|---:|---:|---:|
| 基準 | 9.5 ms | 48% | 15.2 ms | 76% |
| 500 組 | **10.6 ms** | 53% | 16.3 ms | 82% |
| 完全隨機 | **18.8 ms** | **94%** | 26.7 ms | **>100%** |

**4 子幀抖動（10-bit；每子幀仍是 8-bit 畫布）**

同一份 500 組排列，逐個子幀量（每子幀 = 清畫布 ＋ 寫入 ＋ 交硬件層）：

| 子幀 | 時間 |
|---|---:|
| 第 1 | 10.29 ms |
| 第 2 | 10.68 ms |
| 第 3 | 10.64 ms |
| 第 4 | 10.68 ms |
| **合計** | **42.28 ms** |

- **冇下降趨勢**：4 個子幀都是 ~10.3–10.7 ms（第 1 個略快是快取冷熱差），總數就是 4×。
- 「一次過生成 4 幀」不會令平均延遲變好：
  - 批次生成：第 1 幀 10.3 ms、第 4 幀 42.3 ms → 平均 **(10.3+21.3+31.9+42.7)/4 ≈ 26.5 ms**；
  - 邊生成邊送（pipeline）：每個子幀延遲 ≈ 10.6 ms，但送燈每子幀 18 ms，仍然 **72 ms／視覺幀 → 13.9 fps**。
- CPU 每子幀 10.6 ms **細過**送燈 18 ms，所以抖動時 CPU 不是瓶頸，**總線才是**。

### 7.184 2026-11-22（完全隨機值唔值得做永久表？— 結論：唔值得，已自動跳過）

用家問：完全隨機加永久表 vs 舊做法（每次重建），如果冇優勢就唔做。

**逐項數（9,600 像素）**：

| 項目 | 完全隨機 | 500 組 | 1 組 |
|---|---:|---:|---:|
| 建立（bench，無競爭） | **137 ms** | 40 ms | 38 ms |
| 表大小 | **≈231 KB**（prog 0.5 ＋條目 76.8 ＋段 153.6） | ≈12.5 KB | ≈0.6 KB |
| 讀表（實測／推算） | **≈120–190 ms（推算）** | **10.2 ms（實測）** | 3.6–4.1 ms（實測） |
| 結論 | **冇優勢（讀≈建）** | 快 4–55× | 快 10× |

- 讀表推算基礎：0.6 KB → 3.6 ms、12.5 KB → 10.2 ms（固定開銷 ~3.5 ms、之後 ~0.5 ms/KB）。
- 完全隨機的表 231 KB，讀一次 ≈ 120–190 ms，同重建 137 ms 同級；而且存表要多寫 231 KB、
  讀表要多一份 231 KB 暫存（bench 實測時觸發 OOM／重啟）。
- 所以：**完全隨機唔存快取**（用家原則：冇優勢就唔做）。

**已實作**：`nc_px_layout_cache_bytes()`；pair 只在表 ≤ **32 KB** 時存快取，
超過就 log「快取表太大（>32 KB）→ 唔存（讀表同重建差不多）」，下次直接重建。

**保留快取的範圍**：500 組（12.5 KB）→ 讀 10.2 ms（板上第一次 550 ms，含寫入）；
單組（0.6 KB）→ 讀 3.6–4.1 ms。呢兩個情況快取有明確優勢。

### 7.185 2026-11-22（兩條路線的實測數據：開機預建表；配對搬入效果層）

**A. 開機預建組合表（你定義的流程）**

實測（9,600 px、500 組的 mode，刪掉快取後第一次切換）：

| 步驟 | 時間 |
|---|---:|
| 建立組合（build） | **38 ms** |
| 寫入快取（FAT，0.6–12.5 KB 都一樣） | **265 ms** |
| 之後切換（讀快取） | **10 ms**（500 組）／3.6–4.1 ms（單組） |

⇒ 每個「缺少的組合」在第一次要 **≈300 ms**（38 建立 ＋ 265 寫檔）。
開機預建 N 個缺的組合 ≈ **N × 0.3 s**（只有第一次／內容改過才付），之後每次切換 **10 ms**。

| 缺幾個 | 開機多付 |
|---:|---:|
| 1 | 0.3 s |
| 4 | 1.2 s |
| 8 | 2.4 s |
| 16 | 4.9 s |

**B. 配對搬入效果層（配對層只留抖動）**

同一份 500 組排列、9,600 px，實測三種寫法（min of 5）：

| 寫法 | 時間 | 相對現況 |
|---|---:|---:|
| 現況：清畫布 ＋ 寫畫布 ＋ 複製去交貨區 | **7.67 ms** | — |
| 合併：直接寫交貨區（仍然清畫布） | **3.52 ms** | **省 4.15 ms（54%）** |
| 合併：直接寫交貨區（全覆蓋，唔清） | **1.36 ms** | **省 6.31 ms（82%）** |
| 4 個子幀（抖動）直接寫 | **13.8 ms** | 每子幀 3.5 ms |

- 效果層本身算值 39 µs（256 值）／1.14 ms（9,600 值），搬入去幾乎免費。
- 所以「配對搬入效果層」的真正價值是**刪掉『交去硬件層』那次複製（4.15 ms）**，
  如果組合覆蓋全部像素，連清畫布都可以省（再多 2.2 ms）。
- 抖動仍然要 4 個子幀：CPU 13.8 ms（可以），但送燈 4×18 ms = 72 ms → 13.9 fps。

### 7.186 2026-11-22（抖動批次：一次掃表寫 4 份 vs 掃 4 次）

用家觀察：完全隨機時每幀都要處理隨機組合表；抖動要 4 個子幀，理應「一次過」會快一點。
**量測證實：完全隨機時快 48%。**

| 排列 | 段數 | 4 次獨立掃表 | 一次掃表寫 4 份 | 節省 |
|---|---:|---:|---:|---:|
| 500 組 | 500 | 17.54 ms | 16.54 ms | **1.0 ms（5%）** |
| **完全隨機** | 9,598 | **59.65 ms** | **30.70 ms** | **28.95 ms（48%）** |

- 原因：完全隨機的組合表 **153.6 KB**（9,598 段 × 16 B），掃 4 次＝讀 614 KB；
  一次掃表只讀 153.6 KB，其餘是 4 份輸出寫入（4 × 38.4 KB）。
- 500 組的表只 8 KB，重讀成本細，所以批次幾乎冇分別（5%）。
- 就算批次，4 子幀仍然係 30.7 ms CPU（> 20 ms 預算）＋ 72 ms 送燈（13.9 fps）。

**結論**：如果做抖動，**一定要一次掃表同時寫 4 份**（尤其完全隨機）；但抖動的牆仍然是送燈時間。

### 7.187 2026-11-22（抖動的正確配置：150 × 16 = 2,400 px）

用家指出：抖動 4 倍，每條線的長度也要減 4 倍 → **150 顆 × 16 條線 = 2,400 像素**。

**2,400 像素實測（500 組）**：

| 項目 | 時間 |
|---|---:|
| 建立組合 | 17.9 ms |
| 配對寫入（8 位） | **0.84 ms**（1,000 段） |
| 配對寫入（12 位） | 0.88 ms |
| 完全隨機（2,395 段） | 2.69 ms |
| 4 子幀批次（一次掃表） | **3.54 ms** |
| 4 子幀批次（掃 4 次） | 4.14 ms |

**送燈（每條線 150 顆、800 kHz）**：150 × 24 bit ÷ 800 kHz = **4.5 ms／子幀**；
4 個子幀 = **18 ms／視覺幀 → 55.5 fps（50 fps 有餘）**。

**關鍵洞見：總線頻寬一樣**

| 配置 | 每視覺幀送出 | 送燈時間 | 位深 | 解像度 |
|---|---|---:|---:|---:|
| 600 × 16、1 幀 | 9,600 顆 | 18 ms | 8 | 9,600 |
| **150 × 16、4 幀** | 4 × 2,400 = 9,600 顆 | **18 ms** | **10** | **2,400** |
| 600 × 16、4 幀 | 4 × 9,600 = 38,400 顆 | 72 ms | 10 | 9,600 |

⇒ 抖動不是「多花時間」，而是**用解像度換位深**（同樣 18 ms 送 9,600 顆，只是分成 4 幀）。

**2,400 px 的記憶體**：DMA 槽 11,000 B × 3 = 33 KB（internal）；packed 10.8 KB；
環 4 格 = 38.4 KB；畫布 4 份 = 38.4 KB；組合表 8 KB（讀快取 ~5 ms）。

### 7.188 2026-11-22（兩邊都做完全隨機：8-bit 9,600 vs 10-bit 2,400）

測試環境確認後重跑，**兩邊都用完全隨機**（固定種子）：
8-bit → 16 × 600 = 9,600 px（9,598 段）；10-bit → 16 × 150 = 2,400 px（2,395 段）。

| 項目 | 8-bit 9,600 px | 10-bit 2,400 px |
|---|---:|---:|
| 組合建立（隨機） | **137.0 ms** | **22.6 ms** |
| 掃描寫入（單幀） | 12.28 ms | 2.69 ms |
| 直接寫（清＋寫，唔複製） | **14.75 ms** | **2.60 ms** |
| 4 子幀（掃 4 次） | 59.67 ms | 12.23 ms |
| **4 子幀（一次掃表寫 4 份）** | 30.70 ms | **6.30 ms** |
| 送燈（每視覺幀） | 18 ms | 18 ms（4 × 4.5） |
| 視覺 fps | 50 | 50（子幀 200 Hz） |

**判讀**：

1. 完全隨機時，**10-bit 2,400 px 的 CPU 反而低**（6.30 ms vs 14.75 ms）：
   雖然要寫 4 份，但組合表只有 38 KB（2,395 段），而 9,600 px 是 153.6 KB（9,598 段）。
   → 隨機懲罰主要來自**表的大小**，不是像素數。
2. 兩邊送燈都是 18 ms／視覺幀（9,600 顆 × 24 bit），所以 50 fps 都成立。
3. 抖動一定要「一次掃表寫 4 份」：2,400 px 由 12.23 → **6.30 ms（省 48%）**；
   9,600 px 由 59.67 → 30.70 ms（同樣省 48%）。
4. 建立成本：137 ms vs 22.6 ms（10-bit 配置細 4 倍，重建都便宜 6 倍）。

### 7.190 2026-11-22（優化：唔清畫布、零拷貝；用詞更正）

用家指出兩點：
1. 全覆蓋時唔需要清畫布；2. 「交給硬件」用詞錯——硬件係直接讀同一個緩衝區（零拷貝）。

**實測（min of 5，融合版：抖動併入配對寫入）**：

| 配置 | 現況（清＋寫＋複製） | 清＋寫 | **只寫（唔清、零拷貝）** |
|---|---:|---:|---:|
| 9,600 px、500 組 | 7.71 ms | 3.52 ms | **1.36 ms** |
| 9,600 px、完全隨機 | 18.79 ms | 14.78 ms | **12.29 ms** |
| 2,400 px、500 組 | 0.77 ms | 0.43 ms | **0.42 ms** |
| 2,400 px、完全隨機 | 3.44 ms | 2.60 ms | **2.62 ms** |

**10-bit（4 子幀、一次掃表）**：

| 配置 | 有清 | **唔清（全覆蓋）** |
|---|---:|---:|
| 9,600 px、500 組 | 16.38 ms | **5.96 ms** |
| 9,600 px、完全隨機 | 30.45 ms | **20.04 ms** |
| 2,400 px、500 組 | 3.48 ms | **0.95 ms** |
| 2,400 px、完全隨機 | 6.21 ms | **4.63 ms** |

**結論**：

1. **清畫布**只在「新一幀冇覆蓋全部像素」時才需要。建立組合時計
   `Σ run.count == px` → 全覆蓋 → 跳過清；否則只清未覆蓋的部分（換場時一次）。
   9,600 px 的清畫布成本 2.2 ms；2,400 px 只 0.1 ms。
2. **零拷貝**：配對層直接寫入硬件讀取的那個緩衝區（環的槽），冇「交」這個步驟。
   刪掉「畫布 → 環」複製，9,600 px 省 **4.15 ms**。
3. 優化後每格：8-bit 9,600（500 組）**1.36 ms**、隨機 **12.29 ms**；
   10-bit 2,400（4 子幀）500 組 **0.95 ms**、隨機 **4.63 ms**。送燈仍然 18 ms。

### 7.193 2026-11-23（16×600 各部件時間總表；加入 I80 波形合成／推送）

用家要求把**硬件推送**與 **I80 波形合成**也納入；圖：`docs/assets/pixel-pairing/parts-16x600.png`。

| 部件 | 時間（9,600 顆） | 出處 |
|---|---:|---|
| 效果層（256 值） | **0.039 ms** | §7.176 |
| 效果層（每顆一值） | **1.14 ms** | §7.186 |
| 抖動層（12→8） | **3.43 ms** | §7.175.4 conv |
| 配對層（500 組、1,000 段） | **4.06 ms** | §7.183 |
| 配對層（完全隨機、9,598 段） | **12.29 ms** | §7.183 |
| 清畫布（現況、將刪） | **2.0 ms** | §7.177 |
| 複製去環（現況、將刪） | **4.5 ms** | §7.177 |
| **I80 波形合成** | **19.44 ms**（pack 8.2 ＋ merge 11.3） | §7.122（0xF062） |
| **推送（CPU 提交 86.8 KB msync）** | **1.16 ms** | §7.123 |
| 物理送燈（16 條線 × 600 顆、800 kHz） | **18 ms**（同 CPU 重疊） | 計算 |
| 真 I80 frame | **37.9–39.6 ms → 33–34 fps** | §7.123 |

**判讀**：

1. mock 管線（只 effect／pair）10.2 ms → 50 fps；但**真 I80 的牆是波形合成 19.4 ms**。
2. 硬件層每格已 20.6 ms（合成 19.4 ＋ 推送 1.2）＝ **20 ms 預算的 103%**，
   加上效果／配對 ≈16 ms → frame 38 ms、33–34 fps（§7.123 同時觸發 Task WDT）。
3. 所以 16×600 要 50 fps，下一步不是優化配對，而是**砍波形合成**：
   pack（8.2 ms，逐 lane 位元組轉置）與 merge（11.3 ms，8 lane 合併）都要重做
   （例如 LUT/byte-shuffle、per-lane group 分開打包、減少合併次數）。

### 7.194 2026-11-23（正弦收斂：引擎只剩一個 `nc_px_engine_sin_lin()`）

用家指示：**唔要再留幾個「伏人」的方法，次次都要重新解釋** —— 直接只留一個（混合正弦）。

**刪除**（`sw/pixel/engine/nc_px_engine_tables.c`）：

| 刪除的函數 | 表 | 之前誰用 |
|---|---|---|
| `nc_px_engine_sin()` | 1024 點 Q15 查表，2 KB | 內建效果（BREATH／WAVE） |
| `nc_px_engine_sin_q12()` | 0（MP 多項式） | DATA program（MATH_NOW／SQUARE／PULSE_WAVE） |

**保留**：`nc_px_engine_sin_lin()`（256 錨點＋線性插值，514 B）＝ **唯一**正弦。

**改咗嘅 call site**

| 位置 | 之前 | 之後 |
|---|---|---|
| `nc_px_engine_program.c`（12-bit 波） | `sin_q12(ph)` | `sin_lin(ph)` |
| `nc_effect_breath.c` | `sin(ph & 1023)`（Q15） | `((int32_t)sin_lin((ph & 1023) << 6) - 2048) << 4` |
| `nc_effect_wave.c`（×2） | 同上 | 同上，再 `<<2`／`<<3`（等價縮放，保持原 Q15 單位） |

**代價（要知）**：DATA program **唔再同 MicroPython 位元級一致**（MP 用多項式）。這是刻意的取捨：
精度 0.72 LSB（sin_lin）vs 3.74 LSB（多項式）。副作用：`pxProgram_test` 的波表指紋／取樣期望值、
以及 `pxEngine_test` 的 golden（WAVE／BREATH 的 sine 變了）都要重影 —— **本機冇 host 編譯器**
（`gcc` 缺 `cc1`、無 MSVC），所以今次未跑、未重影；有編譯器的機器跑一次 `--update` 即可。

**量測（板上，2026-11-23，S3 240 MHz、SuperMini）**

| 項目 | 舊（sin_q12） | 新（sin_lin） |
|---|---:|---:|
| 正弦 ns/call（`0xF063`，50k×3 取最小） | 157 | **142** |
| 正弦 max 誤差（全相位 65536 掃） | 3.74 LSB | **0.72 LSB** |
| program `per_val_ns`（`0xF065`） | 464–468 ns/值 | **437–445 ns/值** |
| 建波表 256 個值（四段波全建） | ≈119 µs | **≈113 µs** |

每幀值流（波表快路徑，`0xF065`、值流在 internal）：**29.4 ns/值**，64／960／9,600 值都一樣 →
64 px 2.0 µs、960 px 28.3 µs、9,600 px 0.282 ms、9,600×4（38,400 值）**1.13 ms**（5.6% of 20 ms）。
值流落 PSRAM 變 138 ns/值（5.30 ms，26.5%）；無波表的 interpreter 21.05 ms（>20 ms）。

**工具／測試同步**

- `0xF063 SINBENCH` 只回 `{"n","smp","lin":{"ns","err"}}`。
- **順手修真 bug**：`on_sinbench` 冇確保 `nc_px_engine_tables_init()`。EffectPerf 映像冇開 pixel 任務時
  表係全 0 → `sin_lin` 永遠回 2048、誤差報 **2048 LSB**（今次上板第一次量就踩到）。已加自行初始化。
- `sin_bench`（host）只剩 ★ `eng_sin_lin`；`pxEngine_test` 的 sine 斷言與精度閘門只留一個；
  `dev_probe.py`、`commands.md`、`wave-performance.md`、`config-layers.md`、`testing.md` 同步。
- 驗證：bench 映像、tests 映像、tests+bench 映像三個建置 rc=0；上板 `0xF063` 回 142 ns／0.72 LSB。

### 7.197 2026-11-24（送燈要分兩部份：CPU 提交 vs WS2812 線路等待）

用家指正：「物理送燈 18 ms」其實係兩件完全唔同嘅事，要分開講。

| 部份 | 時間 | 係咩 | 佔 CPU？ |
|---|---:|---|---|
| **CPU 提交** | **1.16 ms** | 把 86.8 KB packed 交去 esp_lcd（msync） | ✅ 佔 |
| **WS2812 線路等待** | **18 ms** | 600 顆 × 24 bit ÷ 800 kHz（協議硬性） | ❌ 唔佔（CPU 可做效果／配對） |

- 16×600（1 幀）：線路 18 ms；16×150（4 子幀）：每子幀 4.5 ms × 4 ＝ **同樣 18 ms**。
- 50 fps 的硬條件是 **線路 18 ms ≤ 20 ms**（剩 2 ms 餘量），唔是 CPU。
- CPU 每格只需 1.16 ms 提交；其餘時間可以做效果（5.03 ms）、配對（4–12 ms）等。
- 圖已改成三把尺：A 每格 CPU、B 每格線路等待、C 一次性。
  `docs/assets/pixel-pairing/extreme-bars.png`、`extreme-cards.png`。

### 7.198 2026-11-24（外部靈感：I80 波形合成、隨機配對）

**現況確認**：清畫布仍在程式碼（`nc_pair_task.c:225 memset(s_content.canvas…)`），
零拷貝亦未做；兩者都是計劃 Task 1。I80 波形合成 19.44 ms、配對完全隨機 12.29 ms
是兩個最值得投資的熱點。

#### A. I80 波形合成（19.44 ms）——參考 FastLED 的 LCD/I80 時鐘less 驅動

來源：
- [FastLED `channel_driver_lcd_clockless.h`](https://github.com/FastLED/FastLED/blob/2bd00c3299ed2bc7df0b65dd93c255609e8da250/src/platforms/esp/32/drivers/lcd_spi/channel_driver_lcd_clockless.h)
- [FastLED `wave3.h`](https://github.com/FastLED/FastLED/blob/master/src/fl/channels/wave3.h)
- [FastLED lcd_cam implementation notes](https://github.com/fastled/fastled/blob/6e37141d0f70d56e7ab73bb45ad51e859e8d4256/src/platforms/esp/32/drivers/lcd_cam/implementation_notes.md)

1. **Chunked streaming DMA（最大啟發）**：FastLED 用 3 個 DMA 槽的環，每個 chunk 約
   90 個輸入 byte（30 顆 LED／lane），ISR 在 chunk 完成時「轉置下一個 chunk（約 1–2 µs）
   並即刻提交」。即係**打包同送燈重疊**：總時間 ≈ max(送燈, 打包)，而唔係相加。
   我們現在是「整幀打包 19.4 ms → 一次提交 18 ms」＝ 37.4 ms 序列。
   若改成 chunked：16×600 有機會由 33 fps 升到 ~50 fps。
2. **wave3 nibble LUT**：16 個 u16（32 B）把 4 bit 展開成 12 個 tick；再對 2／4／8／16
   lane 做轉置。我們的 merge（8 lane）可以照這個做法，改成 16 lane 一次轉成 u16 字。
3. **bits_per_pixel = 16、1 bit／lane／PCLK**：整個 frame buffer 就是「每個 tick 一個
   16-bit 字」，同我們 packed 的概念一樣，但 FastLED 是**逐 chunk 產生**，不是整幀。

#### B. 隨機配對（12.29 ms）——9,598 段、每段 1 顆

觀察：runs 已按 dst 排序，所以寫入是**順序**的；成本在「每段一次」的固定開銷
（9,598 段 × ~1.2 µs）。

1. **run 表 16 B → 8 B**（dst／src u16、count u16、job u16）：表 153.6 KB → 76.8 KB，
   PSRAM 讀取即減半。
2. **每段預先算好通道起點**（c0／c1／c2）存入 run：省 3 次 `%` 與 2 次間接（job→aprog）。
3. **count == 1 特化**：完全隨機時全部是 1 顆段；用專用迴圈直接寫一個 u32，省掉內層
   for 與邊界檢查。
4. （可選）**chunk 重疊**：配對寫入下一個環槽時，DMA 正在送上一槽（環已支援 3 槽）。

預計：隨機配對 12.29 → **6–8 ms**；合併 zero-copy（省 4.5 ms 複製、2.0 ms 清畫布）後
9,600 px 的配對層可回到「幾 ms」級。

### 7.199 2026-11-24（更正：I80 已經是 async，打包與送燈本來就重疊）

用家指正：DMA 一早已經開，為何會是 37.4 ms 序列？查證後確認**用家正確**：

- `nc_i80_drv_submit()` 用 `esp_lcd_panel_io_tx_color()`：**排進佇列即返回**（`tx_us` 1.16 ms
  只是 cache writeback，不是等 18 ms）。
- 完成由 ISR 回呼 `on_color_done()` 標記（每個槽有單調 order，避免回收後認錯格）。
- 3 個 DMA 槽 + `reclaim()`：上一幀傳輸中，下一幀可以同時 encode。
- 所以 throughput ≈ **max(encode, 線路)**，唔係相加。

**修正後的預期（16×600、現行程式碼）**：

| 項目 | 舊量測（§7.123，async 之前） | 現行預期 |
|---|---:|---:|
| encode | 19.4 ms | 19.4 ms |
| 線路 | 18 ms | 18 ms |
| frame | 37.9–39.6 ms（33 fps） | **≈ max(19.4, 18) ≈ 19.4–21 ms（48–50 fps）** |

⚠️ 這個預期**未在 16-lane 板重測**（Zero 只有 1 條 data pin，起唔到 16 lane）。
下次上 SuperMini/16-lane 板要用現行固件重測 `frame_us`／`enc_us`／`tx_us`。

**對計劃的影響**：Task 6（chunked streaming）由「必需」降為「可選」——
async 已經重疊，chunked 只在「要再削 encode（19.4 ms）或降延遲」時才做。
真正要盯的是：encode 19.4 ms 距離 20 ms 預算只剩 0.6 ms，而且同 core 的 pair 要分開核。

### 7.200 2026-11-24（原型實測：隨機配對 12.28 → 6.22 ms；波形合成可加速 5–8 倍）

**A. 隨機配對原型（0xF077 variant 9）**

在 bench 先建立「平面 8 B run 表」（每顆：dst、c0、c1、c2 都已預先算好），
再跑「讀 8 B、寫 4 B」的最小迴圈：

| 做法 | 時間（9,600 px、完全隨機 9,598 段） |
|---|---:|
| 現在 `nc_px_layout_apply`（16 B run、job→aprog、3 次 %） | **12.28 ms** |
| **平面 8 B＋預算通道起點** | **6.22 ms（−49%）** |
| 平面表建立（一次性，跟組合建立／快取一起做） | 14.8 ms |

→ 證實計劃 Task 7 的方向；混合做法（count>1 保留 16 B、count==1 用平面表）亦可。

**B. 波形合成：FastLED 唔係上限**

| 做法 | 狀態 | 時間／估算 |
|---|---|---:|
| 現在：逐 lane `wave_pack` → `merge8` 逐 byte 移位／OR | 實測 | **19.44 ms**（86.4 KB → 4.5 MB/s，CPU-bound） |
| 8×8 SWAR 轉置（64-bit 三步交換） | 估算 | **3–6 ms**（5–8 倍） |
| FastLED wave3 nibble LUT（16×u16）＋ 16-lane 轉置 | 參考 | 同類做法，LUT 32 B 常駐 cache |
| chunked DMA（3 槽、每 chunk ~30 顆） | 參考 | async 已存在；只降單次 encode 延遲 |
| RMT | **不可行** | ESP32-S3 RMT 冇 DMA；16×600 一定要 LCD_CAM（I80） |

- 參考：[FastLED lcd clockless](https://github.com/FastLED/FastLED/blob/2bd00c3299ed2bc7df0b65dd93c255609e8da250/src/platforms/esp/32/drivers/lcd_spi/channel_driver_lcd_clockless.h)、
  [wave3.h](https://github.com/FastLED/FastLED/blob/master/src/fl/channels/wave3.h)、
  [ESP RMT FAQ（S3 冇 DMA）](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/rmt.html)。
- 圖：`docs/assets/pixel-pairing/approach-now-vs-plan.png`（現在 vs 打算）。

**下一步**：SWAR 轉置要先寫一個獨立原型（bench）對照 `merge8`，量到真實倍數才改 `nc_i80_out.c`。

### 7.201 2026-11-24（USB 實測推翻估算：SWAR 轉置只有 1.22×，不是 5–8×）

用家要求直接用 USB（OTA＋nc4）驗證，唔好講估算。做法：0xF077 variant 10，
在板上建 16 條 lane 的 packed 資料（每條 600 px → 5,400 B），比較：

| 做法 | 實測（3 次，min） | 正確性 |
|---|---:|---|
| 現況 `merge8_store` ＋ `merge8`（兩組 8 lane） | **22.52 ms** | — |
| 8×8 SWAR 轉置（64-bit 三步交換） | **18.51 ms** | **match=1（輸出逐 byte 相同）** |

即 **1.22×**，不是我之前估的 5–8×。原始回覆（USB nc4）：
`merge8=22515us swar=18516us match=1`。

**為什麼估算錯**：瓶頸不是轉置算術，是 **gather**——每個 byte index 要由 16 個
不同 lane 緩衝各讀 1 byte（PSRAM／cache），輸出 86.4 KB；算術只佔少數 cycle。
SWAR 每輪仍要做 16 次 load ＋ 組 u64 的移位，所以只得 22% 改善。

**下一步（改寫計劃 Task 6）**：真正有機會的是「**展開＋轉置一次過**」——
FastLED 的 wave3 正是每 16 個輸入 byte 直接產生 24 個 u16（48 bytes），
唔需要先 `wave_pack` 成 lane-major 再做第二次 merge。要先用原型量到真實倍數，
唔再靠估算。

### 7.202 2026-11-24（隨機配對三步實測：12.29 → 6.23 → 4.53 ms）

用家要求逐步解釋＋繼續測試。**「B」＝ byte（位元組）**：16 B ＝ 16 個位元組，唔係 16 bit。

| 做法 | 每顆資料 | 每顆要做什麼 | 實測 |
|---|---|---|---:|
| 現在 | 每「段」16 B：dst 4B／src 4B／count 4B／job 4B | 查 job → 查 aprog（兩次間接）、每顆 3 次 `%` | **12.29 ms** |
| 平面 8 B | dst 2B／c0 2B／c1 2B／c2 2B | 讀 8 B、寫 4 B；冇間接、冇 `%` | **6.23 ms** |
| **平面 4 B** | dst 2B／src 2B | 讀 4 B、3 次 `%`、寫 4 B | **4.53 ms（−63%）** |
| 平面 8 B 只讀表（唔寫入） | — | 量「讀表＋算值」成本 | 3.95 ms |

- 6.23 ms 的分解：**讀表＋算值 3.95 ms、寫入約 2.3 ms**。
- 4 B 版最快：表 76.8 → 38.4 KB，PSRAM 讀取減半；3 次 `%` 比讀 4 B 便宜。
- 建表成本 14.8 ms（一次性，跟組合建立／快取一起做）。
- 圖：`docs/assets/pixel-pairing/pairing-explained.png`。

### 7.203 2026-11-24（三種表在四種排列下的完整實測）

| 排列 | run 16 B（每段） | 平面 8 B（每顆） | 平面 4 B（每顆） |
|---|---:|---:|---:|
| 1 段（9,600 px） | 2.98 ms | 3.50 ms | **2.28 ms** |
| 500 組（500 段） | 3.73 ms | 3.29 ms | **2.38 ms** |
| 500 組（1,000 段） | 4.10 ms | — | — |
| 完全隨機（9,598 段） | 12.29 ms | 6.23 ms | **4.50 ms** |

**用家的模型檢討**：
1. 「組合內部越多越便宜」——對**表的大小**成立（段數 × 16 B），但對**執行時間**唔完全成立：
   run 版每段有固定開銷（查 job／aprog、3 次 `%`、內層 setup），所以就算 1 段都要 2.98 ms。
2. 「平面 8 B＝每顆都有自己的固定支出」——正確；但佢在「1 段」時反而慢過 run（3.50 vs 2.98），
   因為表由 16 B 變成 76.8 KB。
3. **第三個方案（4 B／顆）**：都係每顆固定支出，但只存 dst＋src，通道起點即時算；
   表 38.4 KB。**四種排列全部最快**（2.28 / 2.38 / 4.50 ms）。

**結論**：4 B 平面表係目前最佳；建表 14.8 ms 一次性、可入快取。

### 7.204 2026-11-24（平面 4 B／顆犧牲了什麼）

4 B 版最快（1 段 2.28、500 組 2.38、隨機 4.50 ms），但係用「通用性」換返嚟：

| 能力 | run 16 B | 平面 4 B |
|---|---|---|
| 每段不同**寫入型別**（rgb／rgbw／單通道／ww） | ✅ 有 `job` 指去 job（含 write） | ❌ 只支援 RGB（或每種型別一張表） |
| 一個 mode **多個效果**（各自值流） | ✅ `job`→`aprog` | ❌ 只有一個 `src`，冇 aprog 編號 |
| 覆蓋範圍／反轉／stride／跨型別 slice | ✅ 建表時展開 | ✅ 一樣（建表時已展開） |
| 效果值 12-bit | ✅ | ✅（表只存索引） |
| 像素上限 | u32（40 億） | **u16（65,535）** |
| 值索引上限 | u32 | **u16（65,535）** |
| 表大小 | 16 B × 段數（1 段 ＝ 16 B） | **4 B × 像素**（9,600 → 38.4 KB） |
| 快取檔 | 12.5 KB → 讀 10 ms | 38.4 KB → 讀約 25–40 ms（推算） |
| 建表 | 40–137 ms | **+14.8 ms**（一次性） |

**取捨**：
1. **寫入型別**：run 版每段可以是 rgb／rgbw／單通道／ww；4 B 版冇 job，只能 RGB。
2. **多效果混合**：run 版可同時引用多個效果（每段一個 aprog）；4 B 版只有一個 src，
   要合併值流（索引可能爆 65,535）或者加 byte。
3. **範圍**：dst／src 都係 2 B → 65,535。今日 9,600 px／38,400 值冇問題；
   將來超過就要 4 B 欄位（變 8 B／顆）。
4. **記憶體與快取**：表按像素增长（38.4 KB），run 表按段（1 段 16 B）。
   快取檔變大、讀取變慢；或者選擇唔快取、每次重建 14.8 ms。

**建議（保留能力）**：run 表仍然是「編譯後的正本」；載入時如果平均段長 < 門檻（例如 < 3 顆），
才展開成 4 B 平面表（+14.8 ms，可選快取）。rgbw／多效果維持 run 路徑。
如果想兩者兼得，可以試 **6 B／顆**（dst 2、src 2、aprog 1、write 1）——待原型。

### 7.205 2026-11-24（三表對比；用家指正：寫入型別重要、多效果要講清楚）

**用家指正**：
1. 「每段不同寫入型別（rgb／rgbw／單通道／ww）」是重要特性，不能丟。
2. 「一個 mode 多個效果」要講清楚：正常效果是**輪流播放**，不應該重疊；
   一個 mode 的 `map[]` 每條 = group ＋ effect ＋ write。

**釐清後的結論**：4 B 平面表可以**保留**這兩樣——只要「**每個 entry 一張表**」，
表頭帶 `write` 同 `aprog`，值流各自。所以：

| 能力 | 16 B run | 8 B／顆 | 4 B／顆 |
|---|---|---|---|
| 每段不同寫入型別 | 得（job→write） | 要分表（每表一型別） | 要分表（每表一型別） |
| 一個 mode 多個效果 | 得（job→aprog） | 要分表（每 effect 一張） | 要分表（每 effect 一張） |
| 覆蓋／反轉／stride／slice | 得 | 得 | 得 |
| 12-bit 值 | 得 | 得 | 得 |
| 像素／值索引上限 | u32 | u16（65,535） | u16（65,535） |
| 表大小（9,600 px） | 16 B × 段數 | 76.8 KB | **38.4 KB** |
| 快取讀取（推算） | 12.5 KB／10 ms | 76.8 KB／50–80 ms | 38.4 KB／25–40 ms |
| 建表 | 40–137 ms | +14.8 ms | +14.8 ms |

**實測速度**：1 段 → 2.98／3.50／**2.28**；500 組 → 3.73／3.29／**2.38**；
完全隨機 → 12.29／6.23／**4.50** ms。

**真正犧牲**（不是寫入型別／多效果）：
1. **範圍**：u16 → 65,535（今日 9,600／38,400 冇問題）。
2. **記憶體**：4 B × 像素（38.4 KB），run 表只 16 B × 段數。
3. **快取**：檔由 12.5 KB 變 38.4 KB，讀取變慢（推算 25–40 ms）。
4. **建表**：+14.8 ms（一次性）。
5. **8 B 版被 4 B 支配**：更大、冇多能力、1 段時更慢。

### 7.206 2026-11-24（重疊行為實測；分表成本實測）

**A. 群組重疊（A=0:4、B=2:5，重疊 px2、px3）**

host 測試（pxLayout_test [9]）：A 全部 0x100（>>4 = 16）、B 全部 0x200（>>4 = 32）：

| 像素 | 結果 |
|---|---|
| px0、px1（只有 A） | 16 |
| **px2、px3（A＋B）** | **32** |
| px4（只有 B） | 32 |

→ 現行行為是**後面的 entry 贏**（不是相加；相加會是 48）。所以用家想要的「最新／最後套用」**已經是現況**，
唔需要改；只需寫入文件（同一通道：後者覆蓋；不同通道：共存）。

**B. 分表成本（兩個 entry 各自一張 4 B 表）**

| 做法 | 實測（9,600 px、完全隨機） |
|---|---:|
| 單表 4 B（1 entry） | **4.50 ms** |
| 兩張表 4 B（2 entry，切半） | **6.23 ms（+1.7 ms）** |

→ 分表**唔係免費**：原型量到 +38%。原因未確定（兩個緊迴圈、同樣總寫入量），
所以「每個 entry 一張表」暫時唔可以當零成本；要再改原型（例如一次掃兩張表、共用一個迴圈）
先可以判斷係實質成本定係原型寫法造成。

**C. 8 B vs 4 B 的能力**：完全一樣（都係每顆一筆、都要分表）；4 B 只係細一半、快啲。
所以 8 B 被 4 B 支配（除非將來想要「免計算」而犧牲記憶體）。

### 7.207 2026-11-24（釐清 c0/c1/c2、寫入型別、多效果在 4 B 表的位置）

**c0/c1/c2 係咩**：現行 `nc_px_layout_apply` 只用一個 `src`，再按 write 型別算通道索引：

| write | 通道值索引 |
|---|---|
| rgb | `3×src`、`+1`、`+2`（每顆 3 個值） |
| rgbw | `4×src`、`+1`、`+2`、`+3`（每顆 4 個值） |

所以「9,600 顆 × 4 通道各自有值」**係效果層的值流**，配對層只需要「呢顆用第幾個 src」。
4 B（dst＋src）**保留這個特性**：stride 由表頭的 write 決定，src 逐顆前進。

**4 B 做不到的**：每顆的四個通道用**任意、唔連續**的值索引（例如 R 用 5、G 用 900）。
但注意：**現行 run 表都做唔到**（run 只有一個 src），所以 4 B 冇失去現有能力；
8 B（c0/c1/c2）可以做到，但我們建表時都係連續的，冇用到。

**寫入型別／多效果去邊**：搬去**表頭**（每個 entry 一張表）：
`{ write, aprog, nv, base }` ＋ 每顆 `{ dst 2B, src 2B }`。
所以 per-pixel 的 `job`／`aprog` 可以移除（用家講的「把記這個位的地方移除」＝對）。

**如果將來要「同一個 entry 內混合寫入型別」**：要加 1 B（5 B／顆），或者把群組拆開。

**未解決**：分表量到 +1.7 ms（4.50 → 6.23），要先查清楚係實質成本定係原型寫法。

### 7.208 2026-11-24（用測試決定：4 B 單表 vs 6 B 單表 vs 分表）

| 排列 | 4 B 單表（1 entry） | 4 B 兩表（2 entry） | 6 B 單表（write+aprog） |
|---|---:|---:|---:|
| 1 段（9,600 px） | **2.28 ms** | — | 2.98 ms |
| 500 組（500 段） | **2.38 ms** | — | 2.86 ms |
| 完全隨機（9,598 段） | **4.50 ms** | **6.23 ms** | **5.34 ms** |

**結論**：
1. **單 entry**（一個效果、一種 write）→ **4 B 表最快**（4.50 ms），write 放表頭。
2. **多 entry**（多效果／多 write）→ **6 B 單表**（5.34 ms）**贏過分表**（6.23 ms），
   而且 write／aprog 直接放每顆，冇分表切換。
3. 所以按 mode 決定：`nentry == 1` 用 4 B；`nentry > 1` 用 6 B。
4. 兩者都快過 run 表（run：2.98／3.73／12.29 ms，隨機最差）。

**記賬**：6 B 比 4 B 慢 0.5–0.85 ms（多讀 2 B／顆），但換來「一張表處理混合 write／多效果」。

### 7.209 2026-11-24（定案：統一用 6 B 表；4 B 只留文件做隱藏優化）

用家決定：**唔做複雜分層，統一用 6 B**（兼容性要緊；只差零點幾 ms）。
4 B 的測試只寫入文件，將來真的需要再查。

**「兩種情況」其實係咁講**（之前講得太複雜）：

| 情況 | 意思 | 之前建議 |
|---|---|---|
| 一個 mode 只有 **一個效果** | mode 的 `map[]` 只有一條 entry | 4 B 表（快 0.5–0.85 ms） |
| 一個 mode 有 **多個效果** | `map[]` 有多條 entry（不同 group／write） | 6 B 表 |

**定案**：無論幾多個效果，**一律用 6 B 單表**：
`{ dst 2B, src 2B, write 1B, aprog 1B }` ＋ 一次掃完。
- 實測（完全隨機）：**5.34 ms**（4 B 是 4.50、分表 6.23、run 12.29）。
- 兼容性：write／aprog 逐顆都有，唔需要分表。
- ⚠️ 實作注意：5.34 ms 係「全部 RGB」的量測；真正有混合 write 時，
  應該**按 write 分組**（entry 排序或預先分桶），避免每顆一個 switch。

**4 B 隱藏方法（文件備查，唔實作）**：單 entry 時 4 B 可省 0.5–0.85 ms；
若將來要追極限，查本節與 §7.208。

### 7.210 2026-11-24（為什麼 entry 上限是 8？來源與現況）

`NC_PX_MODE_ENTRY_MAX 8` 的來源：

- 它驅動 `NC_PX_JOB_MAX` → `nc_px_plan_t.job[8]`，每個 `nc_px_job_t` = **552 B**
  （含已編譯 program），所以 `nc_px_plan_t` = **4,424 B**（8 × 552 ＋ 表頭）。
- 這是**舊的 show 路徑**（`nc_px_show_plan()`）用的固定陣列；現時 grep 顯示
  **production 冇呼叫**，只有 host 測試（pxMap_bench／pxShow_test／pxLayout_test）用。

**production 實際用的 layout 路徑**（`nc_px_layout_build`）冇 8 這個限制：
entry 係動態 pool，真正的上限是：

| 項目 | 上限 | 原因 |
|---|---:|---|
| 一個 mode 的 distinct 效果（aprog） | **32** | `NC_PX_LAYOUT_ASSET_MAX`；每個 aprog 536 B（32 × 536 ≈ 17 KB） |
| 整棵樹的效果（assets） | **32** | `NC_PX_ASSET_MAX` |
| group 數 | 65,535 | `NC_PX_GROUP_MAX`（u16） |
| 像素 | 65,535 | 6 B 表的 dst 是 u16 |
| entry 數（同一效果可重複） | 動態 | 由 pool 容量／記憶體決定 |

**結論**：8 係舊路徑的殘留，唔係 production 限制；要更多效果，先要提
`NC_PX_ASSET_MAX`／`NC_PX_LAYOUT_ASSET_MAX`（aprog 536 B／個，32 個 ≈ 17 KB）。
舊 `nc_px_show_plan()` 如果冇用，可以刪或把上限改成動態。

### 7.211 2026-11-24（assets 與 aprog 是什麼；為什麼有 32／8）

**兩個名詞**：

| 名詞 | 意思 | 大小 | 現在上限 |
|---|---|---|---:|
| **assets** | `effects/effects.json` 裡的效果定義（program、pixel_n、step…）——整棵樹共用 | 262 B／個 | **32**（`item[32]`，`n` 是 uint8） |
| **aprog** | 某個 mode 引用某個效果時，**編譯出來的可執行副本**（536 B）；同一效果在同一 mode 只編一次，多條 entry 共用 | 536 B／個 | **32**（`NC_PX_LAYOUT_ASSET_MAX`） |

- 32 個 assets ≈ **8.4 KB**；32 個 aprog ≈ **17.2 KB**。
- 舊 `NC_PX_JOB_MAX 8`（plan 的 8 × 552 B = 4.4 KB）production 冇用，只有 host 測試。

**用家的原則：唔要魔法數字，用足欄位闊度**。照這個原則改：

| 項目 | 改成 | 新上限 |
|---|---|---|
| assets | 掃描 `effects.json` 數量後 lalloc | `n` uint16 → 65,535；實際由記憶體決定（262 B × N；255 個 ≈ 67 KB） |
| aprog | build 先數 distinct 效果，再 lalloc | `job.aprog` u16 → 65,535；記憶體 536 B × N（255 個 ≈ 137 KB） |
| 6 B 表 aprog | 1 B 欄位 | **255**（欄位闊度，不是魔法數字） |
| 舊 plan | 刪除或改動態 | 不再有 8 |

**耗盡時**：要明確 log（記憶體不足／欄位上限），唔好靜靜跳過。

### 7.212 2026-11-24（動態配置：寫幾多個就開幾多個）

用家要求：唔要固定 32，**寫幾個就開幾個**。查完現況：

| 位置 | 現況 | 大小 | 改成 |
|---|---|---:|---|
| `nc_px_asset.c` 的 `static nc_px_asset_tbl_t s_tbl` | 固定 32 | **8,386 B BSS** | 掃 count → lalloc（262 B × N） |
| pair 的 `s_content.assets` | 固定 32 | **8,386 B PSRAM** | 依 mode 的 distinct 效果數（最多 nentry × 262 B） |
| `layout_build` 的 aprog | 固定 32 | **17,152 B PSRAM** | 先數 distinct 效果 → 精確 lalloc（536 B × N） |
| 舊 `nc_px_plan_t` | 固定 8 | 4,424 B | 刪除（production 冇用） |

**做法**：`nc_px_asset_tbl_t` 改成 flexible array：

```c
typedef struct {
    uint16_t n;
    nc_px_asset_t item[];      // 實際數量由載入時決定
} nc_px_asset_tbl_t;

// 載入：第一次掃數，第二次 lalloc(n * sizeof(item)) 再填
```

**省幾多（常見情況）**：
- 整棵樹只有 2 個效果：8,386 → **524 B**（省 7.8 KB BSS）
- 一個 mode 用 3 個效果：8,386 → **786 B**（省 7.6 KB PSRAM）
- aprog 3 個：17,152 → **1,608 B**（省 15.5 KB PSRAM）

**上限**：`n` 用 uint16 → 65,535；實際由記憶體回答（lalloc 失敗 → 明確 log）。
**注意**：改成 flexible array 會改 ABI（`sizeof(nc_px_asset_tbl_t)` 由 8,386 變 2），
所有 `lalloc(sizeof(nc_px_asset_tbl_t))` 要改成 `sizeof + n * sizeof(item)`。

### 7.213 2026-11-24（夜間執行：零拷貝、開機預建、aprog 動態、抖動路徑）

依計劃逐個 Task 做，詳見 `docs/00_pm/night-report-2026-11-24.md`。

**完成並驗證**：
1. **零拷貝**：`ring_begin → apply → commit`，無 canvas、無 memset、無複製；
   64 px 上板 `clear=0`、`apply 31 µs`、`commit 7 µs`、**49.9 fps**；0xF06B 預覽正常。
2. **開機預建組合表**：首次缺 1 個 → `built=1, 269 ms`；第二次 → `built=0, 7 ms`；
   結果經 0xF079 的 `prebuild` 讀回。
3. **aprog 動態**：先數 distinct 效果再配置（移除固定 32）；suite 只剩兩個既有失敗。
4. **抖動**：`apply4`（多輸出）＋ `apply_dither`（逐子幀）；host 測試 [11] 過；
   2400 px × 4 子幀上板：`apply 9.06 ms`／視覺幀。
5. **Kconfig**：`SUBFRAMES`／`RING_SLOTS`／`MOCK_ONLY`；新 project Show2400 可建置。

**未完成**：mock-only lane 未生效（render 仍走 i80 → fps 13.9 係線路限制）；
assets 動態（仍 32）；6 B 單表；render 子幀接線完整驗證；chunked DMA（可選）。

**重要**：
- `pxEngine_test`／`pxProgram_test` 失敗係 **HEAD 既有**（`07cbc0c`），
  之前被舊 binary 遮住；`run_all.sh` 已改成每次強制重建。
- 裝置 FAT 寫入被鎖（`err_busy`），OTA 正常；要**斷電重插**，
  之後重新上傳 Zero-Show 的 `/pixel` 內容（目前板上係 2400 測試集）。

**補測（mock lane 生效後，2,400 px × 4 子幀）**：render **199.9 fps**（200 Hz 子幀、
視覺 50 fps）、apply **6.03 ms**／視覺幀、commit 12 µs、CPU ≈6.3 ms（31.5%）。
之前「mock lane 未生效」係 OTA 失敗、板跑舊映像，非程式問題。
