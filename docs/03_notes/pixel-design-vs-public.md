# pixel 設計理念 vs FastLED／WLED：衝突檢查

> 2026-11-19。目的：抄演算法之前，先確認唔會撞爛我們的分層、配置與失敗政策。
> 結論先講：**演算法可以抄，架構唔可以抄；三條紅線見 §6。**

## 1. 我們的原則（出處）

| 原則 | 出處 |
|---|---|
| **七區分層、箭頭只能向下**（hw→sys→sw→action/tasks） | [component-layout.md](component-layout.md) §1 |
| **一個裝置／bus 只有一個 owner**；任務唔准自己 `xTaskCreate` | [contracts.md](contracts.md) §1 |
| **硬件事實 build-time**（flash/PSRAM/CPU/console/partition/target 由 sdkconfig 決定） | [config-layers.md](config-layers.md)、`tools/board.py check` |
| **單一失敗不能變磚**：看得見、可回復、逐項隔離 | [failure-policy.md](failure-policy.md) |
| **sw 可 host 測試**（`tools/hosttest/pixel_test` 339 assertions，純 C） | `tools/hosttest/pixel_test` |
| 熱路徑整數、預配置、少 heap；效果（pixel_task）與渲染（render_task）分離 | [pixel-renderer.md](pixel-renderer.md) |

## 2. FastLED 的理念

- Arduino/PlatformIO **library**；平台與燈型用 compile-time template／macro
  （`addLeds<WS2812, PIN, GRB>`）；
- **channel driver 抽象**＋worker task＋ISR 接力；driver 擁有排程與 encode；
- 熱路徑 `FL_IRAM`；PSRAM DMA＋`esp_cache_msync`；
- 容器用 `fl::vector`／`shared_ptr`（少量 heap，初始化為主）；
- `show()` 由 application 呼叫，library 唔負責 FPS。

## 3. WLED 的理念

- **完整 application firmware**（唔係 library）：web UI／JSON 設定；
- `BusManager`＋NeoPixelBus；RMT／並行硬件 driver **runtime 選擇**；
- 一支 bus 失敗 → `BusPlaceholder` 保留設定，重開機再試；
- 功能層：segments／palettes／ABL 電流限制／per-pixel color order／RGBW+CCT。

## 4. 衝突矩陣

| 議題 | 我們 | FastLED | WLED | 衝突？ | 決定 |
|---|---|---|---|---|---|
| **分層／依賴** | 七區單向；sw 純 C 可 host test | 大量 template／`fl::`；平台 header 交織 | 單體 application | ⚠️ 高 | **只重寫演算法入 sw，唔 link FastLED／WLED** |
| **硬體設定** | build-time（sdkconfig／project） | compile-time | runtime JSON／web | ⚠️ 高 | 保持 build-time；**唔抄 WLED runtime 重配 pin** |
| **工作擁有權** | render_task 擁有 encode＋排程；driver 薄 | driver worker＋ISR 排程 encode | BusManager 呼叫 library | ⚠️ 中 | chunked DMA 由 **render task 分塊 encode/提交**，driver 只排隊＋完成通知 |
| **記憶體類別** | 明示 internal／PSRAM／DMA＋plan | PSRAM 優先、失敗退 internal | `memUsage()` 估算 | 🟡 低 | 可以 fallback，但**要 log＋可配置**，唔好靜默 |
| **色彩模型** | canonical R,G,B,W（4 B/px），codec 丟 W | CRGB／RGBW template | RGBW＋CCT＋auto-white＋ABL | ⚠️ 中 | 保持 canonical；W／CCT／ABL 喺 **producer 上層**轉換，codec 保持格式表 |
| **FPS／時間** | render_task `frame_interval_ms`（50 fps） | app 自己 call `show()` | 主循環固定 rate | ✅ 無 | 保持我們的節拍擁有權 |
| **抖動** | 實驗性、預設關 | library 內建 dithering | PWM dithering | 🟡 低 | 抄演算法做 option，**預設仍然關** |
| **IRAM** | 大部分 code 在 flash（XIP） | 熱路徑全 IRAM | 部分 IRAM | ⚠️ 中 | 只搬 ISR／最熱小段；**表要放 RAM**；量 IRAM 預算 |
| **測試** | host 339 assertions，純 C | unit tests 依賴 Arduino stub | 無 host 測試 | ⚠️ 高 | 演算法重寫＋加 host test；**唔引入 dependency** |
| **失敗政策** | 不變磚、可見、可回復、逐項隔離 | 回傳 error；library 唔管 | placeholder bus 保留設定 | ✅ 目標一致 | 機制唔同：我們保留明確 log／status，**唔靜默降級** |
| **動態重配** | project 固定；OTA 換 app | compile-time | runtime 改 bus | ⚠️ 高 | 唔抄 runtime 硬件重配 |

## 5. 對實驗清單（E1–E6）的判定

| 實驗 | 判定 | 要點 |
|---|---|---|
| **E1 IRAM** | ⚠️ 局部做 | 只搬 `on_color_done`（必要時 merge 最熱段）；`s_spread_nibble` 等**表放 RAM**；`idf.py size` 量 IRAM 預算（WiFi 要用） |
| **E2 lib8tion 數學** | ✅ 做 | C 重寫 sin8／scale8_video／gamma8 LUT；唔引入 FastLED header；效果層（producer）改，codec 不動 |
| **E3 binary dither** | ✅ 做（option） | 保持 `CONFIG_NETCORE_PIXEL_DITHER=0` 預設；用 benchmark flag 量 |
| **E4 RMT backend** | ✅ 做（可選） | 新 backend 實作 `nc_pixel_backend_t`；driver 生命週期仍由 hw／project 擁有；唔抄 FastLED 動態 bus 重配 |
| **E5 chunked DMA** | ⚠️ 要改設計 | **render task 分塊 encode＋提交**，driver 只排隊、完成回呼；**唔好**把 encode 搬入 ISR（違反「任務擁有工作」） |
| **E6 ABL／color order** | ✅ 功能層 | 放 producer／render 上層；ring 仍然 canonical RGBW；ABL 只降亮度，唔改 codec |

## 6. 三條紅線

1. **唔引入 FastLED／WLED 作為 dependency**：演算法（LUT／轉置／dither／ABL 公式）
   用純 C 重寫入 `sw/pixel`，保持 host tests 可跑。
2. **硬件事實保持 build-time**：唔加 runtime pin／bus 重配；project 換硬件＝換
   `sdkconfig.defaults`／`wiring.json`，唔係 runtime JSON。
3. **encode／排程留在 render_task**：driver 只擁有 DMA 與完成通知；任何「chunked／
   ISR 接力」都要以 render task 分塊提交的形式落地，唔可以拉 encode 入 driver/ISR。

## 7. 參考

- FastLED LCD clockless driver：
  https://github.com/FastLED/FastLED/blob/master/src/platforms/esp/32/drivers/lcd_spi/channel_driver_lcd_clockless.h
- FastLED 8-bit 數學：https://github.com/FastLED/FastLED/tree/master/src/fl/math
- FastLED spread transpose：
  https://github.com/FastLED/FastLED/blob/master/src/fl/channels/detail/bit_spread_lut.hpp
- WLED bus manager：
  https://github.com/wled/WLED/blob/main/wled00/bus_manager.cpp
