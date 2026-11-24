# FPS 穩定度：量測與判讀

> 目標：唔止「平均 50 fps」，而係**每一幀都準**（絕對穩定）。
> 工具：`tools/fps_trace.py`（0xF081）；圖：`docs/assets/pixel-pairing/parts-time.png`。

---

## 1. 為什麼平均 fps 會騙人

`0xF071` 的 `fps_x10` 係「每秒平均」：一秒內有 48 幀 + 2 幀特別慢，平均仍然似 50。
真正影響觀感的是**逐幀間隔**（jitter）同**長 gap**。所以 firmware 另外記：

| 指標 | 意思 |
|---|---|
| `target_us` | 設定節拍（system.json `frame_interval_ms` × 1000） |
| `n` | 已記錄幾幀 |
| `avg_us` / `sd_us` | 平均間隔／標準差（sd 細 = 穩） |
| `min_us` / `max_us` | 最短／最長間隔（max 最有用） |
| `bad` | `|dt − target| > 20%` 的次數 |
| `hist[64]` | 1 ms 一格（0–62 ms，63 = ≥63 ms）的分佈 |
| `starved` | render 想取新格但環冇（生產者唔夠快） |
| `late` | 這一輪做超過一個 period |
| `frames` | 累計送出的幀 |

---

## 2. 用法

```bash
# 追 10 秒（每 0.5 秒取樣一次），開始前清零
python3 tools/fps_trace.py --port /dev/cu.usbmodemXXXX --seconds 10 --reset

# 出 CSV ＋ PNG（histogram＋fps 時間線）
python3 tools/fps_trace.py --port /dev/cu.usbmodemXXXX --seconds 30 \
    --reset --csv /tmp/fps.csv --plot /tmp/fps.png
```

終端摘要例：

```
target      20000 µs（50.00 fps）
frames      500
avg         20003 µs（49.99 fps）
sd          41 µs（0.21% of target）
min / max   19940 / 20890 µs（max deviation 890 µs）
bad         2（|dt-target| > 20%）
hist top    20 ms×498，21 ms×2
starved     0（環冇新格）；late 0；frames 500
verdict     STABLE
```

判讀：

| verdict | 條件（工具用） | 意思 |
|---|---|---|
| `STABLE` | sd ≤ 1% target，max deviation ≤ 25% | 可以接受 |
| `BORDERLINE` | sd ≤ 2% | 偶爾有尖峰，要查 hist |
| `UNSTABLE` | sd > 2% 或 max deviation > 25% | 要修 |

---

## 3. 不穩時查什麼（按 histogram 形狀）

| 形狀 | 可能原因 | 查 |
|---|---|---|
| 全部集中 target，1–2 格有尖峰 | 偶發 task／USB ISR／flash 寫入 | `late`、`starved`、`drop` |
| 分佈向左（< target） | timer 太密／重複提交 | `frames` vs `n` |
| 分佈向右（> target） | 生產者慢（effect/pair）、環空 | `starved`、`0xF071` 的 `pair_n`／`eff_n` |
| 一段時間整體偏移 | 溫度／頻率／電源 | 長追蹤 30–60 s，睇時間線 |

**絕對穩定**的下一步（未有板，未做）：
- 如果 `starved` 高：render 可以「hold 最後一格」照送（保持線路節拍），
  但會在環恢復時多送舊幀；要同「用戶寫什麼就顯示什麼」原則對齊。
- 如果 `late` 高：render 一輪做太多（多 lane 編碼），要分 lane／分核。
- 如果尖峰跟 USB／log 有關：量測時關 log 或提高 render 優先級。

---

## 4. 零件時間（同一頁睇）

![零件時間](../assets/pixel-pairing/parts-time.png)

- 每幀 20 ms：CPU 約 6.3 ms（effect 0.3 ＋ apply 5.4 ＋ white 0.1 ＋ commit 0.5）。
- 16×600：線路 18 ms（硬下限）> encode 6.4 ms，所以 CPU 有陰影。
- 一次性：cache 10 ms／build 40–152 ms／flatify 0.55–14.5 ms／預建 12 ms／swap 246 µs。

---

## 5. 現況

- Firmware 0xF081／0xF082 已加入，Zero-Show 建置成功；host tests 全綠。
- **板嘅 USB 未返**（2026-11-25 01:24 之後），所以工具**未上板驗證**。
- 恢復步驟見 `status.md` §7.233：斷電重插 → 仍無 port 就 BOOT＋插電入 download mode，
  用 `artifacts/ESP32-S3-Zero.bin` 重燒。
