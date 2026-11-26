# Pixel 輸出後端（pixel_encode_task 的硬件格式）

> 2026-11-25。第三層原本叫 `pixel_wave_task`；但「wave」只描述 WS2812 波形，
> 同一個任務其實要出多種硬件的線材格式，所以改名 `pixel_encode_task`（編碼）。

## 1. 角色

```text
pixel_paint_task 寫環（4 B/px：R,G,B,W）
        │
pixel_encode_task：環 → 該後端的格式（bus word／bitstream／RMT symbol／PARLIO word）
        │  只傳「格索引」隊列
render_task：DMA 完成還格 → submit → 硬件
```

`pixel_encode_task` **唔知**硬件長什麼樣：它只叫 `render` 對每條 lane 做
`acquire → encode → 入隊`；encode 由後端自己實作。新增一種硬件 = 新增一個
`nc_pixel_backend_t`，任務／`tasks.json` 不用改。

## 2. 支援矩陣

| backend 名 | 硬件 | IDF 驅動 | 狀態 |
|---|---|---|---|
| `i80` | LCD_CAM 8080（8／16 lane 並行） | `esp_lcd` + `drv_i80` | **已實作**（production） |
| `spi` | WS2801／APA102（MOSI+SCLK） | `drv_spi_led` | **已實作**（production） |
| `rmt` | WS2812／自訂波形（單線） | `esp_driver_rmt` | **未實作**（後端 adapter + pins 待做） |
| `parlio` | P4 並行輸出（1–16 data + clk） | `esp_driver_parlio` | **未實作**（後端 adapter + pins 待做） |
| `direct` | 軟件 relay（量測） | — | 已實作 |
| `mock` | 只計數（量 CPU） | — | 已實作 |

> `driver.json` 的 `LED.backend` 已經收 `rmt`／`parlio`（`ncportgen.py` 有 enum），
> 但 `setup_backends()` 會大聲講「未有 firmware 支援」並停用該 lane —— 唔會靜默
> 退到 mock。
>
> **I80 的腳位 schema**（稀疏 `data`、`-1`＝未用、自動 8↔16-bit、自動補 dummy、
> `wr`／`dc` 可省）見 [`pixel-i80-pin-schema.md`](pixel-i80-pin-schema.md)（2026-11-26 設計稿，未實作）。

## 3. 後端契約（`nc_pixel_backend_t`，見 `nc_render.h`）

每個後端要交：

- `acquire(post_seq)`：拿一條比 `post_seq` 新的環格（唔 release；hold 到 done）
- `encode(slot)`：把該格寫成硬件格式（bus word／symbol／bitstream）
- `submit(slot, seq)`：交給硬件（async；成敗要回報）
- `reclaim()`：**DMA／發射完成才** release 環格（提前還 = torn frame）
- `json()`：狀態（可選）

## 4. RMT（C 系列／S3）

- 每條 lane 一個 RMT TX channel；`rmt_new_bytes_encoder()`（或 bytes→symbol）出
  WS2812 波形。
- 線材 byte 次序由格式決定（WS2812 = GRB；WS2812W = GRBW）。
- 時序由 `driver.json` 的 LED 段（`format`／`clk_hz`／`bit_cells`／`t0h`／`t1h`）
  換成 RMT tick；reset ≥ 50 µs low。
- 為什麼重要：C3／C6／S3 冇 I80 那麼多 lane，RMT 係最直接的 WS2812 路徑。
- 腳位：RMT data GPIO list；`hardware.json` 未有 RMT typed 段，暫用 Kconfig／
  板預設過渡（同 I80 未遷移板一樣），最終要入 hardware 層。

## 5. PARLIO（P4）

- P4 的 `esp_driver_parlio`：`parlio_new_tx_unit()` 建 data_width（1–16）＋
  output clock；`parlio_tx_unit_transmit()` 收整段 bus word，完成 callback 還格。
- 編碼與 I80 相同：`nc_px_codec_pack_bus()` 逐 tick 出 bus word（8／16-bit），
  PARLIO 的 clock 就是 bit clock。
- 與 I80 的分別：PARLIO 係 P4 新增周邊，clock／valid 可配置，唔佔 LCD_CAM；
  將來 LCD 都可以用同一條 render 輸出層。
- 腳位：`data[]`＋`clk`；`hardware.json` 未有 PARLIO typed 段（待加）。

## 6. 未做（實作順序）

1. `drv_rmt` component（IDF `esp_driver_rmt`）＋ `nc_rmt_out.c` adapter；C3 板驗證。
2. `hardware.json` 加 `RMT` typed 段（`data[]`／`resolution`），`ncportgen` 生成。
3. `drv_parlio` component（IDF `esp_driver_parlio`）＋ `nc_parlio_out.c` adapter；
   P4 板驗證。
4. `hardware.json` 加 `PARLIO` typed 段（`data[]`／`clk`／`freq`）。
5. 兩者都要有 `bus_ok`／checksum 閘門與真機數字，才寫入 status「已支援」。
