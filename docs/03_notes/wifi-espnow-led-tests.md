# Wi-Fi／ESP-NOW／I80 測試紀錄（2026-11-09 晚）

> **性質：測試，不是定案。** 今天只有「緩衝區管理」算定案；以下全部是量測與可行性。
> 憑證只存在測試命令／裝置，不寫進 repo。

## 1. 測試平台

| 角色 | MAC | Port | 說明 |
|---|---|---|---|
| A | `d8:85:ac:d4:2b:4c` | `D885ACD42B4C1` | 測試版、Wi-Fi 已編入 |
| B | `d8:85:ac:d4:2f:2c` | `D885ACD42F2C1` | 測試版、測試版自動 init ESP-NOW |

兩台都是 ESP32-S3、8 MB PSRAM、16 MB flash、IDF 6.0.3（測試版）。

## 2. ESP-NOW P2P ＋ router：**通過**

| 指標 | A | B |
|---|---:|---:|
| `init/ch/parser` | 1 / 1 / 1 | 1 / 1 / 1 |
| link `ESPNOW` | frames 4 / bytes 62 / drops 0 | —（只回覆） |
| `rx` | **1** | **12** |
| `tx_ok` | 4 | **8** |
| `last_hex` | B 的回覆（cmd `0xF004`，190 B） | A 的命令 |
| `last_rssi` | -20 dBm | -21 dBm |
| bad / drop | 0 / 0 | 0 / 0 |

完整 chain：

```
A CDC → route CDC=["decode0","ESPNOW"] → nc4_link "ESPNOW"
  → esp_now_send(broadcast) → B rx
  → B 的 espnow_rx task → nc4_parser_frame → handler
  → ctx->send = now_link_write → esp_now_send
  → A rx（190 B，cmd 0xF004）
```

**關鍵修正**：原本在 ESP-NOW receive callback 裡直接 parse＋handler＋send，B 收到但 `tx_ok=0`；
改成 callback 只 copy 入 queue、獨立 `espnow_rx` task 解析後，回覆成功。
（在 Wi-Fi callback 內再 `esp_now_send` 不可靠。）

## 3. Wi-Fi AP／STA：部分通過

### AP 模式（A）

```json
{"ap":1,"ssid":"NC-TEST","ch":6,"err":"ESP_OK"}
{"ap":1,"ap_ip":"192.168.4.1"}
```

- A 用 `WIFI_MODE_APSTA`（AP + STA 並存）；ESP-NOW 綁 STA，所以 **AP-only 不行，APSTA 可以**。
  這與舊項目 `now_bus.py` 的實測一致（STA-only 可送，AP-only 回 `ESP_ERR_ESPNOW_IF`）。

### STA 連 A 的 AP（B）

- B `esp_wifi_connect` 回 `ESP_OK`，RSSI 約 **-19 dBm**（很近），
- 但 8 秒後 `ip=0.0.0.0`、`sta=0`：**有射頻關聯，沒有拿到 DHCP**。
- A 的 AP netif 有 `192.168.4.1`；B 的 STA netif 已建立並顯式 `esp_netif_dhcpc_start`，仍未取得 IP。
- 已排除：密碼長度、auth mode、channel（A ch6）、ESP-NOW 佔用（APSTA + STA 可並存）。
- 待查：AP 的 DHCP server 是否真的啟動、B 的 netif 是否 attach 到 STA、事件時序。

### 連本地 本地 AP（STA）

- A 設 `wifi.enable=1` + 本地 AP 憑證，開機 `wifi.state=error`；
- 用執行期命令 `0xF02C` 連 本地 AP，回 `ESP_OK` 後 **A 立即 reset／USB 重新列舉**。
- AP 模式不會 reset，所以不是「Wi-Fi 一定不能開」；比較像是 **STA 連外 AP 的 TX／DHCP 電流尖峰／電源**，或 STA+ESP-NOW 的初始化順序。
- **未通過，未定案。**

### 指令（測試版）

| Cmd | 用途 |
|---|---|
| `0xF02A` | ESP-NOW stats |
| `0xF02C` | `WIFI_STA`：`[ssid_len][ssid][pw_len][pw]`，執行期連 AP（憑證不落檔） |
| `0xF02D` | `WIFI_AP`：`[ssid_len][ssid][pw_len][pw][ch]`，開 AP |
| `0xF02E` | `WIFI_INFO`：STA IP／AP IP／RSSI |

## 4. 舊項目參考（mp_Net-Core）

- `slave/lib/sys/now_bus.py`
  - `MAX_PAYLOAD=250`；`MAX_PEERS=20`、加密 6；廣播固定佔 1 格。
  - **MAC 一定是 6-byte bytes**；hex 字串要 `mac_bytes()` 轉。舊 bug：hex 字串直接餵 `add_peer` 靜默失敗，只有廣播能用。
  - 被動學習 peer 有上限（預設 16），避免雜訊食光 peer 表；明確綁定不受限。
  - **ESP-NOW 綁 STA**：AP-only send 回 `ESP_ERR_ESPNOW_IF`。
- `slave/lib/sys/peer_registry.py`
  - 雙層定址：NC4 `cid`（邏輯）＋射頻 `mac`（實體），兩者都要記。
  - 一個 peer 一筆，key = 6-byte MAC hex；`learn_from_frame` 被動學習；樂觀（登記不代表在線）。
- `todo/05_node_pairing.md`、`todo/04_remote_pairing.md`：配對流程（IDENTIFY_REQ/RSP、master_cid 驗證）。

## 5. I80／50 FPS（設計，未實作）

### 事實

| 項目 | 數值 |
|---|---|
| 你的配置 | **16 lanes × 600 LEDs**、GRBW（4 B/LED 儲存） |
| WS2812（24-bit RGB）位元時間 | 30 µs/LED |
| SK6812（32-bit GRBW）位元時間 | **40 µs/LED** |
| 24-bit 每幀 | 600 × 30 µs = **18.0 ms** → 55.5 FPS |
| **32-bit GRBW 每幀** | 600 × 40 µs = **24.0 ms** → **41.7 FPS** |
| 50 FPS 預算 | 20.0 ms／幀 |

> **重要修正**：18 ms 是 **24-bit RGB（WS2812）** 的線時，不是 GRBW。
> 如果每個 LED 真的送 **GRBW 32 bit**（SK6812），600 顆是 **24 ms**，
> 標準 800 kbit/s 下 **50 FPS 不可能**（上限 41.7 FPS）。
> 要 50 FPS 只有三條路：① 只送 24-bit（W 不用）、② 超頻到 ≥1.0 Mbit/s、
> ③ 每 lane 減到 ≤500 顆 GRBW（500×40 µs=20 ms）。
> **16 lane 是並行，不會縮短每一條 lane 的串列時間。**
| GRBW frame | 16 × 600 × 4 = **38,400 B** |
| 雙緩衝 | 76.8 KB（PSRAM） |
| 三緩衝 | 115.2 KB（PSRAM） |
| S3 I80 DMA | 只能內部 SRAM；每 LED 約 3 B 波形（24 bit）|
| 16 lane 波形 | 600 × 3 × 16 ≈ **28.8 KB**／幀（單緩衝；實際以 IDF link 為準） |

> 舊項目 `mp_LEDdriver/DESIGN.md` 的結論：S3 用 **LCD_CAM I80 16 lane** 是唯一能上多條的路
>（RMT 只有 4 lane）；GRBW 統一 4 B/LED；DMA 在背景吐、CPU 同時算下一幀；
> 50 FPS 是**推導值**，不是設定值。

### 50 FPS 量測結構（要建立）

1. **render core**：一個 task 只做「產生 frame → 交給 I80 DMA」， pinned core。
2. **stress core**：另一個 task 做 PSRAM copy／CPU 迴圈，製造最大負載。
3. **frame counter**：每次 DMA 完成 callback `frames++`；每 1 s 取樣一次。
4. **時間**：`esp_timer_get_time()` 記 `frame_start`、`encode_us`、`submit_us`、`done_us`；
   計 p50／p99／max；50 FPS 要求 **p99 ≤ 20 ms**。
5. **GPIO 標記**：render start→done 拉高／拉低，用邏輯分析儀／示波器看實際週期。
6. **比較**：
   - 舊中斷式 bitstream（blocking；`1/(傳輸+計算)`）
   - 分段 DMA／雙緩衝（`1/max(傳輸, 計算)`）
   - 量 `dropped`（render 來不及）、`blocked_us`（等 DMA slot）。
7. **通過條件**：16×600、GRBW、stress 同時跑，p99 ≤ 20 ms、dropped=0、無 torn frame。

### 現況

- 目前 C 固件**沒有 I80 LED driver**；只有 `nc_fx` 效果測試，沒有 16-lane 輸出。
- `mp_LEDdriver/modled_bus.c` 只到 C0（配置 buffer，`show()` 未接硬體）；`DESIGN.md`/`C_API.md` 是設計母本。
- `fastLED/tools/patch_fastled_i80.py` 是舊 I80 patch 的參考；`mp_lcd_bus/esp32_src/i80_bus.c` 是 I80 bus 参考。
- 要真的測 16×600／50 FPS，需先落一個 **I80 WS2812 編碼器＋雙緩衝**（新工作，不是今晚能完成）。

## 5.1 NeoPixelBus／GRBW／I80 的判斷

你的方向：**高概率用 NeoPixelBus**，每個 LED 一律以 **GRBW 4 bytes** 儲存
（R,G,B,W；W 是 SK6812 的白通道）。舊項目 `mp_LEDdriver/C_API.md` 也是同一結論：
像素緩衝固定 4 B/LED，`order` 只決定**硬體輸出顺序**，不改緩衝格式。

但實體層要分清楚：

| 層 | 用什麼 | 16 lane × 600 可行？ |
|---|---|---|
| 高層 API／顏色模型／GRBW 儲存 | **NeoPixelBus**（`NeoGrbwFeature`、`RgbwColor`） | ✅ 緩衝 38.4 KB/frame |
| S3 實體輸出 | NeoPixelBus 內建 **RMT／I2S** | ⚠️ RMT 只有 4 lane；I2S 亦不是 16 lane 並行 |
| S3 16 lane 實體輸出 | **LCD_CAM I80**（自寫 encoder）或 FastLED yves I80 patch | ✅ 16 lane，30 µs/LED |
| P4 | PARLIO ×2 unit（32 lane） | ✅（P4 另計） |

**所以「用 NeoPixelBus」和「S3 I80 16 lane」不是同一件事**：
NeoPixelBus 可以作為上層顏色／緩衝模型，但 S3 的 16-lane 實體層要用 I80 自寫
（或 FastLED 的 yves I80 patch）。如果硬用 NeoPixelBus 的 RMT，16 lane 不可能。

**建議**：
- 緩衝格式：GRBW 4 B/LED，frame 38,400 B（16×600）。
- 上層：NeoPixelBus 的 `RgbwColor` 語意（或自寫等價 API），方便效果／舊 code 移植。
- 下層：S3 I80 16-lane encoder；雙緩衝；DMA 完成 callback。
- 測試：先 1 lane × 600 驗時序，再 16 lane × 600 驗 50 FPS；用 `0xF01C/0xF01D`
  量 PSRAM 影響，render core 與 stress core 分開。

## 5.2 I80 DMA 實測：CPU 真的釋放（合成波形，0xF030）

測試條件：16-bit I80、28,800 B（16×600×3，24-bit 等價）、內部 DMA 雙緩衝、
測試腳 33–37 避開（N16R8 octal PSRAM），資料腳 1/2/3/8–14/17/18/21/38–40。

| pclk | submit（CPU） | wire（DMA） | 整幀 busy loop | 切片 fill（7,200 B） | 切片 wire |
|---:|---:|---:|---:|---:|---:|
| 2 MHz | **122 µs** | 7,333 µs | 321,614 | **62 µs** | 7,266 µs |
| 4 MHz | 80 µs | 3,691 µs | 160,526 | 64 µs | 3,636 µs |
| 8 MHz | 80 µs | 1,890 µs | 80,559 | 63 µs | 1,863 µs |

- **CPU 只花 70–122 µs submit**；DMA 跑 1.9–7.3 ms；其餘時間 CPU 可以做事（busy loop 32–48 萬次）。
- 切片：fill 下一塊 62–64 µs，遠小於該片 wire 1.8–7.3 ms → **換緩衝完全被 DMA 隱藏**。
- 1 MHz 被 `esp_lcd_new_panel_io_i80` 拒絕（`ESP_ERR_NOT_SUPPORTED`），最低要 2 MHz。

### 另一核高負載（0xF031 STRESS，core1）

| 情境 | submit | wire | fill | busy |
|---|---:|---:|---:|---:|
| baseline | 70 µs | 7,281 µs | 62 µs | 480,373 |
| core1 PSRAM copy 32 KB 迴圈 | **125 µs** | **7,341 µs** | **91 µs** | 483,466 |
| core1 CPU 迴圈 | 70 µs | 7,280 µs | 65 µs | 481,309 |

- PSRAM 壓力令 submit +55 µs、fill +29 µs（仍然 <0.15 ms），**wire 幾乎不變**；
- CPU 壓力完全唔影響（因為 render 核同 stress 核分開）。
- 結論：**I80 DMA + 雙緩衝真的把 CPU 釋放**；50 FPS 的瓶頸是線時／protocol，不是 CPU。

### 重要踩坑：GPIO33–37 是 octal PSRAM

第一次用 33–48 當 16 data pins，一送 I80 就 reset；N16R8 的 octal PSRAM 用了 33–37。
實際 16-lane LED 排線**一定要避開 33–37**（以及 26–32 flash/PSRAM、19/20 USB、
4–7/15/16 SD）。這次測試改用 1/2/3/8–14/17/18/21/38–40。

## 6. PSRAM vs SRAM（已量）

| 區域 | 大小 | memset | read | copy |
|---|---:|---:|---:|---:|
| internal | 32 KB | 483–507 MB/s | 51 MB/s | 240 MB/s |
| PSRAM | 64 KB+ | 29–33 MB/s | 30–32 MB/s | 20–21 MB/s |
| PSRAM | 16 KB（cache-hot） | 212 MB/s | 52 MB/s | 152 MB/s |

- 38.4 KB frame：整幀 PSRAM copy 1.8–1.9 ms；internal copy 0.15–0.3 ms。
- 另一核同時 PSRAM copy：p2p 由 21 → **10 MB/s**（腰斬）。
- 對 render core 的影響：如果 render 每幀要掃 38.4 KB PSRAM 多次，會同 stress core 爭 bus；
  50 FPS 預算 20 ms，2–3 個全幀 pass 就 4–6 ms，仍可，但 p99 會受另一核影響。
- 建議：**render 的中間運算用 internal tile（≤16 KB）**，PSRAM 只放前/後 frame；
  I80 DMA 讀 internal 波形 buffer；PSRAM frame 只在換頁時 copy。

## 7. 緩衝管理器的用法（測試版）

- `nc_pool`：small／bulk／forward（forward 已由 `nc_hub` 取代，config 可關）。
- `nc_hub`：SPSC 嚴格 FIFO、generation、core/violations；用於轉寄、tap。
- 查詢：
  - `STATUS keys=mem` → `mem.pool`、`mem.hub`
  - `STATUS keys=net` → `net.links`、`net.fwd`
  - `0xF01C`／`0xF01D` → PSRAM/internal 頻寬、整幀 vs 分塊
- 設計：[buffer-pool.md](buffer-pool.md)、[comm-buffers.md](comm-buffers.md)。

## 8. 未完成／下一步

| 項目 | 狀態 |
|---|---|
| ESP-NOW P2P + router | ✅ 已驗 |
| AP 模式 | ✅ 已驗（192.168.4.1） |
| STA 連 AP 取 IP | ❌ 有 RSSI、無 DHCP IP |
| STA 連 本地 AP | ❌ A reset（待查電源／初始化） |
| Wi-Fi 上的 NC4（TCP/UDP） | ❌ 未實作 S3 transport |
| I80 16×600 GRBW driver | ❌ 未實作（只有設計） |
| 50 FPS 量測結構 | ❌ 未建立 |
| PSRAM/SRAM 對其他核影響 | 部分量到（競爭腰斬） |
| 舊項目配對邏輯 | 已讀，待移植 |

> 这些全部是測試；不要把 Wi-Fi/IP、I80、50 FPS 當成已定案。
