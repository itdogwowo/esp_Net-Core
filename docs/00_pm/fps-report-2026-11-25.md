# FPS 穩定度與零件時間：整合報告（2026-11-25）

> 板：ESP32-S3（4 MB flash／2 MB PSRAM）。所有真機數字都係 NC4 OTA 後量。
> 相關：`status.md` §7.222–7.236、`docs/02_guides/fps-stability.md`。

---

## 0. 一句話

- 節拍**仍然由設定的週期 timer 控制**（`frame_interval_ms`）；
- 另加一個**時間緩衝（slack）**吸收臨時波幅；**唔改週期、唔追趕**；
- 用 `0xF081` 逐幀統計＋`fps_trace.py`／`fps_drift.py`／`fps_longrun.py` 做小時級驗證。

![零件時間](assets/pixel-pairing/parts-time.png)

---

## 1. 零件同時間（9,600 px、50 fps）

| 階段 | 時間 | 備註 |
|---|---:|---|
| effect（1 條值流） | ≈0.3 ms | 核心 0 |
| pair apply（6 B 表 PSRAM） | **5.4 ms** | 核心 1 |
| white mix | ≈0.1 ms | |
| ring commit | ≈0.5 ms | 零拷貝 |
| **每幀 CPU 合計** | **≈6.3 ms（31%）** | 20 ms 預算 |
| I80 encode（16×600） | 6.4 ms（舊 24.0，3.77×） | 喺 18 ms 線路陰影內 |
| WS2812 線路 | **18 ms** | 硬下限 |

一次性成本：

| 項目 | 時間 |
|---|---:|
| layout 快取讀取 | 10 ms |
| layout 重建（500 組／隨機下限） | 40 ms／152 ms |
| flatify（真實 map／隨機下限） | 0.55 ms／14.5 ms |
| playlist 預建下一張 | 12–20 ms |
| 切換 swap | **246 µs** |

記憶體（9,600 px）：環 115.2 KB、flat 57.6 KB×2、值流 19.2 KB×2、layout 12.5 KB×2；
internal 主要留畀 stack／DMA／功能緩衝。

---

## 2. FPS 穩定度設計（已實作）

### 2.1 節拍

`esp_timer_start_periodic(frame_interval_ms)` — 完全按 `system.json`。
**唔會**因為遲到而縮短下一幀，亦**唔會**用 deadline 重新 arm。

### 2.2 時間緩衝（slack）

`nc_fps_clock`（純 C、host 可測）：

- **初始就滿**（`NETCORE_PIXEL_SLACK_MS`，預設 50 ms），避免開機追趕影響同步。
- 早完成 → 存入（上限 cap）。
- 遲到 → 由緩衝吸收：
  - 吸收得到 → `catchup +1`（可接受波幅）
  - 吸收唔到 → `overrun +1`（尖峰超出緩衝）
- **overrun 政策**：臨時尖峰先睇緩衝；**長期 overrun 要改效果或降 FPS**，
  唔應該靠加大 cap（cap 越大＝容忍越大延迟）。

### 2.3 量測

`0xF081` → `0xF082`：

```
target_us, n, avg_us, sd_us, min_us, max_us, bad,
starved, late, frames, slack_us, catchup, overrun, sum_us, hist[64]
```

- `hist[64]`：1 ms 一格（0–62 ms，63 = ≥63）。
- `bad` = `|dt − target| > 20%`。
- `starved` = 環冇新格；`late` = 一輪超時。

### 2.4 工具

| 工具 | 用途 |
|---|---|
| `tools/fps_trace.py` | 10–60 秒快測：sd／max／bad／slack／catchup／overrun，判 STABLE／BORDERLINE／UNSTABLE；可出 CSV＋PNG |
| `tools/fps_drift.py` | 重複 N 輪（每輪 30–60 s）量 ppm，換算兩塊 ESP 的建議強制同步間隔 |
| `tools/fps_longrun.py` | **小時級**長跑（預設 4 h、每 60 s 取樣）；自動等板／重連；CSV＋摘要 |
| `tools/fps_night.sh` | 等板 → OTA 安全映像 → 4 h 長跑（一鍵） |

### 2.5 ppm → 同步間隔

兩塊 ESP 相對漂移保守用 `2 × worst ppm`：

```
resync 秒數 = tolerance / (相對漂移 × 1e-6)
```

例：worst 20 ppm → 相對 40 ppm → 1 ms 偏差 25 s；5 ms 125 s；20 ms 500 s。

---

## 3. 長跑測試（小時級）

```bash
# 4 小時，每分鐘取樣
python3 tools/fps_longrun.py --port auto --hours 4 --interval 60 \
    --csv artifacts/fps_longrun.csv
```

- 板重啟會記 `gap` 並自動重連；CSV 每行一個樣本。
- 完結摘要：`ppm mean/sd/worst`、`bad`、`overrun` 總數、gap 次數、建議同步間隔。
- 夜間一鍵：`bash tools/fps_night.sh`。

**現況**：板嘅 USB 未返（見 §5），watcher 已啟動等板；有 port 就會自動
OTA 安全映像再跑 4 小時，輸出 `artifacts/fps_longrun.csv`。

---

## 4. apply 優化實驗（負結果＋一個正結果）

| 實驗 | apply（9,600 px、1 run、RGB） | 判斷 |
|---|---:|---|
| 基準（6 B 表 PSRAM） | 4.99–5.39 ms | — |
| uniform-RGB 專用內圈 | 5.01 ms | ❌ 無增益 |
| pipe core0／8 KB | 5.42 ms | ❌ 無增益 |
| pipe core1／16 KB | 5.42 ms | ❌ 無增益 |
| 值流 pool 放 internal | 5.39 ms | ❌ 無增益 |
| **4 B 表（bench，uniform RGB）** | **777／923 µs**（對 1,602／5,027） | ✅ **2.1×／5.4×** 潛力 |

**解讀**：production apply ≈0.56 µs/px，成本係「flat 57.6 KB 讀＋值流 57.6 KB 讀＋
環 38.4 KB 寫」合成；switch／chunk／值流位置都唔係瓶頸。
4 B 表（dst16＋src16，write/aprog 由 layout 統一）值得做，但要先喺 lab 驗證完整 boot。

![apply 實驗](assets/pixel-pairing/apply-experiments.png)

---

## 5. 事故與 recovery

- 2026-11-25 01:24：把 4 B 表接入 production 上板（9600／full256）後，
  `/dev/cu.usbmodem*` 同 OS USB tree 都冇 device。
- 懷疑 app 喺 USB 初始化前 panic／卡住 → 冇 CDC → 冇 OTA。
- 已回退 production 4 B 表改動；只保留 bench variant。
- 安全映像：`artifacts/ESP32-S3-Zero.bin`（Zero-Show、64 px）。

Recovery：

1. 斷電重插；有 port → `python3 tools/ota.py --port <PORT> artifacts/ESP32-S3-Zero.bin`
2. 仍無 port → 按住 BOOT 插電 2–3 s 入 download mode：
   ```bash
   IDF_PATH=$HOME/esp/esp-idf python3 make.py flash ESP32-S3-Zero \
       --project ESP32-S3-Zero-Show --port /dev/cu.usbmodemXXXX --full
   ```
3. 恢復後 `/pixel` 要重新上傳 Zero-Show 內容（板上仍係 9600 測試集）。

**教訓**：新表格式／新 boot 路徑先喺 lab（EffectPerf）行完整 boot，再入 production。

---

## 6. 現況與下一步

| 項目 | 狀態 |
|---|---|
| 零件時間圖 | ✅ `parts-time.png` |
| FPS 0xF081＋時間緩衝 | ✅ 實作；host tests 全綠 |
| fps_trace／drift／longrun 工具 | ✅ 完成 |
| 真機長跑 | ⏳ 等板（watcher 已啟動） |
| 4 B 表 production | ⏸ 已回退，待 lab 驗證 |
| 16-lane 實機 baseline | ⏸ 冇硬件 |

下一步（板返後）：
1. OTA 安全映像 → 跑 `fps_trace.py --reset --seconds 30`，確認 `sd <1%`、`overrun=0`。
2. 跑 `fps_longrun.py --hours 4`，早上睇 ppm／gap／overrun。
3. 用 ppm 決定兩塊 ESP 的強制同步間隔。
4. 4 B 表先喺 EffectPerf 驗證 boot，再入 production（預期 apply 5.4 → 2–3 ms）。
