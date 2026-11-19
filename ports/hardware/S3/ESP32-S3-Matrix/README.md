# ESP32-S3-Matrix

Waveshare ESP32-S3-Matrix（SKU 27119）：**ESP32-S3FH4R2 → 4 MB flash ＋ 2 MB PSRAM
（quad 80 MHz）**、512 KB SRAM、240 MHz；板載 64 顆 WS2812B-0807（8×8，資料腳
GPIO14，NEO_RGB，800 kHz），Dout 可再串；板載 QMI8658 IMU（I2C）。

| 文件 | 內容 |
|---|---|
| `board.json` | 硬體事實（flash／PSRAM／CPU／console／features） |
| `sdkconfig.defaults` | flash 大小、PSRAM（quad 80 MHz）、4 MB 分割表、I80 腳位／2.5 MHz PCLK（Kconfig） |
| `wiring.json` | 接線（LED／I2C 的 IMU／USB）；I2C 目前只是文件影子（`nc_cfg` 未 parse） |
| `partitions.csv` | 4 MB 分割表（app 1.5 MB × 2 ＋ FAT 896 KB） |

## 模組與記憶體

| 項目 | 值 | 備註 |
|---|---|---|
| 模組 | ESP32-S3FH4R2 | 4 MB flash ＋ 2 MB PSRAM（quad） |
| CPU | 240 MHz | 已最高 |
| PSRAM | 2 MB quad，**80 MHz** | 實際上限定，見下節 |
| Flash | 4 MB，DIO 80 MHz | `partitions.csv` 也是 4 MB |
| Console | none | log 走 TinyUSB CDC；USB OTG FS＝GPIO19/20 |

### 為什麼 PSRAM 是 80 MHz，不是 120 MHz

IDF 的 Kconfig 寫「Quad PSRAM 120 MHz is stable」，但那是晶片層；實際受 **flash 與
PSRAM 的 MSPI 時鐘組合**限制：

- S3 的 timing 表只有 `CORE_CLK_120M_MODULE_CLK_120M_STR_MODE`，**沒有 core 120／
  module 80** 這一格。
- 即 PSRAM 要 120 MHz，flash 也要一起 120 MHz；這塊板 flash 規格是 80 MHz（spec 外）。

實測（2026-11-19）：只加 `CONFIG_SPIRAM_SPEED_120M=y` → 編譯失敗
（`MSPI_TIMING_FLASH_CONFIG_NUM_CORE_CLK_120M_MODULE_CLK_80M_STR_MODE undeclared`）。
**所以 80 MHz 是上限，不是保守調低。**

## 8×8 燈陣：I80 腳位

I80 需要 8 條資料線 + WR + DC 都是有效 GPIO；**只有 D0（GPIO14）真正接燈**，
其餘 9 支是 dummy（會出現在排針上，不要接東西）：

| 訊號 | GPIO | 說明 |
|---|---|---|
| D0 | 14 | WS2812 資料（板載 LED_DIN） |
| D1..D7 | 38, 39, 40, 41, 42, 47, 48 | dummy（將來的 8-lane 燈板用得到） |
| WR | 1 | dummy（I80 PCLK） |
| DC | 2 | dummy（固定低） |

現行渲染器設定（build-time，`sdkconfig.defaults`）：

```ini
CONFIG_NETCORE_PIXEL_LEDS=64
CONFIG_NETCORE_PIXEL_I80_LANES=1
CONFIG_NETCORE_PIXEL_I80_PINS="14,38,39,40,41,42,47,48,1,2"
CONFIG_NETCORE_PIXEL_PCLK_HZ=2500000
CONFIG_NETCORE_PIXEL_BIT_CELLS=3
```

上板驗證（2026-11-19，`ESP32-S3-Matrix-LED-Textkit` 映像 `1e23943d`）：
`0xF061` → `lanes=1、px=64、fps=50、enc_us=348`。

避開：IO10–13（QMI8658）、19/20（USB）、26–32（flash）、43/44（UART0）、
0/3/45/46（strapping）。

## IMU：QMI8658（**未接驅動，先記錄**）

| 項目 | 值 | 來源 |
|---|---|---|
| 型號 | QMI8658（QST，6 軸） | vendor demo `WS_QMI8658.cpp` |
| 介面 | I2C，400 kHz | `Wire.begin(11, 12)` |
| **SDA／SCL** | **GPIO11／GPIO12** | `#define I2C_SDA 11`／`I2C_SCL 12` |
| 位址 | `0x6A`（SA0 低）／`0x6B`（高） | demo 用 `QMI8658_L_SLAVE_ADDRESS` |
| WHO_AM_I | `0x00`，慣例 `0x05` | **未在板上實測**；寫 driver 前先讀一次確認 |

demo 初始化（可照抄）：`Wire.begin(11,12)` → `QMI.begin(Wire, 0x6A, 11, 12)` →
`configAccelerometer(4G, 1000Hz, LPF_MODE_0, true)` →
`configGyroscope(64DPS, 896.8Hz, LPF_MODE_3, true)` → `enableGyroscope/Accelerometer`。

**現況**：`wiring.json` 的 I2C 段已填 `sda:11／scl:12／enable:1`，但 `nc_cfg` 還未
型別化 I2C → **執行期不會初始化，GPIO11／12 現在仍是自由腳**。寫 driver 時先
`nc_hw_claim(11/12)`，再 `i2c_master_bus_add_device(0x6A)`。

## USB：OTG MSC

S3 的 OTG 與 USJ 共用同一條 PHY，只能二選一。這塊板用 **TinyUSB OTG**：

- `system.json`：USB link `{kind:"usb",index:0,msc:1}`（CDC log ＋ MSC 隨身碟）
- `sdkconfig.defaults`：`CONFIG_ESP_CONSOLE_NONE=y`、`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
- 上板後：`NC-xxxx` MSC 碟（896 KB）＋ CDC serial（VID 303A:4003）；bootloader 早期
  log 不會有，app log 由 CDC（B09 之後）出
- **燒錄**：app 執行時 USJ 不在 → esptool 看不到 port。用 **OTA（NC4 0x22xx，經 CDC）**
  或**按住 BOOT＋RESET** 入 download mode。

要回到 USJ console（MSC 失效）：`system.json` 的 USB link 改回 `usb_jtag`、`msc=0`、
`sdkconfig.defaults` 改 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`，再 `--fresh` 重建。
USB 配置地圖見 [docs/03_notes/usb-layers.md](../../../../docs/03_notes/usb-layers.md)。

## 建置／燒錄

```bash
python3 make.py build matrix-textkit --tests     # 專案 ESP32-S3-Matrix-LED-Textkit
python3 make.py build s3-matrix                  # 板子的預設 project
```

## 相關

- 原理與驗證：`docs/03_notes/rgb-matrix-dither.md`
- 指令：`0xF061 RENDER`（`docs/01_protocol/commands.md`）
- 配置分層：`docs/03_notes/config-layers.md`
