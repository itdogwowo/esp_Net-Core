# 渲染任務（render）＋ 效果任務（pixel）＋ 驅動

> 2026-11-18。取代舊的 `components/dev/test_rgb_led_matrix_tasks`（`rgbmatrix` 測試任務）。
> **2026-11-20 M2 落地**：內建效果搬去 `components/effects/pixel_effects/`
> （`nc_effect_*.c`）；引擎（契約、registry、抖動、emit）住 `sw/pixel/engine/`
> （`nc_px_engine_*.c`）。`sw/pixel` 只留工具：環、編碼器、環註冊表、引擎。
> 拆法見 [behavior-taxonomy.md](behavior-taxonomy.md) §2.0／§9。
> **同日兩次修正**：
> 1. 原本全部塞在 `components/sys/pixel`（環、效果、驅動、任務混在一起）→ 拆出
>    `sw/`（純軟件）、`tasks/`（任務）、`driver/`（硬體）；
> 2. 原本效果與渲染共用一條 loop → 再拆成 **兩個任務**：
>    `pixel`（效果／Map 生產者）＋ `render`（渲染；將來 LCD 同 LED 共用）。
>
> 命令：`0xF060 MATRIX`（效果生產者）、`0xF061 RENDER`（渲染器）；
> host 測試：`tools/hosttest/pixel_test`。

---

## 1. 四層分工

| 層 | 路徑 | 內容 | 依賴 |
|---|---|---|---|
| `sw` | `components/sw/pixel/` | 環、編碼器、**效果框架**（契約／registry／program interpreter；純 C） | `esp_timer`／`log` |
| `tasks` | `components/tasks/pixel_task/` | **效果／Map 生產者任務**：按自己的節奏算一格、寫環 | `task`、`pixel` |
| `effects` | `components/effects/pixel_effects/` | **內建效果內容**（時間＋參數 → canonical 畫面）；框架在 `sw/pixel` | `pixel` |
| `tasks` | `components/tasks/render_task/` | **渲染任務**：環 → 編碼 → 後端 → 硬件（LED 現在，LCD 將來） | `task`、`mem`、`hw`、`core`、`pixel`、driver |
| `driver` | `components/driver/drv_i80/`、`drv_spi_led/` | 腳位、PCLK、DMA、完成回呼 | `esp_lcd`／`esp_driver_spi` |

```text
tasks/pixel_task（loop prd，**不控 FPS**）
   效果／Map 算一格 canonical R,G,B,W
        │  寫入環（SSPC、滿了不覆蓋）
        ▼
   環（由 render 任務建立／登記；名字例如 "i80.prd"）
        │
        ▼
tasks/render_task（loop render，讀 system.json frame_interval_ms）
   取最新一格 → 共用編碼器 → 後端轉接（i80／spi／direct／mock）
        │
        ▼
driver/drv_i80、drv_spi_led（腳位／PCLK／DMA／完成）
```

**邊界一句話**：

- `pixel` 任務：只管「畫面內容」——效果、Map、參數；
- `render` 任務：只管「把環裡的幀送到硬件」——唔跑效果；
- `sw`：兩邊共用的工具（環、編碼、layout、**效果引擎** `nc_px_engine_*`）；
- `effects`：具體內建效果（實作）；M2 已由 `nc_pixel_producer.c` 拆出（`nc_effect_*.c`）；
- `driver`：只知硬體，唔知像素。

**為什麼效果計算唔搬入 tasks**：同一份 `nc_px_engine_frame()` 要能被 host 測試、
`.nce` 外掛、主機推送共用。放 `tasks` 會令它依賴 FreeRTOS／任務管理器，失去可測試性；
「幾時算」在 `pixel_task`，「點算」在 `effects/pixel_effects/`。

## 2. 任務與 loop

| 任務 | loop | 節奏 | stack／prio | 做什麼 |
|---|---|---|---|---|
| `render` | `render` | **system.json `frame_interval_ms`**（預設 20 ms = 50 fps） | 4,096／租戶上限 | 環 → 編碼 → 後端 |
| `pixel` | `prd` | **不控 FPS**：有空間就填，滿了就 1 ms 監控一次 | 3,072／租戶上限−1 | 效果 → 環 |

`frame_interval_ms` 係用戶喺 `/data/config/system.json` 改得到的節奏（20 = 50 fps）；
`CONFIG_NETCORE_PIXEL_FPS` 只在拿不到設定時做後備。render loop 用 **esp_timer
週期信號量**做節拍（唔用 `vTaskDelay` 的 1 ms 粒度、唔 busy-wait），所以實測
`fps` 同 `frame_interval_ms` 一致（20 ms → 50.0 fps、`late=0`）。

生產者唔用節拍：環有空間就寫，填滿就每 1 ms 監控一次等 renderer 還格。
renderer 每次 peek 會取最新一格並假消費較舊的格，所以一次 tick 會空出 2–3 格，
生產者平均會做到約 100–150 格／秒（多數格內容相同、只為保持環常滿）；
真正送燈仍然係 50 格／秒。效果動畫用 `esp_timer` 的實際時間，所以生產快慢不影響速度。

- 兩者都是 `kind=normal`、level 4；`tasks.json` 兩條都要開（`affinity` 各自可設）。
- **啟動次序**：`main.c` 先登記 `render`、後 `pixel`。`render` 的 `on_start` 建立／
  登記環；`pixel` 的 `on_start` 才找得到那條環（按名字含 `prd`）。
- 兩個任務只透過環見面；效果停住 → 燈保持最後一格；渲染停住 → 效果照樣寫環（滿了跳過）。

## 3. 關鍵資料結構

| 東西 | 位置 | 說明 |
|---|---|---|
| `nc_pixel_q_t` | `sw/pixel/include/nc_pixel_core.h` | 環本體（純資料，可放 stack）；**不配置記憶體**，用 plan 交付的區域 |
| `nc_px_hdr_t` | 同上 | 每格 24 B 表頭（seq／ts／gen／src／flags／drops） |
| 環註冊表 | `sw/pixel/nc_pixel_registry.c` | `nc_px_ring_register/find/count/at`；render 登記、pixel 按名找回 |
| `nc_pixel_backend_t` | `tasks/render_task/include/nc_render.h` | 後端 ops：acquire／encode／submit／reclaim／json；`ring` 由 `bind_lanes()` 設定 |
| `nc_px_codec_cfg_t` | `nc_pixel_core.h` | 格式、波形（pclk／bit_cells／t0h／t1h）、抖動位數、區大小 |
| `nc_i80_drv_cfg_t` | `driver/drv_i80/include/nc_i80_drv.h` | 10 個腳位、PCLK、每筆 byte 數、DMA 槽數 |

環的每格：`64 B 表頭 + px × 4 B`，再進位到 64 的倍數。`seq` 是最後的發布旗標。

## 4. 六條契約（違反了會靜默出錯）

1. **生產者一定要寫 `begin_ex()` 交出來的那一格**，`commit()` 只發布同一格。
   環的 writer 游標每 commit 前進一格；綁死第 0 格＝第二幀之後寫舊格、發布新格。
2. **後端要登記**（`nc_px_backend_register()`），`bind_lanes()` 是用名字找回後端的。
3. **後端要拿到環**：`bind_lanes()` 會寫 `be->ring = q`；i80／spi 的 acquire 用 `b->ring`。
4. **encode 不可以再 peek**：acquire 當下記住 payload 指標；重新 peek 會拿到
   「新內容配舊 seq」。
5. **DMA 完成才還格**：`reclaim()` 只看完成旗標；I80 的完成回呼是 FIFO，但槽索引
   不是 —— 用單調 `order` 對回完成的槽（在 `drv_i80` 內）。
6. **緩衝按實際 px／lanes 配置**：`NC_PX_MAX_LEDS` 的靜態上限＝1 lane 128 KB；
   S3 沒有 PSRAM 時連 link 都過不了。

## 4.1 I80 8-bit／16-bit（2026-11-18 加）

`CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH=8`（預設，D0..D7＋WR＋DC = 10 個腳位）或
`=16`（D0..D15＋WR＋DC = 18 個腳位，最多 16 lane）。

- **傳輸時間同 8-bit 一樣**：16-bit 一個 PCLK 出 2 個 byte（八條 lane 一組），
  所以 600 顆 × 16 lane 的 DMA 時間 = 600 顆 × 8 lane（2.5 MHz、3 cells ≈ 17.4 ms）；
- **編碼時間仍然同 lane 數成正比**：每條 lane 要各自產生 72 個單元／顆，
  16 lane 的 CPU 成本 = 8 lane 的 2 倍（見 §9 量測）；
- 16-bit 的 byte 次序：lane0..7 → 低位元組、lane8..15 → 高位元組（little-endian），
  即 `out[u*2]` = D0..D7、`out[u*2+1]` = D8..D15；
- S3 實測：用 16-bit 設定（D0..D5 接同一塊 Textkit、D6..D15 dummy）可以起 I80、
  50 fps、`enc_us≈825`、`fail=0` —— 硬件支援 16-bit i80。

## 5. 抖動（實驗功能，預設關）

`CONFIG_NETCORE_PIXEL_DITHER=0`、`DITHER_BITS=8` 是預設：8-bit 直出，唔抖動。
舊的 12 位時間抖動只作實驗保留：

- 要試：`0xF060 op=2`（模式 1／2）或 `op=8`（位數 12），或改 Kconfig；
- 代價／原理見 [rgb-matrix-dither.md](rgb-matrix-dither.md)；
- 預設 demo 亦已關（demo 段落會開 12 位抖動）；`0xF060 op=13,1` 可開。

## 6. 2026-11-18 修好的一批問題

| # | 問題 | 影響 | 修法 |
|---|---|---|---|
| 1 | `nc_pixel.h` include 在 `#ifdef NC_ENABLE_TESTS` 內，但呼叫在測試區外 | release 編不過 | include 移出測試區；加 component 依賴 |
| 2 | `s_nplan` 先用後宣告、`.chain` 欄位不存在、腳位字串塞 `int[]` | 編不過 | 重新安排宣告；`parse_i80_pins()` |
| 3 | i80 用 `d->codec[ln]`（一個指標） | 編不過 | codec 改陣列 |
| 4 | 後端緩衝按 `NC_PX_MAX_LEDS` 靜態配置 | S3 link 爆（1 MB＋393 KB） | 按 px／lanes 用 heap 配置 |
| 5 | i80／spi 建好後端但沒有 register | lane 只剩 mock，燈冇輸出 | 建好即登記 |
| 6 | i80／spi 的 `d->q` 從來沒有設定 | acquire 永遠 false | `nc_pixel_backend_t.ring` 由 `bind_lanes()` 設定 |
| 7 | 生產者綁死環第 0 格 | 第二幀之後畫面錯／黑 | `begin_ex()` 交出來的格才寫 |
| 8 | `0xF061` JSON 把 `"prd"` 塞在 `"lanes":[` 之後 | 回覆不是合法 JSON | 重寫 |
| 9 | 渲染 fps 寫死 60（Kconfig 冇作用） | 文件與行為不符 | 預設跟 Kconfig |
| 10 | I80 完成回呼「掃第一個未完成的槽」 | 槽回收重排後認錯格 → 提早還格 | 單調 order |
| 11 | GPIO3 被當保留腳 | LED Textkit（GPIO1–6）起不到 I80 | 只准 GPIO3 當 lane 資料線（warn） |
| 12 | 驅動／任務／效果在 `sys/pixel` | 分層錯 | `sw/`＋`tasks/`＋`driver/` |
| 13 | 效果與渲染共用一條 loop | 效果節奏同送燈綁死 | 拆成 `pixel`（效果）＋`render`（渲染）兩個任務 |
| 14 | `tasks.json` 舊 preset 只有 `pixel` 條目 | 重燒後 `render` 變 (0,0)，燈唔更新 | 兩條都寫；舊設備 `STATUS_SET fs_format=1` 重 seed |

## 7. 指令

| 命令 | 作用 |
|---|---|
| `0xF060` | 效果生產者（op 表與舊 rgbmatrix 相同，0–16） |
| `0xF061 op=0` | `{"producer":{…},"render":{…}}` 一個回覆 |
| `0xF061 op=1` | 預留（目前沒有 reset API，回 ok） |

工具：`tools/matrix.py`（0xF060）、`tools/nc4.py`（STATUS）。

## 8. 驗證

- host：`tools/hosttest/pixel_test` → **339 passed**（環、位元、多 lane、後端回收）；
  已加入 `tools/hosttest/run_all.sh`。
- 建置：Textkit tests `0x146040`、release `0x11df30`，pixel／render 相關 0 warning；
  交叉建置 S3-Matrix、Mini-Nano（pixel 關）、C3（無 I80）通過。
- 上板（S3-SuperMini ＋ LED Textkit **6 條鏈 × 20 顆**）：
  - `status tasks`：`render` 有 `render@c1` running；`pixel` 有 `prd@c1` running；
  - `0xF061`：`producer.ready=1`、`dither=0`、`bits=8`、`demo=0`；
    render **fps 50.0**（`frame_interval_ms=20`）、`late=0`、`be=i80`、`lanes=6`、
    `px=20`、`fail=0`、`enc_us≈716–1052`、環 `fill` 保持在 1–3（常滿）；
    生產者自由填環（`fps_x10≈1000`、即 ~100 格／秒）；
  - 6 條鏈 × 20 顆全亮（solid 30,30,30、gain 120）。

## 9. 極限測試：16 × 600 × 50 FPS（2026-11-18）

> ⚠️ **2026-11-19 更正**：這一節的 14.33 ms 是 `sram=1`（11 個緩衝全在 internal）的
> 數字。真機（tests 映像、WiFi 佔用 internal）量到 **19.4–20.1 ms**，50 fps 只是
> 剛好在邊界 —— 見 §13。

測試方法：測試映像新增 `0xF062 PXBENCH`（`[lanes:u8][px:u16][frames:u8]`），
用**同一份 codec** 量「逐 lane 編碼（canonical RGBW → 72 個時序單元／顆）
＋ 多 lane OR 合成」的每幀成本；payload 用會變的 pattern（避免「同上一顆就抄」的
快取路徑造成假快），scratch／輸出放 PSRAM（真實 backend 的 scratch 要 internal，
所以數字係保守值）。S3 @240 MHz、Textkit 測試映像。

```bash
python3 tools/nc4.py --port $P raw 0xF062 10580205   # 16 lane × 600 px × 5 幀
```

| 配置 | µs／幀 | 上限 fps | 20 ms（50 fps） |
|---|---:|---:|---|
| **16 × 600** | **143,166** | **6.9** | ❌ 差 **7.2 倍** |
| 8 × 600 | 72,053 | 13.8 | ❌ 差 3.6 倍 |
| 16 × 300 | 55,076 | 18.1 | ❌ |
| 6 × 600 | 54,153 | 18.4 | ❌ |
| 6 × 512 | 45,448 | 22.0 | ❌ |
| 8 × 256 | 20,140 | 49.6 | ❌（差 140 µs） |
| 8 × 200 | 11,909 | 83.9 | ✅ |
| 6 × 320 | 21,997 | 45.4 | ❌ |
| 6 × 256 | 15,099 | 66.2 | ✅ |
| 6 × 20（現況） | 998 | 1002 | ✅ |

**結論：現有 S3 ＋ 現有架構做唔到 16×600×50。** 三個獨立瓶頸：

1. **控制器**：S3 只有一個 LCD_CAM／I80，但支援 **16-bit 模式**（已實測），
   所以 16 lane 可以用一條 16-bit bus；**DMA 傳輸時間同 8 lane 一樣**，唔係瓶頸。
2. **記憶體**：16-bit、600 px 的一幀 = 86,800 B；2 個 DMA 槽 = 173.6 KB，
   release 映像的 DMA-capable internal 只有 164.9 KB → **差 ~9 KB**；
   1 槽（86.8 KB）＋ lane scratch（43.4 KB）勉強放得落，但單槽冇得 encode/DMA 重疊。
   （**測試映像**只剩 20 KB，因為 NCM/WAMR＋bench＋test_action 佔了約 152 KB internal；
   release 有 172.7 KB —— 見 status §7.113。）
3. **CPU**：143 ms／幀 vs 20 ms 預算 → 7.2 倍；16-bit 只解決傳輸，唔解決編碼。

**要達到 16×600×50 的方向**（未實作）：

- **顏色 pattern 快取**（最大槓桿）：文字／圖案通常只有幾種顏色，把每種 RGBW 的
  72-byte 波形快取，逐像素只做 memcpy＋OR，省掉 bit 展開（估計 3–5×）；
- 兩個核心各編 8 lane；
- 或 P4（400 MHz、internal 更大、PSRAM DMA）＋ 16-bit I80；
- 或者兩塊 S3（各 8 lane）同步，每塊先做上述快取。


## 10. 優化（2026-11-18 晚：對照舊 mp_Net-Core 的原則）

舊專案（MicroPython）的原則：**全程整數、固定數值域、唔逐 pixel 呼叫、一次掃整條
buffer**。C 版本來已經係整數，但熱路徑逐 pixel 呼叫＋逐 bit 寫入，仍然慢。已做：

1. **波形 LUT**：8-bit 值 → 8 個 bit 的時序單元（`bit_cells × 8` bytes）共用一張
   256 項表（3 cells = 6 KB、8 cells = 16 KB）；逐線材 byte 變 memcpy。
2. **8-bit／無抖動快路徑**：`dither_mode=0 || bits<=8` 直接取 R,G,B,W，唔入
   `dither_ch`。
3. **Direct-OR API**：`nc_px_codec_wave_frame_or()` 直接把每條 lane 的波形 OR 進
   I80 byte（`out[u*stride+off] |= bit<<shift`），**取消 lane scratch**
   （省 43 KB internal ／一次讀寫）。
4. **Packed bitstream＋8×8 轉置**（你質疑「波形唔應該咁慢」之後再加）：
   `nc_px_codec_wave_pack()` 把一條 lane 的波形壓成 bitstream（bit_cells=3 時
   每線材 byte 3 bytes），再用 `nc_px_codec_pack_merge8()` 一次合併 8 條 lane
   （8 個單元一組），輸出仍然係 I80 byte-per-unit。I80 backend 已改用這條路徑。

`0xF062` 量測（測試映像、scratch/輸出放 PSRAM，所以係**保守值**；release 的
internal 會更快）：

| 配置 | 優化前 | 優化後（direct-OR） |
|---|---:|---:|
| 16 × 600 | 143 ms（6.9 fps） | **38.5 ms（25.9 fps）** |
| 8 × 600 | 72 ms（13.8 fps） | **19.2 ms（52.0 fps）** ✅ |
| 6 × 600 | 54 ms（18.4 fps） | **17.6 ms（56.7 fps）** ✅ |
| 16 × 300 | 55 ms（18.1 fps） | **18.9 ms（52.9 fps）** ✅ |
| 6 × 256 | 15 ms（66 fps） | **7.2 ms（138 fps）** |

**packed 拆開量（16×600）**：pack（查表＋memcpy bitstream）15.3 ms、
merge（8×8 轉置＋寫 I80 byte）24.3 ms。即係**查表唔係瓶頸**，merge／輸出寫入才係；
benchmark 嘅輸出放 PSRAM（真實 backend 寫 internal DMA 槽，會快好多）。

**結論（packed 之後）**：

- **8 × 600 @ 50 fps ✅**、**16 × 300 @ 50 fps ✅**、6×600 56.7 fps；
- **16 × 600 @ 50 fps 仍然差**：packed 已減到 38.5 ms（PSRAM；internal 會更快），
  但 16-bit 一幀 86,800 B，**2 個 DMA 槽 173.6 KB > release DMA 164.9 KB（差 8.7 KB）**；
  單槽（86.8 KB）＋ packed 暫存（43.2 KB）雖然放得落，encode 同 DMA 冇得重疊。
  要再上：P4／兩塊 S3、或把 packed scratch 縮到 4 條 lane 一組（槽記憶體仍然係硬牆）。


## 11. 純 SRAM 量測（release 映像，0xF062 內建）

用 `CONFIG_NETCORE_PXBENCH=y` 建 release（無 NCM／測試任務），benchmark 緩衝全部
internal（`sram=1`，11/11 塊）：

| 配置 | PSRAM 版 | **純 SRAM** | 50 fps |
|---|---:|---:|---|
| **16 × 600** | 38.5 ms（25.9 fps） | **35.4 ms（28.2 fps）**（pack 13.44＋merge 21.80） | ❌（差 1.8×） |
| **8 × 600** | 19.2 ms（52.0 fps） | **17.8 ms（56.2 fps）** | ✅ |
| **16 × 300** | 18.9 ms（52.9 fps） | **17.7 ms（56.4 fps）** | ✅ |
| 6 × 600 | 17.6 ms（56.7 fps） | **16.1 ms（62.0 fps）** | ✅ |

**結論改變**：純 SRAM 只快 ~8–10%，即係**剩低嘅瓶頸係 CPU（逐 pixel 映射＋8×8 合併），
唔係記憶體頻寬**。下一步：

1. 專化 pack 熱路徑（ws2812/8-bit：直接 3 個 LUT memcpy，唔經 `lane_px` 函數）；
2. 8×8 bit-transpose 用位元技巧（少 3–5× 指令）；
3. 兩核各處理一組 8 lane（16×600 可望 ~18 ms）；
4. 16-bit 雙緩衝仍然要 173.6 KB DMA > release 164.9 KB（差 8.7 KB）——
   要再擠出 9 KB DMA-capable，或改用單槽＋更快的 encode（<2.6 ms）才追得上 50 fps。


## 12. FastLED／WLED 對照後的第二輪優化（2026-11-19 深夜）

參考 FastLED 的 ESP32-S3 LCD_CAM clockless driver：
- `Wave3BitExpansionLut`：**nibble（16×u16）** 查表，而不是 256 項；
- `bit_spread_lut.hpp`：`kSpreadNibble[16]`（64 B）＋ u32 OR，將 8/16 條 lane
  的脈衝位元一次攤平成 output byte，官方註明比 naive 快 ~2×；
- **chunked DMA + 3 環 + queue depth 2 + ISR 接力**（chunk 預設 1024 input bytes）；
- DMA ring buffer 用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`（S3 `SOC_PSRAM_DMA_CAPABLE=1`），
  提交前 `esp_cache_msync(C2M)`。

已照做並量測（release、純 SRAM、0xF062）：

| 階段 | 之前 | **FastLED 式之後** |
|---|---:|---:|
| pack（16×600） | 13.44 ms | **7.07 ms** |
| merge（8×8 轉置） | 21.80 ms | **7.08 ms** |
| **合計** | **35.4 ms（28.2 fps）** | **14.33 ms（69.7 fps）** |

其他配置：8×600 = 7.24 ms（138 fps）、16×300 = 7.18 ms（139 fps）、
6×600 = 5.88 ms（170 fps）。

同時實作 **PSRAM DMA 槽**（`drv_i80`）：
- `heap_caps_aligned_calloc(64, …, MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA|MALLOC_CAP_8BIT)`；
- 提交前 `esp_cache_msync(..., C2M)`；失敗才退 internal。
→ 16-bit × 600 顆的兩個 86,800 B 槽可以放 **PSRAM**，2 MB 空間充足，
  記憶體牆消失。

**16×600 新算式**（2 槽 overlap）：

```
CPU encode 14.33 ms；DMA 17.36 ms
frame = max(14.33, 17.36) = 17.36 ms → 57.6 fps 上限
⇒ 50 fps（20 ms）現在 CPU 同記憶體都過關，DMA 成為主要成本
```

WLED 對照：WLED 自己唔寫波形，交 NeoPixelBus；驅動分 RMT／PARHW，
平行 I2S 最多 8 lane、**每條 output 限 300 顆**（記憶體），所以他們從未做
16×600 一條 lane——我們用 PSRAM DMA＋chunk 化先有可能。

---

## 13. 極限與「約定俗成」的上限（2026-11-19 上板實測；**程式不擋**）

原則：**不在程式寫死上限** —— 需求會變，硬上限會變成另一種坑。這裡只寫約定與
後果，配置的人自己決定要不要踩線。超過了不會被拒絕，而是**掉幀、`late` 上升、
最後 Task WDT 重啟**（見 §13.3）。

### 13.1 兩個互相獨立的限制

| 限制 | 取決於 | 公式 | 50 fps 的安全值 |
|---|---|---|---|
| **CPU（編碼）** | lane 數 × px | `pack` 隨 lane 線性；`merge` 每 8 條一組（≤8 條固定一份） | `lanes × px ≲ 8,000` |
| **DMA（送燈）** | **只有 px** | `units = px × lane_bytes × 8 × bit_cells + 200`；時間 = `units ÷ PCLK` | `px ≲ 570` |

`bit_cells=3`、`PCLK=2.5 MHz`、`lane_bytes=3` 時：
`units = px × 72 + 200`，所以 **600 px → 17.36 ms（57.6 fps 硬上限）**、
500 px → 14.48 ms、400 px → 11.60 ms。

### 13.2 實測（同一塊 S3，`0xF062`，非推論）

| 配置 | 編碼 `us_packed` | pack | merge | DMA | 誰是瓶頸 |
|---|---:|---:|---:|---:|---|
| **4 × 600** | **6.44 ms** | 1.77 | 4.66 | 17.36 | **DMA**（57.6 fps） |
| 8 × 600 | 8.21 ms | 3.55 | 4.66 | 17.36 | DMA |
| 16 × 600 | 16.37 ms | 7.08 | 9.28 | 17.36 | 打平（真機超支） |
| 16 × 500 | 13.65 ms | 5.91 | 7.74 | 14.48 | CPU（仍 <20 ms） |
| 16 × 400 | 10.56 ms | 4.72 | 5.81 | 11.60 | DMA |

要換成**真機**數字：`bench × ~1.18`（16-bit PSRAM 寫入＋真實 codec），再加
效果計算 `≈ 0.47 µs/px`（同核時）。所以 16×600 真機 = 16.37 × 1.18 + 1.7 ≈ 21 ms。

### 13.3 超過會發生什麼（實測）

- **16 × 600**：`enc_us` 18.4–20.1 ms、`tx_us` 1.16 ms → 每幀 19.6–21.2 ms，
  剛好壓在 20 ms 上。
- render loop 佔住 core 1 近 100% → **IDLE1 餓死 → Task WDT（5 s、PANIC）** →
  每 ~8 秒重啟（`up_s` 一直停在 8、`status` 回應斷斷續續）。
- 沒被 WDT 打死時也是 `frame_us ≈ 38 ms`（~26–34 fps）、`late`／`skipped` 上升。

### 13.4 其他約定

- **所有 lane 共用同一個 px**：做不到「lane0=600、lane1=100」。要混合長度得改成
  per-lane px（每個 lane 本來就一個 codec，改動不大）。
- **「只用 4 條、其他 0」不會縮短幀時間**：I80 是並行時鐘，空的 lane 一樣要掃；
  只省 CPU（pack 少 3/4），DMA 時間只看 px。
- **插槽有上限**：DMA 槽固定 3、環固定 3 槽。`slot = units × (bus_width/8)`；
  三槽總量 ≤48 KB 走 internal（免 msync），否則 PSRAM（每幀一次 C2M writeback，
  16×600 實測 1.16 ms）。
