# ESP32-P4-FireBeetle2

| 項目 | 值 |
|---|---|
| 品牌／型號 | DFRobot FireBeetle 2 ESP32-P4（SKU **DFR1172**） |
| 舊名／別名 | `ESP32-P4-C6`、`P4-C6`（`aliases`，舊指令仍然 build 得到） |
| 板本 | v1.0（ESP32-P4 rev1.0 pre-rev3 ＋ 外掛 ESP32-C6） |
| 晶片 | `esp32p4`（ESP32-P4R32） |
| Flash | 16 MB @ 40 MHz DIO |
| PSRAM | 32 MB HEX @ 200 MHz |
| CPU | 360 MHz（官方板本；P4 rev1.0 配方見 `sdkconfig.defaults`） |
| Console | UART0（`GPIO37 TX / GPIO38 RX`）；USB-Serial-JTAG 是次 console |
| 能力碎片 | `wifi-hosted-c6`（`eth-rmii` 只保留 EMAC 編譯能力，runtime 關閉） |
| 尺寸 | 25.4 × 60 mm |

## 這塊板子是什麼

DFRobot 的 ESP32-P4 AI 視覺開發板，P4 做主要運算，外掛 **ESP32-C6** 提供
Wi-Fi 6／BLE（`esp_hosted`，經 SDIO）。板載 MIPI-DSI、MIPI-CSI（Raspberry Pi
排線相容）、PDM 麥克風、TF slot、24 支 GPIO。

### 兩個 USB-C 不是 interchangeable

| 接口 | 位置 | 控制器／PHY | 用途 |
|---|---|---|---|
| USB CDC | RST 按鈕旁 | USB-Serial-JTAG（FS_PHY1，GPIO24/25） | 供電、燒錄、log／console；枚舉 **303A:1001** |
| USB OTG 2.0 | 另一邊 | USB-OTG **HS**（UTMI，pin 49/50） | TinyUSB（CDC／MSC）；枚舉 **303A:4003** |

兩條 PHY 獨立，所以 OTG 在跑的時候 JTAG console 不會死；這是 P4 與 S3
最大差別（S3 兩者共用同一對 D+/D-，只能二選一）。

### 沒有乙太網

DFR1172 官方介面冇 Ethernet PHY（DFRobot wiki／cnx-software 都係咁寫）。
`board.json` 保留 `eth-rmii` 只為令 P4 的 EMAC 程式碼保持可編，
runtime 由 `wiring.json` 的 `ETH.enable=0` 明確關閉；`features.eth` 記為
`emac-only`，唔好再寫 `rmii-ip101`（呢個板冇 IP101）。

### Wi-Fi 走外掛 C6

`network.json` 的 `wifi` 段仍然係同一個 schema；真正 driver 是 `esp_hosted`
（`CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y`，見 `ports/_profiles/wifi-hosted-c6.defaults`）。
C6 的 slave firmware 要版本匹配；C6 經 SDIO 接 P4，TF slot 亦係 SDIO，
兩者腳位未從 schematic 逐條核對 → `SDIO.enable=0`。

## 驗證過什麼

- [x] build（2026-11-16，IDF 6.1；見 `docs/00_pm/status.md` §7.65）
- [x] USB OTG HS 枚舉（`303A:4003`，2026-11-11）
- [x] Wi-Fi hosted C6 連 AP＋HTTP/TCP 小量 request（2026-11-16；burst 被 SDIO staging 卡住，
      見 `docs/03_notes/p4c6-wireless-test-2026-11-16.md`）
- [x] UART0 console／USJ OTA
- [ ] TF slot 腳位
- [ ] MIPI-DSI／CSI 面板（`driver.json` 未接 runtime）

## 相關

- USB 事實與模型：`docs/03_notes/usb-config.md`、`docs/03_notes/p4-usb.md`
- 無線測試：`docs/03_notes/wifi-stability-2026-11-16.md`、`docs/03_notes/p4c6-wireless-test-2026-11-16.md`
- 命名公約：`ports/README.md` §10
