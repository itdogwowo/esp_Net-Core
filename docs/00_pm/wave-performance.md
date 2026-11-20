# 波形計算性能報告（六工具合成）

> **量什麼**：`keep`／`math_now`／`square_wave_now`／`pulse_wave`／`pulse`／`starter`
> 六種畫波工具，每個型別跑 **10,000 步** 的計算成本。
> **為什麼按步**：pixel 子系統係步進式（一個 frame = 一步），`speed` 只係「幾步走一格」。
> **最後更新**：2026-11-20

---

## 1. 量測點（三個實作、同一種 workload）

| 實作 | 位置 | 量法 | 平台 |
|---|---|---|---|
| **C（板上真正跑）** | `firmware/components/sw/pixel/engine/nc_px_engine_program.c` | `nc_px_engine_program_value()` × 10,000 步 | host x86 ＋ ESP32-S3 240 MHz |
| C 的 sine | `engine/nc_px_engine_tables.c` | `0xF063 SINBENCH`（50,000 次取最小） | ESP32-S3 實機 |
| **MicroPython（語義權威）** | `mp_Net-Core/slave/lib/sw/PixelMathMethod.py` | `value_at()` × 10,000 步 | CPython（PC） |

重跑：

```bash
# ① C（host，唔需要 IDF／板）
bash tools/hosttest/program_bench/run.sh

# ② C 的 sine（要 tests 映像；板上量）
python3 tools/hosttest/sin_bench/dev_probe.py --port <PORT> --sin

# ③ MicroPython（PC 對照；同一支可上板跑）
cd ../mp_Net-Core && python3 -B test/pixel/bench_wave_segments.py 10000
```

---

## 2. C 實作（host x86，10,000 步）

| 型別 | ns/步 | µs/步 | 一幀 64 px（µs） |
|---|---:|---:|---:|
| `keep` | 2.80 | 0.003 | 0.17 |
| `starter` | 2.80 | 0.003 | 0.16 |
| `math_now` | 6.20 | 0.006 | 0.36 |
| `square_wave_now` | 6.10 | 0.006 | 0.37 |
| `pulse` | 6.00 | 0.006 | 0.29 |
| `pulse_wave` | 11.60 | 0.012 | 0.67 |

- `compile()`：**0.01 µs/次**（10,000 次 = 0.10 ms）—— 只在載入時做。
- `keep`／`starter` 冇正弦，係純查表／常數，所以最快。
- `pulse_wave` 較慢（正弦 + 門檻比較 + 分支），仍然係 64 px 之下 0.67 µs。

## 3. C 的 sine（ESP32-S3 240 MHz 實機，`0xF063`）

| 函數 | ns/次 | max 誤差（LSB） | 用途 |
|---|---:|---:|---|
| `nc_px_engine_sin`（1024 點表） | **88** | 13.31 | 內建效果（WAVE 等） |
| `nc_px_engine_sin_lin`（256 錨點） | **146** | **0.72** | DATA program 用 |
| `nc_px_engine_sin_q12`（MP 多項式） | **157** | 3.74 | MP 對位 |

**由 sine 推 program 的實機成本**：host 上 `math_now`（6.2 ns）比 `sin_q12`（1.8 ns）
多 ~4.4 ns 的段查找＋算術；板上同樣開銷約 30–60 ns →
`math_now ≈ 157 + 50 ≈ **200 ns/px**`。

| 畫布 | 每幀（math_now） | 50 FPS 預算佔比 |
|---|---:|---:|
| 64 px（8×8） | ≈ 13 µs | 0.07% |
| 960 px（16×60） | ≈ 0.19 ms | 1% |
| 9,600 px（16×600） | ≈ 1.9 ms | 9.6% |

結論：**16×600 全畫布仍然做得**（9.6% 預算），但 `pulse_wave` 類（+90%）同
每像素再做色彩轉換會推高佔比；要留 headroom 就將 sine 換 `sin_lin`（146 ns）。

## 4. MicroPython 實作（CPython 10,000 步，PC 對照）

| 型別 | `value_at` µs/步 | 一幀 64 px（µs） |
|---|---:|---:|
| `keep` | 0.21 | 4.28 |
| `starter` | 0.19 | 4.00 |
| `math_now` | 0.58 | 4.27 |
| `square_wave_now` | 0.59 | 4.14 |
| `pulse_wave` | 0.58 | 4.16 |
| `pulse` | 0.26 | 4.02 |

- `compile()` **0.73 µs/次**；建 100 步波表 **3.8 µs**（只在載入／warm-up）。
- `speed>1` 有 step 記憶：每 N 步只算 1 次（`speed 1→4`：4.28→1.21 µs/幀）。
- CPython 比 C（host）慢約 **100×**（per value）—— 呢個係解釋器成本，唔關算法事；
  板上 MicroPython 會比 CPython 快，但仍然遠慢過 C。

## 5. 判讀

1. **算法本身唔貴**：C host 每步 2.8–11.6 ns，實機 sine 88–157 ns；瓶頸唔喺波形，
   而係「每像素要 call 幾多次」同「之後嘅色彩／編碼」。
2. **畫布大先係問題**：9,600 px × 200 ns ≈ 1.9 ms/幀；如果每 px 再多 2 次 sine 就 ×3。
3. **`sin_lin` 係最平衡**：實機比多項式快 7%、準 5.2×（§3）；但 `math_now` 兩者相差
   只係幾十 ns，除非大畫布否則用 `q12`（MP 對位）都可以。
4. **一次性成本可忽略**：`compile` 0.01 µs（C）／0.73 µs（CPython）；
   波表建立 3.8 µs（100 步）—— 只要唔好每幀重建。

## 6. 視覺對照

- Web 工具（`web_tools/webmaster/`）「🌊 波形」頁：貼 JSON／開檔案 → 即時畫出
  **整條燈鏈 + 時間曲線**（同 firmware 公式一致，唔使連板）。
- CLI：`python3 tools/hosttest/../PC/pixel_curve.py`（mp repo）出 ASCII 曲線／`--csv`。

## 7. 已知限制

- 板上量測要 **tests 映像**（`0xF063` 只在 `NC_ENABLE_TESTS` 內）；release 映像冇。
- host 數字（x86）只代表相對成本；實機 cache／in-order 會拉高 absolute。
- 未量：多任務搶核、PSRAM 波表（現時波表喺內部 RAM）、`spacing≠0` 的 cache 行為。
