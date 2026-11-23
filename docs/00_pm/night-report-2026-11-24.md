# 夜間執行報告（2026-11-24 02:24–04:00）

> 按 `docs/00_pm/pixel-tiers-plan.md` 逐個 Task 做，每個都跑測試。
> 所有改動只在本機 commit，**未 push**（等用家出門前自己推）。

---

## 一、已完成（有測試證據）

### Task 1：配對層零拷貝、移除清畫布 ✅

- `content_frame_once()` 改成：`ring_begin_ex()` → `nc_px_layout_apply()` 直接寫環槽 →
  `ring_commit_slot()`。刪除 `memset(canvas)` 與「畫布→環」複製；canvas 不再配置。
- 預覽（0xF06B）改成「要求時才由環槽複製一份」（`s_snap_wanted`）。
- 上板（64 px）：`clear=0`、`apply 31 µs`、`commit 7 µs`、`pair 40 µs`、**49.9 fps**；
  0xF06B 回 `hex=0c…`（預覽正常）。
- Commit：`e13d673 perf(pair): 零拷貝寫環、移除清畫布；測試 harness 強制重建`

### Task 2：開機預建缺少的組合表 ✅

- `cache_prebuild_missing()`：掃全部 mode → 缺的就 build＋存 `<pixel_dir>/cache/<key>.pxlt`
  （表 >32 KB 跳過），結果經 **0xF079 PIXDIAG** 的 `prebuild` 欄位讀回。
- 驗證：
  - 改一條 mode JSON（hash 變）→ 重開：`present=1, built=1, ms=269`
  - 再重開：`present=2, built=0, ms=7`（idempotent）

### aprog 按實際數量配置 ✅（Task 8 的第一部分）

- `nc_px_layout_build()` 先數 distinct 效果，再 `lalloc(n × 536 B)`；移除固定
  `NC_PX_LAYOUT_ASSET_MAX` 的「滿了就跳過」。
- 全 suite 重跑：只剩兩個**既有**失敗（見第三節），其餘全綠。
- 上板：`naprog=1`，功能正常。
- Commit：`e7c8d27 perf(layout): aprog 按實際 distinct 效果數量配置（移除固定 32）`

### 抖動：apply4（一次掃表寫 4 份）＋ apply_dither（逐子幀）✅

- 新增 `nc_px_layout_apply4()`（多輸出，給未來一次寫 4 格用）與
  `nc_px_layout_apply_dither()`（單子幀；因為環 API 一次只可以 begin 一格）。
- host 測試 `pxLayout_test [11]`：4 子幀平均 = `12-bit>>4`（±1）、`base=255` 不會溢位。
- Commit：`f266cd8 feat(layout): apply4 抖動（一次掃表寫 4 子幀）＋ host 測試`

### 10-bit 生產路徑（Kconfig＋pair 子幀 loop）✅（CPU 層面）

- Kconfig：`NETCORE_PIXEL_SUBFRAMES`（1–8）、`NETCORE_PIXEL_RING_SLOTS`（3–16）、
  `NETCORE_PIXEL_MOCK_ONLY`。
- pair：`SUBFRAMES > 1` 時逐子幀 `begin → apply_dither → white_mix → commit`，
  每視覺幀寫 N 格；統計 apply/white/commit 為 N 次總和。
- 新 project `ports/project/S3/ESP32-S3-Zero-Show2400`（LDES=2400、SUBFRAMES=4、
  RING_SLOTS=5、FPS=200、MOCK_ONLY=y）可建置、可上板。
- 實測（2,400 px、4 子幀、layout 1 段）：
  - `pair_n 1049`（約 4 次提交／視覺幀）、`apply 9.06 ms`、`commit 35 µs`、
    render `fps 13.9`。
  - **fps 13.9 是單 lane 2,400 px 的線路時間（72 ms/幀）造成，不是 CPU**；
    CPU 每視覺幀 ≈ 9.4 ms（20 ms 的 47%）。

### 測試 harness 修正 ✅

- `run_all.sh` 每次先 `rm -rf <test>/build`：之前**舊 binary 遮住 header 改動**，
  令 `pxEngine_test` 假通過。修正後才發現下面兩個既有失敗。

---

## 二、未完成 / 需跟進

| 項目 | 狀態 | 原因／下一步 |
|---|---|---|
| Task 4 render 子幀接線 | ✅ 完成 | mock-only lane 生效（之前係 OTA 失敗、板跑舊映像） |
| Task 5 2,400 px × 200 Hz 量測 | ✅ 完成 | 見下面「mock lane 實測」 |
| Task 7 6 B 單表 | 未做 | 已定案（6 B），但需要改 layout 表結構＋apply 分組，屬較大重構 |
| Task 8 assets 動態配置 | 部分 | 只做了 aprog；`nc_px_asset_tbl_t` 仍是 `item[32]`（8.4 KB BSS），要改 flexible array＋5 個檔的配置 |
| Task 6 chunked DMA | 未做（可選） | 先量 async 現況再決定 |
| 舊 `nc_px_plan_t` 8 條上限 | 未刪 | production 冇用，但 host 測試仍用 |
| `pxEngine_test` / `pxProgram_test` 失敗 | 未修 | **既有**（commit `07cbc0c`，用家的 WIP；見第三節） |

---

## 三、重要發現（需要你知道）

### 1. 兩個既有測試失敗（不是今晚造成）

在 HEAD（`07cbc0c`）就已經失敗，只是先前 binary 太舊冇重建而「假通過」：

- `pxEngine_test`：`breath-gain255` 指紋由 `ed828bfa…` 變 `b4df7fe5…`
- `pxProgram_test`：eyes／breathing 多段值不符（15 個檢查）

`git worktree` 在乾淨 HEAD 重跑，一樣失敗 → 確認是 `07cbc0c` 帶來的改動未同步更新
測試期望（或改壞）。**今晚冇改過 engine/program。**

### 2. 裝置 FAT 寫入被鎖住（需要斷電重插）

- 由某個時間起，`/pixel/*` 上傳全部回：
  `err_busy: FAT 唔係裝置手上（MSC 掛住）或已有 session`
- **OTA 仍然正常**（走 flash，不經 FAT）。
- 已試：reboot、`/api/port/release`／`reclaim`、`/api/file/undo`／`confirm` 都無效。
- 結論：要**拔電重插**（或按 reset 鍵）清除狀態。
- 目前板上 `/pixel` 內容仍是 2400 測試集（mode `full256`），
  重插後請重新上傳 `ports/project/S3/ESP32-S3-Zero-Show/data/pixel`。

### 3. mock-only lane：其實正常（之前係 OTA 失敗）

- 重新 OTA 一次（乾淨映像）之後，0xF079 顯示 `lanes = [mock]`，證實設定有效。
- **mock lane 實測（2,400 px、4 子幀、200 Hz）**：
  - render `fps = 199.9`（子幀率 200 Hz，視覺 50 fps）
  - `apply = 6.03 ms`／視覺幀（4 個子幀合計）、`commit 12 µs`
  - `pair_n` 2 秒 +340（約 170 次提交／秒）、`full 533`（環 5 格偶爾滿）
  - CPU 每視覺幀 ≈ 6.3 ms ＝ **20 ms 的 31.5%**

---

## 四、今晚的量測數字（真機）

| 項目 | 數值 |
|---|---:|
| 64 px、零拷貝後 | apply 31 µs、commit 7 µs、pair 40 µs、49.9 fps |
| 64 px、0xF06B 預覽 | 正常（hex 0c…） |
| 開機預建（首次缺 1 個） | 269 ms |
| 開機預建（第二次） | 7 ms、built=0 |
| 2,400 px × 4 子幀 | apply 9.06 ms／視覺幀、commit 35 µs |
| 2,400 px × 4 子幀 fps | 13.9（單 lane 線路 72 ms/幀，非 CPU） |

---

## 五、Commit 清單（本機，未 push）

```
e13d673 perf(pair): 零拷貝寫環、移除清畫布；測試 harness 強制重建
e7c8d27 perf(layout): aprog 按實際 distinct 效果數量配置（移除固定 32）
f266cd8 feat(layout): apply4 抖動（一次掃表寫 4 子幀）＋ host 測試
（今晚最後一個 commit：子幀接線、Kconfig、2400 project、文件）
```

> 註：以上 commit 只在本地；`status.md`、計劃、今晚新增的圖與腳本會在最後一個 commit 一起入。
