# ESP32-S3-Generic-PSRAM（中央示範 pair：硬件）

> **用途**：監察「最新情況」的**基準硬件＋project pair**之一（有 PSRAM（2 MB quad））。
> 呢塊係**通用示範板**，唔綁任何實體板；改 wiring／sdkconfig 要 rebuild。
> 姊妹 pair：`../ESP32-S3-Generic-NoPSRAM/`。
> 中央 preset（runtime 值來源）：[`ports/presets/s3-generic-psram/`](../../../presets/s3-generic-psram/)。

## 內容

| 檔案 | 內容 |
|---|---|
| `board.json` | S3、4 MB flash、有 PSRAM（2 MB quad）、240 MHz、console none |
| `wiring.json` | I80 直驅：data = GPIO14/15、USB OTG（GPIO19/20） |
| `driver.json` | LED 2 lane × 256 px WS2812（2 lane、px 256、timing 2.5 MHz／3 cells） |
| `sdkconfig.defaults` | flash／PSRAM／CPU／console／4 MB 分區／pixel 2 lane、RING/PACK 在 PSRAM |
| `partitions.csv` | 4 MB（app 1.5 MB × 2＋FAT 896 KB） |

## 建置＋監察

```bash
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Generic-PSRAM --project ESP32-S3-Generic-PSRAM --fresh
python3 tools/ota.py --port <PORT> artifacts/ESP32-S3-Generic-PSRAM.bin
python3 tools/nc4.py --port <PORT> status sys
python3 tools/nc4.py --port <PORT> status cfg     # state.hit／schema.bad／act.match
python3 tools/nc4.py --port <PORT> status mem     # static／plan／PSRAM
python3 tools/nc4.py --port <PORT> status tasks   # 四層四任務
python3 tools/state_dump.py --port <PORT>         # 逐 key 對 JSON
```

## 保持最新（唔會再「project 更新唔全面」）

```bash
python3 tools/sync_preset_pairs.py --check   # run_all 已加閘門
python3 tools/sync_hardware_presets.py --check
python3 tools/sync_driver_presets.py --check
```

- `system／network／tasks／resources`：由 `ports/presets/s3-generic-psram/data/config/` 同步。
- `hardware.json`：由 `wiring.json` 經 `ncportgen` 生成（`sync_hardware_presets.py`）。
- `driver.json`：由 `board.json`＋`driver.json` 經 `ncportgen` 生成（`sync_driver_presets.py`）。
