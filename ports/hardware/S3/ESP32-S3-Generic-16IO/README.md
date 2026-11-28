# ESP32-S3-Generic-16IO（功能測試：16 條獨立 LED lane）

> **用途**：測試「每條線獨立內容」功能；16 條 I80 直驅 WS2812 lane，每條 **16×16＝256 px**。
> GPIO 用 Zero 可引出的 16 支非保留腳：**1,2,4,5,6,7,8,9,10,11,12,13,14,15,21,38**
> （避開 3／19-20 USB／26-37 flash+PSRAM／43-46 UART/strapping）；已實測 OTA 上 Zero。
> project：[`ports/project/S3/ESP32-S3-Generic-16IO/`](../../../project/S3/ESP32-S3-Generic-16IO/)

## 內容

| 檔案 | 內容 |
|---|---|
| `board.json` | S3、4 MB flash、2 MB PSRAM、240 MHz、console none |
| `wiring.json` | I80 16-bit：16 支 data = GPIO1,2,4..17；USB OTG |
| `driver.json` | `LED_WS2812.list` 16 條線（index 0..15、px 256、GRB） |
| `sdkconfig.defaults` | 16 lane／16-bit 直驅、2.5 MHz×3 cell、RING/PACK PSRAM、CONTENT |
| `partitions.csv` | 4 MB（app 1.5 MB ×2＋FAT 896 KB） |

## 建置

```bash
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Generic-16IO \
  --project ESP32-S3-Generic-16IO --fresh
```

- 16×600 的同類極限 benchmark 見 status §7.311；呢個測試係 16×256（4096 px）。
- 內容：每條線一個唔同 effect（16 個），其中 4 個 `end_Time` 8,192／16,384／32,768 幀做長時間測試。
- 亮度刻意調低（l_max 64–384／4096），避免 4096 顆全亮電流。
- 已 OTA 上 Zero（16IO image）：`act.match=1`、四個 pixel task running、無 crash；
  但未有 16 條實體燈鏈，所以視覺效果未驗。上板時要再確認 level shifter、電源、reset gap
  同 16 條線的實際排列。
