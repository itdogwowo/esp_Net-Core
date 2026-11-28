# 中央示範設定（`ports/presets/`）— 現況

> **這份係生成物**：`python3 tools/preset_check.py --write-status`。
> `run_all.sh` 會用 `--check-status` 驗它冇過期；改 preset／schema 之後要重跑。
> 設計說明：[`README.md`](README.md)｜改 driver：[`driver-config.md`](../../docs/02_guides/driver-config.md)

## Schema

- schema v2、**286 key**、**16 unit**、內容 sha256 前 8 位 `3a37b070`
- 下面嘅 `hit`＝preset JSON 真有寫、而且合 schema 範圍；`missing`＝sparse 未寫（多數係未實作的 hardware unit，屬預期）。`type` 一定要 0。

## `s3-generic-nopsram`

_同一套設定的「冇 PSRAM」版本——所有 buffer 只可以放 internal…_

| 檔案 | 存在 | JSON | version | bytes |
|---|---|---|---|---:|
| `hardware.json` | ✅ | ✅ | 1 | 1815 |
| `system.json` | ✅ | ✅ | 1 | 950 |
| `network.json` | ✅ | ✅ | 1 | 446 |
| `tasks.json` | ✅ | ✅ | 1 | 966 |
| `resources.json` | ✅ | ✅ | 1 | 332 |
| `driver.json` | ✅ | ✅ | 1 | 748 |

| 檔案 | schema key | hit | missing | type |
|---|---:|---:|---:|---:|
| `hardware.json` | 197 | 3 | 194 | 0 |
| `system.json` | 76 | 10 | 66 | 0 |
| `network.json` | 3 | 3 | 0 | 0 |
| `tasks.json` | 0 | 0 | 0 | 0 |
| `resources.json` | 0 | 0 | 0 | 0 |
| `driver.json` | 10 | 0 | 10 | 0 |
| **合計** | **286** | **16** | **270** | **0** |

### driver.json → LED（唯一已實作的 device）

| key | config 路徑 | 實際值 | type／範圍 |
|---|---|---|---|
| `LED_ENABLE` | `LED.enable` | — | bool（0..1） |
| `LED_BACKEND` | `LED.backend` | — | enum：none／i80／spi／rmt／parlio／direct／mock |
| `LED_BUS` | `LED.bus.index` | — | i8（-1..15） |
| `LED_LANES` | `LED.lanes` | — | u8（1..16） |
| `LED_PX` | `LED.px` | — | u16（1..65535） |
| `LED_FMT` | `LED.format` | — | enum：ws2812／ws2812w／ws2815／ws2801／apa102 |
| `LED_CLK` | `LED.timing.pclk_hz` | — | u32（0..80000000） |
| `LED_BIT_CELLS` | `LED.timing.bit_cells` | — | u8（0..64） |
| `LED_T0H` | `LED.timing.t0h` | — | u8（0..64） |
| `LED_T1H` | `LED.timing.t1h` | — | u8（0..64） |

> ⚠ 實際 parser 範圍同 schema 有出入（例：`timing.bit_cells` schema 寫 1..64，`nc_cfg_load.c` 只收 0..8）。設定前睇 [`driver-config.md`](../../docs/02_guides/driver-config.md) §4。

### tasks.json（四層四任務）

| 任務 | level | affinity |
|---|---:|---|
| `render` | 4 | [0, 1] |
| `pixel_encode_task` | 4 | [0, 1] |
| `pixel_paint_task` | 4 | [0, 1] |
| `pixel_effect_task` | 4 | [1, 0] |

### sdkconfig.defaults（build 層）

- `CONFIG_SPIRAM` = `n`
- `CONFIG_NETCORE_PIXEL_LEDS` = `64`
- `CONFIG_NETCORE_PIXEL_I80` = `y`
- `CONFIG_NETCORE_PIXEL_I80_DIRECT` = `y`
- `CONFIG_NETCORE_PIXEL_I80_LANES` = `1`
- `CONFIG_NETCORE_PIXEL_I80_PINS` = `"14"`
- `CONFIG_NETCORE_PIXEL_RING_PSRAM` = `n`
- `CONFIG_NETCORE_PIXEL_PACK_PSRAM` = `n`
- `CONFIG_NETCORE_CONTENT` = `y`
- `CONFIG_NETCORE_WIFI` = `（未寫）`

## `s3-generic-psram`

_一份「齊全、跟得過」的 S3 示範設定，唔綁任何一塊板。_

| 檔案 | 存在 | JSON | version | bytes |
|---|---|---|---|---:|
| `hardware.json` | ✅ | ✅ | 1 | 1627 |
| `system.json` | ✅ | ✅ | 1 | 815 |
| `network.json` | ✅ | ✅ | 1 | 446 |
| `tasks.json` | ✅ | ✅ | 1 | 791 |
| `resources.json` | ✅ | ✅ | 1 | 332 |
| `driver.json` | ✅ | ✅ | 1 | 903 |

| 檔案 | schema key | hit | missing | type |
|---|---:|---:|---:|---:|
| `hardware.json` | 197 | 3 | 194 | 0 |
| `system.json` | 76 | 10 | 66 | 0 |
| `network.json` | 3 | 3 | 0 | 0 |
| `tasks.json` | 0 | 0 | 0 | 0 |
| `resources.json` | 0 | 0 | 0 | 0 |
| `driver.json` | 10 | 0 | 10 | 0 |
| **合計** | **286** | **16** | **270** | **0** |

### driver.json → LED（唯一已實作的 device）

| key | config 路徑 | 實際值 | type／範圍 |
|---|---|---|---|
| `LED_ENABLE` | `LED.enable` | — | bool（0..1） |
| `LED_BACKEND` | `LED.backend` | — | enum：none／i80／spi／rmt／parlio／direct／mock |
| `LED_BUS` | `LED.bus.index` | — | i8（-1..15） |
| `LED_LANES` | `LED.lanes` | — | u8（1..16） |
| `LED_PX` | `LED.px` | — | u16（1..65535） |
| `LED_FMT` | `LED.format` | — | enum：ws2812／ws2812w／ws2815／ws2801／apa102 |
| `LED_CLK` | `LED.timing.pclk_hz` | — | u32（0..80000000） |
| `LED_BIT_CELLS` | `LED.timing.bit_cells` | — | u8（0..64） |
| `LED_T0H` | `LED.timing.t0h` | — | u8（0..64） |
| `LED_T1H` | `LED.timing.t1h` | — | u8（0..64） |

> ⚠ 實際 parser 範圍同 schema 有出入（例：`timing.bit_cells` schema 寫 1..64，`nc_cfg_load.c` 只收 0..8）。設定前睇 [`driver-config.md`](../../docs/02_guides/driver-config.md) §4。

### tasks.json（四層四任務）

| 任務 | level | affinity |
|---|---:|---|
| `render` | 4 | [0, 1] |
| `pixel_encode_task` | 4 | [0, 1] |
| `pixel_paint_task` | 4 | [0, 1] |
| `pixel_effect_task` | 4 | [1, 0] |

### sdkconfig.defaults（build 層）

- `CONFIG_SPIRAM` = `y`
- `CONFIG_NETCORE_PIXEL_LEDS` = `256`
- `CONFIG_NETCORE_PIXEL_I80` = `y`
- `CONFIG_NETCORE_PIXEL_I80_DIRECT` = `y`
- `CONFIG_NETCORE_PIXEL_I80_LANES` = `2`
- `CONFIG_NETCORE_PIXEL_I80_PINS` = `"14,15"`
- `CONFIG_NETCORE_PIXEL_RING_PSRAM` = `y`
- `CONFIG_NETCORE_PIXEL_PACK_PSRAM` = `y`
- `CONFIG_NETCORE_CONTENT` = `y`
- `CONFIG_NETCORE_WIFI` = `（未寫）`

## 兩版差異（`s3-generic-nopsram` → `s3-generic-psram`）

| 檔案 | 路徑 | s3-generic-nopsram | s3-generic-psram |
|---|---|---|---|
| `hardware.json` | `I80.list` | [{"data": [14], "freq": 80000000}] | [{"data": [14, 15], "freq": 80000000}] |
| `system.json` | `num_pixels` | 64 | 512 |
| `driver.json` | `LED_WS2812.list` | [{"bus": {"type": "i80", "index": 0}, "px": 64, "order": "GRB", "d_stay": 0}] | [{"bus": {"type": "i80", "index": 0}, "px": 256, "order": "GRB", "d_stay": 0}, {"bus": {"type": "i80", "index": 1}, "px": 256, "order": "GRB", "d_stay": 0}] |

## 為什麼 project 的 data/config 唔一定齊 6 個檔

- project 目錄只放「要有自己值」的 seed；`driver.json`／`resources.json` 可以缺，
  開機會用 ncportgen 生成版／`k_min_resources` 骨架補（runtime 一定有 6 個檔）。
- `driver.json` 的真正 build-time 真相在 `ports/hardware/<chip>/<board>/driver.json`；
  project 有鏡像就先嵌，冇就嵌 ncportgen 的 `preset_driver.json`。
- 檢查 project 鏡像有冇漂移：
  `python3 tools/sync_hardware_presets.py --check`、`python3 tools/sync_driver_presets.py --check`、`python3 tools/board.py check`。

## 驗證

```bash
python3 tools/preset_check.py              # 結構＋schema 範圍＋四層四任務
python3 tools/preset_check.py --status     # 呢份報告
python3 tools/preset_check.py --check-status   # 驗 STATUS.md 冇過期
python3 tools/schema_coverage.py --config ports/presets/s3-generic-psram/data/config -v
```
