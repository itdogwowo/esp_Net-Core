# ESP32-S3-SuperMini

| 項目 | 值 |
|---|---|
| 晶片 | `esp32s3`（ESP32-S3FH4R2，QFN56 rev v0.2） |
| Flash | 4 MB 內嵌（XMC）@ 80 MHz DIO |
| PSRAM | 2 MB 內嵌（AP_3v3）quad @ 80 MHz |
| CPU | 240 MHz（雙核） |
| Console | USB-Serial-JTAG |
| 硬體能力 | Wi-Fi（S3 原生 radio） |

## 這塊板子是什麼

一塊細小的 ESP32-S3 開發板，只有一個 USB-C，直接接到 S3 的原生 USB（GPIO19/20）——
**冇 USB-UART 橋接晶片**，所以 console 只能是 USB-Serial-JTAG（枚舉成 `303A:1001`）。

USB-OTG 與 USJ 共用同一條 FSLS PHY，**只能二選一**（同其他 S3 板一樣）。
換 personality 要改 `system.json` 的 `links[].kind` 再重啟。

esptool 實讀（2026-11-16）：

```text
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz,
                    Embedded Flash 4MB (XMC), Embedded PSRAM 2MB (AP_3v3)
USB mode:           USB-Serial/JTAG
MAC:                e4:b3:23:f9:38:68
Detected flash size: 4MB
```

## 驗證過什麼

- [ ] build
- [ ] 開機 log
- [ ] USB-Serial-JTAG 的 CDC 通 NC4
