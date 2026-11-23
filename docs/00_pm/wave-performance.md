# 波形計算性能報告（六工具合成）

> **量什麼**：`keep`／`math_now`／`square_wave_now`／`pulse_wave`／`pulse`／`starter`
> 六種畫波工具，每個型別跑 **40,000 步** 的計算成本（= 10,000 px × RGBW 各一個值）。
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

## 2. C 實作（host x86，40,000 步）

| 型別 | ns/步 | µs/步 | 40,000 步總時間 | 一幀 64 px（µs） | checksum |
|---|---:|---:|---:|---:|---|
| `keep` | 3.80 | 0.004 | **0.152 ms** | 0.16 | 27D6E145 |
| `starter` | 2.92 | 0.003 | **0.117 ms** | 0.15 | AC0C97C5 |
| `math_now` | 7.12 | 0.007 | **0.285 ms** | 0.32 | AF021CC5 |
| `square_wave_now` | 5.70 | 0.006 | **0.228 ms** | 0.30 | 67C19825 |
| `pulse_wave` | 5.55 | 0.006 | **0.222 ms** | 0.28 | 67C19825 |
| `pulse` | 3.02 | 0.003 | **0.121 ms** | 0.18 | 968D79C5 |

- `compile()`：**0.01 µs/次**（40,000 次 = 0.36 ms）—— 只在載入時做。
- `keep`／`starter` 冇正弦，最快；`pulse_wave` 同 `square_wave_now` 的 checksum 一樣，
  因為 `pulse=2047` 同「≥2048」在這個波形上等價（實測，唔係假設）。
- **40,000 步 = 10,000 px × R/G/B/W 各一個值**：C host 全部型別都係 **0.12–0.29 ms**。

## 3. C 的 sine（ESP32-S3 240 MHz 實機，`0xF063`）

**2026-11-23 起引擎只有一個正弦**：`nc_px_engine_sin_lin`（256 錨點＋線性插值，514 B）。
舊的兩個（1024 點 Q15 查表、MP 多項式）已從固件刪除 —— 三個並存會令人每次都要問「而家用邊個」。

| 函數 | ns/次 | max 誤差（LSB） | 用途 |
|---|---:|---:|---|
| `nc_px_engine_sin_lin`（256 錨點） | **146** | **0.72** | **全部**：內建效果＋DATA program |

（刪除前同一塊板的數字，留作歷史對照：1024 點 Q15 表 88 ns／13.31 LSB、MP 多項式 157 ns／3.74 LSB；
見 `status.md` §7.138。）

**由 sine 推 program 的實機成本**：program 熱路徑每值一次 sine ＋ 段落派發，實測 460–468 ns/值
（`0xF065` 2026-11-23，三種波都一樣）；但生產路徑會**預算 256 點波表**，之後每幀只是索引
（29.4 ns/值）。所以 sine 只影響「建表一次」（256 × ~460 ns ≈ 118 µs）。

**每幀值流（波表已建好，只讀表＋寫值流）**：實測每值 **29.4 ns**（64 / 960 / 9,600 值都一樣）。

| 畫布 | 值 | 每幀（波表快路徑） | 50 FPS 預算佔比 | 建表一次（4 段波） |
|---|---:|---:|---:|---:|
| 64 px（8×8） | 64 | **2.0 µs** | 0.01% | ~0.11 ms |
| 960 px（16×60） | 960 | **28.3 µs** | 0.14% | ~0.11 ms |
| 9,600 px（16×600） | 9,600 | **0.282 ms** | 1.4% | ~0.11 ms |
| **9,600 px × 4 值（RGBW）** | 38,400 | **1.13 ms**（值流 internal）／5.30 ms（落 PSRAM） | 5.6%／26.5% | ~0.11 ms |
| 同上，每值加一次 `% total`（生產 `cached_vals`） | 38,400 | 2.03 ms（internal） | 10.2% | ~0.11 ms |
| 同上，冇波表（interpreter fallback） | 38,400 | 20.4 ms | **102%（超支）** | — |

（`0xF065` 2026-11-23，S3 240 MHz、值流緩衝在 internal；每值成本 29.4 ns 在 64／960／9,600 值完全一樣。）
結論：**16×600 全畫布仍然做得**（1.4% 預算）；真正貴的是「值流落 PSRAM」（×4.7）與「冇波表的 interpreter」（×18）。

## 4. MicroPython 實作（CPython 40,000 步，PC 對照）

| 型別 | `value_at` µs/步 | 40,000 步總時間 | 一幀 64 px（µs） | checksum |
|---|---:|---:|---:|---|
| `keep` | 0.45 | **18.0 ms** | 4.25 | 27D6E145 |
| `starter` | 0.43 | **17.2 ms** | 4.00 | AC0C97C5 |
| `math_now` | 0.82 | **32.6 ms** | 4.22 | AF021CC5 |
| `square_wave_now` | 0.81 | **32.5 ms** | 4.13 | 67C19825 |
| `pulse_wave` | 0.81 | **32.4 ms** | 4.14 | 67C19825 |
| `pulse` | 0.51 | **20.6 ms** | 4.75 | 968D79C5 |

- `compile()` **0.75 µs/次**；建 100 步波表 **3.9 µs**（只在載入／warm-up）。
- `speed>1` 有 step 記憶：每 N 步只算 1 次。
- CPython 比 C（host）慢約 **100–120×**（per value）—— 解釋器成本，唔關算法事。

## 5. 驗證（結果可信嗎？）

**跨語言 checksum 對撞**：C 同 MicroPython 係兩份獨立實作，用**同一組 program**
（F=10、l_max=4095、l_lim=100、phi=0、end_Time=100、pulse=2047）各跑 40,000 步，
對每一粒值做 FNV-1a：

| 型別 | C checksum | MP checksum | 一致？ |
|---|---|---|---|
| `keep` | 27D6E145 | 27D6E145 | ✅ |
| `starter` | AC0C97C5 | AC0C97C5 | ✅ |
| `math_now` | AF021CC5 | AF021CC5 | ✅ |
| `square_wave_now` | 67C19825 | 67C19825 | ✅ |
| `pulse_wave` | 67C19825 | 67C19825 | ✅ |
| `pulse` | 968D79C5 | 968D79C5 | ✅ |

- `min`／`max` 亦逐項一致（例如 `math_now` 100..4094、`starter` 0..0）。
- checksum 非 0 亦證明量測冇被優化掉（編譯器唔會幫你算 40,000 次再湊出同一個 hash）。
- 另有 `tools/hosttest/pxProgram_test`（89 個檢查）用 MicroPython 做 oracle 逐值對過
  （含段邊界、pulse 開關、繞回），係語義層的閘門。

## 6. 判讀

1. **算法本身唔貴**：C host 每步 2.8–11.6 ns，實機 sine 88–157 ns；瓶頸唔喺波形，
   而係「每像素要 call 幾多次」同「之後嘅色彩／編碼」。
2. **畫布大先係問題**：9,600 px × 200 ns ≈ 1.9 ms/幀；如果每 px 再多 2 次 sine 就 ×3。
3. **`sin_lin` 係最平衡**：實機比多項式快 7%、準 5.2×（§3）；但 `math_now` 兩者相差
   只係幾十 ns，除非大畫布否則用 `q12`（MP 對位）都可以。
4. **一次性成本可忽略**：`compile` 0.01 µs（C）／0.73 µs（CPython）；
   波表建立 3.8 µs（100 步）—— 只要唔好每幀重建。

## 7. 視覺對照

- Web 工具（`web_tools/webmaster/`）「🌊 波形」頁：貼 JSON／開檔案 → 即時畫出
  **整條燈鏈 + 時間曲線**（同 firmware 公式一致，唔使連板）。
- CLI：`python3 tools/hosttest/../PC/pixel_curve.py`（mp repo）出 ASCII 曲線／`--csv`。

## 8. 已知限制

- 板上量測要 **tests 映像**（`0xF063` 只在 `NC_ENABLE_TESTS` 內）；release 映像冇。
- host 數字（x86）只代表相對成本；實機 cache／in-order 會拉高 absolute。
- 未量：多任務搶核、PSRAM 波表（現時波表喺內部 RAM）、`spacing≠0` 的 cache 行為。
