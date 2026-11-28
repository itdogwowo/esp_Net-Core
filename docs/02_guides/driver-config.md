# `driver.json`：應用硬件（device）層設定指南

> **這一頁給誰**：要微調 `/data/config/driver.json`（尤其 LED）的人。
> **一句話**：**腳位永遠在 `hardware.json`**；`driver.json` 只講「用邊個 backend、引用邊條 bus、
> 幾多 lane、每 lane 幾粒、線材格式、時序」。
>
> 目前 firmware **只有 LED 已實作**（另有 `WS2812`／`led` 別名）。TFT／PCA9685／SDcard／
> PCA9685／GT1151Q／HUSB238／uartMotor／XL9555 寫 `enable=1` 會開機大聲 warn、唔會生效。
>
> 生成狀態：[`ports/presets/STATUS.md`](../../ports/presets/STATUS.md)（兩個中央示範的實際值）
> ｜schema 一覽：[`config-schema.md`](config-schema.md) §1
> ｜權威 code：`firmware/components/sys/config/nc_cfg_load.c` 的 `apply_devices()`／`apply_dev_led()`

---

## 1. 三個位置、邊個贏

| 次序 | 位置 | 邊個改 | 幾時生效 | 備註 |
|---|---|---|---|---|
| 1（最高） | 裝置 FAT `/data/config/driver.json` | USB MSC／WebMaster／`nc4_file.py` | **reboot** | B02 讀；舊名 `devices.json` 都接受（只限 FAT） |
| 2 | `ports/project/<chip>/<board>/data/config/driver.json`（project 鏡像） | 開發者 | rebuild＋OTA；裝置已有舊檔要 `fs_format` 才重新 seed | CMake 優先嵌入這個檔；由 `sync_driver_presets.py` 從板層生成 |
| 3（build-time 真相） | `ports/hardware/<chip>/<board>/driver.json` | 開發者 | rebuild（`--fresh`） | `ncportgen` 生成 `preset_driver.json` 嵌入；板子換了就是改這裡 |

- **seed 只補缺**：FAT 已有 `driver.json` 就永遠唔覆蓋；要出廠狀態：
  `nc4_file.py set '{"System":{"fs_format":1}}' 1` → reboot。
- **腳位不在這裡**：LED 的資料腳在 `hardware.json` 的 `I80.list[]`／`SPI.list[]`；
  `driver.json` 用 `bus:{type,index}` 引用。改腳位要改 `hardware.json`（runtime 可改，reboot 生效）。
- 驗證一致性：`status cfg` 的 `act.match`（FAT 的 hardware／driver 同 build 預置一唔一致）、
  `schema.bad`（code 造出的 record 同 schema 有冇 drift）。

---

## 1.5 新系列：`LED_WS2812`（2026-11-28 起；每 item = 一條線）

```json
{
  "version": 1,
  "LED_WS2812": {
    "enable": 1,
    "list": [
      { "bus": { "type": "i80", "index": 0 }, "px": 256, "order": "GRB", "d_stay": 0 },
      { "bus": { "type": "i80", "index": 1 }, "px": 256, "order": "GRB", "d_stay": 0 }
    ]
  }
}
```

- **`bus.index` = 第幾條 data line**（S3 只有一個 I80；0..N-1 對應 wiring 的 `I80.list[0].data[]`）。
- `px` = 每條線幾顆；**每條線有自己內容**，px 可以不同：
  - frame（環）= Σ px：`[line0][line1]...` 每段獨立；
  - DMA／傳輸時間 = max(px)；短線完成後出 reset；
  - 冇寫入 list 的線＝0 buffer。
- `order`：`GRB`／`RGB` → `ws2812`；`GRBW`／`RGBW` → `ws2812w`。
- **冇 `timing`／`format`／`backend`／`lanes`**：選咗 `LED_WS2812` 就由 driver 決定時序；
  legacy `LED` block 仍然接受（過渡期），但新 project／新 pair 建議用 `LED_WS2812`。
- 其他系列（`LED_PWM`／`LED_PCA9685`／`LED_APA102`）config 已預留，backend 未實作。

## 2. 完整可複製範例

### A. S3 + 1 lane × 256 px WS2812（GPIO1，直驅 I80）— 你手上 Zero 類

`hardware.json`（腳位）：

```json
"I80": {
  "enable": 1,
  "list": [ { "data": [1], "freq": 80000000 } ]
}
```

`driver.json`：

```json
{
  "version": 1,
  "LED": {
    "enable": 1,
    "backend": "i80",
    "bus": { "type": "i80", "index": 0 },
    "lanes": 1,
    "px": 256,
    "format": "ws2812",
    "timing": { "pclk_hz": 2500000, "bit_cells": 3, "t0h": 1, "t1h": 2 }
  }
}
```

build 層要開直驅＋相同幾何（`sdkconfig.defaults`；改完 `--fresh`）：

```text
CONFIG_NETCORE_PIXEL=y
CONFIG_NETCORE_PIXEL_I80=y
CONFIG_NETCORE_PIXEL_I80_DIRECT=y
CONFIG_NETCORE_PIXEL_I80_PINS="1"       # 同 hardware.json data 對上（Kconfig 是 fallback）
CONFIG_NETCORE_PIXEL_I80_LANES=1
CONFIG_NETCORE_PIXEL_LEDS=256
CONFIG_NETCORE_CONTENT=y
```

> 直驅版只 route 真正接燈的 lane；`data` 之外的 dummy 腳唔會出現在波形（真面板走 esp_lcd，
> 要齊 data／WR／DC，見 [`pixel-i80-pin-schema.md`](../03_notes/pixel-i80-pin-schema.md)）。

### B. 2 lane × 256 px（中央示範 `s3-generic-psram`）

```json
{
  "version": 1,
  "LED": {
    "enable": 1, "backend": "i80", "bus": { "type": "i80", "index": 0 },
    "lanes": 2, "px": 256, "format": "ws2812",
    "timing": { "pclk_hz": 2500000, "bit_cells": 3, "t0h": 1, "t1h": 2 }
  }
}
```

- `hardware.json` 的 `I80.list[0].data` 要有 **≥ 2 支**（例：`[14, 15]`）。
- 總像素 = `lanes × px` = 512；`system.json` 的 `num_pixels` 寫 512（資訊用，方便核對）。
- 環大小 = `64 B 表頭 + px×4` × 3 格 × lane 數。

### C. SPI 時脈式燈（APA102／WS2801）

```json
{
  "LED": {
    "enable": 1, "backend": "spi", "bus": { "type": "spi", "index": 0 },
    "lanes": 1, "px": 300, "format": "apa102",
    "timing": { "pclk_hz": 4000000 }
  }
}
```

- `hardware.json` 的 `SPI.list[0]` 要 `enable=1`、`mosi`／`sck` ≥ 0（可選 `baud`；driver 的
  `pclk_hz` 有寫就蓋過 `baud`）。
- 時脈式格式（`ws2801`／`apa102`）一定走 SPI；歸零碼格式（`ws2812*`／`ws2815`）一定走 I80。

### D. 唔點燈但要量 CPU（EffectPerf 類）

```json
{ "LED": { "enable": 1, "backend": "mock", "lanes": 1, "px": 9600, "format": "ws2812" } }
```

或直接 `enable:0`（有宣告但唔用燈）→ render 只留 mock lane。兩者都唔會碰 GPIO。

---

## 3. LED 10 個 schema key（`driver.json:LED.*`）

| key | JSON 路徑 | 型別 | schema 範圍／enum | 實際 parser（`nc_cfg_load.c`） | 作用 |
|---|---|---|---|---|---|
| `LED_ENABLE` | `LED.enable` | bool | 0..1 | 收 `true/false` 或 `1/0` | 1 = 用這組燈 |
| `LED_BACKEND` | `LED.backend` | enum | none／i80／spi／rmt／parlio／direct／mock | 未知字串 → invalid＋**停用**；缺 → 只留 mock | i80／spi 係 production；rmt／parlio 未實作（lane 停用＋log）；direct／mock 量測用 |
| `LED_BUS` | `LED.bus.index` | i8 | -1..15 | **-1..1**（第 3 點，見 §4） | 引用 `hardware.I80[.]`／`hardware.SPI[.]`；-1 = 冇 bus |
| `LED_LANES` | `LED.lanes` | u8 | 1..16 | 1..16；I80 會 clamp 到實際有效腳數 | 幾條並行資料線（SPI 只 1） |
| `LED_PX` | `LED.px` | u16 | 1..65535 | 1..65535 | **每條 lane** 幾顆；總數 = lanes × px |
| `LED_FMT` | `LED.format` | enum | ws2812／ws2812w／ws2815／ws2801／apa102 | 未知 → invalid＋回落 `ws2812` | 線材格式（GRB／GRBW／時脈式） |
| `LED_CLK` | `LED.timing.pclk_hz` | u32 | 1,000,000..80,000,000 | **100,000..80,000,000**；別名 `clk_hz` | I80 取樣時鐘；SPI 時鐘 |
| `LED_BIT_CELLS` | `LED.timing.bit_cells` | u8 | 1..64 | **0..8**；0 = 用格式預設 | 一個 bit 幾個時鐘 |
| `LED_T0H` | `LED.timing.t0h` | u8 | 0..64 | **0..8**；0 = 用格式預設 | 0 碼高電平 cell 數 |
| `LED_T1H` | `LED.timing.t1h` | u8 | 0..64 | **0..8**；0 = 用格式預設 | 1 碼高電平 cell 數 |

- timing **唔寫／寫 0** → 用格式預設（`nc_px_wave_default()`）；WS2812 常用
  `pclk_hz 2,500,000、bit_cells 3、t0h 1、t1h 2`（1.2 µs/bit）。
- `backend` 同 `bus.type` 要一致（i80 ↔ `"i80"`、spi ↔ `"spi"`）；唔一致 → invalid＋停用。
- `bus.index` 指唔到／`hardware` 的對應 bus `enable=false`／腳位不全 → **該 lane 停用＋log**，唔會靜默退 mock。

---

## 4. 已知：schema 範圍同 parser 有出入（2026-11-27）

`nc_state_set_*()` 只驗 schema；`nc_cfg_load.c` 的 JSON parser 有自己一套範圍。兩者未完全對齊：

| 欄位 | schema | parser 實際 | 後果／建議 |
|---|---|---|---|
| `LED.timing.bit_cells` | 1..64 | 0..8 | >8 會被 parser 當 invalid（保留 0 → 用格式預設）。**寫 2..8** |
| `LED.timing.t0h`／`t1h` | 0..64 | 0..8 | 同上；WS2812 通常 1／2 |
| `LED.timing.pclk_hz` | ≥ 1 MHz | ≥ 100 kHz | 寫 ≥1 MHz 最安全 |
| `LED.bus.index` | -1..15 | -1..1 | 現時硬件每個 chip 最多 1–2 條 I80；用 0 最穩 |
| `LED.px`／`lanes` | 1..65535／1..16 | 相同 | ✅ |

> 經 `status set`／`nc_state_set_*()` 寫入的值只驗 schema；如果寫咗 parser 唔收的範圍，
> boot 時 state override 可能直接套入 cfg（繞過 parser）。**微調時以本表「parser 實際」為準**，
> 長遠要對齊 schema ↔ parser（已列入待修）。

---

## 5. 驗證（改完 driver 之後）

```bash
# 1) 靜態：JSON／schema 範圍／四層四任務
python3 tools/preset_check.py
# 2) 裝置：config 有冇 parse 問題、state hit、schema drift
python3 tools/nc4.py --port <PORT> status cfg
#    → cfg.dev 的 miss/bad/inv/unk；state.hit／schema.bad；act.match
# 3) 燈有冇真係出：show 映像用 0xF06B 取最近一幀
python3 tools/show_verify.py --via http://127.0.0.1:8011 --max-led 40
#    或 python3 tools/nc4.py --port <PORT> raw 0xF06B
# 4) 記憶體：環／packed 在 PSRAM 定 internal
python3 tools/nc4.py --port <PORT> status mem
# 5) 任務：四層四任務 level=4 started=1 running=1
python3 tools/nc4.py --port <PORT> status tasks
```

開機 log 會印（tag `devices`／`render`）：

```text
devices: /data/config/driver.json（led=開 px=256 lanes=1 backend=i80；未支援已開=0）
render: 輸出層就緒：1 道 lane、50 fps（frame_interval_ms=20）；lane0 = i80（環 i80.prd）
```

- `inv` > 0＝有欄位超範圍／型別錯（parser 會用預設值或停用該組）；
- `unk` > 0＝未知欄位（打錯名／未支援段）；
- `未支援已開` > 0＝寫了未實作的 device，firmware 大聲 warn、唔會生效。

---

## 6. 常見陷阱

| 症狀 | 原因 | 處理 |
|---|---|---|
| 燈全黑但 `status tasks` 四條 running | `backend` 寫了未實作值（rmt／parlio）→ lane 停用；或 `hardware.I80.enable=0` | 改回 `i80`／`spi`；check log「這道 lane 停用」 |
| 只著一部分 | `lanes` > hardware data 有效腳數（I80 會 clamp）；`px` 同實際鏈長不符 | 對齊 `I80.list[].data` 長度同 `px` |
| 顏色次序錯（紅藍互換） | `format` 寫 `ws2812w`（GRBW）但線材係 GRB | 改 `ws2812`；白通道格式才用 `ws2812w` |
| 改咗 FAT `driver.json` 冇效 | 改錯 layer（改 project 鏡像但冇 rebuild＋`fs_format`） | 直接改 FAT 就 reboot；或 rebuild 後 `fs_format` 重 seed |
| `act.match=0` | FAT 的 driver／hardware 同 build 預置唔同 | 正常（改過 FAT）；想一致就改 build 層或 `fs_format` |
| `px` 改了但畫面舊 | `/data/pixel` 內容、ring 幾何或 `system.num_pixels` 未同步 | 改 system.num_pixels（資訊）＋重上 show 內容；`fs_format` 重 seed |
| 寫 `enable:1` 的 TFT／PCA9685 冇反應 | 未實作 | 預期；log 有 warn（鐵律 9：唔靜默） |

---

## 7. 相關

- 中央示範：`ports/presets/s3-generic-psram/`（有 PSRAM）、`s3-generic-nopsram/`（冇 PSRAM）；
  實際值＋驗證狀態：[`ports/presets/STATUS.md`](../../ports/presets/STATUS.md)
- key 一覽：[`config-schema.md`](config-schema.md)｜分層理由：[`config-layers.md`](../03_notes/config-layers.md)
- I80 腳位／直驅 vs esp_lcd：[`pixel-i80-pin-schema.md`](../03_notes/pixel-i80-pin-schema.md)
- runtime driver 化歷史：status §7.291–.303、`docs/03_notes/state-system.md`
