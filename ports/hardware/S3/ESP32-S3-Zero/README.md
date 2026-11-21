# ESP32-S3-Zero

Waveshare **ESP32-S3-Zero**（ESP32-S3FH4R2：4 MB flash、2 MB PSRAM quad 80 MHz、
240 MHz，原生 USB Type-C）。本目錄是這個 repo 的硬體事實；改這裡要 rebuild。

| 文件 | 內容 |
|---|---|
| `board.json` | 硬體事實（flash／PSRAM／CPU／console／features） |
| `sdkconfig.defaults` | flash 4 MB、PSRAM、4 MB 分割表、I80 資料腳／時序 |
| `wiring.json` | 接線；LED = GPIO1，I2C disabled |
| `driver.json` | USB CDC、LED 宣告 |
| `partitions.csv` | 4 MB 分割表 |

## LED

- **資料腳：GPIO1**（用家指定，外部燈鏈）。板載 WS2812 在 **GPIO21**，本專案不用它。
- 預設當 **64 顆 WS2812（8×8）**；如果只接 1 顆，firmware 仍送 64 顆的資料，
  第一顆會正常顯示，鏈尾多出的資料會被忽略。
- I80 D0 = GPIO1；D1..D7 = 38,39,40,41,42,47,48；WR = 2；DC = 4。
  這些 dummy 腳會出現在排針，**不要接東西**。
- 格式 `ws2812`（GRB、無 W）；`white` 由效果層把 R=G=B。

## Console／USB

`console: none`，log 走 TinyUSB CDC；USB OTG FS = GPIO19/20。如果這塊板實際
只出 USB-Serial-JTAG，才改 console 與 sdkconfig（S3 的 OTG／USJ 共用 PHY，
只能二選一）。

## 建置

```bash
python3 make.py build ESP32-S3-Zero --project ESP32-S3-Zero-Show --fresh
```

## 已知假設

- flash 4 MB／PSRAM 2 MB：依 ESP32-S3FH4R2 規格；未在這塊板逐項實測。
- LED 64 顆：沿用 8×8 Show；如只接單顆，顯示仍可用。
