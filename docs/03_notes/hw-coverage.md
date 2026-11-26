# 硬件覆蓋率：大表 vs 三顆晶片的真實資源

> 2026-11-27。用家要求：「有什麼硬件要判斷，做一張列表，看現在的設定能否完整支援覆蓋」。
> 呢份係**對照結果**：[`hw-matrix.md`](hw-matrix.md)（380 key 的大表）＋
> [`schema/state-keys.json`](../../schema/state-keys.json)（現時 25 key）到底覆蓋咗幾多。
>
> 晶片數字全部由官方文件核對（datasheet／TRM／ESP-IDF v6.1.1 API 指南／`soc_caps.h`），
> 來源見 §7。**呢份唔係估，係抄官方的「週邊例化數」**（instantiated controllers），
> 唔係 GPIO matrix 可映射的訊號數。

---

## 1. 結論（先睇呢五句）

1. **大表的形狀是對的**，但有 **4 個單元數量寫錯**（I80 2→1、I2S 1→2/3/1、ETH 在 S3 根本冇
   RMII、PIN 48 只對 S3 而 S3 實際 45／P4 55）。
2. **有一個真 bug 級風險**：`ports/project/S3/*/data/config/hardware.json` 有
   `"ETH": {"driver": "rmii"}`——S3 **冇內建 MAC**，enable 咗會 build／開機失敗。
3. **大表缺 9 類晶片資源**：RMT／PCNT／MCPWM／ADC／TWAI／PARLIO／LCD(RGB)／touch／camera。
   全部都是「有 index 維度」的資源，正好係你講要用 index byte 嘅原因。
4. 加上缺的 9 類 ≈ **+185 key**（總數 565），worst-case blob ≈ **5.1 KB**；
   但 store 只載**已啟動**的 unit（典型 30–60 key），所以 SRAM 仍然細。
5. **activation 必須由 build-time 搬去 boot-time**，否則「enable=0 零成本」只係生成期的
   巧合，唔係 runtime 保證（現時 `nc_port_activation()` 只有 `status` 用）。

---

## 2. 三顆晶片資源（官方數字，2026-11-27 核對）

| 資源 | S3 | P4 | C3 | 對 schema 的意義 |
|---|---:|---:|---:|---|
| GPIO（可程式） | **45**（0–21、26–48） | **55**（0–15 LP＋16–54 HP） | **22**（部分封裝 16） | index 要 ≥55；**腳號→功能表要三份** |
| 扣 flash/PSRAM 後可用 | ≈38（octal 33） | 55（flash 用專用腳） | 16（in-package flash） | `pin_count` 要 per-chip |
| LEDC（PWM） | **8 通道／4 timer**、14-bit | **8／4**、**20-bit** | **6／4**、14-bit | 表寫 8；C3 只有 6 → 要 per-chip max |
| RMT | **4TX＋4RX** | **4TX＋4RX** | **2TX＋2RX** | 大表**冇**；C3 只有 4 |
| PCNT | **4 unit** | **4 unit** | **0** | 大表**冇**；C3 要 0 |
| MCPWM | **2 unit**（12 路） | **2 unit**（12 路） | **0** | 大表**冇** |
| I2C | **2** | **3**（2 HP＋1 LP） | **1** | 表寫 2；P4 3／C3 1 |
| SPI（可當 master） | **2**（SPI2/3） | **2**（＋LP-SPI） | **1**（SPI2） | 表寫 2 ✔（C3 要 1） |
| UART | **3** | **6**（5 HP＋1 LP） | **2** | 表寫 3；P4 6／C3 2 |
| I2S | **2** | **3**（＋LP RX） | **1** | 表寫 **1 ✗**（S3 已經有 2） |
| ADC | **2 unit／12-bit／20 通道** | **2／12-bit／14 通道** | **2／12-bit／6 通道** | 大表**冇** |
| DAC | **冇** | **冇** | **冇** | 三顆都冇（只有舊 ESP32/S2） |
| TWAI／CAN | **1** | **3** | **1** | 大表**冇**；全部要外加 transceiver |
| SDIO／SDMMC | **1 host／2 slot**、1/4/8-bit | **1 host／2 card** | **0** | 表寫 1 ✔；C3 0 |
| Ethernet | **冇 MAC**（只可 SPI 外置 PHY） | **內建 MAC**、MII/RMII | **冇** | **S3 寫 rmii = 錯** |
| USB | OTG FS ＋ Serial-JTAG（**共用 PHY，二選一**） | HS OTG ＋ FS OTG ＋ JTAG（**可同時**） | **只有 Serial-JTAG** | device 表的 `usb 2` 要分型 |
| LCD_CAM／I80 | **1**（8–16-bit；8-bit<80 MHz、16-bit<40 MHz） | **1**（8/16/24-bit） | **0** | 表寫 **2 ✗** |
| PARLIO | **0** | **1**（1/2/4/8/16-bit，≤40 MHz） | **0** | 大表**冇**（P4 專有） |
| 電容觸摸 | **14** | **14**（＋proximity） | **0** | 大表**冇** |
| 溫度感測器 | 1（內建） | 1（內建） | 1（內建） | 唔使 key（唯讀） |
| deep-sleep GPIO | **22** | **16**（LP only） | **6**（但冇 `rtc_gpio_*` API） | 喚醒腳要 per-chip |

> 「可映射」≠「多一個」：三顆的數位週邊都經 GPIO matrix 任意 route，但**控制器數量係硬例化**，
> 所以 schema 的 `count` 一定要 per-chip；用最大值會喺 C3 靜靜越界（違反鐵律 9）。

---

## 3. 大表 380 key：逐個 unit 對照

| unit（大表） | 通道上限 | 真實上限（S3/P4/C3） | 判斷 | 要做 |
|---|---:|---|---|---|
| `sys.system` | 1×12 | — | ✔ | 補 links／routes／tasks（現時唔在表） |
| `hw.pin` | **48**×3 | 45／55／22 | ⚠ 48 只啱 S3 一半 | 改 `pin_count` per-chip；表用 56 slot |
| `hw.pwm` | 8×2 | 8／8／6 | ✔（C3 要 6） | 補 `resolution_bits` 上限（14 vs 20） |
| `hw.uart` | 3×6 | 3／6／2 | ⚠ 少過 P4 | 上限改 6（C3 落 2） |
| `hw.spi` | 2×19 | 2／2／1 | ✔ | —— |
| `hw.i2c` | 2×3 | 2／3／1 | ⚠ P4 3 | 上限改 3 |
| `hw.i2s` | 1×5 | **2／3／1** | ✘ 少 | 上限改 3 |
| `hw.sdio` | 1×8 | 1／1／0 | ✔ | C3 要 0 |
| `hw.i80` | **2**×22 | **1／1／0** | ✘ 多 | 改 1 |
| `hw.eth` | 1×8 | **0**／1／0 | ✘ S3 冇 | `driver` 加閘：S3 只准 `spi` |
| `hw.enc` | 1×2 | （軟件） | ✔ | —— |
| `dev.usb` | 2×4 | 2／2／1 | ✔ | 要分 `otg`／`jtag` 型 |
| `dev.led` | 2×10 | 掛 bus | ✔ | —— |
| `dev.apa102` | 2×5 | 掛 SPI | ✔ | —— |
| `dev.pca9685` | 2×4 | 掛 I2C | ✔ | —— |
| `dev.tft` | 1×8 | 掛 I80/SPI | ✔ | 加 RGB 型（P4 24-bit） |
| `dev.xl9555` / `gt1151q` / `husb238` / `pcm5102` / `uart_motor` / `sd` / `audio` | 1–2×2–5 | 掛 bus | ✔ | —— |

**合計 380 key；其中 4 個 unit 的「通道上限」要改（i80／i2s／uart／i2c／pin／eth）。**

---

## 4. 大表缺的 9 類（建議新增 unit）

| 新 unit | 通道上限 | 欄位 | keys | 來源 |
|---|---:|---:|---:|---|
| `hw.rmt` | 8（C3 4） | 4（gpio／mode／mem_block／dma） | 32 | 遙控／WS2812／IR |
| `hw.pcnt` | 4（C3 0） | 3（gpio／ctrl／unit） | 12 | 編碼器／轉速 |
| `hw.mcpwm` | 2（C3 0） | 6（unit／timer／gpioA／gpioB／freq／duty） | 12 | 馬達／舵機 |
| `hw.adc` | 20（P4 14／C3 6） | 3（unit／channel／atten） | 60 | 感測 |
| `hw.twai` | 3（S3/C3 1） | 5（tx／rx／bitrate／mode／enable） | 15 | CAN |
| `hw.parlio` | 1（P4 專有） | 6（data_width／clk／data[]／freq） | 6 | 多 lane 輸出 |
| `hw.lcd` | 1 | 12（data 16/24／hsync／vsync／de／pclk／freq） | 12 | RGB 屏（P4） |
| `hw.touch` | 14（C3 0） | 2（gpio／threshold） | 28 | 觸控 |
| `hw.cam` | 1 | 8（data／pclk／vsync／href／xclk） | 8 | DVP／CSI |
| **合計** | | | **+185** | 380 → **565 key** |

**成本**（用 2026-11-27 的 u32 id：key 3 B varint＋值 ≤5 B）：

| 情境 | keys | blob |
|---|---:|---:|
| 典型（只開 I80＋LED＋PIN 20） | 52 | 12＋52×9 ≈ **480 B** |
| 全部 565 key 非預設（worst） | 565 | **≈ 5.1 KB** |

- FAT 上完全冇問題（現在 50 B）。
- 若日後想放 NVS（單 blob 4 KB）就要拆兩個 blob 或壓縮——**而家用 FAT，唔急**。
- **store 只載已啟動 unit**：典型 30–60 key × 32 B ≈ 1–2 KB SRAM；現時上限 64 筆
  （2 KB）**夠用但冇餘裕**，建議 S4 時一齊提到 96–128 筆（3–4 KB）。

---

## 5. 對 schema 的硬性要求（由上面的差異推出來）

1. **capability 表（每顆晶片一份）**：`{unit, max_channels, max_field_width...}`；
   唔存在的 unit 直接 `max=0`，生成期就擋（C3 開 RMT ch5 應該 build／parse 就錯）。
2. **per-chip pin 表**：S3 有 22–25 空缺、P4 有 34–38 strapping、C3 只到 21；
   **同一個 index 唔代表同一支腳**，腳號→功能要 per-chip。
3. **activation 由 boot-time 決定**（`enabled = cfg.enable && deps_ok`），
   表只登錄「呢個 firmware 支援咩」；store 只載 enabled 的 unit。
4. **數字上限 per-chip**：LEDC duty 14 vs 20 bit、LCD 16 vs 24-bit、SPI 80 MHz（P4 LP 40）…
   用單一最大值 = 靜默越界。
5. **`hw.eth` 要加 driver 閘**：S3 只准 `spi`（W5500 類），`rmii` 只 P4。
   → 直接影響 `ports/project/S3/*/hardware.json` 現有的 `"driver": "rmii"`（enable=0 所以
   未爆，但一 enable 就會出事）。

---

## 6. 建議落地次序（接到 state 計劃的 S 編號）

| 步 | 內容 | 驗收 |
|---|---|---|
| **S4a** | `nc_cfg_chip` 加 capability（每顆的 unit 上限）；`hw.eth` 加 driver 閘 | C3 寫 RMT ch5 → 明示錯；S3 寫 `driver:"rmii"` → 明示錯 |
| **S4b** | activation 搬 boot-time；store 只載 enabled | disable unit → `state.n` 減；set 該 unit → `not_active` |
| **S5a** | 大表補 9 類（一個 unit 一 commit＋host test） | 565 key、`schema.bad=0` |
| **S5b** | store 上限 64 → 128（量 SRAM） | `status mem` 見到、`h_int` 仍然 >150 KB |
| **S6** | 生成模板 JSON（全 `enable:0`）＋`preset_check` 驗證 | 模板可以 seed 上板 |

---

## 7. 來源（全部 2026-11-27 抓取）

- **Datasheet**：[S3 v2.2](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)、
  [P4 v0.7（pre-release）](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf)、
  [C3 v2.5](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
- **TRM**：[S3 v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)（§29 LCD_CAM、§32 USB）、
  [P4 v1.0](https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf)（§41 LCD_CAM、§53 PCNT、§62 RMT）
- **ESP-IDF v6.1.1 API**：peripheral index 與各週邊頁（`/esp32s3/`、`/esp32p4/`、`/esp32c3/`），
  例如 [ledc](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/ledc.html)、
  [rmt](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/rmt.html)、
  [i2s](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/i2s.html)
- **soc_caps.h（v6.1.1）**：[S3](https://github.com/espressif/esp-idf/blob/v6.1.1/components/soc/esp32s3/include/soc/soc_caps.h)、
  [P4](https://github.com/espressif/esp-idf/blob/v6.1.1/components/soc/esp32p4/include/soc/soc_caps.h)、
  [C3](https://github.com/espressif/esp-idf/blob/v6.1.1/components/soc/esp32c3/include/soc/soc_caps.h)

> 未確認：P4 24-bit LCD 的最高 PCLK（TRM 只列 8/16-bit 的 GDMA 限制）；
> S3 datasheet 寫 40 MHz，TRM v1.8 寫 8-bit <80 MHz／16-bit <40 MHz——兩者並列，實測再定。
