# Pixel 管線技術進化日誌（每一步都有數字）

> **為什麼有這份文件**：`git log` 只講「改了什麼」，`status.md` 只留最新快照。
> 要答「點解 encode 由 20 ms 變 6 ms？中間試過什麼？幾時決定唔做？」要翻 5 個
> commit 加 3 節 status 才拼得出來。呢份係**時間線**：一條 = 一個改動 → 量到幾多 →
> 為什麼有效（或者為什麼失敗）。
>
> 新讀者（包括 AI）由呢份入手就夠，唔使做偵探。
>
> - **性質**：歷史紀錄，唔會被覆蓋；新的一條 append 在尾。
> - **現況數字**以 [status.md](../00_pm/status.md) 為準（快照）；
>   呢份回答「點行到呢度」。
> - **量測方法**見 [measurement-method.md](../02_guides/measurement-method.md)。
> - 最後更新：2026-11-26（首次建立；涵蓋編碼／配對／環／FPS 四條主線）。

---

## 0. 先睇名詞（三個最容易誤會的地方）

| 名詞 | 意思 | **唔係** |
|---|---|---|
| **SWAR** | SIMD Within A Register：喺一個 64-bit 整數裡面同時處理 8 個 byte（呢度做 8×8 位矩陣轉置） | 唔係 SIMD 指令集、唔係多核並行 |
| **8×8 轉置** | 8 條 lane × 每個線材 byte 的 8 個 bit ＝ 8×8 **位矩陣**，轉成「8 個 tick × 8 條 lane」的行輸出 | **完全唔關 8×8 燈板／8×8 對陣事**（`rgb-matrix-dither.md` 那個 8×8 係 64 顆實體燈；呢個 8×8 係 64 個 bit，同燈的排布零關係） |
| **lane／tick** | lane = 一條 WS2812 data 線；tick = I80 一個時序單元（16-bit 模式 = 2 bytes） | lane 唔係「第幾顆燈」；tick 唔係 frame |
| **bit_cells** | 一個 bit 用幾個時序單元表示（WS2812 預設 3） | 唔係 bit 數 |
| **packed bitstream** | 把一個線材 byte 的 8 個 bit 波形壓成 `bit_cells` 個 byte 存起，之後淨係 memcpy | 唔係壓縮，只係換佈局 |
| **gather** | 由 N 條各自獨立的中間緩衝各讀 1 byte（位址分散） | 唔係 DMA scatter-gather |
| **廣播（broadcast）** | 16 條 lane 內容一樣時，同一個 mask 寫給全部 lane，唔逐 lane 算 | 唔係無線廣播 |
| **enc_us** | `0xF061`／`0xF079` 回報「上一幀 backend encode 用時」 | 唔係整幀時間（唔含 DMA 送線） |

### 0.1 到底「8×8 SWAR 轉置」係咩（用 16-lane 實際情況講）

I80 要送的是一串 **bus word**，每個 tick 一個 word；16-bit 模式下 word 的
**第 k 個 bit = 第 k 條 lane 這個 tick 想高定低**。

但我們的資料是「lane-major」：每條 lane 各自有一個線材 byte（8 個 bit = 8 個 tick）。
所以要把

```
lane0 byte: b7 b6 b5 b4 b3 b2 b1 b0
lane1 byte: c7 c6 c5 c4 c3 c2 c1 c0
...
lane7 byte: h7 h6 h5 h4 h3 h2 h1 h0
```

**轉成**

```
tick 7 的 mask: h7 g7 f7 e7 d7 c7 b7 a7
tick 6 的 mask: ...
...
tick 0 的 mask: h0 g0 f0 e0 d0 c0 b0 a0
```

即「8 條 lane × 8 個 bit」的**位矩陣轉置**。8 個 byte 剛好塞入一個 `uint64_t`，
所以可以在**一個暫存器**裡做，唔使逐個 bit 搬。

用 4 lane × 4 bit 的例子（8×8 同一個道理，只係大兩倍）：

| lane | byte（低 4 bit） |
|---|---|
| 0 | `1001` |
| 1 | `0110` |
| 2 | `1111` |
| 3 | `0000` |

轉置後，每個 tick 的 mask：

| tick | 邊幾條 lane 要高 | mask |
|---|---|---|
| 3（bit3） | lane0、lane2 | `0101` |
| 2（bit2） | lane1、lane2 | `0110` |
| 1（bit1） | lane1、lane2 | `0110` |
| 0（bit0） | lane0、lane2 | `0101` |

實作就係 [nc_pixel_encode.c](../../firmware/components/sw/pixel/nc_pixel_encode.c#L669)
的 `pack_bus_t8()`：三步「shift＋mask＋xor」，每步交換間距 2/4/8 的 bit 群
（Hacker's Delight §7-3 的 64-bit 版），大約 15 條指令處理 64 個 bit。

```c
t = (x ^ (x >> 7))  & 0x00AA00AA00AA00AAULL; x = x ^ t ^ (t << 7);
t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL; x = x ^ t ^ (t << 14);
t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL; x = x ^ t ^ (t << 28);
```

16 條 lane 就係兩個 `uint64_t`（lane 0–7、lane 8–15）各自轉置，再逐 bit 拼成
16-bit bus word（[nc_pixel_encode.c:761-777](../../firmware/components/sw/pixel/nc_pixel_encode.c#L761-L777)）。

> ⚠️ **同之前那個「8×8 對陣」的關係：零。** 那邊講的是 64 顆 WS2812 的實體排布
> （8 行 × 8 列、蛇形接線、分區抖動）；這邊的 8×8 只是「8 個 byte = 64 bit」，
> 唔涉及燈的位置、數量、接線。
>
> 而且記住：**單獨換成 SWAR 轉置只有 1.22×**（見 §1 A4）。真正快起來係因為
> 拿走了中間緩衝（gather），唔係因為位運算寫得靚。

---

## 1. 主線 A：編碼（畫波）— 143 ms → 6.36 ms

### 1.1 一覽

| # | 日期 | commit | 改動 | 16×600 | 量測點 | 倍數 |
|---|---|---|---:|---|---:|---:|
| A0 | 11-18 | — | 逐 timing unit shift／OR | **143 ms** | `0xF062` bench（PSRAM） | — |
| A1 | 11-19 早 | — | 波形 LUT ＋ direct-OR | **38.5 ms** | 同上 | 3.7× |
| A2 | 11-19 | — | 純 SRAM 對照（證明唔係頻寬） | **35.4 ms** | `0xF062` release，`sram=1` | 1.09× |
| A3 | 11-19 08:17 | `467af43` | packed bitstream ＋ nibble spread ＋ PSRAM DMA 槽 | **14.33 ms**（pack 7.07＋merge 7.08） | `0xF062` release 純 SRAM | 2.47× |
| A4 | 11-24 | —（只係 bench 探路） | ❌ 只把 merge8 換成 SWAR | 22.5 → **18.5 ms**（1.22×） | `0xF077` variant 10 | 失敗 |
| A5 | 11-24 20:54 | `197e1e0` | `pack_bus()` 原型（展開＋轉置一次過） | host x86 2.72× | host test | 未接 production |
| A6 | 11-24 21:44 | `dd64ef8` | production 接線＋廣播＋單 lane 快路徑 | **24.0 → 6.36 ms** | `0xF077` variant 17 同一次 A/B | **3.77×** |
| A7 | 11-25 | — | 1 lane 例外路徑 | 64 px：840 → **139–165 µs** | `0xF061` 上板 | 5.1–6.0× vs 840 |
| A8 | 11-25 21:49 | `e578be6` | `pixel_wave_task` → `pixel_encode_task` | 不變 | — | 純改名 |

⚠️ **唔可以跨行比倍數**：A0–A3 係 `0xF062`（release 映像、PSRAM buffer，量 pack／merge
本體），A4／A6 係 `0xF077`（EffectPerf 映像、同一份 payload 的新舊路徑 A/B，
量整個 encode），A7 係真 production `enc_us`。**唯一可以直接對比的，是同一次 A/B
的 24.0 → 6.36 ms**；plan 裡面的「舊 `enc_us` 19.4 ms」係另一個量測點的舊值。

### 1.2 逐條

**A0 原始（143 ms）** — 每個 bit 逐個 timing unit `if (k < hi) out[...] |= 1 << shift;`。
16×600 一幀 86.4 KB 輸出、69 萬次分支＋移位，6.9 fps。瓶頸係純 CPU。

**A1 波形 LUT ＋ direct-OR（38.5 ms，3.7×）** — 256 項表：8-bit 值 → `bit_cells×8`
個時序單元；逐線材 byte 變 `memcpy`。再開 `wave_frame_or()` 直接 `|=` 入 I80 槽，
取消 lane scratch。
**教訓**：查表贏 3.7×，但仍然係「每條 lane 各自寫一次 86.4 KB」，寫入次數冇减少。

**A2 純 SRAM 對照（35.4 ms）** — 同一份 code，buffer 由 PSRAM 搬去 internal：
只快 8–10%。**證明瓶頸係 CPU，唔係記憶體頻寬** —— 呢個結論直接決定後面走
「減少每幀工作量」而唔係「換更快記憶體」。

**A3 packed bitstream ＋ nibble spread（14.33 ms，2.47×）** — 兩件事：
1. `wave_pack()`：每條 lane 的波形壓成 bitstream（600 px × 3 線材 byte × 3 B
   ＝ 5,400 B／lane）；
2. `pack_merge8()`：8 條 lane 一組，用 `s_spread_nibble[16]`（4 B/項）把每條 lane
   的 nibble 攤平，u32 OR 埋 → 一次寫 8 個 tick。
再加 DMA 槽改 PSRAM（`MALLOC_CAP_SPIRAM|DMA`＋`esp_cache_msync`），16×600 兩槽
173.6 KB 放得落（之前 internal 差 8.7 KB）。
**代價**：多了一趟中間緩衝（下一個階段就係拆它）。

**A4 失敗實驗：只換 SWAR 轉置（22.5 → 18.5 ms，1.22×）** — 估算本來係 5–8×
（因為「64-bit 三步交換」聽起來快好多）。上板實測：
`merge8=22515us swar=18516us match=1` → **只有 1.22×**。
**原因**：瓶頸唔係移位算術，係 **gather** —— 每個 byte index 要由 16 條獨立
lane 緩衝各讀 1 byte（PSRAM／cache），輸出 86.4 KB。SWAR 每輪仍然要做 16 次
load ＋移位組 u64。
**呢個負結果係整個 Task 6 的轉折點**：唔可以再改算術，要改**資料流**。

**A5 原型：展開＋轉置一次過（`nc_px_codec_pack_bus`）** — 形狀照 FastLED wave3：
每顆燈即時取 lane 位元組（`lane_px()`，抖動狀態照樣推進）→ 8×8 轉置 → 逐 tick
直接寫 bus word。**冇 `wave_pack`、冇 `merge8`、冇 172.8 KB 中間緩衝**
（86.4 KB 寫 ＋ 86.4 KB 讀返）。
host 逐 byte 對拍 6 個配置（8/16 lane、RGBW、8/12-bit dither、`bit_cells` 3/4/8、
12 lane 高 4 lane 空）＝ **360 checks 全過**；x86 速度 0.117 → 0.043 ms（2.72×，
唔可以外推）。

**A6 production 接線 ＋ 廣播快路徑（24.0 → 6.36 ms，3.77×）** — 兩個改動：
1. `i80_encode()` 改成一次過 `pack_bus()`，刪走 8 個 packed 中間緩衝
   （16×600 省 43.2 KB PSRAM）；
2. **廣播**：production 預設 8-bit／無抖動，16 條 lane 共用同一份 payload 與 codec
   設定 ⇒ `lane_px()` 對每條 lane 回同一個 byte。舊路徑照樣做 16 次 `wave_pack`
   ＋兩次 `merge8`（**15/16 係白工**）；新路徑編一次，mask 直接 `all` 或 `0`，
   連轉置都唔使。
上板 A/B（ESP32-S3、EffectPerf、同板同 payload、逐 byte `memcmp`）：

| px | 舊 | 新 | 倍數 | `bus_ok` |
|---|---:|---:|---:|---:|
| 9,600（16×600） | 24,018 µs | **6,365 µs** | **3.77×** | 1 |
| 2,400（16×150） | 4,765 µs | **1,166 µs** | **4.09×** | 1 |

reps=16 重跑數字一樣 → 唔係偶然。

**A7 1 lane 的例外（回歸修正）** — 上板後發現：1 lane／8-bit 時逐 tick 廣播
**反而慢**（64 px：840 µs vs 舊 357 µs），因為每個 tick 只寫 1 byte，卻要做
`pack_bus_emit()` 的分支。所以 `i80_encode()` 對 `lanes==1 && bus==8` 保留
`nc_px_codec_wave_frame()` 的 LUT memcpy → **139–165 µs**（比舊 357 µs 快 ~2.2×）。
**教訓**：快路徑唔係全域越快越好，要按 lane 數分流；呢個係上板才看得到的回歸。

**A8 改名（0 性能影響）** — `pixel_wave_task` → `pixel_encode_task`，因為「wave」
只涵蓋 WS2812 波形，實際要出 I80／SPI／RMT／PARLIO 多種格式
（[pixel-output-backends.md](../03_notes/pixel-output-backends.md)）。

### 1.3 為什麼係 3.77×（三份貢獻）

| 貢獻 | 機制 | 證據 |
|---|---|---|
| 中間緩衝消失（最大） | 每幀少 86.4 KB 寫 ＋ 86.4 KB 讀（＝172.8 KB 流量）＋ 冇 gather | A4 單獨改算術只有 1.22× → 證明省掉 gather 才是主因 |
| 廣播（第二） | 8-bit／無抖動時 16 條 lane 同一個 byte，舊路徑做 16 次白工 | production 預設正是 8-bit／無抖動；A6 同一次 A/B 直接受益 |
| 轉置算術（第三） | nibble spread（每 8 byte 16 次查表＋8 次 store）→ 3 步 SWAR | A4 量到算術本身只值 22% |

> 未逐項拆帳（要拆可以再開一個 bench variant）。任何「幾成係邊一項」的說法，
> 如果冇量就不要寫。

### 1.4 現況與界線

- 16×600：encode **6.36 ms** < WS2812 線路時間 **18 ms** ⇒ **encode 已經唔係瓶頸**。
- **16-lane × 16-bit 實機仍然未驗證**（冇 16 條線的板）：bench 係同一份 C 在板上跑，
  但未經真實 16 條線輸出；`0xF077` A/B 的 16-lane 數字要這樣理解。
- 剩下的限制搬到記憶體（DMA 槽）與後端實作（RMT／PARLIO 未做），唔再係編碼 CPU。

---

## 2. 主線 B：配對（layout）— runs → 6 B 單表

### 2.1 一覽

| # | 日期 | commit／§ | 改動 | 9,600 px（單幀 apply） | 倍數 |
|---|---|---|---:|---:|---:|
| B0 | 11-24 | — | runs（每段要 11 次 `% nv`） | 3,745 µs（500 組）／11,950 µs（9,598 段） | — |
| B1 | 11-24 13:43 | `540ef90` | 6 B 單表（dst2／src2／write1／aprog1） | 2,954 µs | 1.27× |
| B2 | 11-24 13:58 | `c5c2ddb` | 表放 **internal**（唔放 PSRAM） | 2,954 vs 5,155（PSRAM）；1000 組 2 段 2,185 vs 4,645 | 0.60× → **2.13×** |
| B3 | 11-24 14:29 | `4e8b468` | 極端：9,598 條單顆段 | **3,007 vs 11,950** | **3.97×** |
| B4 | 11-24 14:49 | `a3fe75c` | 極端：每粒燈一個 group（9,600 job） | **3,009 vs 19,025** | **6.32×** |
| B5 | 11-24 15:01 | `4ec8a27` | 通道級隨機（每通道一條 entry） | 1,589 vs 12,144 | 7.64× |
| B6 | 11-25 00:36 | `c9d595f`／§7.228 | **定案**：flat 一律 PSRAM | 3.07（internal）／5.37（PSRAM）ms | — |
| B7 | 11-25 00:57 | `d965934` | playlist 預建下一張＋swap | **swap 246 µs**（原本 rebuild 40–150 ms） | ~160–600× |

### 2.2 逐條重點

**B1／B2 擺位決定一切** — 同一份程式，表放 PSRAM 係 **0.60×（慢過 runs）**，
放 internal 係 **2.13×**。原因唔係算術，係頻寬：6 B × 9,500 ＝ 57 KB，每幀都要
由 PSRAM 讀一次；runs 表只有 8–32 KB，32 KB L1 裝得落，跨幀都熱。
→ 所以 `nc_px_layout_flatify()` 預設 internal；唔夠就唔建表、行 runs，
**唔會靜默降級**（鐵律 9）。

**B3／B4／B5 分佈越散，贏得越多** — 成本由「佈局形狀」變成「px 數」：
9,598 條單顆段同 500 條連續段都係 ~3.0 ms。呢個係配對層的上限
（≈0.31 µs/px），同 mode／map 寫法無關。

**B6 定案：一律 PSRAM** — 理由係 6 B/px 會隨 px 長大，internal 唔係穩定容量。
代價係每幀 +2.3 ms（3.07 → 5.37 ms），仍然 <20 ms 預算。current＋next 兩張
＝115 KB（PSRAM 的 6.8%）。

**B7 換場** — 播放時按 `registry.list` 次序預建下一張（19.7／12.2 ms），
切換只 swap 指針：**246 µs**。

### 2.3 負結果（同樣係資產）

| 實驗 | 量到 | 結論 |
|---|---|---|
| 4 B 表（dst16＋src16） | bench 777 vs 1,602 µs（2.06×）／923 vs 5,027（5.45×） | 值得做，但 **production 接線後板 USB 消失**（冇 CDC＝冇 OTA），已回退；只留 bench variant 24（§7.233） |
| flat chunk 預取（core0 memcpy） | bench 1,587 vs 5,027 µs；production 4.99 vs 5.37 ms | **lab 有效、production 冇增益**；Kconfig 預設關（§7.231） |
| RGB 專用內圈（去 switch） | 5,005 vs 4,991–5,388 µs | 無增益（§7.232） |
| pipe core0／core1 大 chunk | 5,422 µs | 無增益（§7.232） |
| 值流 pool 放 internal | 5,388 µs | 無增益（§7.232） |
| 4 個 byte store 改一次 32-bit store | 2,954 → 4,393 µs（慢 1.5×） | byte store 會被 store buffer 併入同一 cache line（§7.217） |

---

## 3. 主線 C：環／零拷貝（形狀與一個真 bug）

| 日期 | 改動 | 為什麼 |
|---|---|---|
| — | 舊路徑：效果先寫 canvas → 再複製到環；每幀 `memset` 整格 | 兩趟記憶體＋清畫布 |
| 11-19 | `233346e`：I80 槽改用 `merge8_store`，省掉每幀 86.8 KB 的 memset（註解估 2–3.6 ms PSRAM 寫入） | 第一組 store 已經寫死整格，唔使先清零 |
| 11-24 02:29 | `e13d673`：配對直接 `begin()` → `layout_apply` → `commit()` 寫環格，**刪 canvas、刪每幀清畫布**；開機只寫黑一次 | 零拷貝、唔清畫布、唔補黑（鐵律 4） |
| 11-24 | `197e1e0`：**真 bug** 16-bit 高位 byte 用 OR → lane 由亮變暗會殘留（半邊「卡住」）；改成 store | 低位（off=0）與高位（off=1）係不同 byte，store 唔會互相覆蓋 |

環契約（`begin_ex()` 拿到的格才寫、`commit()` 只發布同一格、encode 不可再 peek、
DMA 完成才 reclaim）見 [pixel-ring-flow.md](../03_notes/pixel-ring-flow.md)。
環前綴 64 B（24 B 表頭＋padding）、3 個 DMA 槽、完成回呼帶單調 order 避免認錯格。

---

## 4. 主線 D：FPS 穩定度（時間銀行）

| 日期 | 改動 | 結果 |
|---|---|---|
| 11-25 | `0xF081` 逐幀間隔統計＋`fps_trace.py` | 睇得到 sd／min／max／bad |
| 11-25 | `nc_fps_clock`：早完成存 slack（上限 50 ms，初始全滿）、遲到由 slack 吸收 | `catchup`／`overrun` 可數 |
| 11-25 | **修正**：節拍仍然係 `esp_timer_start_periodic()`，slack **只做緩衝／判定**，唔追趕、唔改週期 | 用家明確要求 |
| 11-25 | 政策：長期 `overrun` → 改效果或降 FPS，**唔好加大 cap** | 防止用 buffer 遮問題 |

**呢條主線冇 throughput 提升**，目標係穩定度與可觀測性；數字見
[fps-report-2026-11-25.md](../00_pm/fps-report-2026-11-25.md)。
（注意：呢一節的改動當時板 USB 未返，部分驗證未上板。）

---

## 5. 跨主線的七條教訓

1. **先量，唔好估**：SWAR 估算 5–8×，上板 1.22×。估算只可以用來排優先次序，
   唔可以用來落結論。
2. **改資料流 > 改算術**：1.22× 之後真正贏的是「拿走中間緩衝／gather」。
3. **擺位決定一切**：同一份程式，PSRAM 0.60×、internal 2.13×。
4. **快路徑要有例外**：廣播對 16 lane 好，對 1 lane 反而慢 2.4× → 按 lane 數分流。
5. **正確性閘門先過**：新舊路徑逐 byte `memcmp`（`bus_ok=1`）；16-bit 高位殘留
   那個 bug 要在 A/B **之前**修，否則舊 path 的殘留會被當成新 path 的錯。
6. **負結果要寫低**：4 B 表、chunk 預取、RGB 專用、pipe、word store —— 六個做過
   但冇用（或者暫時冇用）的方向，全部有數字。唔寫低，下一次會再試一次。
7. **production 接線要 lab 先**：4 B 表直接上 9600 production，板 USB 消失、
   冇 CDC＝冇 OTA，要斷電重插甚至 BOOT 入 download mode（§7.233）。

---

## 6. 未完成

| 項目 | 狀態 | 卡在哪 |
|---|---|---|
| 16-lane × 16-bit 實機 encode A/B（Step 0 基線） | 未做 | 冇 16 條線的板 |
| 16×600 @ 50 fps 全鏈 | 未驗證 | 同上；但 encode 6.36 ms < 線路 18 ms |
| RMT（C 系列）／PARLIO（P4）後端 | 未實作 | [pixel-output-backends.md](../03_notes/pixel-output-backends.md) |
| 4 B 表入 production | 已回退 | 要 EffectPerf 完整 boot 驗證先 |
| flat chunk 預取入 production | 預設關 | production 未見增益（§7.231） |
| 9,600 px × 4 子幀 | 唔做 | 每視覺幀 ≈20 ms，4 子幀只配 2,400 px |

---

## 7. 想重跑邊個數字（指令對照）

| 想量什麼 | 用什麼 | 備註 |
|---|---|---|
| encode 新舊路徑 A/B（16 lane） | `0xF077` variant 17／18（`modules/lab/i80_pack_lab/run_ab.py --px 9600 --variant 17 --reps 8`） | EffectPerf 映像；test-only |
| 現場 encode 用時（production） | `0xF061` 的 `enc_us` | Matrix／Textkit 類 |
| pack／merge 本體 | `0xF062` PXBENCH（`CONFIG_NETCORE_PXBENCH=y`） | release＋bench |
| 配對 apply（真機 map） | `0xF077`＋`tools/layout_bench.py`／`0xF079` | EffectPerf |
| FPS 穩定度 | `0xF081` → `0xF082`＋`fps_trace.py`／`fps_drift.py` | 見 fps-stability 指南 |

---

## 8. 維護規則

1. **新的一條 append 在尾**，編號接落去（A9、B8…），唔好插入中間改歷史。
2. 每條要有：**日期、commit、改了什麼、量到的數字、量測點（bench／production／host）、
   結論（有效／失敗／未驗證）**。冇數字的條目，標「未量」。
3. **失敗的實驗一樣要寫**：寫「假設 → 實測 → 為什麼錯」，唔好只留成功。
4. 數量級不同的量測點唔可以互相對比，表內要標清楚（見 §1.1 的警告）。
5. 呢份係**歷史**；最新數字覆蓋去 [status.md](../00_pm/status.md)，
   設計理由去 [pixel-renderer.md](../03_notes/pixel-renderer.md)，唔好三份講同一件事講到唔同版本。
6. **唔屬 pixel 的主線**（NC4／通道／緩衝／效果層／USB-FAT）寫去
   [tech-evolution.md](tech-evolution.md)（全樹版）；呢份只收 pixel 管線，
   兩份唔好重複貼同一批數字。

---

## 9. Commit 索引（時間序）

| 日期 | commit | 一句話 |
|---|---|---|
| 11-18 | — | 逐 unit 版本：143 ms |
| 11-19 早 | — | LUT＋direct-OR：38.5 ms |
| 11-19 08:17 | `467af43` | packed＋spread＋PSRAM DMA 槽：14.33 ms |
| 11-19 17:54 | `233346e` | Matrix 板 2 MB PSRAM；I80 改 `nc_i80_out.c`；省 memset |
| 11-23 00:14 | `eb03a0d` | 三層配對（效果→抖動→配對）＋組合快取 |
| 11-24 | §7.201 | SWAR 單獨試：1.22×（失敗） |
| 11-24 13:43 | `540ef90` | 6 B 單表 |
| 11-24 13:58 | `c5c2ddb` | 6 B 表放 internal：2.13× |
| 11-24 14:29 | `4e8b468` | 完全隨機 9,598 段：3.97× |
| 11-24 14:49 | `a3fe75c` | 每粒一 group：6.32× |
| 11-24 15:01 | `4ec8a27` | 通道級隨機：7.64× |
| 11-24 02:29 | `e13d673` | 零拷貝寫環、移除 canvas／每幀清畫布 |
| 11-24 20:54 | `197e1e0` | `pack_bus` 原型＋host 360 checks |
| 11-24 21:44 | `dd64ef8` | production 接線：24.0 → 6.36 ms（3.77×） |
| 11-25 00:36 | `c9d595f` | flat 一律 PSRAM |
| 11-25 00:57 | `d965934` | playlist swap 246 µs |
| 11-25 01:05 | `8837a59` | 去固定 32：40 個效果 |
| 11-25 01:12 | `49a788c` | chunk 預取（lab 有效、prod 無） |
| 11-25 01:21 | `5f324bc` | 三項 apply 實驗全部無增益 |
| 11-25 01:27 | `325511e` | 4 B 表 bench＋USB 消失事故 |
| 11-25 21:49 | `e578be6` | wave → encode 改名 |
