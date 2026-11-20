# sin_bench — 正弦算法量測（host ＋ 真機）

> **不是** pass/fail 測試，所以唔入 `run_all.sh`。目的係答三個問題：
> 「邊個算法最快／最準」、「出貨嘅三個函數實際誤差幾多」、
> 「喺真機上逐次 sine 值幾多」。

## 檔案

| 檔案 | 角色 |
|---|---|
| `sin_bench.c` | host 微基準：**直接 link 出貨中的 `nc_px_engine_tables.c`** ＋ 幾個對照算法 |
| `run.sh` | 建置＋跑（POSIX，`CC=clang` 可換編譯器） |
| `dev_probe.py --sin` | 真機 `0xF063 SINBENCH`：逐次 sine 成本＋精度（唔使切效果） |
| `dev_probe.py`（預設） | 真機 `0xF060`：切效果讀 `render_us`，量合成斜率（舊模式） |

## Host

```bash
bash tools/hosttest/sin_bench/run.sh            # 完整（REP=120）
bash tools/hosttest/sin_bench/run.sh --quick    # 快（REP=20）
bash tools/hosttest/sin_bench/run.sh --json     # 機器可讀（存檔／比較）
```

★ = 固件出貨中的函數（工具唔會再自己抄一份，固件改咗就跟住改）。
2026-11-20，x86-64／gcc -O2；100k 次 × 120 批取 min：

| case | 表 | min ns | max 誤差（LSB） | 註 |
|---|---|---|---|---|
| ★ `eng_sin` | 2 KB | 1.3 | 13.31 | 1024 點 Q15 最近點；內建效果用 |
| ★ `eng_sin_lin` | 514 B | 1.8 | **0.72** | 256 錨點＋線性插值；四捨五入後 |
| ★ `eng_sin_q12` | 0 | 1.8 | 3.74 | MP 多項式；DATA program 用 |
| `lut_4096` | 8 KB | 0.4 | 3.91 | 對照：4096 點最近點 |
| `hyb64_lin` | 130 B | 1.1 | 3.52 | 對照：64 錨點 |
| `hyb128_lin` | 258 B | 1.1 | 1.68 | 對照：128 錨點 |
| `cordic_q15` | 60 B | 16.8 | 1.57 | 對照：15 次迭代 |
| `sinf_libm` | — | 2.2 | 1.00 | 對照：libm |

誤差域 = 0..4095（1 LSB = 1/4096 滿刻度），ideal 先夾到 [0, 4095]（12-bit 表示不到 4096）。

**讀數要小心**：`min ns` 在虛擬機／有背景工作的機器上會跳（同一份 code 見過 1.2–2.0）；
相對次序穩定：`lut_4096` < `hyb*` ≈ `eng_sin_lin` < `eng_sin_q12` ≪ `cordic`。

### 實機（ESP32-S3 240 MHz，`0xF063 SINBENCH`，2026-11-20）

| case | ns/call | ≈cycles | max 誤差 |
|---|---:|---:|---:|
| `eng_sin` | 88 | 21 | 13.31 |
| `eng_sin_lin` | 146 | 35 | 0.72 |
| `eng_sin_q12` | 157 | 38 | 3.74 |

板上 `sin_lin` 比多項式快 7%、準 5.2×（host 兩者打平）。誤差必須**全相位 65536 點**掃，
每 16 點取樣會漏掉峰值（10.37 vs 13.31）—— 0xF063 已改成全掃。

## 真機

```bash
# ① 逐次 sine（推薦；build 要有 0xF063）
python tools/hosttest/sin_bench/dev_probe.py --port COM24 --sin
python tools/hosttest/sin_bench/dev_probe.py --port COM24 --sin --iters 200000 --json

# ② 合成斜率（只有 0xF060 的 build 都可以）
python tools/hosttest/sin_bench/dev_probe.py --port COM24 --samples 40
```

模式① 直接回（`err` 係 milli-LSB，即 ×100）：

```json
{"n":50000,"smp":4096,
 "q12":{"ns":12,"err":374},"lin":{"ns":5,"err":72},"sin":{"ns":4,"err":1331}}
```

模式② 會切 `BITS=8`、`DITHER=off`、`ZMASK=0xFF`，量完還原（分區效果跟全域、effect=BREATH）。

## 注意

- host 數字只代表**相對**成本；板上 cache／in-order 會拉高 absolute → 用 `--sin` 量實機。
- 精度閘門已經搬入 `pxEngine_test`（`check_sin_accuracy()`）：三個函數超出界限就紅，
  所以「改壞正弦」唔會靜靜過關。呢支工具只負責量同研究，唔做 gate。
- `--json` 輸出可以存檔對比（例如換算法前後）；格式：`{n, rep, fpp, cases:[{name,min_ns,...,max_err,table}]}`。
