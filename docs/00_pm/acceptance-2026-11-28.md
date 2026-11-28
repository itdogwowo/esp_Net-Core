# 全會話全面驗收報告（2026-11-28）

> 範圍：本次對話由「配置／狀態格式」到「LED 系列」、「每線獨立 pixel」、
> 「16IO / 16×600 測試」、「動態上限／cache／PSRAM」全部工作。
> 數字以**最新一次真機量測**為準；舊 status 章節的歷史政策數字（128 KB／32 KB／4-mode 預建）
> 已被 §7.323–7.328 取代。

## 0. 總結

**驗收狀態：PASS（有已知、非阻塞的警告與環境限制，見 §6）。**

- 18 個 project 全部 build 成功；
- host tests 全部通過；
- preset／schema／pixel config gates 全部通過；
- 16IO 真機 50 fps、cache 4/4 hit、狀態 NCSTATE hit；
- 發現並修正 5 個動態化後的隱藏問題（uint8 溢位、id 截斷、1-byte mode id、
  segment offset、cache 清理時序）。

## 1. 對話目標 vs 實際行為

| # | 目標 | 實際行為 | 結果 |
|---|---|---|---|
| G1 | 配置／狀態格式統一、可持久化 | 6 款 config JSON → typed cfg；NCSTATE sparse cache；schema 286 keys | ✅ |
| G2 | LED 用系列 `LED_*`，唔再逐 driver 魔法設定 | `LED_WS2812` 中央 class；每線 `bus{type,index}`、px、order、d_stay；無 timing／lanes | ✅ |
| G3 | 每線獨立內容、px 可不同 | i80 segmented codec；frame=Σpx、DMA=max px；16 lanes 已跑 | ✅ |
| G4 | pixel 設定唔受 8／16／32 magic limit | map／mode／show／entry／effect／segment 全動態；舊 legacy 常數改名 | ✅ |
| G5 | hash 相同直接讀、不同才重建 | NCLAYOUT key＝mode＋map＋px＋assets＋ver；hit/load、miss/build | ✅ |
| G6 | 啟動／reload 完成全部建表 | `PREBUILD_MAX=0` 全部預建；reload 後再跑 | ✅ |
| G7 | 舊 cache key 唔累積 | 有效 key 清單＋pre-clean stale `.ncc` | ✅ |
| G8 | pixel 全 PSRAM、唔佔 SRAM | `HOT_SRAM_KB=0`；tables／layout／flat 全 PSRAM | ✅ |
| G9 | 16IO 50 fps | frame 約 19.97–20.59 ms | ✅ |
| G10 | 16×600 極限 | 線路18 ms、CPU約14.8–17.6 ms；margin約2–5 ms | ✅ 偏緊 |
| G11 | 16 IO 功能測試（不同效果／格數） | 4 modes、28 effects、grid4/grid16/sync/test | ✅ |
| G12 | 文件數字同步 | 本報告＋status §7.329 為最新 | ✅（歷史章節保留） |

## 2. Build／測試矩陣

### 2.1 所有 project build

| project | build |
|---|---|
| C3/ESP32-C3-DevKitM | ✅ |
| P4/ESP32-P4-ETH | ✅ |
| P4/ESP32-P4-FireBeetle2 | ✅ |
| P4/ESP32-P4-WIFI6-DB | ✅ |
| S3/Generic-16IO | ✅ |
| S3/Generic-NoPSRAM | ✅ |
| S3/Generic-PSRAM | ✅ |
| S3/Matrix | ✅ |
| S3/Matrix-EffectPerf | ✅ |
| S3/Matrix-LED-Textkit | ✅ |
| S3/Matrix-Show | ✅ |
| S3/Mini-Nano | ✅ |
| S3/N16R8 | ✅ |
| S3/SuperMini | ✅ |
| S3/SuperMini-LED-Textkit | ✅ |
| S3/Zero-Show | ✅ |
| S3/Zero-Show2400 | ✅ |
| S3/Zero-Show9600 | ✅ |

### 2.2 Host／gate／檢查

| 檢查 | 結果 |
|---|---|
| `tools/hosttest/run_all.sh` | ✅ 全部通過 |
| `sync_preset_pairs.py --check` | ✅ 2 對 preset↔hardware↔project |
| `preset_check.py --check` | ✅ 2 presets、286 schema keys 範圍 OK |
| `state_budget.py --check` | ✅ 文件冇過期 |
| `sync_driver_presets.py --check` | ✅ |
| `sync_hardware_presets.py --check` | ✅ |
| `schema_coverage.py` | ✅ 286 keys、270 未用（正常） |
| `id_check.py` | ✅ 20 個 ID token 全部指得到 code |
| pixel config（7 個 project） | ✅ `show_upload --check` 全部 OK |
| `board.py check` | **0 錯誤、22 警告**（見 §6） |
| `selftest.py --skip-ota` | **18/21**；3 個係專案環境預期（見 §6） |

## 3. 真機驗收（ESP32-S3-Zero、16IO project、全 PSRAM）

### 3.1 系統／配置／狀態

| 項目 | 值 |
|---|---:|
| cfg 載入 | 29.8 ms |
| state 載入 | 9.8 ms |
| NCSTATE hit | **1（cmp_ok=1、rebuilt=0）** |
| state records | **19 筆／139 B** |
| store cap／used | 27／19 |
| state apply | 19 |
| schema | **286 keys、16 units、bad=0** |
| activation | n=22、en=3、units=`usb,i80,led`、match=1 |
| dirty | 0 |

### 3.2 LED／pixel

| 項目 | 值 |
|---|---:|
| driver | `LED_WS2812`、16 線、每線 256 px、i80 index 0–15 |
| active tasks | render／encode／paint／effect 全部 enabled |
| px | 4,096（16 lanes×256） |
| frame | **19.99 ms（約 50 fps）** |
| pair | **3.12 ms**（clear 0.80＋apply 2.35＋white／commit） |
| apply | 2.35 ms |
| encode | 9.39 ms |
| tx | 0.83 ms |
| 0xF07F A/B | internal 0.716 ms／PSRAM 1.249 ms |
| flatify | 0.312–0.343 ms |
| active cache | 1,600–7,456 B |
| prebuild | **present=4、built=0、26 ms**（第二次開機） |
| names.ncc | 5,244 B（369 名） |
| fs_format 後首開 | built 3、總共 4 張、約 1,238 ms |
| cache hit swap | 188–206 µs |
| FAT | total 872,448 B／free 663,552 B（648 KiB） |
| internal free | 125–196 KB |
| PSRAM free | 1.82 MB |

### 3.3 16×600 極限（9,600 px、16 lanes 並行）

| 階段 | 時間 |
|---|---:|
| 線路（硬底） | **18.0 ms** |
| encode（16 條獨立內容） | **10.37 ms** |
| apply（PSRAM） | **4.44 ms** |
| clear（估算） | ~1.7 ms |
| CPU 保守合計 | **~17.6 ms** |
| 20 ms margin | **~2.4 ms**（50 fps 可行、偏緊） |

## 4. 本次體檢修正的隱藏問題

1. **uint8 迴圈溢出**：`find_asset()`、paint map/mode 掃描、program value 等用
   `uint8_t i < tbl->n`；表已 uint32，>255 會 wrap／越界。已全部改 uint32/uint16。
2. **數字 id 截斷**：`ref_is()` cast uint8，id>255 數字查找錯；已改 uint16。
3. **mode 切換只支援 1-byte id**：`0xF06D` 只能 0–255；已支援 1／2 byte LE，
   `s_pending_mode` 改 uint16（0xFFFF = 無 pending）。
4. **segment pool offset uint16**：全域 segment 總數 >65,535 會 wrap；`seg_off` 改 uint32。
5. **cache cleanup 時機**：原本先 prebuild 後清舊 key；舊 cache 佔滿 FAT 會令新 save 失敗。
   已改成先收集有效 key → **pre-clean** → 再 prebuild。
6. 之前已修：prefetch/prebuild 的 `assets_hash` 錯用 active mode；next layout build 後冇 save。

## 5. 功能域逐項核對

### 5.1 配置／狀態

- 6 款 config JSON（hardware／driver／system／network／tasks／resources）由 `sys/config` 讀；
- NCSTATE header/record、src_hash、CRC、atomic tmp→bak；
- state store 動態配置、schema 286 keys、LED 預設 min0；
- host tests：cfg／cfgstruct／stateBlob／stateFile／stateSchema／stateStore 全部通過；
- 真機 NCSTATE hit、19 records/139 B、dirty0。

### 5.2 LED 系列／driver

- `LED_WS2812` 中央模型：每 item 一條線、`bus{type,index}`、px、order、d_stay；
- 無 `timing`／`lanes`、無每線 backend；
- 真機 driver.json 16 線、px256、i80 0–15；pixdiag render lanes=16；
- `ncportgen`／board.py 驗證；build matrix 全通過。

### 5.3 Pixel renderer／content

- canonical RGBW 4 channel；單通道 write 只寫自己通道，paint 按 needs_clear 清 canvas；
- 動態 map/mode/show/entries/effects/segments；
- NCLAYOUT binary cache：hash key、version2、variable segments、CRC；
- startup/reload 全 mode 預建；舊 key pre-clean；
- 16IO 4 modes／28 effects；grid4／grid16／sync／test；
- 真機 50 fps、cache 4/4、stale cleanup 已驗證。

### 5.4 Presets／pairs／projects

- `sync_preset_pairs.py`：2 對 generic pair 同步；
- 16IO／Generic pair 等 project build 全綠；
- 7 個 pixel project 本機 JSON 檢查全部 OK；
- board.py 0 errors。

## 6. 已知警告／限制（非阻塞）

### 6.1 board.py 22 個警告

| 類別 | 說明 |
|---|---|
| 舊 artifacts sdkconfig 未 fresh | 改 sdkconfig.defaults 後要 `--fresh`（提示） |
| 部分 project sdkconfig.defaults 覆寫 NETCORE_PIXEL_* | 外接燈鏈由 project Kconfig 決定；正常 |
| P4-ETH hardware driver `WS2812.list[0]` 舊單線寫法 | `enable=0`、firmware 未讀；未影響 build／runtime |
| 部分 project driver.json 有未知鍵 `USB` | board.py 警告；runtime `unk=0`；屬舊 project 鏡像 |
| USB-only 板冇 build-time driver.json | 允許 |

**沒有 error。**

### 6.2 selftest 18/21

3 個 fail 係專案環境，不是 regression：

1. `hub 有 probe`：16IO project 冇 hub；
2. `BENCH_READY`／`BENCH`：16IO project 冇開 test/bench action。

### 6.3 真正限制

| 項目 | 說明 |
|---|---|
| 名稱長度 | **已改 name dictionary（names.ncc）**，冇 24 B cap；字串只存一份 |
| 檔名 64 B | FATFS／VFS 路徑限制；建議 ASCII |
| map/mode id 0–65535 | uint16；protocol 已支援 2-byte |
| 單一 asset segments 65,535 | uint16 欄位；總 pool offset 已 uint32 |
| FAT 容量 | cache 太多／FAT 滿仍會 save 失敗 |
| cache cleanup | 只在全部 mode 成功預建時執行（保守） |
| 16×600 margin | 約 2–5 ms；CPU 大突發可能掉幀 |
| `next_us` | wall time 含 encode／paint 搶佔，不是純 CPU |

## 6.4 UTF-8 中文字支援（新測試）

| 項目 | 結果 |
|---|---|
| JSON 內 **raw UTF-8** 中文名 | ✅ 支援（name 長度以 byte 計） |
| `\uXXXX` 逃逸 | ✅ 已解碼成 UTF-8（BMP；surrogate pair 暫不支援） |
| 24 B name 的容量 | 中文 3 B／字 → **7 個完整中文字（21 B）** |
| 8 個中文字（24 B） | ✅ 乾淨截到 7 字；**唔會切半個 codepoint**（新增 UTF-8 邊界 trim） |
| 檔名 64 B | 建議 ASCII；中文檔名要視 FATFS／VFS LFN 支援，未列為保證 |
| 測試 | pxAsset_test 新增 raw 中文、7 字、8 字截斷、`\u` 逃逸，全部 PASS |

**注意**：`name[24]` 係 **byte cap 不是「字數」**；要更多中文字需改 dynamic string 或加大欄位。

## 6.5 結構尺寸增量重測（id／seg_off 改寬後）

| 結構 | 之前 | 現在 | 增量 |
|---|---:|---:|---:|
| `nc_px_asset_t` | 42 B | **44 B** | +2 B |
| `nc_px_map_t`（host） | 48 B | 48 B | 0（padding 吸收） |
| `nc_px_mode_t`（host） | 80 B | 80 B | 0（padding 吸收） |
| MCU 32-bit 估算 | — | — | mode 約 +4 B／個、map 0、asset +2 B |

- 16IO 實際：3 maps＋4 modes＋28 assets ≈ **+76 B**，量唔到；
- 1,000 modes／effects：約 **+6 KB**；
- 65,535 modes／effects：約 **+0.4 MB**（仍在 PSRAM 容量內，但屬上限附近）。
- 重測 frame **20.00 ms／50 fps**、pair 3.11 ms、apply 2.34 ms；
  PSRAM free **1.81 MB**、FAT free 648 KiB → 冇性能／容量回歸。

## 6.7 最終驗證（name dictionary 接入後）

- **18/18 project build 成功**（最後一輪，含 EffectPerf bench 名寫入修正）。
- host tests 全部通過（nameDict／pxAsset／pxLayer／pxShow／pxScan／pxLayout…）。
- 真機最後一輪：cache **present=4、built=0、26 ms**；frame **19.99 ms／50 fps**；
  pair 3.12 ms、apply 2.35 ms、encode 9.39 ms；PSRAM free **1.82 MB**、internal free 196 KB。
- names.ncc **5,244 B／369 名**；手動刪檔重建實測會清 4 個舊 `.ncc`，再 reload 4/4 hit。
- 長中文名：pxAsset_test 18 字（54 B）完整保留；冇 24 B cap。

## 6.8 副檔名／檔案分類（2026-11-28 整理）

| 副檔名 | 類別 | 位置 | 刪除 |
|---|---|---|---|
| `.json` | 來源／設定 | effects／map／modes／registry／config | 唔好隨手刪 |
| `.nce` | 編譯 effect | effects | 可重編 |
| `.nct` | 編譯 task | tasks | 可重編 |
| `.ncc` | derived cache（names＋layout） | pixel/cache | 可刪，自動重建 |
| `.part`／`.tmp`／`.bak` | atomic 暫存／備份 | 目標旁 | 可刪 |
| `.bin` | firmware | artifacts／OTA | 可 rebuild |
| `.md` | 文件 | docs | 唔掃 |

- cache 由 `names.ncd`＋`<key>.layout` 統一成 **`names.ncc`＋`<key>.ncc`**；
- header magic（`NCNAMES`／`NCLAYOUT`）分辨內容；
- cleanup／trim 跳過 `names.ncc`；刪它才重建字典並清 layout cache；
- 舊 `.layout`／`names.ncd` 自動 migration 清走；
- `.nce`／`.nct` 保留，因為 loader 用副檔名分 effect／task；
- 詳細：[pixel-storage-layout.md](../03_notes/pixel-storage-layout.md)。

## 7. 結論

- **功能目標與實際代碼行為一致**：設定動態、LED 系列、每線獨立、cache hash/rebuild、
  startup 預建、全 PSRAM、16IO 50 fps；
- **今次體檢額外修正 5 個隱藏問題**；所有 build／host／gate 全綠；
- 剩餘問題屬已知格式上限、FAT 容量、環境警告，不阻塞交付。

**驗收：PASS。**
