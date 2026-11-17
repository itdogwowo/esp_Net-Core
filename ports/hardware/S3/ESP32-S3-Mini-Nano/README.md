# ESP32-S3-Mini-Nano

| 項目 | 值 |
|---|---|
| 品牌／型號 | （白牌）ESP32-S3 Mini Nano；模組 **S3-N16R8** |
| 別名 | `S3-Mini-Nano`（`aliases`） |
| 晶片 | `esp32s3`（Xtensa LX7 雙核） |
| Flash | 16 MB @ 80 MHz DIO |
| PSRAM | 8 MB octal @ 80 MHz |
| CPU | 240 MHz |
| Console | none（OTG MSC＋CDC log；USJ 與 OTG 共用 PHY） |
| 能力 | Wi-Fi ＋ BLE 5（S3 原生 radio）；無 Ethernet |

## 這塊板子是什麼

AliExpress 上常見的「ESP32-S3-MINI Nano / S3-N16R8」Nano 尺寸開發板：
16 MB flash ＋ 8 MB octal PSRAM，Type-C，排針引出大部分 GPIO。
同一塊板有幾個不同賣家版本，**USB 接法與腳位要先確認**再寫進 `wiring.json`。

### 兩個要實測的點

1. **USB 是不是原生 USB**：2026-11-16 插上電腦見到
   `/dev/cu.usbmodemD885ACD42F2C1`（不是 CH340／CP210x 的 `usbserial`），
   所以是 S3 原生 USB；可以走 OTG。
2. **腳位**：板上只有 USB／按鈕／LED 的話 `wiring.json` 可以全部 `enable=0`；
   要接 WS2812、SD、SPI 屏之前，先量清楚再填，不要照其他 S3 板抄。
   （S3 的 USB-OTG 與 USJ 共用同一對 D+/D-，只能二選一。）

## USB：OTG MSC（2026-11-16 起）

S3 的 OTG 與 USJ 共用同一條 PHY，只能二選一。這塊板現在用 **TinyUSB OTG**：

- `system.json`：`links[].kind="cdc"`、`usb.msc=1`、`usb.cdc_log=1`；route `cdc0→decode0`
- `sdkconfig.defaults`：`CONFIG_ESP_CONSOLE_NONE=y`、`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
- 16 MB flash 用預設 `firmware/partitions.csv` → `data` 7 MB，主機見到約 7 MB 的 `NC-xxxx` 碟
- 上板後預期：MSC 碟 ＋ CDC serial（app log）；**bootloader 早期 log 冇**
- `msc=1` 時 B10 把 FAT 交 host；device 只剩唯讀 `/ro`

要回到 USJ console（MSC 會失效）：link 改回 `usb_jtag`、`usb.msc=0`、
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`，再 `make.py build ESP32-S3-Mini-Nano --fresh`。
完整 USB 配置地圖見 [docs/03_notes/usb-layers.md](../../../../docs/03_notes/usb-layers.md)。

## 驗證過什麼

- [ ] `python3 tools/board.py check`（建 port 時已通過）
- [ ] build
- [ ] 上板：開機 log／`esptool chip_id`
- [ ] USB 型態與 console 確認

## 建置

```bash
python3 make.py build ESP32-S3-Mini-Nano
python3 make.py flash ESP32-S3-Mini-Nano
```

## 相關

- 命名公約：`ports/README.md` §10
- 同晶片參考：`ports/hardware/S3/ESP32-S3-N16R8/`（同 16 MB ＋ 8 MB 配置，console 已驗證）
