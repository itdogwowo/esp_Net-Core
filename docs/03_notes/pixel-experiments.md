# pixel 優化實驗清單（FastLED／WLED 對照）

> 2026-11-19。目的：把「公開項目有、我們也有但優化得更好」的零件變成可執行、
> 可量測、可回滾的實驗。每項都有假設、改動位置、量法、通過條件。
> 實驗前後都照第 1 節流程，唔好跳步。

## 0. 目前基線（量完先改）

環境：release 映像 + `CONFIG_NETCORE_PXBENCH=y`（暫時開，量完還原），
`0xF062` 緩衝 internal-first（`sram=1`），frames=2，payload 用變動 pattern。

| 配置 | total | pack | merge | 等效 fps |
|---|---:|---:|---:|---:|
| **16 × 600** | **14.33 ms** | 7.07 ms | 7.08 ms | 69.7 |
| 8 × 600 | 7.24 ms | 3.53 | 3.57 | 138 |
| 16 × 300 | 7.18 ms | 3.53 | 3.52 | 139 |
| 6 × 600 | 5.88 ms | 2.65 | 3.51 | 170 |

- I80 傳輸（16-bit、600 顆）：**43,400 PCLK @2.5 MHz = 17.36 ms**。
- DMA 槽已改 PSRAM（`MALLOC_CAP_SPIRAM|DMA`、64 B 對齊、`esp_cache_msync` C2M），
  2 槽 173.6 KB 放得落；`frame = max(14.33, 17.36) = 17.36 ms`（57.6 fps 上限）。
- 板上實測：6×20 @50 fps、`fail=0`；host tests 339 passed；`make.py check` 0 error。
- 已知缺口：
  - 16 lane 未有實物，16×600 全部係合成量測；
  - 內建效果 producer 固定 8×8=64 顆，600 顆只填前 64；
  - PSRAM DMA 路徑未在真實 16 lane 上驗證。

## 1. 每次實驗的標準流程

1. **記錄起點**：`git status --short`、`git rev-parse --short HEAD`。
2. **host 測試**（快，先攔編譯／語義錯）：
   ```bash
   bash tools/hosttest/pixel_test/run.sh      # 期望 339 passed
   bash tools/hosttest/run_all.sh
   ```
3. **build**
   - tests 映像（有 0xF060/0xF061/0xF062）：
     ```bash
     IDF_PATH=$HOME/esp/esp-idf python3 make.py build esp32-s3-supermini \
       --project textkit --tests --no-doctor --no-archive
     ```
   - release + `0xF062`（量純 CPU／SRAM）：暫時在
     `ports/project/S3/ESP32-S3-SuperMini-LED-Textkit/sdkconfig.defaults`
     加 `CONFIG_NETCORE_PXBENCH=y`，`--fresh` build；**量完刪返**。
4. **上板（OTA，不用 esptool）**
   ```bash
   python3 tools/ota.py --port /dev/cu.usbmodem* <esp_net_core.bin>
   ```
   1.17 MB 約 **188 s（6.1 KB/s）**；`--no-apply` 之後要送 `0x2220` 才切換。
   esptool 只留給 bootloader／分割表／救磚。
5. **驗證**
   - `python3 tools/nc4.py --port <p> status sys` → 核對 `app` hash／`up_s`；
   - tests 映像：`0xF061` 看 `fps`、`fill`、`fail`、`enc_us`；
   - 量測：`0xF062`，payload `[lanes:u8][px:u16 LE][frames:u8]`；
     回覆要有 `us_pack/us_merge/us_packed/sram/max_fps_x10`。
6. **記錄**：填第 3 節一行；唔通過就回滾，唔好疊住改。
7. **收尾**：還原 sdkconfig、跑 host tests＋`make.py check`。

## 2. 實驗清單（逐項開 checkbox）

### E1 熱路徑搬 IRAM（優先：穩定性）
- **假設**：flash 寫入（OTA／NVS）時 cache 關閉；我們 `drv_i80` 完成 ISR 同
  codec 熱路徑在 flash，會 stall／interrupt watchdog。FastLED 用 `FL_IRAM` 保命。
- **改**：`drv_i80` 的 `on_color_done`（及必要的 submit 小函數）標 `IRAM_ATTR`；
  如把 merge 搬 IRAM，`s_spread_nibble` 等表要放 RAM（malloc／`.iram1`），
  唔可以留 flash。
- **量**：6×20 50 fps 跑住，連續 3 次 OTA（或 NVS commit）；睇 panic／`late`；
  `idf.py size` 睇 IRAM 用量。
- **通過**：0 panic、`late=0`、fps 不變、IRAM 增加 <8 KB。
- **風險**：IRAM 滿 → build fail；漏搬表 → cache 關閉時 fault。
- [ ] E1

### E2 8-bit 效果數學（lib8tion 式 LUT）
- **假設**：生產者用多項式 sine／乘除；換 256-entry LUT（sin8、scale8_video、
  gamma8）可令 `render_us` 降 1.5–3×，低亮度更準。
- **改**：新增 `nc_px_math`（或 producer 內）LUT；替換 `nc_pixel_producer.c`
  的 sine／gamma／`x*y/255`。
- **量**：tests 映像 `0xF061` 的 producer `render_us`（20 顆與 64 顆）；
  固定輸入截圖比對（差異 ≤2/255）。
- **通過**：`render_us` ≤ 60% 原值；視覺差異 ≤2/255；host tests 綠。
- **風險**：現有 producer 只 64 顆，600 顆效果要另寫，唔好混為一談。
- [ ] E2

### E3 Binary dither（開抖動時）
- **假設**：FastLED `dither.h` 的 per-channel 16-bit 相位＋比較，比我們 per-zone
  error-feedback 便宜、無累積誤差。
- **改**：`nc_pixel_encode.c` 加 dither mode 3（binary）；`0xF062` 加一個
  dither flag（或分開 build）。
- **量**：host 測試 256 幀平均誤差；`0xF062` dither off/on 的 16×600 成本。
- **通過**：平均誤差 <1 LSB；成本 < +0.7 ms；無閃爍（人眼／錄影）。
- **風險**：模式切換要同步 config 與 benchmark。
- [ ] E3

### E4 RMT backend（可選，少 lane）
- **假設**：≤8 條 lane 時 RMT DMA 比 I80 省記憶體、延遲低（WLED 也是 RMT 優先）。
- **改**：新 `nc_pixel_backend_rmt.c`（`driver/rmt_tx.h`，WS2812 bytes encoder），
  按 lanes 選擇 backend。
- **量**：8×600 的 frame 時間／記憶體 vs I80；6×20 有無 glitch。
- **通過**：frame ≤ I80 +10%；RAM 槽 <50 KB；0 glitch。
- **風險**：RMT 8 通道限制、符號記憶體配置。
- [ ] E4

### E5 Chunked DMA ring（可選）
- **假設**：FastLED 用 3 環 chunk＋queue depth 2＋ISR 接力，唔需要整幀槽；
  可再省記憶體／PSRAM 流量。
- **改**：`drv_i80` 加 chunk 提交／完成接力；確保 chunk 之間空隙 < WS2812 reset。
- **量**：6×20 連續 10 分鐘 soak；16×600 合成 DMA 排程；記憶體用量。
- **通過**：無可見／解析 glitch；slot 記憶體 ≤1/3；OTA 中不 panic。
- **風險**：chunk 空隙過大 → 燈提早 latch；要示波器或長時間 soak。
- [ ] E5

### E6 WLED 功能（非效能）
- **ABL 電流限制**：逐 bus 估 mA、超限降亮度（供電安全）。
- **Per-pixel ColorOrderMap／RGBW auto-white**：混合燈帶／RGBW 才有感。
- **改**：config＋backend 上層；**量**：公式對照 WLED、frame time 不回歸 >5%。
- [ ] E6

### E7 真實 16 lane 驗證（blocked）
- 需要 16 條×600 顆實物；驗證 PSRAM DMA 槽、14.33 ms、50 fps、無撕裂。
- [ ] E7（blocked：硬件未到）

## 3. 實驗記錄表（每次一行）

| 日期 | commit | app hash | 實驗 | 改動 | 量測（16×600／其他） | 結果 | 判定 |
|---|---|---|---|---|---|---|---|
| 2026-11-19 | — | c0787583 | 基線 | FastLED spread LUT＋PSRAM 槽 | 14.33 ms（pack 7.07/merge 7.08） | 69.7 fps | ✅ |
|  |  |  |  |  |  |  |  |

判定：✅ 通過　⚠️ 有條件　❌ 回滾

## 4. 風險與回滾

- **OTA 慢**（6.1 KB/s）：每次上板 ~3 分鐘；先 `--no-apply` 驗 sha，再 APPLY。
- OTA 冇回覆 → 先 power cycle；**唔好**隨手用 esptool（USJ 易卡 download mode）。
- SDKCONFIG 實驗開關（`PXBENCH`）量完即刪，避免 release 意外帶測試指令。
- PSRAM DMA：64 B 對齊＋`esp_cache_msync(C2M)` 必須；有 glitch 先切回 internal 槽對照。
- host tests 339 係最低門檻；效果數學一改要同步 golden。

## 5. 參考

> 抄之前先睇設計理念衝突檢查：[pixel-design-vs-public.md](pixel-design-vs-public.md)。

- FastLED LCD clockless driver：
  https://github.com/FastLED/FastLED/blob/master/src/platforms/esp/32/drivers/lcd_spi/channel_driver_lcd_clockless.h
- FastLED spread transpose：
  https://github.com/FastLED/FastLED/blob/master/src/fl/channels/detail/bit_spread_lut.hpp
- FastLED wave3：
  https://github.com/FastLED/FastLED/blob/master/src/fl/channels/wave3.h
- FastLED 8-bit 數學（lib8tion）：
  https://github.com/FastLED/FastLED/tree/master/src/fl/math
- FastLED PSRAM DMA／cache sync：
  https://github.com/FastLED/FastLED/blob/master/src/platforms/esp/32/drivers/lcd_spi/lcd_spi_peripheral_esp.cpp.hpp
- WLED bus/driver 選擇：
  https://github.com/wled/WLED/blob/main/wled00/bus_manager.cpp
