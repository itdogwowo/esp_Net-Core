# 8×8 WS2812 矩陣：I80 波形 ＋ 分區時間抖動

> 2026-11-11。**性質：測試任務（`CONFIG_NETCORE_RGB_MATRIX`，只在測試映像）**，
> 第一版未上板。
> 相關：[led-controller-plan.md](led-controller-plan.md)（正式 LED Controller 的規劃）、
> [psram-surface.md](psram-surface.md)（I80／DMA 頻寬）、
> [commands.md §2](../01_protocol/commands.md)（`0xF060`）。

---

## 0. 一句話

WS2812 每顆只有 **8-bit PWM**，暗階一格一跳。8×8 板每秒送 450 幀，
我們把「想要的亮度」保留 8.8 定點精度，**每幀用誤差回饋決定送 floor 定 ceil** ——
人眼平均之後等於多出 8 bit 灰階。8 個邏輯區各自用不同速率／相位抖動，
避免整塊板用同一個頻率一齊閃。

---

## 1. 硬體事實（由官方原理圖＋Demo 核對）

| 項目 | 值 | 來源 |
|---|---|---|
| 板子 | Waveshare ESP32-S3-Matrix（SKU 27119） | 產品頁 |
| 燈 | 64 × **WS2812B-0807**，8×8，串成一條鏈 | 原理圖 `sch.pdf` |
| 資料腳 | **GPIO14**（`LED_DIN`），另有 `Dout` 可再串 | 原理圖＋Demo |
| 顏色次序 | **NEO_RGB**、800 kHz | Demo `Adafruit_NeoPixel(64, 14, NEO_RGB + NEO_KHZ800)` |
| IMU | QMI8658，IO10（INT1）／IO11／IO12／IO13（INT2） | 原理圖 |
| flash／PSRAM | 4 MB flash、**無 PSRAM** | 產品頁 |
| console | USB-Serial-JTAG（USB-C） | Demo／board.json |

> ⚠️ 亮度不要開太大：產品頁明寫「亮度过高会导致温度急速上升，从而导致板子损坏」。
> 測試預設 `gain=255` 只用在低階效果（呼吸最大亮度受 gamma 壓低），
> 長時間全白請自己降 `gain`（`0xF060 op=4`）。

### 1.1 為什麼是「14 支 IO」還是「1 支 IO」

板子上真正接燈的**只有 GPIO14 一條線**。但 I80（LCD_CAM 的 8080 並列介面）
最少要 **8 條資料線 + WR + DC** 都是有效 GPIO：`esp_lcd_new_i80_bus()` 見到
任何一條 < 0 就直接回 `ESP_ERR_INVALID_ARG`（S3 沒有 IOMUX 版本可以省）。

所以本任務佔 10 支腳，**只有 D0 有作用**：

| 訊號 | GPIO | 作用 |
|---|---|---|
| D0 | 14 | WS2812 位元流 |
| D1..D7 | 38, 39, 40, 41, 42, 47, 48 | dummy；將來 8-lane 燈板會用到同一條 bus |
| WR | 1 | I80 PCLK 4 MHz（dummy） |
| DC | 2 | 固定低（dummy） |

避開：10–13（IMU）、19/20（USB）、26–32（flash）、43/44（UART0）、
0/3/45/46（strapping），以及 `nc_hw_reserved()` 在 S3 視為 PSRAM 的 33–37。

改腳位：`CONFIG_NETCORE_RGB_MATRIX_I80_PINS`（`ports/S3/ESP32-S3-Matrix/sdkconfig.defaults`）。

---

## 2. 時序：為什麼是 8.889 MHz／8 cycles（2026-11-11 上板實測）

第一版用「最標準」的 4 MHz／5 cycles（bit 1.25 µs、一幀 7,936 B ＝ 1.98 ms），
上板之後量到 **一幀實際要 2.44 ms**（410 fps）—— 唔係 PCLK 錯，而係
**esp_lcd 的 I80 每筆交易之間有固定開銷**：

| 設定 | 一幀 bytes | 理論傳輸 | 全速實測 `dt_us` | 差額 |
|---|---:|---:|---:|---:|
| 4 MHz、5 cycles | 7,936 | 1.98 ms | **2.44 ms** | ~0.46 ms |
| 8 MHz、5 cycles（只測試時序） | 7,936 | 0.99 ms | 1.44 ms | ~0.45 ms |
| **8.889 MHz、8 cycles（現在）** | **12,544** | 1.41 ms | **1.65 ms** | ~0.24 ms |

所以「4 MHz、1.25 µs／bit」在 esp_lcd 這條路上**到不了 450 fps**。改成
80 MHz ÷ 9 = **8.889 MHz**，一個 bit 用 **8 cycles**：

| 碼 | 8 個 byte（bit0 → D0） | 波形 | WS2812B 窗口 |
|---|---|---|---|
| `0` | `01 01 01 00 00 00 00 00` | 高 337 ns | T0H 250–550 ns ✅ |
| `1` | `01 01 01 01 01 01 00 00` | 高 675 ns | T1H 650–950 ns ✅ |

bit 週期 0.900 µs（0.65–1.85 µs ✅）。一幀 = 64 × 24 × 8 = **12,288 B**
＋ 256 B 收尾 ≈ **1.41 ms**；加交易開銷 ≈ **1.65 ms** ⇒ 全速 606 fps，
450 fps 有 ~0.57 ms 餘裕（reset 由「兩幀之間拉低」提供，實測 > 350 µs）。

> 3 格 DMA buffer（`NC_RGB_SLOTS`）是必要的：送第 N 幀時 CPU 已經編好 N+1、N+2，
> 就算編碼要 0.34 ms，bus 都唔會空等。單緩衝 408 fps、兩格 410 fps、三格＋新時序 **450.0 fps**。

`NC_RGB_FPS_MAX = 460`，預設 450；`0xF060 op=5` 可以調，`fps=0` = 全速（量 pipeline 容量）。

### 2.2 節拍一定要用 esp_timer，唔可以 busy-wait

第一版用「計時 + `esp_rom_delay_us`」補足每幀餘下的 ~0.2–0.6 ms。後果：
本任務 prio 6、core1，spin 嘅時候 **core1 嘅 idle task 永遠冇機會跑** →
`CONFIG_ESP_TASK_WDT_PANIC=y` 之下 **5 秒後 Task WDT reset**（實測 `rst=6`，開機
→ 跑 4.4 秒 → 重啟，無限循環，睇落好似「板子當機」）。

現在改成：`esp_timer` 每 `1e6/fps` µs 給一次二元信號量，loop 用
`xSemaphoreTake(s_tick, 20 ms)` **阻塞**等節拍 —— idle task 有時間跑，WDT 唔再觸發，
而且幀週期比 busy-wait 穩定（`dt_us` 2205–2225）。

實測（`0xF060 stats`，2026-11-11 21:0x）：

```json
{"fps_x10":4500, "late":0, "dt_us":2220, "enc_us":341, "tx_err":0, "tx_bytes":12544}
```

`late` 只在開機／FAT 重建那一下加（12 次），之後穩定 0。

### 2.1 為什麼不用 RMT

RMT 是 WS2812 最直接的做法，但這個專案的下一個目標是 **16-lane 並行燈板**
（見 [led-controller-plan.md](led-controller-plan.md) §4）：I80 一條 bus 8 條資料線、
一個 byte 推一個 PCLK，天生就是 8 lane 同步。先用 1 lane 把波形與抖動驗完，
換 8 lane 只是把每顆燈的 bit 放到 byte 的不同 bit（編碼器已經留了位）。

如果 I80 在真機上出現「bit 之間有縫」之類的波形問題，後備方案是 RMT + DMA，
抖動／效果／指令這一層不用改。

---

## 3. 抖動（dither）怎麼做

### 3.1 精度鏈

```text
效果（float）→ 16-bit linear（0..65535）→ gamma → 8.8 定點（0..65280）
     → 每幀誤差回饋 → 實際送出 8-bit（0..255）
```

`gamma` 是 10-bit 查表 + 線性內插（1024 × u16 = 2 KB RAM），
所以**暗部不會先被 gamma 表本身量化**。

### 3.2 誤差回饋（per pixel, per channel）

```c
want = target_8_8 * rate + acc;      // acc = 上一個決策週期剩下的誤差
v    = want / (256 * rate);          // 實際送出的 8-bit
acc  = want - v * 256 * rate;        // 留給下一個週期
```

- `rate = 1`：每幀更新，抖動頻率 = 幀率（450 Hz）。
- `rate = 2`：每兩幀更新一次（225 Hz），兩幀的總量仍然等於 `2 × target`。
- 長期平均**一定等於**目標值，不需要任何隨機數。

### 3.3 8 個邏輯區

一區 = 一橫行（8 顆）。每區有自己的 `rate`（1..8）與 `phase`（錯開的起始幀）：

| 區 | 預設 rate | 抖動頻率 | 相位 |
|---|---|---|---|
| 0 | 1 | 450 Hz | 0 |
| 1 | 2 | 225 Hz | 1 |
| 2 | 1 | 450 Hz | 2 |
| 3 | 2 | 225 Hz | 3 |
| … | … | … | … |

目的不是「每區看起來不一樣」，而是**不要全部同相**：同相時整塊板的亮度會
一起跳（低灰時最明顯）；錯開之後，任何一瞬間「亮著的暗點」數量比較穩定。

同一區裡面 8 顆也不會同相：每顆、每通道的抖動誤差初始值按位置錯開
（`i*37 + c*91 mod 256`，位置決定、不是隨機數），所以同一幀不會 8 顆一齊跳；
長期平均不受影響（初始值只是相位，不是偏差）。

> ⚠️ `rate` 越大閃得越慢：`rate = 8` → 56 Hz，**會見到閃爍**。
> 預設只用到 2；想試就自己用 `0xF060 op=6` 調，再對照 `op=2` 關抖動。

### 3.4 位數 ↔ 抖動頻率（450 fps）

內容先量化到 `bits` 位，再逐幀用誤差回饋補回 8-bit 之間的差。**位數越高，需要越多幀
平均，抖動頻率越低**：

| 模擬位數 | 一級 = 幾分一個 8-bit | 抖動週期 | 頻率（450 fps） | 肉眼 |
|---:|---:|---:|---:|---|
| 8 | 1 | 1 幀 | — | 原生，唔閃但見到階梯 |
| 9 | 1/2 | 2 幀 | **225 Hz** | 睇唔到閃 |
| 10 | 1/4 | 4 幀 | **112 Hz** | 睇唔到閃 |
| 11 | 1/8 | 8 幀 | **56 Hz** | 邊緣，多數人睇唔到 |
| **12** | **1/16** | **16 幀** | **28 Hz** | **暗部會見到「呼吸式閃」** |
| 13 | 1/32 | 32 幀 | 14 Hz | 明顯 |
| 15 | 1/128 | 128 幀 | 3.5 Hz | 好明顯 |

所以「12 位 + 純時間抖動」喺 450 fps 之下**一定**有 28 Hz 嘅暗部閃 —— 呢個係物理，
唔係調參數可以解決。要 12 位又唔閃，只有兩條路：

1. **空間抖動**（下一步）：把不足一級嘅差分散到隔鄰像素（2×2／4×4），時間上唔使再
   等 16 幀；代價是極輕微的空間雜點（8×8 板近睇才見到）。
2. **提高 fps**：WS2812 鏈上限約 600 fps（0.9 µs／bit 已經係實測容量），
   12 位仍然只有 37 Hz，幫助有限。

### 3.5 空間抖動（ordered 4×4）——補 12 位暗階

時間抖動嘅最低頻率 = fps / 2^(bits-8)：12 位 = 26 Hz，暗部一定見到「一卡一卡」。
純空間抖動（`dither spatial`）唔等幀：用 4×4 Bayer 門檻，令同一幀入面
唔同位置嘅燈分擔「不足一級」嘅差，4×4 區域平均就等於目標；每幀把 pattern 轉一格，
每顆燈嘅開關頻率維持約 200 Hz（睇唔到），而平均亮度唔變。

- `dither 0/1/2`（off / temporal / spatial），`0xF060 op=2`。
- demo 第 2 段（stage 1）就係「12 位空間抖動」，同 stage 0（時間抖動）同亮度、
  同效果並排比較 —— 邊個順一睇就知。
- 代價：空間上多咗極細嘅紋理（8×8 板近睇才見到）。

**實測方法（唔使改 code）**：8 區可以各自設位數，一次過並排比較：

```bash
python3 tools/matrix.py --port $P sweep          # 第 0..7 行 = 8..15 位
python3 tools/matrix.py --port $P effect solid
python3 tools/matrix.py --port $P color 8 8 8    # 同一隻暗灰
python3 tools/matrix.py --port $P gain 170
```

睇邊一行最順又唔閃 → 嗰個就係時間抖動嘅實用上限。`bits 12` 設回全域、
`zone <idx> <rate> <phase> <bits>` 改單區。

---

## 4. 效果

| # | 效果 | 用途 |
|---|---|---|
| 0 | `OFF` | 全黑（對照） |
| 1 | `ZONE1` | 只亮第一區：8 顆由暗到亮的暖白斜線。**先驗這個**：有漏顆／沾色／次序錯即刻見到 |
| 2 | `BREATH` | RGB 輪流呼吸（2.5 s 一段，R→G→B，餘弦曲線）。**暗階改善的主要觀察對象** |
| 3 | `WAVE` | 斜向波浪＋色相流動；暗部佔多數，適合看抖動下的漸層 |
| 4 | `SOLID` | 單色（`op=3` 設 r/g/b）；A/B 抖動用 |
| 5 | `RAMP` | 64 顆由 0 到 126（0..255 尺度的暗部）一條線；**抖動開／關差最明顯** |

`zone_on`（`op=7`）可以只開某幾區，逐區加，對應「第一區先做，再慢慢加」。

---

## 5. 驗證步驟（第一版未上板，照這個次序做）

```bash
python3 make.py build esp32-s3-matrix --tests
# 第一次一定要 --full：這塊板用 4 MB 分割表，app / data 的 offset 同 16 MB 那份不同
python3 make.py flash esp32-s3-matrix --tests --full
# 之後全部用 tools/matrix.py（0xF060 的殼）
export NC_PORT=/dev/cu.usbmodem*        # 換成實際埠
```

1. **接線／不漏顆**：`python3 tools/matrix.py --port $NC_PORT effect zone1`。
   第一行 8 顆應該由暗到亮排列（暖白斜線），其餘 56 顆全黑。
   有任何一顆不亮／沾色 → 先查焊接與鏈的方向，不要調效果。
2. **幀率**：`stats`。`fps_x10` 應 ≈ 4500（450.0 fps）、`late` 成長要慢、
   `tx_us` ≈ 5（提交本身）、`dma_us` ≈ 2.2 ms（含 pacing）、`enc_us` ≈ 340。
   `late` 每幀都加 = 週期追不上（降 fps 或查優先級）。
3. **呼吸**：`effect breath`。抖動開（預設）看暗部是否平滑；再 `dither off` 看一次
   —— 應該明顯見到階梯。這是「抖動有沒有效」的 A/B。
4. **色階線**：`effect ramp`，同樣 A/B。64 顆放在 0..126，
   抖動關閉時很多顆會是**同一個灰階**（8-bit 量化），開啟後應該接近連續。
5. **波浪**：`effect wave`，看移動邊緣有沒有斷層、每區有沒有節奏不一致。
6. **逐區加**：`zmask 0x01` → `0x03` → `0x07` → `0xFF`，每次看新加的區有沒有雜點。
7. **試不同抖動頻率**：`zone 1 4 0`（第 1 區改 112 Hz）→ 應該開始見到閃；
   再 `zone 1 1 0` 調回。這一步把「頻率／閃爍」的邊界記錄下來。

回覆範例（`op=0`）：

```json
{"ready":1,"eff":2,"dither":1,"gain":255,"fps":450,"fps_x10":4500,
 "frames":123456,"late":3,"tx_err":0,"dt_us":2220,"dt_max_us":2301,"tx_us":5,
 "render_us":30,"enc_us":341,"dma_us":2230,"tx_bytes":12544,
 "rate":[1,2,1,2,1,2,1,2],"phase":[0,1,2,3,4,5,6,7],"zmask":255}
```

判讀：

| 欄位 | 正常 | 有問題的意思 |
|---|---|---|
| `fps_x10` | 4480–4505 | 明顯低 → `late` 一直加，查誰在搶 CPU |
| `late` | 幾乎不動 | 每幀都加 → 週期 < `tx_us`（fps 太高） |
| `tx_us` | < 20（只是提交） | 大 → `tx_color` 卡住 |
| `enc_us` | 300–400 | > 1000 → CPU 被搶／編碼退化 |
| `tx_err` | 0 | 非 0 → I80 送出失敗（看 log `rgbmtx`） |
| `ready` | 1 | 0 → `rgb.tx` 拿不到或 I80 起不來（看開機 log） |

> `rgbmatrix` 任務要跑起來，`data/config/tasks.json` 要有
> `{"name":"rgbmatrix","level":4,"affinity":[0,1]}`（新 port 已經預置）。
> 冇寫 = `(0,0)` = 唔跑（見 [task-model.md](task-model.md)）；`on_start` 仍然會行，
> 所以 `ready` 會是 1，但 `frames` 不會動。

---

## 6. 現在的邊界

| 項目 | 現況 | 下一步 |
|---|---|---|
| lane 數 | 1（D0） | 8 lane：同一條 bus，把 8 顆燈的 bit 放進同一個 byte |
| 抖動頻率 | 每區 rate 1..8（450..56 Hz） | 若要更低頻又不閃，需要 blue-noise／相位最佳化（不是再降頻） |
| 顏色 | NEO_RGB 固定 | 換燈種／次序 → `CONFIG_*` 或 runtime 參數 |
| 亮度 | `gain` 0..255（linear，再過 gamma） | 逐區 gain／白平衡 |
| I80 波形 | 8.889 MHz、8 cycles/bit（8.889/8 = 0.9 µs/bit） | 示波器量 T0H 337 ns／T1H 675 ns／reset |
| 熱 | 全白長開會燙 | 產品頁明寫會燒板；測試預設避開全白長開 |
| 配置 | 腳位在 Kconfig（build-time） | `hardware.json` 的 I80 段型別化之後搬回去 |

### 6.1 為什麼腳位放 Kconfig 而不是 `hardware.json`

`hardware.json` 已經有 `I80` 段（`data`／`wr`／`cs`／`freq`），但 `nc_cfg` **還沒有
型別化它** —— 文件自己寫明「改它們不會生效」。在 `nc_cfg_load.c` 補 I80 的型別之前，
把它放在 port 的 `sdkconfig.defaults` 是誠實的做法：改了一定會重新 build 生效。

搬回去的時機：`led-controller-plan.md` 的 `drv_i80` 正式化（多實例、runtime 換腳位）時，
連同 `hardware.json` 的 I80 型別一起做。

---

## 6.2 2026-11-11 上板記錄（第一版）

| 項目 | 結果 |
|---|---|
| 首次燒錄 | `make.py flash esp32-s3-matrix --tests --full`（USB-Serial-JTAG，esptool） |
| 之後更新 | **OTA over NC4**（`tools/ota.py`，734 KB／4.1 s ≈ 175 KB/s）—— USB-Serial-JTAG 被 TinyUSB 接管之後就只剩這條路 |
| 開機 | 4 MB 分割表、FAT 896 KB、cID 54EC、CDC → decode0 ✓ |
| 配置持久化 | 第一次 boot 的 FAT 被舊 firmware 的 NVS `provisioned` 影響，config 冇 seed → `rgbmatrix` 被補成 `[0,0]`；用 `0x1103 STATUS_SET {"System":{"fs_format":1}}` ＋ reboot 重建 FAT 之後正常 |
| 幀率 | **450.0 fps、late≤1、tx_err=0**（原本 4 MHz 只有 407.7 fps 而且每幀 late） |
| 踩雷 | busy-wait 節拍 → idle 餓死 → **Task WDT 每 5 秒重啟**（`rst=6`）；改 `esp_timer` 節拍後連跑 60 s+ 正常 |
| 全速容量 | **606 fps**（`fps=0`） |

---

## 7. 檔案

| 檔案 | 內容 |
|---|---|
| `firmware/components/tasks/test_rgb_led_matrix_tasks/nc_rgb_matrix.c` | I80、波形編碼、抖動、效果、任務 |
| `…/include/nc_rgb_matrix.h` | 對外 API（效果／抖動／每區參數／JSON） |
| `firmware/components/action/test_action/nc_test_action.c` | `0xF060` handler |
| `ports/S3/ESP32-S3-Matrix/` | 板子事實、4 MB 分割表、預設 tasks |
| `firmware/main/Kconfig.projbuild` | `NETCORE_RGB_MATRIX*` |
