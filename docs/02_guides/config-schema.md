# 可以設定什麼、在哪裡設、怎樣生效

> **用途**：你要改一個值之前，這一頁回答三件事：**它在哪一層**、**改完要 rebuild 定 reboot**、
> **用咩驗**。設計理由見 [config-layers.md](../03_notes/config-layers.md)；
> schema registry 契約見 [state-schema.md §8](../03_notes/state-schema.md)。
>
> 圖：[設定流轉與權威層](../00_pm/config-layers.svg)｜
> [schema registry：可以設定什麼](../00_pm/schema-registry.svg)（兩張都由
> `python3 tools/config_diagram.py` 生成）

---

## 0. 一句話

```text
build 事實（board.json／sdkconfig／wiring）───┐
build 預置（ports/project/.../data/config）──┼─→ B02 nc_cfg_boot() ─→ cfg struct
FAT live（/data/config/*.json，最高權威）───┘                          │
                                                                      ├─→ 消費者（net／tasks／render／activation）
state.ncst（稀疏快取，可重建）← nc_state_boot_sync() ← 派生自上面      │
        └─→ RAM store ─→ nc_state_api（get／set，schema 驗證）────────┘
```

**權威鏈**：FAT live > build 預置 > 出廠 const（L0）。
**state.ncst 係快取**，任何時候都可以由前兩者重建，唔係第二個真相。

---

## 1. schema 覆蓋的設定（唯一名單）

> 下表由 `python3 tools/gen_state_keys.py --doc` 生成；三份生成檔＋本表同
> `schema/state-keys.json` 唔同步，`tools/hosttest/run_all.sh` 會直接失敗。
> 所以「有哪些值可以設定」永遠只有一個答案。

<!-- BEGIN GENERATED: schema -->
**現時：schema v2、25 個 key、4 個 unit（sys 3／i80 5／spi 7／led 10）**

| unit | key（C 名） | type | 可接受值 | config 路徑（人讀） | 生效 |
|---|---|---|---|---|---|
| sys | `NC_KEY_SYS_FRAME_MS` | u16 | 1..1000 | `system.json:frame_interval_ms` | reboot |
| sys | `NC_KEY_SYS_NUM_PIXELS` | u16 | 0..65535 | `system.json:num_pixels` | reboot |
| sys | `NC_KEY_SYS_LOG_DEBUG` | u8 | 0..1 | `system.json:log_debug_mode` | reboot |
| i80 | `NC_KEY_I80_COUNT` | u8 | 0..16 | `hardware.json:I80.list[0].data` | reboot |
| i80 | `NC_KEY_I80_WR` | i8 | -1..54 | `hardware.json:I80.list[0].wr` | reboot |
| i80 | `NC_KEY_I80_DC` | i8 | -1..54 | `hardware.json:I80.list[0].dc` | reboot |
| i80 | `NC_KEY_I80_CS` | i8 | -1..54 | `hardware.json:I80.list[0].cs` | reboot |
| i80 | `NC_KEY_I80_FREQ` | u32 | 1000000..80000000 | `hardware.json:I80.list[0].freq` | reboot |
| spi | `NC_KEY_SPI_ENABLE` | bool | 0..1 | `hardware.json:SPI.enable` | reboot |
| spi | `NC_KEY_SPI_BAUD` | u32 | 100000..80000000 | `hardware.json:SPI.list[0].baudrate` | reboot |
| spi | `NC_KEY_SPI_PHASE` | u8 | 0..1 | `hardware.json:SPI.list[0].phase` | reboot |
| spi | `NC_KEY_SPI_POL` | u8 | 0..1 | `hardware.json:SPI.list[0].polarity` | reboot |
| spi | `NC_KEY_SPI_SCK` | i8 | -1..54 | `hardware.json:SPI.list[0].GPIO.sck` | reboot |
| spi | `NC_KEY_SPI_MOSI` | i8 | -1..54 | `hardware.json:SPI.list[0].GPIO.mosi` | reboot |
| spi | `NC_KEY_SPI_MISO` | i8 | -1..54 | `hardware.json:SPI.list[0].GPIO.miso` | reboot |
| led | `NC_KEY_LED_ENABLE` | bool | 0..1 | `driver.json:LED.enable` | reboot |
| led | `NC_KEY_LED_BACKEND` | enum | enum：none／i80／spi／rmt／parlio／direct／mock | `driver.json:LED.backend` | reboot |
| led | `NC_KEY_LED_BUS` | i8 | -1..15 | `driver.json:LED.bus.index` | reboot |
| led | `NC_KEY_LED_LANES` | u8 | 1..16 | `driver.json:LED.lanes` | reboot |
| led | `NC_KEY_LED_PX` | u16 | 1..65535 | `driver.json:LED.px` | reboot |
| led | `NC_KEY_LED_FMT` | enum | enum：ws2812／ws2812w／ws2815／ws2801／apa102 | `driver.json:LED.format` | reboot |
| led | `NC_KEY_LED_CLK` | u32 | 1000000..80000000 | `driver.json:LED.timing.pclk_hz` | reboot |
| led | `NC_KEY_LED_BIT_CELLS` | u8 | 1..64 | `driver.json:LED.timing.bit_cells` | reboot |
| led | `NC_KEY_LED_T0H` | u8 | 0..64 | `driver.json:LED.timing.t0h` | reboot |
| led | `NC_KEY_LED_T1H` | u8 | 0..64 | `driver.json:LED.timing.t1h` | reboot |

> 「生效」全部係 reboot：B02 由 JSON 重算 cfg → state.ncst；runtime `nc_state_set_*` 只改 RAM（見 §3）。
<!-- END GENERATED: schema -->

**enum 的寫法**：config JSON 寫名字（例 `LED.backend = "i80"`），`state.ncst` 存 index
（`i80` → 1）；schema 的 `values[]` 就是這個對照，`nc_state_set_u32()` 寫 index。

### 1.1 怎樣加／減一個 key

```bash
# 1) 改 schema/state-keys.json（id = domain|unit|index|field，各 8 bit；填 type／min／max／enum）
# 2) 生成 C 表＋本表
python3 tools/gen_state_keys.py --doc
# 3) 如果開機要寫入：nc_state_boot.c 的 build_records() 加一行由 cfg 取值
# 4) rebuild（改過 sdkconfig.defaults 要 --fresh）
```

- 生成器會驗：id 的 unit byte 同 unit id 一致、id 唔可以係 0、id／name 唔重複、範圍在型別內、enum 由 0 連續。
- 刪 key 之後，舊 `state.ncst` 的未知 key 會自動跳過（向前兼容），唔使清檔。
- `status cfg` 的 `schema.bad` 會發現「code 加咗 key、schema 未加」的 drift。

---

## 2. 未入 schema 但可設定（仍然係 cfg struct）

以下欄位由 `nc_cfg_load.c` 解析、B02 生效；改完 **reboot** 生效，但**未**納入
schema registry（所以 `nc_state_set_*` 暫時改唔到，`status cfg.schema` 亦唔會顯示）。

| 檔案 | 欄位（代表） | 效果 | 備註 |
|---|---|---|---|
| `system.json` | `nc4.links[]`（kind／enable／port／rx） | 開邊條通道、buffer 幾大 | `enable=0` → 唔宣告緩衝、唔建 bus |
| `system.json` | `nc4.routes[]`（in → decodeN） | 邊一核解碼／轉發 | 單核板自動收斂 decode1→decode0 |
| `system.json` | `frame_interval_ms`、`num_pixels` | 節拍（FPS）、像素數 | 已入 schema（見上表） |
| `system.json` | `usb.msc`、`usb.cdc_log`、`log_debug_mode` | USB 交碟／log 管道／除錯等級 | `log_debug_mode` 已入 schema |
| `tasks.json` | `tasks[].level`／`affinity` | 任務開唔開、跑邊核 | `-1`＝唔開；`[0,0]`＝唔開 loop |
| `resources.json` | `resources[]`（buf／cmd／hw） | 任務之間誰提供／需要 | 模組 `provides` 會合併 |
| `network.json` | `wifi.enable`／`ssid`／`ssid_pw` | WiFi 開關與連線 | build 冇編 `CONFIG_NETCORE_WIFI` 就只會 warn |
| `hardware.json` | `ETH`／`SDIO`／`PIN`／`PWM`／`UART`／`I80`／`SPI` | 腳位與週邊 | `I2C`／`I2S`／`ENC`／`XL9555` 未 typed（改咗唔生效） |
| `driver.json` | `LED`（enable／backend／lanes／px／format／timing） | 燈鏈幾何與時序 | 已入 schema（10 個 key）；其他 device 未實作 |

> **判斷法則**：`schema/state-keys.json` 搵到 → 屬 registry 管；
> 搵唔到但 `nc_cfg_load.c` 有 parse → 屬 cfg struct；兩者都冇 → 改咗唔會生效。

---

## 3. 三種設定路徑（選錯層 = 改完唔生效）

| 路徑 | 做法 | 幾時生效 | 適合 |
|---|---|---|---|
| **FAT live**（最高權威） | 用 USB MSC／WebMaster／`nc4_file.py` 改裝置 `/data/config/*.json` | **reboot** | 現場調參、唔想重燒 |
| **build 預置** | 改 `ports/project/<chip>/<board>/data/config/*.json` 或 `sdkconfig.defaults` | **rebuild（`--fresh`）→ OTA**；裝置已有舊檔時要 `fs_format` 才會重新 seed | 出廠預設、換 project |
| **runtime API**（RAM） | `nc_state_set_u32/i32/bool/str()`（driver／工具／測試） | **即時**（只改 RAM＋`dirty`）；reboot 由 JSON 重算 | 臨時調參、測試；永久改仍然要改 JSON |

- **build 預置只會在裝置缺檔時 seed**；檔案存在就唔會被覆蓋（`fs_format=1` 才會清 `/data` 重 seed）。
- **改 `sdkconfig.defaults` 一定要 `--fresh`**，否則 build dir 會沿用舊 `sdkconfig`
  （2026-11-26 實際撞過：main stack 仍然 3,072 → 爆棧、USB 唔 enumerate、燈全黑）。

---

## 4. 改完之後的驗證清單

```bash
# 1) config 有冇 parse 問題、state 有冇 hit、schema drift
python3 tools/nc4.py --port <PORT> status cfg
#    state : {"hit":1,"cmp_ok":1,"rebuilt":0,"n":18,"bytes":124,...}
#    act   : {"src":...,"en":4,"units":"usb,i2c,i80,led","fat":...,"match":1}
#    schema: {"ver":2,"crc":...,"keys":25,"units":4,"bad":0,"dirty":0}

# 2) 任務開咗未（四層四任務要 level=4 started=1 running=1）
python3 tools/nc4.py --port <PORT> status tasks

# 3) 通道開咗未（links[].enable=0 就唔會出現）
python3 tools/nc4.py --port <PORT> status net

# 4) 換 project／改壞 config：清 /data 重 seed
python3 tools/nc4_file.py --port <PORT> set '{"System":{"fs_format":1}}' 1
python3 tools/nc4.py --port <PORT> reboot
```

| 欄位 | 正常 | 唔正常代表 |
|---|---|---|
| `cfg.*.miss/bad/inv/unk` | 0（或缺檔時 miss>0） | `bad`＝壞檔、`inv`＝超範圍、`unk`＝未知欄位 |
| `state.hit` | 1 | 0 ＝ config 改過（值有變會重建，`rebuilt=1`） |
| `state.rebuilt` | 0 | 1 ＝ 今次寫過盤（值有變／壞檔） |
| `act.match` | 1 | 0 ＝ FAT 的 hardware／driver 同 build 預置唔同（activation 係舊 build） |
| `schema.bad` | 0 | >0 ＝ code 造出的 record 同 schema 型別／範圍唔一致（要修） |

---

## 5. 與圖的對應

| 圖 | 檔案 | 睇咩 |
|---|---|---|
| 設定流轉／權威層 | [config-layers.svg](../00_pm/config-layers.svg) | 三來源 → B02 → cfg → 消費者；state 快取角色；三種改法 |
| schema registry | [schema-registry.svg](../00_pm/schema-registry.svg) | 一個 JSON → 三份生成檔 → 查表／API；四個 unit 的 key 與範圍 |
