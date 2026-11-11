# ESP32-S3-Matrix

Waveshare ESP32-S3-Matrix（SKU 27119）：4 MB flash、無 PSRAM、240 MHz，
板載 64 顆 WS2812B-0807（8×8，資料腳 GPIO14，NEO_RGB，800 kHz），Dout 可再串。

| 文件 | 內容 |
|---|---|
| `board.json` | 硬體事實（flash／PSRAM／CPU／console） |
| `sdkconfig.defaults` | flash 大小、無 PSRAM、4 MB 分割表、I80 腳位／8.9 MHz PCLK（Kconfig） |
| `partitions.csv` | 4 MB 分割表（app 1.5 MB × 2 ＋ FAT 896 KB） |
| `data/config/system.json` | CDC 一條通道 → decode0 |
| `data/config/tasks.json` | `rgbmatrix` 預設跑 core1 |

## 8×8 燈陣的 I80 腳位（為什麼是 10 支）

I80 需要 8 條資料線 + WR + DC 都是有效 GPIO；**只有 D0（GPIO14）真正接燈**，
其餘 9 支是 dummy（會出現在排針上，不要接東西）：

| 訊號 | GPIO | 說明 |
|---|---|---|
| D0 | 14 | WS2812 資料（板載 LED_DIN） |
| D1..D7 | 38, 39, 40, 41, 42, 47, 48 | dummy（將來的 8-lane 燈板用得到） |
| WR | 1 | dummy（I80 PCLK，4 MHz） |
| DC | 2 | dummy（固定低） |

I80 PCLK ＝ 8.9 MHz（÷9 = 8.889 MHz），WS2812 bit ＝ 0.9 µs（0 碼 337 ns、1 碼 675 ns）。

避開：IO10–13（QMI8658）、19/20（USB）、26–32（flash）、43/44（UART0）、
0/3/45/46（strapping）。改腳位只改 `CONFIG_NETCORE_RGB_MATRIX_I80_PINS`。

## 建置／燒錄

```bash
python3 make.py build s3-matrix --tests     # NC_ENABLE_TESTS=ON 才有 rgbmatrix
python3 make.py flash s3-matrix --tests
```

## 相關

- 原理與驗證：`docs/03_notes/rgb-matrix-dither.md`
- 指令：`0xF060 MATRIX`（`docs/01_protocol/commands.md`）
