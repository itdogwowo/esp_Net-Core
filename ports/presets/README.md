# 中央示範設定（`ports/presets/`）

> **用途**：一個唔綁板的「齊全設定」起點。開新 project 時由這裡 copy，
> 唔使再由零砌 6 個 JSON，亦唔會漏咗「四層四任務要 level=4」這類硬性要求。
>
> 這裡**唔係** project（冇 `project.json`、唔參與 `make.py projects`）；
> 要用的話 copy 去 `ports/project/<chip>/<board>/data/config/`，見下面 §3。
>
> **呢兩個就係「中央基準版本」**：所有 config 欄位以它們為完整示例；實際值＋驗證狀態由
> [`STATUS.md`](STATUS.md) 生成（`python3 tools/preset_check.py --write-status`），
> `run_all.sh` 用 `--check-status` 擋過期。改 preset／schema 之後要重跑生成。

---

## 1. 現有版本（暫時只有 S3）

| preset | 適用 | pixel 幾何（示範值） | 大 buffer | 對應硬件＋project pair |
|---|---|---|---|---|
| [`s3-generic-psram/`](s3-generic-psram/) | S3 模組**有** PSRAM | 2 lane × 256 px | 環／packed 放 PSRAM | `S3/ESP32-S3-Generic-PSRAM` |
| [`s3-generic-nopsram/`](s3-generic-nopsram/) | S3 模組**冇** PSRAM | 1 lane × 64 px | 環／packed 放 internal | `S3/ESP32-S3-Generic-NoPSRAM` |

> **監察用 pair**：上面兩個 preset 各有一份完整的 hardware＋project（`ports/hardware/S3/ESP32-S3-Generic-*`
> ＋`ports/project/S3/…`），可以直接 `make.py build`、OTA 上板、用 `status`／`state_dump.py` 睇最新情況。
> `python3 tools/sync_preset_pairs.py --check` 會擋住「project 只更新一部分」的 drift（run_all 已加閘門）。

- 兩個版本除了 PSRAM 相關 Kconfig 與 pixel 幾何，其餘設定一模一樣（方便對照）。
- 腳位（GPIO14／15）係**示範**，換板要改 `hardware.json` 的 `I80.list[0].data`。
- `s3-generic-nopsram` **未上板驗證**（手頭只有有 PSRAM 的 S3 板）；數字按 Kconfig／
  記憶體政策推，上板後要補 `status mem`。

## 2. 每個 preset 有什麼

```text
<name>/
├─ README.md              ← 這個版本的差異與驗證
├─ sdkconfig.defaults     ← build 層（改完要 --fresh）
└─ data/config/
   ├─ hardware.json       ← 腳位與週邊（I80／SPI／ETH／USB…）
   ├─ system.json         ← 身份、FPS、links、routes
   ├─ network.json        ← Wi-Fi（預設關）
   ├─ tasks.json          ← 四層四任務 level／affinity
   ├─ resources.json      ← 任務之間誰提供／需要
   └─ driver.json         ← 應用硬件（目前只有 LED 真的被讀）
```

## 3. 用法

```bash
# 1) 複製 runtime 預置去你的 project
cp -R ports/presets/s3-generic-psram/data/config/. \
      ports/project/S3/<你的板>/data/config/

# 2) 併入 build 層（project 會覆寫 board；改完要 --fresh）
cat ports/presets/s3-generic-psram/sdkconfig.defaults >> \
    ports/project/S3/<你的板>/sdkconfig.defaults

# 3) 重建＋上板
IDF_PATH=$HOME/esp/esp-idf python3 make.py build <board> --project <project> --fresh
python3 tools/ota.py --port <PORT> artifacts/<board>__<project>.bin

# 4) 裝置已有舊 /data/config（seed 只補缺）→ 要出廠狀態就清 /data 重 seed
python3 tools/nc4_file.py --port <PORT> set '{"System":{"fs_format":1}}' 1
python3 tools/nc4.py --port <PORT> reboot
```

## 4. 驗證

```bash
# 靜態檢查：JSON 結構、schema 範圍、四層四任務、兩版差異
python3 tools/preset_check.py
# 狀態快照（生成 ports/presets/STATUS.md；run_all 會驗冇過期）
python3 tools/preset_check.py --status
python3 tools/preset_check.py --check-status

# 上板
python3 tools/nc4.py --port <PORT> status cfg    # schema.bad=0、state.hit
python3 tools/nc4.py --port <PORT> status tasks  # 四條 level=4 started=1 running=1
python3 tools/nc4.py --port <PORT> status mem    # 環／packed 在 PSRAM 定 internal
```

## 5. 加一個新 preset

1. 複製一個現有目錄改名（例：`s3-generic-octal/` 或將來的 `p4-generic/`）；
2. 改 `README.md`／`sdkconfig.defaults`／`data/config/*`；
3. 跑 `python3 tools/preset_check.py`（會自動掃 `ports/presets/*/`）；
4. 在下面登記。

| preset | 狀態 | 登記日 |
|---|---|---|
| `s3-generic-psram` | 中央基準（未逐項上板；欄位已被 host check 驗） | 2026-11-26（2026-11-27 重新驗證） |
| `s3-generic-nopsram` | 中央基準（**未上板**：冇冇-PSRAM 的 S3 板） | 2026-11-26（2026-11-27 重新驗證） |
