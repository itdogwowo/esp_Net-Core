# 終極目標：硬件加速（L3）

> **定案（2026-11-17）**。這份是「目標與里程碑」，唔係實作計劃；細節連去對應
> notes／guides。相關：[module-adoption](../03_notes/module-adoption.md) 的收納階梯、
> [test-register](test-register.md)、[roadmap §0](roadmap.md)、
> [measurement-method](../02_guides/measurement-method.md)。

---

## 0. 一句話

> **打通 L3：熱路徑全部由硬件完成（DMA／GDMA／RMT／LCD_CAM／I2S／ADC-DMA，
> P4 再加 PPA／DMA2D／JPEG／ISP／CAM），CPU 只做控制與排程，外掛模組只做
> 邏輯／配置。用 before/after 數字（fps、bandwidth、latency、CPU%）證明。**

「無敵」的**操作定義**（唔係感覺）：**加功能唔加 CPU；加解像度唔加 jitter。**

## 1. 「無敵」的三級定義

| 級 | 定義 | 驗收（要數字） |
|---|---|---|
| **L1 可更新** | 外掛（wasm／AOT）→ 功能可以唔重燒更新 | `status ncm` ok；tick 正常；OTA 唔使拔線 |
| **L2 可收納** | 原生 C → 熱路徑 1× C | 同 workload native vs wasm/AOT；收納後仍過 budget |
| **L3 硬件化** | CPU 唔搬 data、唔計像素 | 一條完整 pipeline 上板：fps／bandwidth 達標、CPU 佔用低於目標、soak 零 overrun、before/after 表 |

## 2. 硬件能力表（IDF v6.0.3 `soc_caps.h` 實查；driver 在括號）

| 能力 | S3 | P4 | 對應 IDF driver |
|---|---|---|---|
| GDMA／2D-DMA | ✅ GDMA | ✅ GDMA＋**DMA2D** | `esp_driver_dma` |
| RMT（WS2812／自訂波形） | ✅ | ✅ | `esp_driver_rmt` |
| PARLIO（並行輸出，1–16 data＋clk） | ❌ | ✅ | `esp_driver_parlio` |
| LCD_CAM：I80 LCD／RGB LCD／Camera | ✅ | ✅ | `esp_lcd`（I80/RGB）、`esp_driver_cam` |
| I2S（audio） | ✅ | ✅ | `esp_driver_i2s` |
| ADC continuous＋DMA | ✅ | ✅ | `esp_adc` |
| AES／SHA（DMA） | ✅ | ✅ | `esp_security`／PSA |
| PPA（縮放／旋轉／混合） | ❌ | ✅ | `esp_driver_ppa` |
| JPEG codec（enc／dec） | ❌ | ✅ | `esp_driver_jpeg` |
| ISP／Camera | ❌ | ✅ | `esp_driver_isp`／`esp_driver_cam` |

> 重點：**P4 係真正嘅影像加速板**（PPA／DMA2D／JPEG／ISP）；S3 嘅 L3 主力係
> RMT／LCD_CAM／I2S／ADC-DMA／GDMA；P4 另加 PARLIO 並行輸出。所以「終極目標」分兩條：S3 做「輸出／波形／
> 感測」硬件化，P4 做「影像 pipeline」硬件化。

## 3. 目標場景（用嚟證明 L3）

| # | 場景 | 硬件鏈 | 平台 | 目標 |
|---|---|---|---|---|
| A | LED 矩陣（WS2812） | pattern → DMA buffer → **RMT** | S3（Matrix 8×8） | 60 fps＋、CPU <10%、零 bit-bang、零 tearing |
| B | 相機 pipeline | **CAM → ISP → PPA（縮放/旋轉/混合）→ JPEG → SD／網絡** | P4 | 640×480@30 fps 起，CPU 只做控制（目標 <10%） |
| C | LCD 顯示 | **PPA／DMA2D → LCD_CAM（RGB／I80）** | P4（S3 I80） | 全屏刷新 CPU ~0；double buffer 無 tearing |
| D | 音頻 | **I2S＋DMA＋雙緩衝** | S3／P4 | 48 kHz 無 underrun；CPU 只做調參 |
| E | 密碼／校驗 | **AES／SHA＋DMA**（OTA 簽名、檔案驗證） | S3／P4 | 驗簽 CPU 成本↓；OTA 時間可量 |

## 4. 里程碑（唔跳級）

| 里 | 內容 | Exit criteria（要入 test-register） |
|---|---|---|
| **M1** | L1 外掛打通 | 外掛載入＋tick＋白名單；bench：interp vs C ✅（2026-11-17：14.8×） |
| **M2** | AOT 量測（P3） | `wamrc`→Xtensa AOT→S3 PSRAM dual-bus 執行；L1 倍數（目標 2–4× C）；placement 失敗則 fallback |
| **M3** | L2 收納 | capability provider（`nc_cap_*`）＋最少一個真任務由外掛升 C；native 1×；收納後仍過 budget |
| **M4** | L3 第一條 pipeline | 揀 **A（S3 RMT LED）** 或 **B（P4 CAM→ISP→PPA→JPEG）**；交付 before/after 表＋CPU%＋soak |
| **M5** | 「無敵」驗收 | 兩塊板各一條完整 pipeline；主核 CPU 控制開銷 <10%；全鏈無 overrun；外掛可更新；OTA 唔使拔線 |

## 5. 量測方法（唔靠感覺）

- 每個 stage 打 `esp_timer_get_time()` timestamp → p50／p95／max；CPU 用 idle hook／task runtime。
- 既有工具：`0xF050 CPU_BURN`（native baseline）、`0xF01C MEM_BENCH`、
  `0xF01D TILE_BENCH`、`0xF008 BUS_STATS`；
  量測程序照 [measurement-method.md](../02_guides/measurement-method.md)。
- 公平比較：同一塊板、同一時段、同一 workload、checksum 一致（wasm/AOT/C 都要）。
- 每個 milestone 的數字都要寫入 [test-register](test-register.md)，並保留舊數字。

## 6. 前置條件／風險

| 風險 | 影響 | 處理 |
|---|---|---|
| 硬件未接線（P4 的 CAM／LCD 排針、S3 Matrix 的 LED） | L3 場景做唔到 | 先確認板子 wiring；必要時用新板／轉接 |
| P4 driver 成熟度（PPA/JPEG/ISP/CAM 在 IDF 6.0.3） | 可能編唔過／行為未定 | 先做 build＋小範例，同 WAMR 一樣準備 patch |
| PSRAM 帶寬／cache；DMA 只可以 internal | pipeline 速度受限 | 先量 PSRAM/DMA 帶寬（既有 MEM_BENCH），framebuffer 分塊 |
| 信任邊界 | L3 全部係固件／簽名模組 | 唔接受第三方；外掛只做邏輯（L0/L1） |
| 冇 hardware fallback | 硬件一壞就黑畫面 | 每條 pipeline 保留 CPU fallback（wasm/AOT）＋ recovery |

## 7. 下一步（短期）

1. **M2 AOT spike**（安全、改動細）——量 L1 真實倍數，校準門檻；
2. capability provider 重構（`nc_cap_gpio`／`nc_cap_time`／`nc_cap_led`）；
3. **M4 第一個 L3 spike：S3 RMT LED matrix** —— 最貼近現有 `test_rgb_led_matrix`，
   量 fps／CPU，做 before/after；
4. 之後 **P4 CAM → ISP → PPA → JPEG**（要接線／確認板子）。
