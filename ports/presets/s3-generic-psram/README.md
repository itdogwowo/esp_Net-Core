# 中央示範設定：ESP32-S3 通用（有 PSRAM）

> **這份是咩**：一份「齊全、跟得過」的 S3 示範設定，唔綁任何一塊板。
> 用嚟做兩件事：① 睇清楚每個欄位點寫；② 開新 project 時由這裡 copy 起步。
> **數字係示範**（2 lane × 256 px），唔係任何一塊板的物理事實——腳位要按你的板改。
>
> 姊妹版：`../s3-generic-nopsram/`（冇 PSRAM，1 lane × 64 px）。
> 共同說明、差異表、驗證步驟：`../README.md`。

---

## 1. 為什麼分「有 PSRAM／冇 PSRAM」兩版

| 東西 | 有 PSRAM（這份） | 冇 PSRAM |
|---|---|---|
| 環（i80.prd） | PSRAM（`CONFIG_NETCORE_PIXEL_RING_PSRAM=y`） | internal（大 px 會 OOM，明示失敗） |
| packed 波形 | PSRAM（`CONFIG_NETCORE_PIXEL_PACK_PSRAM=y`） | internal |
| 示範幾何 | 2 lane × 256 px | 1 lane × 64 px |
| flat6（6 B/px 落點表） | **PSRAM**（`nc_px_layout_flatify()`，2026-11-25 定案） | internal（冇 PSRAM 時 malloc） |

判準：**大 buffer 先問 PSRAM，細而熱（表／parser／stack）一律 internal**。

## 2. 檔案

```text
s3-generic-psram/
├─ README.md                 ← 你現在睇緊
├─ sdkconfig.defaults        ← build 層（--fresh 才生效）
└─ data/config/
   ├─ hardware.json          ← 腳位（I80 data／wr／dc／cs、USB phy…）
   ├─ system.json            ← 身份、FPS、links、routes
   ├─ network.json           ← Wi-Fi（預設關）
   ├─ tasks.json             ← 四層四任務 level／affinity
   ├─ resources.json         ← 任務之間誰提供／需要
   └─ driver.json            ← 應用硬件（目前只有 LED 真的被讀）
```

## 3. 怎樣用

```bash
# A) 直接做某個 project 的出廠預置（最常見）
cp -R ports/presets/s3-generic-psram/data/config/. \
      ports/project/S3/<你的板>/data/config/

# B) build 層：把 sdkconfig.defaults 的內容併入 project 的同名檔案
#    （project 層會覆寫 board 層；改完一定要 --fresh）
cat ports/presets/s3-generic-psram/sdkconfig.defaults >> \
    ports/project/S3/<你的板>/sdkconfig.defaults

# C) 重新出 image
IDF_PATH=$HOME/esp/esp-idf python3 make.py build <board> --project <project> --fresh
```

上板之後如果 `/data/config` 已經有舊檔（seed 只補缺），要出廠狀態就：

```bash
python3 tools/nc4_file.py --port <PORT> set '{"System":{"fs_format":1}}' 1
python3 tools/nc4.py --port <PORT> reboot
```

## 4. 驗證（真機）

```bash
python3 tools/nc4.py --port <PORT> status cfg    # state.hit／schema.bad／act.match
python3 tools/nc4.py --port <PORT> status tasks  # 四層四任務 level=4 running=1
python3 tools/nc4.py --port <PORT> status mem    # 環／packed 係 PSRAM 定 internal
```

- `schema.bad` 應該 0（boot 由 cfg 造的 record 同 schema 一致）。
- 想再確認 LED 由 state 讀值：`status cfg` 的 `state.store` 應該 ≥ 10（LED 10 個 key）。
