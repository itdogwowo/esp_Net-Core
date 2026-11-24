# gamma_bench — 有 gamma 表 vs 冇 gamma（host；唔需要硬件）

> 答三個問題：**同一個輸入，有表同冇表差幾多？**、**一條 fade 幾時才「看起來一半亮」？**、
> **呢啲數字有冇回歸？**

## 為什麼會有這張表

`nc_px_engine_gamma()` 係 **sRGB 亮度曲線**（`pow(x, 2.2)`），同波形完全無關：

| | 大小 | 誰用 |
|---|---:|---|
| 正弦 `s_sin_lin[257]` | 514 B | 波形數學（建波表時） |
| 波表 `s_wave[]` | 512 B／效果 | 每幀取值 |
| **gamma `s_gam[1024]`** | **2,048 B** | **8-bit 引擎的內建效果**（breath／wave／zone1／ramp） |

**12-bit 三層路徑（效果 → 抖動 → 配對）唔會 call gamma**：`nc_px_layout_apply()`
只做 `>>4`（同 MicroPython 一致）。所以對 12-bit 板，這張表係 2 KB ＋ 0 ns；
對 8-bit 板（`CONFIG_NETCORE_CONTENT=n`）佢就係畫面亮度的定義。

## 跑法

```bash
bash tools/hosttest/gamma_bench/run.sh                  # 建置＋跑＋對 golden
bash tools/hosttest/gamma_bench/run.sh --update         # 曲線真的變了才重影
bash tools/hosttest/gamma_bench/run.sh --csv /tmp/g.csv # 另存 CSV

# 畫圖（Pillow；repo 的 docs/00_pm/gamma-compare.png 就係咁生成）
python tools/hosttest/gamma_bench/plot.py /tmp/g.csv docs/00_pm/gamma-compare.png
```

★ = **直接 link 出貨中的 `nc_px_engine_tables.c`**，印出嚟嘅曲線就係板上跑嘅同一張表。

## 數字（2026-11-24，host x86-64）

| 輸入 level | linear | 冇 gamma | 有 gamma | 差 |
|---:|---:|---:|---:|---:|
| 64 | 16,448 | 64 | **12** | −52 |
| 128 | 32,896 | 128 | **56** | −72 |
| 192 | 49,344 | 192 | **136** | −56 |
| 255 | 65,535 | 255 | 255 | 0 |

- **50% 線性 → 21% 輸出（55／255）**：所以同一條 fade，有 gamma 時要行到 **73% 時間**
  才「看起來一半亮」（`0.5^(1/2.2) = 0.73`），冇 gamma 就 50% 時間。
- 一條正弦 fade 300 幀：最大差 **73／255**、平均差 **36／255**；有 gamma 時最暗的一半
  時間（輸出 < 64）佔 **157／300 幀** —— 即係「暗部停留得久、看得到細節」。
- 12-bit 路徑 = 冇 gamma 那一行（`>>4`），上面嘅差距唔會出現。

板上成本（唔係呢支量，係 `0xF065 eff.gam_ns`）：**151 ns/值**、38,400 值 5.80 ms。
詳見 `docs/00_pm/effect-perf.md` §3.2。

## 檔案

| 檔案 | 角色 |
|---|---|
| `gamma_bench.c` | 曲線表 ＋ 5 個斷言 ＋ fade 比較 ＋ CSV |
| `run.sh` | 建置（`-Wall -Wextra -Werror`）＋ golden diff |
| `golden.txt` | 回歸基準（曲線改了就 diff 出來） |
| `plot.py` | CSV → PNG（灰階兩條 strip ＋ 曲線） |
| `include/` | host 用的 `esp_err.h`／`esp_log.h`／`esp_timer.h`／`sdkconfig.h` shim |
