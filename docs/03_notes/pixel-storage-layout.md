# Pixel／Firmware 檔案分類與副檔名

> 原則：**同一個 lifecycle（來源／編譯產物／cache／暫存）放同一組**；
> 唔用副檔名去分類 lifecycle，因為同一個 lifecycle 會有唔同格式。

## 1. 目錄

```text
/data/pixel/
  effects/        *.json（來源）＋ *.nce（編譯效果）
  map/            *.json（來源）
  modes/          *.json（來源）
  registry.json   來源
  cache/          names.ncc（名稱字典 cache）
                  <key>.ncc（layout cache）
```

## 2. 副檔名

| 副檔名 | 類別 | 位置 | 可否刪除／重建 | 說明 |
|---|---|---|---|---|
| `.json` | 來源／設定 | `pixel/effects`、`pixel/map`、`pixel/modes`、`registry`、`config` | 唔好隨手刪 | 使用者可讀可改 |
| `.nce` | 編譯效果模組 | `pixel/effects` | 可重新編譯 | `ncm` 產物；loader 用 `.nce` 認 effect |
| `.nct` | 編譯 task 模組 | `tasks` | 可重新編譯 | `ncm` 產物；loader 用 `.nct` 認 task |
| `.ncc` | **derived cache** | `pixel/cache` | 可安全刪除，會自動重建 | `NCNAMES`／`NCLAYOUT` 用 header magic 分辨 |
| `.part` | atomic 寫入暫存 | 目標檔旁邊 | 可刪 | crash 後殘留可清 |
| `.tmp` | 設定／狀態暫存 | 目標檔旁邊 | 可刪 | atomic rename 前 |
| `.bak` | 上一版備份 | 目標檔旁邊 | 可刪 | 設定／狀態復原用 |
| `.bin` | firmware image | `artifacts`／OTA | 可重新 build | 唔係 DATA |
| `.md` | 文件 | `docs`／`ports` | 可刪 | 唔會被 runtime scan |

## 3. 為什麼 cache 用同一個 `.ncc`

以前：

```text
pixel/cache/names.ncd       # 名稱字典
pixel/cache/<key>.layout    # layout
```

兩個都係 derived cache、都可以重建，但唔同副檔名／唔同 cleanup 規則。
現在：

```text
pixel/cache/names.ncc
pixel/cache/<key>.ncc
```

- 同一個 cache 目錄、同一個副檔名；
- 內容用 header magic（`NCNAMES`／`NCLAYOUT`）分辨；
- cleanup／trim 只掃 `.ncc`，**永遠跳過 `names.ncc`**；
- 刪 `names.ncc` = 重建字典；同時清 layout cache，避免舊 name_id 對錯；
- 舊 `.layout`／`names.ncd` 由新 firmware 自動清走（migration）。

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
