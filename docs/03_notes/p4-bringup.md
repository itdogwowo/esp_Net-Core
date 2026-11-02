# ESP32-P4 上板紀錄

> 板子：ESP32-P4（rev **v1.3**、ROM `esp32p4-eco2`、32 MB flash、MAC `80:f1:b2:d1:f5:f9`）
> 結論先講：**IDF 6.0.3 可以**。之前「6.0.3 開不起來」是誤判 —— 真正的元兇是 console 設成 USB-Serial/JTAG。

---

## 1. 版本：6.0.3 可用（關鍵在 console）

| IDF | console | 結果 |
|---|---|---|
| 6.0.3 | USB-Serial/JTAG | ❌ ROM `entry` 後完全沉默 |
| 6.0.3 | **UART0** | ✅ **採用** |
| 5.5.5 | UART0 | ✅ 也可（MicroPython 同版） |
| 5.4.1 | 預設（UART0） | ✅ 也可 |

三種版本差別在 pre-rev3 選項，但真正讓 bootloader 卡死的是 **USB-Serial/JTAG console**。

pre-rev3 配方出自 `mp_Make-Tools/README.md`（MicroPython PR #19564）：

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_0=y      # 是 0，不是 100
```

## 2. Console 必須是 UART0

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 在 5.5+/6.0 上會讓 **bootloader 卡死**（ROM 之後完全沒輸出）。
改用 `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` 後一切正常（log 仍可從 USB-Serial-JTAG 埠讀到）。

## 3. PSRAM 先關

root 的 `sdkconfig.defaults` 會替 S3 開 `CONFIG_SPIRAM`；P4 這塊未確認有無。
`SPIRAM_IGNORE_NOTFOUND` 沒開時，沒有 PSRAM 會讓 bootloader 卡在 `SPIRAM_BOOT_INIT`。
P4 port 明確 `# CONFIG_SPIRAM is not set`。

## 4. 乙太網（RMII + IP101）

設定取自 `ports/P4/ESP32-P4-ETH/config.json`：phy_addr=1、mdc=31、mdio=52、ref_clk=50（外部時鐘）。

IDF 6 移除了 `esp_eth_phy_new_ip101()`，只剩 generic 802.3 PHY；5.5.5 兩者都有。
本專案用 `esp_eth_phy_new_generic()`，IP101 走 802.3 自動協商可正常 link（100 Mbps full）。

測試用固定 IP：`10.10.1.50/16`（DHCP 在那個網段拿不到位址）。

## 5. 吞吐（純 TCP，不含 NC4）

| 設定 | RX 吞吐 |
|---|---|
| lwIP 預設（TCP_WND 5744） | **1.10 MB/s** |
| `TCP_WND_DEFAULT=65535`、`TCP_SND_BUF=65535`、`MSS=1460`、收件匣 64 | **7.68 / 7.77 / 8.10 MB/s** |
| `TCP_WND=131070` | ❌ SYN 完全沒回應（memp/TCP segment pool 撐不住） |
| `WND/SND=98304` + `MEMP_NUM_TCP_SEG=64` + TX buf 32 | ❌ 掉到 0.84 MB/s（反而更差） |

### 最終數字（多次重複）

| 方向 | 吞吐 |
|---|---|
| 下載 PC→ESP | **7.32 – 8.08 MB/s** |
| 上傳 ESP→PC | **6.88 – 7.24 MB/s** |
| 雙通道合計 | ≈ 單通道（EMAC 受限） |

### 調過的參數與結果（全部實測）

| 參數 | 值 | 結果 |
|---|---|---|
| ETH_DMA_BUFFER_SIZE | 512 | ❌ DL 1.92 / UL 1.10 |
| | **1600** | ✅ DL 7.4–8.1 / UL 6.9–7.2 |
| ETH_DMA RX/TX | 20/10 | ❌（配 512） |
| | **16/16** | ✅ |
| | 32/32 | ❌ UL 3.65 |
| LWIP_TCP_WND | 5744（預設） | ❌ 1.10 |
| | **65535** | ✅ |
| | 98304 / 131070 | ❌ 0.84 / SYN 無回應 |
| MSS | 1440 / 1460 | 無明顯差異 |
| SACK_OUT | on | 無明顯差異 |
| FREERTOS_HZ | 100 / 1000 | 無明顯差異 |
| PSRAM | on（ignore notfound）/ off | 無明顯差異 |
| CPU 360 MHz | — | 一直如此 |

### 與 MicroPython 的差距（9.35 MB/s）＝ 網路 RTT，不是程式

量測我們這條路徑的 **TCP 握手 RTT**（比 ping 更能代表資料路徑）：

| 環境 | TCP RTT | 視窗 65 KB 的理論上限 | 實測 | 佔上限 |
|---|---|---|---|---|
| 現在（P4 ↔ Mac） | **7.82 ms** | **8.00 MB/s** | 7.2–8.1 | **90–100%** |
| 舊專案紀錄 | ping 3.5 ms | 18 MB/s | 9.35 | ~52% |

**我們的實作已經貼著「視窗 ÷ RTT」的上限在跑**；舊紀錄的差距來自當時的網路往返延遲只有一半。

放大視窗（98 KB / 128 KB）在本環境會直接崩到 1.0 MB/s —— 已重現兩次，不是 segment pool 的問題（96 段也一樣）。

### 頻率（實測）

| 項目 | 值 | 說明 |
|---|---|---|
| CPU | **360 MHz** | 設 400 → 開機迴圈（`SW_CPU_RESET` 無限重啟），這顆樣品上限就是 360 |
| APB | 90 MHz | CPU/4 |
| XTAL | 40 MHz | — |
| Flash | **40 MHz** | IDF 對 `ESP32P4_REV_MIN_0` 直接關掉 80/120 MHz（`depends on !ESP32P4_REV_MIN_0`） |
| MicroPython 當時 | **360 / 40m，完全相同** | 所以頻率不是差距來源 |

### PSRAM：32 MB @ 200 MHz

`esp_psram_get_size()` 回報 **32 MB**，速度 200 MHz（與 MicroPython 相同）。
開與關的網路吞吐無差異（lwIP 緩衝仍在內部 RAM）：7.32 / 7.25 MB/s。

### NC4 over TCP（port 6000）— 收與解分工後

**架構**：RX task（core 1）只做 `recv` → ring（64 KB，滿了**等**不丟，讓 TCP 反壓）；
decode task（core 0）從 ring 取 → parser → handler。

| 項目 | 數值 |
|---|---|
| 控制幀 RTT | 8–10 ms |
| 1 KB 幀 | **4.8 – 7.1 MB/s**（4.9k – 7.2k 幀/s） |
| 4 KB 幀 | 6.2 – 6.6 MB/s（1.6k 幀/s） |
| 8 KB 幀 | **6.1 – 6.7 MB/s**（785 – 853 幀/s） |
| 同時段純 TCP（對照） | 5.6 – 6.0 MB/s |

**NC4 已經追上（甚至略高於）同時段的純 TCP** —— 協議成本在量測誤差內。

### 為什麼之前卡在 4.7

收與解在同一個 task（core 0）：解析期間不 recv → 接收視窗關閉 → 對端停送。
把接收獨立成 core 1 的 task 之後，4.7 → 6.1–6.7。

### 解碼不是瓶頸

| | 純解碼 | 每幀耗時 |
|---|---|---|
| **P4** | **30.70 MB/s** | 267 µs @ 8 KB |
| S3 | 9.53 MB/s | 861 µs @ 8 KB |

853 幀/s 時每幀 1.17 ms，解碼 0.27 ms（23%）。滿載時 ping 中位 5.5 ms（與閒置相同）→ 核心沒飽和。

### 這輪修掉的四個真 bug

| 症狀 | 原因 | 修法 |
|---|---|---|
| NC4 大量幀卡死、只有 1.6 MB/s | decode 迴圈每輪 `vTaskDelay(1)`，`HZ=100` 時是 **10 ms** | 有資料時 `taskYIELD()`，沒資料才睡 |
| 8 KB 幀全部溢位丟棄 | parser 只有一幀大小，跨讀放不下 | 服務宣告 2×（16410 B） |
| 非阻塞 socket 狂印錯誤 | `EAGAIN` 被當成讀取失敗 | 視為「暫時沒資料」 |
| plan 的 parser 緩衝沒被使用 | `bus_alloc` 一直走 `nc4_parser_create`，忽略 `rx_region` | 有 `rx_region` 就走 `nc4_parser_create_in` |

### 已排除的變因

IDF 版本（6.0.3 = 5.5.5 同速）、ETH DMA 大小與環數、lwIP 視窗/segment pool、MSS、SACK、
tick 頻率、PSRAM 開關、TCP_NODELAY、send chunk 大小（4K / 16K）。

驗算：`5744 B ÷ 6 ms RTT ≈ 0.96 MB/s` —— 與量到的 1.10 MB/s 吻合，證明瓶頸就是視窗而非 CPU。

裝置端另設 DMA：`ETH_DMA_BUFFER_SIZE=1600`、RX/TX buffer 各 16（512 B 對 MTU 1500 太小）。

參考：舊 MicroPython P4 專案 9.35 MB/s；S3 + Wi-Fi 0.83 MB/s。

## 6. 踩到的另一個坑：匯流排讀取失敗會吃滿 CPU

P4 這版還沒啟用 TinyUSB，但 CDC bus 仍被註冊 → `tinyusb_cdcacm_read()` 每次都錯誤，
decode task 變成「錯誤 → log → 立刻再試」的緊迴圈，把 CPU 吃光（同一時間乙太網只有 1.1 MB/s）。

修法（已進 `nc4_bus.c`）：讀取失敗時**退避 1 tick 並跳脫本輪**，log 只印前 3 次。

> 通則：任何 `read()` 都可能失敗，失敗路徑必須有退避，不能假設它會阻塞。

---

## 7. 2026-11-02 接手：0xF003 與 NC4/TCP 斷線恢復

### 7.1 0xF003 在 P4 回 read=0 —— 是初始化順序，不是核心被佔住

第一輪交棒時的猜測是「量測 task 被 PULL 的 decode 佔住」。**不是。**

實測：`{"iters":20000,"read":0,"us":3000798,"exit":[3,1]}`。
`exit=3` 是新增的結束原因 `HUB_EXIT_NO_START` —— writer 連 `start` 都沒等到就結束了。
原因是 handler 把 3 秒期限寫在**開 task 之後**：

```c
xTaskCreatePinnedToCore(writer, …);   // P4 上這條立刻在 core1 跑
xTaskCreatePinnedToCore(reader, …);
b.deadline_us = now + 3s;             // 太晚：writer 已經看過 deadline=0
```

writer 一啟動就做 `!start && expired()`，而 deadline 還是 0 → 一律判定逾時 → 直接結束。
S3 沒撞上是因為 core1 當時還有別的 task 擋著，不是程式正確。

修法：deadline 移到開 task 之前；task 結束原因寫進 `exit`。
`0xF003` 加 `mode=1`（`tools/bench/hub_bench.py --late`）**故意用舊順序**。
它重現的是 race：PULL/idle 下 4/4 次都是 `read=0 / exit=[3,1]`；
PUSH 下 core1 有 RX task 插隊，writer 反而贏了競爭（`read=20000`）。
所以它是舉證工具，不是必然失敗的斷言。

### 7.2 修好後的交換成本

| 平台 | 20,000 次 | 每次 | torn |
|---|---|---|---|
| S3（HZ=100） | 145.2 ms / 137,774 /s | 7.3 µs | 0 |
| **P4（360 MHz，HZ=1000，PULL/idle）** | **39.9 ms / 501,505 /s** | **1.99 µs** | 0 |

200,000 次也線性（388.9 ms / 514k /s）。**P4 比 S3 快 3.6 倍。**

⚠️ 同一個指令在 PUSH 收斂模式（RX task prio 10 每 1 ms 醒一次）下只有 60k–210k /s（6–16 µs）而且不穩：
量測 task 是 priority 1，會被 RX task 插隊（`w_full` / `r_null` 從 ~7k 暴增到 40k–128k）。
**報數字要註明系統負載。**

### 7.3 NC4/TCP：RST 之後整條 channel 死掉（PULL 也中）

症狀：`nc4_tcp.py` 的 `sendall` 在 5 s 逾時 → 客戶端被殺 → 之後**連續三條新連線都沒有回覆**。

一次是三個 bug 疊在一起：

| # | bug | 修法 |
|---|---|---|
| 1 | PULL 的 `tcp_nc4_read` 對 `ECONNRESET` 回 -1 但**不關 socket** → fd 永遠 ≥ 0 → accept task 永遠在等它 | 錯誤一律走 `tcp_nc4_close()` |
| 2 | parser 留著斷線當下的半幀；新連線的 bytes 接在後面，小指令全被吞掉 | `nc4_parser_reset()`；新連線時 `nc4_bus_reset()` |
| 3 | （工具）`sendall(16 MB)` 沿用 Reader 收斂後的 2 s timeout，裝置還在收就逾時 | `nc4_tcp.py` 灌資料前把 timeout 設 60 s |

修好後：RST 中斷 → 重連 → **0.01–0.02 s 內回 STATUS**。

### 7.4 另一個潛伏 bug：`0xF00A` 的設定從來沒有生效過

`load_net_tuning()` 在 `nvs_flash_init()` **之前**執行（NVS 初始化藏在 `node_identity_init()` 裡），
所以 `nvs_open("net", NVS_READONLY)` 必定失敗 → `s_rx_split` 一直是預設的 0。
**交棒文件寫的「NVS net/rx_split 1 = PUSH 分核」在這次修好之前是無效的。**

修法：`nvs_boot_init()` 提前到 `load_net_tuning()` 之前（只初始化一次），
`0xF00A` 也開始檢查 `nvs_set_u8 / nvs_commit` 的回傳，寫失敗會回 `{"err":"nvs write",…}`。

### 7.5 三種收斂模式（2026-11-02 深夜，含 10 ms 輪詢修正後）

| 模式 | 下載 1K | 下載 4K | 下載 8K | 複製次數 |
|---|---|---|---|---|
| PULL（同核） | 4.95 | 4.77 | 4.73 | 2 |
| PUSH-slot（分核，舊） | **7.48** | 7.45 | 7.55 | 2 |
| **PUSH-frame（分核，零複製）** | 5.65 | **6.98** | **7.12–7.53** | **0** |

- 對照：純 TCP 單向下載 6.8–7.4、**全雙工 12.1–12.4**（= 線速 97–99%）。
- 單向 ~7.4 是「視窗 64 KB ÷ RTT 8.7 ms」，不是 CPU、也不是 PHY。
- 1 KB 時零複製略慢（每幀固定成本），4 KB 以上打平或更好。
- 上半場（10 ms 輪詢還在）所有 NC4 下載都卡在 6.0–6.5 —— 那不是協議成本，是輪詢。

完整表與讀法見 [testing.md §2.13](../02_guides/testing.md)。

### 7.6 板子現在的狀態

| 項目 | 值 |
|---|---|
| 韌體 | 本輪建置（IDF 6.0.3，`0x8d1c0` B ≈ 565 KB） |
| NVS `net/rx_split` | **1 = PUSH**（分核） |
| NVS `net/rx_core` | 1（RX task 在 core1，decode 在 core0） |
| NVS `net/frame_align` | **1 = 零複製**（一格一整幀） |
| 程式預設 | `s_rx_split=false`、`s_frame_align=true`（沒寫過 NVS 的新板子 = PULL） |

切換（都要 `0x100F` 重開機）：

| 想要的模式 | `0xF00A` payload |
|---|---|
| PULL（同核，最保守） | `0,0,1` |
| PUSH-slot（分核，兩次複製） | `1,1,0` |
| PUSH-frame（分核，零複製，預設） | `1,1,1` |

### 7.7 還沒做

- PUSH 模式下的「插槽滿」計數器（`w_full` 等）還沒進 bus stats；目前是靠吞吐與零丟幀間接確認。
- PUSH 只用 8 KB 幀測過 3 輪 × 16 MB；更長的 soak（> 10 分鐘）還沒跑。
- ~~應用層那次 CPU 複製（`recv → parser`）~~ → 已做（§7.9）。
- S3：這輪發現 `main.c` 的 TCP bus 區塊沒包 `#if CONFIG_IDF_TARGET_ESP32P4`，
  **S3 target 其實一直編不過**（只是沒人用 S3 建置過）。補上之後 `tools/board.sh build s3`
  可以編出 355 KB image，但**沒有燒回 S3 板子上驗證**（板上跑的是舊韌體）。

### 7.8 上傳（打包＋送出）也量了 —— 分核沒有好處

新增 `0xF007 TX_BENCH`：裝置打包 N 幀、全部送出，PC 只收。`mode 0` 同核、`mode 1` 分核
（core1 打包 → 插槽 → core0 送出）。工具 `tools/bench/nc4_tx_bench.py`。

| 方向 | 核心 | 1 KB | 4 KB | 8 KB |
|---|---|---|---|---|
| 下載＋解析 | 同核（PULL） | 4.64 | 4.77 | 4.79–4.90 |
| 下載＋解析 | 分核（PUSH） | 6.32 | 6.39 | 6.04–6.52 |
| **上傳＋打包** | 同核 | 6.09–6.57 | 7.26–7.66 | 6.85–7.39 |
| **上傳＋打包** | 分核 | 5.18–5.45 | 7.15–7.75 | 6.72–7.60 |

- 上傳的分核**沒有好處**：`send` 有 64 KB 緩衝，打包 29 MB/s 遠快於線路 7 MB/s。
- 1 KB 分核反而**慢 13%**：每幀過 hub 的交接成本大於重疊的好處。
- 對照：純 TCP 上傳 6.8–7.1、雙通道上傳 8.15（單次）、**全雙工同時 6.31+5.87 = 12.19 MB/s（單次）
  （= 100 Mbps 線速的 97%）**。→ 單向 7.4 不是 PHY 上限，是「視窗 ÷ RTT」。
- 純 CPU：解析 30.67 MB/s、打包 28.8–29.0 MB/s（每幀 8 KB 約 267/284 µs）——
  **成本幾乎全是 CRC32**。

> 提醒：本專案**沒有加密**。`pack/parse` 只是框架 + CRC32（完整性），
> 別把「CRC」講成「加密」——兩者的安全性質完全不同。

完整對照表（含條件與解讀）見 [testing.md §2.13](../02_guides/testing.md)。


### 7.9 零複製（frame-aligned）—— 已完成並驗證

PUSH 的資料路徑改成「**一格 = 一整幀**」：RX task 先讀 9 B 表頭算出長度，再把整幀讀進同一格；
decode task 用 `nc4_parser_frame()` 在插槽上原地驗 CRC + dispatch。中間沒有 chunk、沒有 ring。

| 模式（16 MB） | `copied` | 解析總時間 | 每幀 | 吞吐 |
|---|---|---|---|---|
| `frame_align=0`（舊：slot→chunk→ring） | 33,558,530 B | 1037.7 ms | 495.3 µs | 7.18–7.87 MB/s |
| **`frame_align=1`（零複製）** | **0 B** | **831.2 ms** | **406–418 µs** | 7.41–7.53 MB/s |

- 每幀少 **16–18%** 的解析時間；16 MB 少做 33.5 MB 的 memcpy。
- **吞吐在誤差內**：這條路徑的瓶頸是「視窗 ÷ RTT」不是 CPU。零複製買到的是 CPU 餘裕、
  延遲（不再有 8 KB memcpy 關中斷）與 1 KB 以下的固定成本（見下）。
- soak：10 輪 × 16.4 MB → 20,020 幀全到、`copied=0`、`resync=0`。
- 邊界：一幀拆 3 段、37 B 垃圾後重同步、假表頭（plen=65535）、8 KB 幀後接指令 —— 全過
  （`tools/bench/nc4_stats.py --edge`）。

> 坑：第一版用 `MSG_PEEK` 看表頭，分段送時第二次 peek 之後看不到新 bytes，
> 2 秒後被判「表頭不完整」而關線。改成「先讀 9 B、不合法滑 1 byte」才對。
> **peek 不能拿來做 framing。**

新增 API：`nc4_header_total()` / `nc4_parser_frame()` / `nc4_bus_frame_begin|end()`；
開關 NVS `net/frame_align`（預設 1）；統計 `0xF008`（`tools/bench/nc4_stats.py`）。

**還沒清的尾巴**：frame 模式下 plan 仍宣告 16,410 B 的 `tcp.rx`（給舊的 ring 用），現在沒人用；
CDC/UART 的 PULL 路徑還是走 ring（`nc4_parser_feed`），零複製只套用在 TCP/PUSH。


### 7.10 真兇：decode task 的 10 ms 輪詢

把 copy 拿掉之後吞吐只從 6.1 走到 6.2，於是回頭量「等待」。`0xF008` 的 RX 診斷顯示：
`hub_full` 每幀 100+ 次、decode 的解析只佔 3% 牆鐘時間 —— 也就是 decode 大部分時間在睡。

原因：decode 迴圈的 `vTaskDelay(10ms)`。它醒來把 8 格吃完、再巡一次發現 hub 剛好空，
就睡 10 ms；生產者正好在這段時間把 8 格填滿。**小幀於是變成「每 10 ms 吃 8 幀」**：

| | 1 KB 幀 | 8 KB 幀 |
|---|---|---|
| 輪詢 | **0.78 MB/s**（785 幀/s） | 6.05 MB/s |
| `nc4_bus_kick()`（producer commit 後 give notification） | **5.65 MB/s** | **7.45 MB/s** |

同一個修法把 `STATUS` RTT 從 **10.1 ms → 3.6 ms** —— 之前量到的「RTT 10 ms」有很大一半
是我們自己的輪詢，不只是網路。

> 教訓：**生產者/消費者之間不要用固定週期輪詢**。代價是「延遲 = 週期」，而且在
> 「剛好空」的那一瞬間會把 burst 全部鎖在週期裡。


### 7.11 NC4 over WebSocket（第一版）

WS 是訊息導向，所以它**不需要插槽**：一則 WS 訊息 = 一個 NC4 幀，直接 `nc4_parser_frame()`
在 httpd 的緩衝上解析（零複製）；回覆走 `httpd_ws_send_frame_async()`。

| 項目 | 值 |
|---|---|
| 檔案 / URI | `firmware/components/sys/transport/nc4_ws.c`，`ws://<ip>:8080/nc4` |
| 同時連線 | 1（第二條在第一則資料幀被拒） |
| 訊息上限 | **24 KB**（一則可塞多個 NC4 幀） |
| RTT 中位 | 6.87 ms（TCP 3.6） |
| 吞吐 1K / 4K / 8K（batch=1） | 1.33 / 3.25 / 4.35 MB/s |
| 吞吐 1K / 4K / 8K（多幀 batch） | **4.30 / 5.72 / 5.26 MB/s**（TCP 5.65 / 7.13 / 7.40） |
| httpd task | core1 / prio 10（預設 prio5 + 不指定核 → 慢 11%） |
| 測試 | `tools/bench/nc4_ws_test.py`（10 項全過：碎片、垃圾、過大、重連、第二條被拒、與 TCP 並存） |

> 每則 WS 訊息固定成本 ≈ 0.7 ms（httpd 對每則做 4–6 次小 `recv`，每次都過 lwIP netconn）。
> 解法是「一則訊息塞多幀」（batching）：1K 從 1.33 → 4.30 MB/s。要高吞吐仍以 TCP 為主；
> WS 的價值是「瀏覽器直接連」。

坑：v6.0.3 的 httpd **不會**用 `method == HTTP_GET` 呼叫 WS handler（只有資料幀會進來），
所以「握手完成」的偵測要改成「第一則資料幀時認領 fd」。


### 7.12 最終通道對比（TCP / UDP / WS）

| 通道 | 下載 1K / 4K / 8K | 上傳 1K / 4K / 8K | RTT |
|---|---|---|---|
| **TCP**（分核＋零複製） | 5.65 / 7.13 / **7.40** | 6.05 / 7.43 / **7.42** | 3.56 ms |
| **WS**（batching） | 4.30 / 5.72 / 5.26 | 4.99 / — / 6.98 | 6.87 ms |
| **UDP**（≤ MTU） | 1.93（掉 16%）/ ✗ / ✗ | **8.51**（掉 0%）/ ✗ / ✗ | **3.73 ms** |

- **NC4 通道上限 ≈ 7.5–8.5 MB/s（單向）**：TCP 卡在「視窗 ÷ RTT」，UDP 卡在 MTU。
  CPU 解析 30.7 MB/s、PHY 合計 11.9–12.4 MB/s，兩者都還有餘裕。
- UDP 的 8.5 MB/s 是「一包一幀、不切片段」才成立；4K/8K 會切 IP 片段，實測掉 33–91%。
- 開 `CONFIG_LWIP_IP_FRAG` / `IP_REASSEMBLY` 反而更糟（重組吃光 pbuf pool）→ 已還原。

完整表與「該用哪一條」見 [testing.md §2.16](../02_guides/testing.md)。


### 7.13 坑：PHY 掉到 10 Mbps half duplex，整條網路死掉

某次重燒後，開機 log 變成：

```
W netcore: ETH link: 10 Mbps, half duplex     ← 之前都是 100 Mbps full
```

ping 不到（`Host is down`）、所有 port 都連不上。原因是 **IP101 的自動協商失敗，
掉到 10M half**，跟 Mac 的 100M full 形成雙工不匹配。

**為什麼重開機救不回來**：這塊板的 PHY **沒有 reset GPIO**（`reset_gpio_num = -1`），
MCU 重開不會重置 PHY，狀態會一直留著（連續 3 次 reset 都是 10/half）。

**修法（已進 `net_eth_start`）**：開機讀 link 速度，不是 100M full 就透過 MDIO
讀寫 BMCR（reg 0）重啟自動協商（`(1<<9)|(1<<12)`、清 power-down），等 3 秒再讀一次；
還是不對就回 `ESP_ERR_INVALID_STATE`，上層因此**不宣告、不啟動** TCP 通道。
實測恢復成功，ping 與 7.27 MB/s 都回來。

> 通則：沒有 reset 腳的 PHY，**開機要驗 link 速率並能主動重啟 autoneg**，
> 不能假設「上次 100M，這次也會 100M」。
