# sin_bench — 正弦算法量測（host ＋ 真機）

> **不是** pass/fail 測試，所以唔入 `run_all.sh`。目的係答兩個問題：
> 「邊個算法最快／最準」同「喺真機上值幾多」。

## 檔案

| 檔案 | 角色 |
|---|---|
| `sin_bench.c` | host 微基準：5 種算法 ×（min／median／p99）＋精度＋記憶體 |
| `run.sh` | 建置＋跑（POSIX，`CC=clang` 可換編譯器） |
| `dev_probe.py` | 經 NC4（USB CDC）在**真機**切效果／分區，量引擎自己報的 `render_us`；跑完會還原設定 |

## Host

```bash
bash tools/hosttest/sin_bench/run.sh
```

輸出（2026-11-20，x86-64／MSVC /O2；10 萬次 × 120 批取 min）：

| 算法 | 表 | ns/call | ms/10k | max 誤差（0..4095） |
|---|---|---|---|---|
| `lut_1024` | 2 KB | 0.20 | 0.002 | 12.38 |
| `lut_4096` | 8 KB | 0.20 | 0.002 | 2.97 |
| `poly_q12`（MP） | 0 | 1.13 | 0.011 | 3.74 |
| `hyb64_lin` | 130 B | 0.49 | 0.005 | 2.53 |
| `hyb128_lin` | 258 B | 0.48 | 0.005 | 0.69 |
| `hyb256_lin` | 514 B | 0.46 | 0.005 | **0.23** |
| `cordic_q15` | 60 B | 12.0 | 0.120 | 0.66 |
| `sinf`（libm） | — | 3.34 | 0.033 | — |

結論：**混合（錨點表＋線性插值，n=256）比多項式快 2.4×、準 16×，表 514 B**。
引擎已落地 `nc_px_engine_sin_lin()`；MP 多項式保留做 `nc_px_engine_sin_q12()`。

## 真機

```bash
python tools/hosttest/sin_bench/dev_probe.py --port COM24 --samples 40
```

前提：板上跑有 `test_action`（`0xF060`）的 build，NC4 走 USB CDC。
會切 `BITS=8`、`DITHER=off`、`ZMASK=0xFF`，量完還原（分區效果跟全域、effect=BREATH）。

最新數字同解讀見 [`docs/00_pm/status.md` §7.134](../../../docs/00_pm/status.md)。

## 注意

- host 數字只代表**相對**成本；板上 cache／in-order 會拉高 absolute。
- `dev_probe.py` 只量到「sine ＋ wave 其餘數學」的合成斜率（≈444 ns／單位），
  逐次 sine 成本要 flash 專用 bench 才分得開。
- 相位一律由 `volatile` 陣列讀，避免被向量化／常數折疊。
