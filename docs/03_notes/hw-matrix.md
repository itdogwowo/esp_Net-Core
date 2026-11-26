# 硬件矩陣：所有支援硬件＋所有 IO 位置（上限可算）

> 2026-11-26。用家設計：
> 1. **JSON 列出所有支援硬件**，預設 `"enable": 0`（＝可選清單／快速選配）；
> 2. 每個硬件有固定「通道上限＋欄位」，用戶通常只填 **GPIO ＋少量數值**；
> 3. 硬件＋IO 位置列成矩陣 → **key 數有限** → 儲存上限**計得出**，唔使估；
> 4. GPIO 用 **48-bit bitmap**（bit0..47），enable 時佔 bit → 衝突檢查 O(1)。

---

## 1. 矩陣（380 keys，覆蓋現時所有支援硬件）

| unit | 通道上限 | 欄位 | keys | 備註 |
|---|---:|---:|---:|---|
| `sys.system` | 1 | 12 | 12 | version／frame_interval／log… |
| `hw.pin` | **48** | 3 | **144** | 全 GPIO 位置矩陣（GPIO↔用途） |
| `hw.pwm` | 8 | 2 | 16 | |
| `hw.uart` | 3 | 6 | 18 | |
| `hw.spi` | 2 | **19** | **38** | 舊專案特別：多線 `data0..7`＋`force_machine`（見 §1.1） |
| `hw.i2c` | 2 | 3 | 6 | |
| `hw.i2s` | 1 | 5 | 5 | |
| `hw.sdio` | 1 | 8 | 8 | |
| `hw.i80` | 2 | 22 | 44 | 含 16 條 data 位置 |
| `hw.eth` | 1 | 8 | 8 | |
| `hw.enc` | 1 | 2 | 2 | |
| `dev.usb` | 2 | 4 | 8 | |
| `dev.led` | 2 | 10 | 20 | WS2812 |
| `dev.apa102` | 2 | 5 | 10 | |
| `dev.pca9685` | 2 | 4 | 8 | |
| `dev.tft` | 1 | 8 | 8 | |
| `dev.xl9555` | 1 | 4 | 4 | |
| `dev.gt1151q` | 1 | 3 | 3 | |
| `dev.husb238` | 1 | 3 | 3 | |
| `dev.pcm5102` | 1 | 2 | 2 | |
| `dev.uart_motor` | 2 | 3 | 6 | |
| `dev.sd` | 1 | 5 | 5 | |
| `dev.audio` | 1 | 2 | 2 | |
| **合計** | | | **380** | |

> 加新硬件＝加一個 unit（通道×欄位）；加新欄位＝+1 key/通道。
> **每加 1 個 key，worst-case 上限 +7 B**（id ≤2 + 值 ≤5）。

### 1.1 SPI 特別處理（照舊專案＋`mp_lcd_bus/esp32_src/spi_bus.c` 實作）

用家想**繼續用舊 JSON 形狀**（下面呢個），內部點轉換冇所謂；但 **`data_pins` 長度會
決定模式**（讀 `mp_lcd_bus` 源碼確認）：

```json
"SPI": {
  "enable": 1,
  "force_machine": 0,
  "list": [{
    "id": 2, "baudrate": 40000000, "phase": 0, "polarity": 0,
    "GPIO": { "sck": 12, "mosi": -1, "miso": 7 },
    "data_pins": [14]
  }]
}
```

**`data_pins` → 模式（lcd_bus 版）**

| n（data_pins 有效數） | 模式 | `spi_bus_config_t` 對應 |
|---:|---|---|
| 0／缺省／`[]`／`null` | 未設定多線 | lcd_bus 用 `(mosi,)`；machine 用 `sck+mosi` |
| **1** | 單線（TX only） | `mosi_io_num = data_pins[0]`（APA102／WS2801 用） |
| 2 | 半雙工（MOSI＋MISO） | `mosi=data[0]`、`miso=data[1]`；`SPI_DEVICE_HALFDUPLEX` |
| 3 | quad（含 WP） | `quadwp = data[2]` |
| 4 | quad（QIO） | `quadwp=data[2]`、`quadhd=data[3]` |
| 5–8 | octal（data4..7） | 再加 `data4..data7`；需要 `SPICOMMON_BUSFLAG_OCTAL`（lcd_bus 冇設，我哋 C 版會設） |

其他實作細節（源碼實測）：

- 建構兩種寫法：**`data=[...]`＋`clk`**（n 必須 1..8）或 **`sck`＋`mosi`**（n=1）；
  兩種都冇 → ValueError。`clk` 與 `sck` 係同一個（舊 config 寫 `GPIO.sck`）。
- `host`（舊 config 叫 `id`）：有效 `1..SOC_SPI_PERIPH_NUM-1`（S3 一般 1..2；舊 config 用 2）。
- `freq`（舊 config 叫 `baudrate`）：lcd_bus 直接食；`phase`／`polarity` 係 machine 版才有。
- `queue_depth`（lcd_bus）：DMA 佇列深度，預設有上限 `SPI_MAX_QUEUE_DEPTH`。
- `dc`（lcd_bus）：可選 DC pin，由 bus 管（輸出、閒置 high）。
- `force_machine=1`：唔用 lcd_bus（同步 polling transaction），APA102 防 DMA 撕裂。
- `data_pins: []`／`null`／缺省 **三者等效**；claim label `spi{id}_d{i}`。

**schema 化（每 SPI 通道 19 個 key，2 通道 = 38 keys）**

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `id`／`host` | u8 | 2 | SPI host |
| `baudrate`／`freq` | u32 | 8 MHz | |
| `phase`／`polarity` | u8 | 0 | machine 版用 |
| `sck` | i8 | -1 | 必填（enable 時） |
| `mosi`／`miso` | i8 | -1 | 單線別名／machine 用 |
| `force_machine` | bool | 0 | 1＝同步、唔用 DMA |
| `queue_depth` | u8 | 4 | lcd_bus DMA 深度 |
| `dc` | i8 | -1 | 可選 DC pin |
| `data0`…`data7` | i8 | -1 | 多線資料線；**有效數 = 模式** |

**模式係衍生值**（parse 時由 data0..7 數出嚟，唔存）：`n = 有效 data 數`；
`force_machine=1` → machine；否則 `n==1` 單線、`n==2` 半雙工、`n>=3` quad／octal
（n>4 要芯片支援 octal，否則明示 error，唔靜默降級）。

---|---|---|
| **lcd_bus.SPIBus**（DMA／多線） | 有 `lcd_bus` 而且 `force_machine != 1` | `data_pins` = **資料線**（1／2／4／8 條，QSPI／octal 都得）；單線＝`data_pins[0]` 或 `GPIO.mosi`；另有 `sck` |
| **machine.SPI**（單線／同步） | `force_machine = 1` 或 lcd_bus 失敗 | `GPIO.sck/mosi/miso`；`data_pins[0]` 會 fallback 當 `mosi` |

其他舊專案細節（要保留）：

- `data_pins: []`／`null`／缺省 **三者等效**（都係「未設定多線」）。
- `force_machine=1` 用途：APA102 等「要穩定、唔想 DMA 撕裂」場景（走同步 transaction）。
- 腳位 claim：`sck`／`mosi`／`miso` ＋ 每條 `data_pins[i]`（label `spi{id}_d{i}`）。
- S3 Octal PSRAM：**GPIO33–37 不可用**（呢個由板級 reserved mask 處理）。

**schema 化（每個 SPI 通道 17 個 key）**

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `id`／`host` | u8 | 2 | SPI2／SPI3 |
| `baudrate` | u32 | 8 MHz | |
| `phase`／`polarity` | u8 | 0 | |
| `sck` | i8 | -1 | 必填（enable 時） |
| `miso` | i8 | -1 | 可選 |
| `mosi` | i8 | -1 | 單線別名 |
| `force_machine` | bool | 0 | 1＝唔用 DMA（APA102 等） |
| `lines` | u8 | 0 | 0＝自動（由 data0.. 首個非 -1 數） |
| `data0`…`data7` | i8 | -1 | 多線資料線（舊 `data_pins[]`） |

→ 2 通道 × 17 = **34 keys**（原本估 14，加 20）。ESP-IDF C 對應：`spi_bus_config_t`
`data0..data7`（octal 有 8 條）、`spi_bus_initialize(host)`；`force_machine` ⇒
`spi_device_polling_transmit`（同步，唔排 DMA queue）。

---

## 2. 儲存上限（唔使估，算式如下）

```text
NCSTATE header = 12 B
per key     = varint(id ≤2 B) + varint(value ≤5 B) = ≤7 B
worst       = 12 + N_keys × 7
```

| 情境 | keys | 大小 |
|---|---:|---:|
| **全部 380 key 都非預設（worst-case）** | 380 | **2,672 B** |
| 典型：只開 I80 + LED | 32 | **236 B** |
| Matrix 實際（21 個改動 key） | 21 | **~160 B** |

- NVS 單 blob 上限 4 KB → **2,672 B OK**（仲有 35% 餘裕）。
- 未來增長規則：`N_keys ≤ (4096 − 12) / 7 ≈ 583`。超過就拆兩個 blob（hw／dev）或
  用 NCSTATE 的「同值壓縮」（連續同 id 前綴）。
- **sparse 只存改動值**：enable=0 嘅硬件完全唔寫 → 通常 <300 B。

原型：`modules/lab/state_schema_lab/hw_matrix_preview.py`（實跑數字同上）。

---

## 3. 模板 JSON（所有硬件 `enable: 0`）

由 schema 生成（唔手寫），示範：

```json
{
  "version": 1,
  "hw": {
    "spi": { "enable": 0, "max": 2, "fields": 7,
             "list": [ { "ch0": {"gpio": -1} }, { "ch1": {"gpio": -1} } ] },
    "i80": { "enable": 0, "max": 2, "fields": 22,
             "list": [ { "ch0": {"data": [], "wr": -1, "dc": -1, "cs": -1, "freq": 80000000} } ] }
  },
  "dev": {
    "led":    { "enable": 0, "max": 2, "fields": 10 },
    "pca9685":{ "enable": 0, "max": 2, "fields": 4 }
  }
}
```

- 生成大小約 **5.1 KB**（380 keys 全部列出、enable=0）——放 FAT 只係模板，
  人睇／改；**儲存唔用呢個**（用 NCSTATE sparse blob）。
- 用戶流程：開模板 → 將要用嘅 unit `enable` 改 1 → 填 GPIO／少數值 → reboot。
- 未知／未支援 unit 名 → parse 明示 warn（唔靜默）。

---

## 4. GPIO 矩陣（48 位 bitmap）

```c
uint64_t gpio_used;     // bit0..47；GPIO48 不存在，bit48 不用
uint64_t gpio_reserved; // flash 26..37、USB 19/20、console 43/44、strapping 0/3/45/46
```

- `enable=1` 時：所有宣告 GPIO（data／wr／dc／cs／sck／mosi…）→ 佔 bit；
  任何 bit 已被佔（或 reserved）→ **即時衝突，parse error**（現時係逐個 scan）。
- 好處：衝突檢查由「掃 380 key」變 **O(1)**；`status` 可直接印 bitmap／free mask。
- Matrix 示例：I80 D0=14、wr=1、dc=2 → `0x4006`（3 支）；
  再要 GPIO14 → 衝突；再要 GPIO10 → 可以。

---

## 5. 同現有 config 層關係

```text
L0 出廠：schema 表（380 key，型別/範圍/default）＋ 模板（enable=0）
L1 FAT： hardware.json／driver.json（人改；路徑名 → schema id）
L2 狀態： NCSTATE blob（sparse，只存改動 key）
壞檔：   明示 warn → 回 L0（唔會靜靜降級）
```

- 儲存**上限已知**（2,672 B worst、236 B typical）→ 可以預先配置 NVS／RAM，唔會
  「唔知幾時爆」。
- schema ver 一改 → 舊 NCSTATE blob 拒讀、由 JSON／L0 重建。

---

## 6. 落地（併入 state-schema 計劃）

| 階段 | 內容 | 估時 |
|---|---|---|
| P1 | `hw-matrix.json`（380 key）＋ncportgen 編 `nc_hw_matrix.c`（表＋容量）＋模板生成 | +0.5 日 |
| P2 | NCSTATE codec＋`nc_state` v2(含 worst-case 靜態檢查：380×7+12 ≤ 4 KB assert) | 1 日 |
| P3 | JSON parser 用矩陣（路徑→id）＋GPIO bitmap claim | 1 日 |
| P4 | `status cfg`：blob size／key 數／bitmap／schema ver | 0.5 日 |

**新增不變量**：編譯期 `static_assert(12 + NC_HW_MATRIX_KEYS*7 <= 4096)` —— 上限寫死在
code 裡，加硬件／欄位時一超出就 build 失敗（唔會 runtime 先爆）。
