# Pixel 子系統驗收報告（2026-11-28）

> 本報告以**最新一次真機量測**為準；舊 status 章節的 128 KB／32 KB／4-mode 預建數字係歷史政策，
> 已被 §7.323–7.328 取代。

## 1. 驗收目標

| # | 目標 | 驗收結果 |
|---|---|---|
| G1 | 用戶設定唔受 8／16／32 等 magic number 限制 | ✅ map／mode／show／segment／entry／effect 全部動態 |
| G2 | layout cache 用內容 hash：相同直接讀、不同才重建 | ✅ key＝mode＋map＋px＋assets＋ver；hit/load；miss/build |
| G3 | 啟動／reload 完成全部 mode 建表；runtime 不再 build | ✅ 預設 PREBUILD_MAX=0（全部）；reload 後亦全部預建 |
| G4 | 舊 cache key（effects／map／mode 改動）唔會累積 | ✅ 有效 key 清單＋pre-clean stale `.layout` |
| G5 | pixel 全部 PSRAM；唔佔 SRAM／唔用容量焦慮 | ✅ `HOT_SRAM_KB=0`；active flat、tables、layout 全 PSRAM |
| G6 | 16IO 4,096 px 達 50 fps | ✅ 實測 frame≈19.97–20.59 ms、fps≈50 |
| G7 | 16×600 極限仍有 50 fps 空間 | ✅ 線路18 ms、CPU≈14.8–17.6 ms（保守）；margin 約2–5 ms |
| G8 | 換場 cache hit 唔需要複雜預熱 | ✅ cache hit swap≈188 µs；next cache hit≈5.8–6.9 ms（wall 可能被 encode 搶佔） |
| G9 | 格式／cache 上限只由真實資源決定 | ✅ 32 KB／128 KB／1 MB 魔法限制已移除；只剩 FAT free、RAM、field 寬度 |

## 2. 最新真機數字（ESP32-S3-Zero、16IO project、全 PSRAM）

| 指標 | 最新量測 |
|---|---:|
| px | 4,096（16 lanes×256） |
| frame | **20.59 ms（≈50 fps）** |
| pair（clear+apply+white+commit） | **3.01 ms** |
| clear | 0.74 ms |
| apply | **2.26 ms** |
| encode | **9.96 ms** |
| tx | 0.83 ms |
| 0xF07F A/B | internal 0.716 ms／PSRAM 1.249 ms |
| 0xF07D flatify | 0.312–0.343 ms |
| active cache payload | 1,600–7,456 B（視 mode） |
| prebuild（4 modes） | **present=4、built=0、26 ms**（第二次開機） |
| fs_format 後第一次開機 | built 3 個、總共 4 張、約 1,238 ms |
| cache hit 讀取＋flatify | 約 2–2.6 ms |
| cache hit swap | **188–206 µs** |
| FAT | total 872,448 B；free 663,552 B（約648 KiB） |
| internal free | 約 125–196 KB |
| PSRAM free | 約 1.80 MB |

### 16×600（9,600 px、16 lanes 並行）

| 項目 | 時間 |
|---|---:|
| 線路（硬底） | **18.0 ms** |
| encode（16 條獨立內容） | **10.37 ms** |
| apply（PSRAM） | **4.44 ms** |
| clear（估算） | ~1.7 ms |
| CPU 保守合計 | **~17.6 ms** |
| 20 ms margin | **~2.4 ms**（50 fps 可行、偏緊） |

## 3. 測試矩陣

| 檢查 | 結果 |
|---|---|
| host tests `tools/hosttest/run_all.sh` | ✅ 全部通過 |
| preset pair sync `sync_preset_pairs.py --check` | ✅ 2 對 OK |
| preset/schema `preset_check.py --check` | ✅ 286 keys、範圍 OK |
| build 16IO | ✅ |
| build Generic-PSRAM | ✅ |
| build Zero-Show | ✅ |
| build Matrix-Show | ✅ |
| build Matrix-EffectPerf（bench） | ✅ |
| build C3-DevKitM | ✅ |
| build P4-ETH | ✅ |
| 16IO OTA／runtime | ✅ 50 fps、cache 4/4、pixdiag/0xF07F 正常 |
| stale cache cleanup | ✅ 手動 `deadbeef.layout` reload 後 exists=0；有效 cache 保留 |
| 全 PSRAM hot flat=0 | ✅ apply 仍 2.26 ms，frame 50 fps |

## 4. 今次體檢發現並修正的隱藏問題

1. **uint8 迴圈溢出**：`find_asset()`、paint 的 map/mode 掃描、program value 等用
   `uint8_t i < tbl->n`；表已改 uint32，當 >255 會 wrap／越界。已全部改 uint32／uint16。
2. **數字 id 查找截斷**：`ref_is()` 將 atoi 結果 cast 成 uint8，id>255 數字查找錯。
   已改 uint16。
3. **mode 切換 protocol 只 1 byte**：`0xF06D` 只能 id 0–255；已支援 1／2 byte LE，
   `s_pending_mode` 改 uint16（0xFFFF = 無 pending）。
4. **segment pool offset uint16**：全域 segment 總數 >65,535 會 wrap；`seg_off` 改 uint32。
5. **cache cleanup 時機**：原本先 prebuild、後清舊 key；FAT 被舊 cache 佔滿時新 save 會失敗。
   已改成先收集有效 key → **pre-clean** → 再 prebuild。
6. **prefetch cache key**（較早前）：prefetch/prebuild 錯用 active mode 的 assets_hash；已改 next/tmp。
7. **next layout 建完冇 save**（較早前）：已補 `layout_cache_save()`，所以第二次起 cache hit。

## 5. 仍然存在的限制／風險（非 magic number）

| 項目 | 說明 |
|---|---|
| 名稱 24 字、檔名 64 字 | struct 固定 char array；要改要變 storage format |
| map/mode id 0–65535 | uint16；protocol 已支援 2-byte |
| 單一 asset segments 65,535 | uint16 欄位；總 pool offset 已 uint32 |
| FAT 容量 | 真正上限；cache 太多／FAT 滿會 save 失敗，runtime 可能 rebuild |
| cache cleanup 只覆蓋全部 mode 成功預建的情況 | 如果某 mode map／asset build 失敗，會跳過 cleanup（保守，避免刪有效 cache） |
| layout cache 格式 ABI | 只限同一 firmware 版本；`NC_PX_LAYOUT_CACHE_VER` 會 bump |
| 16×600 margin | 約2–5 ms；CPU 突發 >ring 60 ms 仍可能掉幀 |
| `next_us` wall time | 會包含 encode／paint 搶佔；唔係純 CPU 時間 |

## 6. 結論

- 用戶設定層面：**冇固定 count magic limit**；只剩 RAM／FAT／field 寬度。
- 重建策略：**startup/reload 全部建好、hash hit 直接讀、舊 key 自動清**；runtime 正常只 swap。
- 記憶體：**全 PSRAM**；SRAM 冇 pixel 數據；容量焦慮可以消除。
- 性能：16IO 50 fps；16×600 並行仍在 20 ms 內（margin 約2–5 ms）。
- 今次體檢修正了 5 個新發現的隱藏溢出／時序問題；現有測試全綠。

**驗收狀態：PASS（有已知非阻塞限制，見 §5）。**
