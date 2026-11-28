# 中央示範：ESP32-S3 通用（冇 PSRAM）

> **這份是咩**：同一套設定的「冇 PSRAM」版本——所有 buffer 只可以放 internal，
> 所以像素數／lane 數要壓細，其餘欄位同有 PSRAM 版一模一樣（方便對照）。
>
> 姊妹版：`../s3-generic-psram/`（有 PSRAM，2 lane × 256 px）。
> 共同說明、差異表、驗證步驟：`../README.md`。

---

## 1. 同有 PSRAM 版的分別（只有三處）

| 位置 | 有 PSRAM | 冇 PSRAM（這份） |
|---|---|---|
| `sdkconfig.defaults` | `CONFIG_SPIRAM=y`、`RING_PSRAM=y`、`PACK_PSRAM=y` | `CONFIG_SPIRAM=n`、`RING_PSRAM=n`、`PACK_PSRAM=n` |
| `driver.json` | `lanes: 2`、`px: 256` | `lanes: 1`、`px: 64` |
| `system.json` | `num_pixels: 512` | `num_pixels: 64` |

- **flat6（6 B/px）預設 PSRAM**（2026-11-25 定案）；冇 PSRAM 時 `malloc` 落 internal，
  唔夠就明示失敗（唔會靜靜退更慢的路）。量測例外才手動 `flatify_place(false)`。
- 冇 PSRAM 時，環／packed 要 internal；唔夠會**明示失敗**（唔會靜靜退更慢的路）。
- 想加大 px：先量 `status mem` 的 `h_int`；9,600 px 這類尺寸冇 PSRAM 一定放唔落。

## 2. 怎樣用

```bash
# A) 做某個 project 的出廠預置
cp -R ports/presets/s3-generic-nopsram/data/config/. \
      ports/project/S3/<你的板>/data/config/

# B) build 層：併入 project 的 sdkconfig.defaults（board 層若是 PSRAM 板，
#    這裡的 CONFIG_SPIRAM=n 會覆寫它 —— 確認你真的是冇 PSRAM 的模組）
cat ports/presets/s3-generic-nopsram/sdkconfig.defaults >> \
    ports/project/S3/<你的板>/sdkconfig.defaults

# C) 重建（一定要 --fresh）
IDF_PATH=$HOME/esp/esp-idf python3 make.py build <board> --project <project> --fresh
```

## 3. 驗證

```bash
python3 tools/nc4.py --port <PORT> status sys   # h_ps 應該接近 0
python3 tools/nc4.py --port <PORT> status mem   # 環／packed 全部 internal
python3 tools/nc4.py --port <PORT> status cfg   # schema.bad=0
```

> 狀態：**未上板驗證**（手頭只有有 PSRAM 的 S3 板）。數字係依 Kconfig／記憶體政策推的，
> 上板之後要補一次 `status mem` 實測。
