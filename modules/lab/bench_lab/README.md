# bench_lab — Matrix 8×8 跑分重跑（2026-11-26）

一次性 lab：把板上現行指標一次過抽出來，再畫成一張圖。唔改 firmware，只用 WebMaster REST。

## 點跑

```powershell
# 前提：WebMaster 跑住（佔住 COM24 係正常；API 同 UI 共用）
python modules/lab/bench_lab/bench_rerun.py     # → artifacts/bench/latest.json
python modules/lab/bench_lab/draw_bench.py      # → docs/assets/bench-latest.png
```

`bench_rerun.py` 抽：
- `status sys`／`tasks`／`mem`（記憶體帳、環槽）
- `0xF071` × 10（fps／frame_us／enc／tx／eff／pair／apply／commit／lat／starved／drops）
- `0xF06B` × 3 mode（rgb_breath／rainbow／white_breath 的顏色數與亮度）

`draw_bench.py` 用 Pillow（Windows 系統 Python 有；**唔需要 matplotlib**）畫：
20 ms 幀預算、CPU 各階段、FPS 樣本、三個 mode 的顏色檢查、其他跑分 footer。

## 2026-11-26 結果摘要（fw a7aad43、app 6c7c1c6e、idf v6.1.1 (WSL)）

- fps **50.0**、drops **0**、frame_us 20,003（min 19,758／max 20,078）
- CPU 一幀 256 µs（1.3%）：eff 14／pair 41／apply 33／enc 150／tx 18
- 線路 1,843 µs（9.2%）、idle 17.9 ms（89.5%）
- `lat` 461 µs（max 762）、`starved` 2、`fill` 1
- rainbow 64 色；rgb_breath／white_breath 各 1 色（切 mode 第一幀會是舊畫面，正常）
- 圖：`docs/assets/bench-2026-11-26.png`（第一次跑的固定檔名；之後重跑覆蓋 `bench-latest.png`）
