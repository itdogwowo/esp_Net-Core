# Pixel／Firmware 檔案分類與副檔名

> 原則：**同一個 lifecycle（來源／編譯產物／cache／暫存）放同一組**；
> 唔用副檔名去分類 lifecycle，因為同一個 lifecycle 會有唔同格式。

## 1. 目錄

```text
/data/
  cache/          names.ncc（名稱字典 cache）
                  <key>.ncc（layout cache）
                  bench_layout.ncc（只喺跑 layout bench 時出現）
  pixel/
    effects/      *.json（來源）＋ *.nce（編譯效果）
    map/          *.json（來源）
    modes/        *.json（來源）
    registry.json 來源
```

## 2. 副檔名

| 副檔名 | 類別 | 位置 | 可否刪除／重建 | 說明 |
|---|---|---|---|---|
| `.json` | 來源／設定 | `pixel/effects`、`pixel/map`、`pixel/modes`、`registry`、`config` | 唔好隨手刪 | 使用者可讀可改 |
| `.nce` | 編譯效果模組 | `pixel/effects` | 可重新編譯 | `ncm` 產物；loader 用 `.nce` 認 effect |
| `.nct` | 編譯 task 模組 | `tasks` | 可重新編譯 | `ncm` 產物；loader 用 `.nct` 認 task |
| `.ncc` | **derived cache** | `/data/cache` | 可安全刪除，會自動重建 | `NCNAMES`／`NCLAYOUT` 用 header magic 分辨 |
| `.part` | atomic 寫入暫存 | 目標檔旁邊 | 可刪 | crash 後殘留可清 |
| `.tmp` | 設定／狀態暫存 | 目標檔旁邊 | 可刪 | atomic rename 前 |
| `.bak` | 上一版備份 | 目標檔旁邊 | 可刪 | 設定／狀態復原用 |
| `.bin` | firmware image | `artifacts`／OTA | 可重新 build | 唔係 DATA |
| `.md` | 文件 | `docs`／`ports` | 可刪 | 唔會被 runtime scan |

## 3. 為什麼 cache 用同一個 `.ncc`

以前：

```text
cache/names.ncd       # 名稱字典
cache/<key>.layout    # layout
```

兩個都係 derived cache、都可以重建，但唔同副檔名／唔同 cleanup 規則。
現在：

```text
cache/names.ncc
cache/<key>.ncc
```

- 同一個 cache 根 `/data/cache`、同一個副檔名；config 亦可共用；
- 內容用 header magic（`NCNAMES`／`NCLAYOUT`）分辨；
- cleanup／trim 只掃 `.ncc`，**永遠跳過 `names.ncc`**；
- 刪 `names.ncc` = 重建字典；同時清 layout cache，避免舊 name_id 對錯；
- 舊 `.layout`／`names.ncd` 由新 firmware 自動清走（migration）。

## 3.1 每個 cache 檔會自我描述

`.ncc` 只係「呢個係 cache」的外層分類；**真正 type 喺檔頭 magic**。
兩個格式都係 little-endian header ＋ payload ＋ CRC：

### `NCNAMES`（名稱字典）

```text
offset  size  field
0       8     magic = "NCNAMES\0"
8       2     version = 1
10      2     flags
12      4     count
16      4     payload_bytes
20      4     payload_crc
24      ...   records: id u32 + len u16 + pad u16 + UTF-8 bytes
```

### `NCLAYOUT`（layout）

```text
offset  size  field
0       8     magic = "NCLAYOUT"
8       4     version = 3
12      4     key（mode/map/px/assets/ver 的 hash）
16      4     px
20      4     naprog
24      4     njobs
28      4     nruns
32      4     nvals
36      4     payload_bytes
40      4     crc32(payload)
44      ...   aprog* + job* + run*
```

- 同一個 `.ncc` 副檔名，loader 用 `magic` 分辨；
- `version` 唔夾 → 當舊格式重建；
- layout 的 `key` 唔夾目前組合 → 重建；
- payload 長度／CRC 唔對 → 當壞檔重建；
- 所以改錯名／放錯 type 都唔會靜靜解讀成另一種表。

快速檢查：

```bash
xxd -l 16 /cache/names.ncc     # NCNAMES
xxd -l 16 /cache/<key>.ncc    # NCLAYOUT
```

## 4. 為什麼 `.nce`／`.nct` 唔合併

兩者都係 ncm 編譯產物，但 loader 用副檔名決定：
- `.nce` → pixel effect 模組；
- `.nct` → task 模組。

合併成 `.ncm` 要改 ncm loader／工具／部署流程，收益只係少一個副檔名，
但會失去「看檔名就知道係邊種模組」的明確性，所以保留。

## 5. 掃描器規則

- pixel 來源掃描只收 `.json`（effects／map／modes）；
- effect 模組只收 `.nce`；
- cache 只收 `.ncc`；
- `.part`／`.tmp`／`.bak`／`.md` 一律唔會入表；
- 目錄 scan 唔會遞歸，cache 唔會當成 source。
