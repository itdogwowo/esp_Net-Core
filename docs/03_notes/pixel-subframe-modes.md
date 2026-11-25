# Pixel 子幀模式（8-bit ↔ 10-bit 抖動）：點開、點推導、點檢查

> 2026-11-26。狀態：**設計稿（未實作）**。
> 相關：[`pixel-ring-flow.md`](pixel-ring-flow.md)（環與交接）、
> [`../04_evolution/pixel-evolution.md`](../04_evolution/pixel-evolution.md)（歷史）。
> 數字以 `status.md` 為準；這頁只講「模式怎樣開、哪些值要一致、怎樣避免魔法數字」。

## 0. TL;DR

- 開 4 子幀（12→10 bit 時間抖動）而家要**動 4 個地方，3 個要重 build**，而且唔一致
  唔會響（只會靜靜掉幀 —— 2026-11-26 就係咁掉咗 3/4）。
- 真正獨立的輸入只有 **兩個**：`visual_fps`（視覺幀率）同 `dither_bits`（抖動位深）。
  其餘（子幀週期、環格數、effect 生產率、每 lane px 上限）**全部可以由這兩個人手值推導**。
- 建議：`system.json` 加一個 `pixel` 節，開機推導＋**開機驗算**，唔一致就大聲報錯
  （唔可以靜默降級 —— 鐵律 9）。Kconfig 只留上限／缺省，舊 project 照舊跑得。

## 1. 現況：要開 4 子幀，總共要動 4 個地方

以 `ports/project/S3/ESP32-S3-Zero-Show2400/` 為例：

| 地方 | 值 | 性質 | 由邊份 code 讀 |
|---|---|---|---|
| `CONFIG_NETCORE_PIXEL_SUBFRAMES` | `4` | **build** | paint 的 `#if`（`nc_px_paint_task.c:264`）、環政策（`nc_pixel_core.h` `nc_px_ring_ordered()`） |
| `CONFIG_NETCORE_PIXEL_RING_SLOTS` | `5` | **build** | `nc_render_task.c:271` → `nc_px_ring_init()` |
| `CONFIG_NETCORE_PIXEL_FPS` | `200` | **build** | effect 的生產率（`nc_px_effect_task.c:319`）；render 只在 system.json 冇值時做後備 |
| `system.json` `frame_interval_ms` | `5` | runtime | render 的 `esp_timer` 週期（`nc_render_task.c:955`） |
| `system.json` `num_pixels` | `2400` | runtime | 每條 lane 幾多 px（plan） |
| map／layout | 150 px/lane | 資料 | 物理上限：線時間 ≤ 子幀週期 |

三個「魔法數字」互相要一致：

- `SUBFRAMES` 決定一個視覺幀要 commit 幾多格；
- `RING_SLOTS` 要 **≥ SUBFRAMES + 1**，否則 paint 交唔齊（`begin()` 回 NULL）；
- `frame_interval_ms` 要係 **子幀**週期（不是視覺幀週期）；
- 每 lane px 要夠少，令線時間塞得入子幀週期。

**任何一個錯：畫面照跑，但掉幀／掉子幀，冇警告。** 2026-11-26 的 bug（peek 只送最新格）
就係呢個結構的產物 —— 它不是單一 bug，係「同一個事實散落三處」的症狀。

### 1.1 順帶：`RING_SLOTS` 的 Kconfig range 有陷阱

`Kconfig.projbuild` 寫 `range 3 16`，但 `NC_PX_MAX_SLOTS = 6`（`nc_pixel_core.h`），
`nc_px_ring_init()` 見到 `slots > 6` 會**回 NULL**（大聲失敗，唔會靜默）。
所以實際上限係 6：4 子幀 → 5 格 OK；8 子幀 → 9 格 **做唔到**。
（8 子幀要麼加 `NC_PX_MAX_SLOTS`，要麼唔支援 —— 要拍板。）

## 2. 心智模型：兩個輸入，其餘推導

```
visual_fps = 50          ← 人：我要幾順（視覺幀率）
dither_bits = 10         ← 人：我要幾多灰階（8 → ns=1；10 → ns=4）

ns          = dither_bits > 8 ? 4 : 1           （子幀數）
sub_hz      = visual_fps × ns = 200             （子幀率）
interval_ms = 1000 / sub_hz = 5                 （一個 tick ＝ 一個子幀）
ring_slots  = ns + 1 = 5                        （4 格在飛 + 1 格緩衝）
effect_fps  = visual_fps = 50                   （effect 每視覺幀只算一次）
px/lane     = 受下面物理上限約束
```

### 2.1 物理上限（唯一真正需要「量」的地方）

```
線時間 (µs) = px_per_lane × bits_per_px × bit_cells × 1e6 / pclk_hz
              WS2812：bits_per_px = 24；RGBW（WS2812W）：32
              bit_cells 由 wave cfg（每個 LED bit 用幾個 bus tick）

要滿足：線時間 ≤ interval_ms × 1000 − 餘裕（建議餘裕 ≥ 10%，留 DMA 完成回呼／排程抖動）
```

實例（16×600 → 16×150 @ 200 Hz）：

| px/lane | bits | bit_cells | pclk | 線時間 | 5 ms 週期 |
|---:|---:|---:|---:|---:|---|
| 600 | 24 | 3 | 2.4 MHz | 18.0 ms | ✘ 360% |
| 150 | 24 | 3 | 2.4 MHz | **4.5 ms** | ✔ 90%（餘裕 10%） |

所以「600 px」同「4 子幀」二選一 —— 呢個就係為什麼要 150 px。**呢條式唔應該靠人記**。

## 3. 一個語義衝突：`CONFIG_NETCORE_PIXEL_FPS` 有兩個意思

| 讀者 | 意思 | 位置 |
|---|---|---|
| effect 任務 | 生產率（每秒算幾多幀值流） | `nc_px_effect_task.c:319` |
| render 任務 | 後備節拍（system.json 冇 `frame_interval_ms` 時才用） | `nc_render_task.c` |

Show2400 設 `FPS=200`（想表達「子幀率 200 Hz」），但 effect 讀到就變成**每秒算 200 幀**，
而 pipeline 只送得 50 個視覺幀／秒 → **effect 白做 4 倍**（多數幀未經 paint 就被下一個覆蓋，
仲要 `vTaskDelay(1)` busy 等 ack）。

結論：`FPS` 應該拆成兩個名（`effect_fps` / `subframe_hz`），而且兩個都由 `pixel` 節推導。

## 4. 建議設計（半自動）

### 4.1 唯一輸入：`system.json` 的 `pixel` 節

```json
"pixel": {
  "visual_fps": 50,        // 視覺幀率（人決定）
  "dither_bits": 10        // 8 = 直出；10 = 4 子幀時間抖動
}
```

可選覆寫（進階／量測用，唔建議日常改）：

```json
"pixel": {
  "subframes": 4,          // 0/缺 = 由 dither_bits 推
  "frame_interval_ms": 5,  // 0/缺 = 由 visual_fps×subframes 推
  "ring_slots": 5          // 0/缺 = subframes+1
}
```

### 4.2 開機流程（B02 讀 config 之後、B06 declare 之前）

1. 讀 `pixel` 節；缺就 fallback 到今日的 Kconfig 值（舊 project 行為不變）。
2. 推導 `ns / interval_ms / ring_slots / effect_fps`。
3. **驗算**（三條，全部要大聲）：
   - `ring_slots ≥ ns + 1`（唔夠 → `ESP_LOGE`，唔建環，lane 停用）
   - `ring_slots ≤ NC_PX_MAX_SLOTS`（6）
   - `線時間 ≤ interval × 1000 × 0.9`（超 → `ESP_LOGE` 寫明「線時間 6.2 ms vs 週期 5 ms、超 24%」）
4. 結果寫入 runtime 結構（`s_px`），paint／ring／effect 全部讀它，唔再讀 Kconfig。
5. `status pixel`（或 `0xF061` 的 `mode` 段）列出：`ns / sub_hz / interval_ms / ring_slots /
   px_per_lane / line_ms / 餘裕%`。

### 4.3 為什麼唔係「全自動」

- 「我想要 10-bit 定 8-bit」係**產品決定**，機器唔知。
- 「寧願掉幀定寧願降 fps」係**取捨決定**，機器唔應該靜靜幫你揀（鐵律 9）。
- 所以：**intent 人手、算式機器、越界大聲**。呢個就係「半自動」的意思。

## 5. 要改的程式（實作清單，未做）

| 檔案 | 改咩 | 風險 |
|---|---|---|
| `firmware/main/Kconfig.projbuild` | 三個 Kconfig 加註「只係缺省／上限」；`RING_SLOTS` range 改 `3 6` | 低 |
| `components/sw/pixel/include/nc_pixel_core.h` | `nc_px_ring_ordered()` 由 `#if` 改食 **runtime flag**（`nc_pixel_q_t.ordered` 或 init cfg 參數） | 中（改 ABI struct 大小，`NC_PX_RING_STORAGE_BYTES` 要對） |
| `components/sw/pixel/nc_pixel_q.c` | `nc_px_ring_init()` 收 `ordered`；`take/untake` 由 flag 決定 | 低 |
| `components/tasks/pixel_paint_task/nc_px_paint_task.c` | `ns` 由 config 傳入（唔再 `#if`）；保留 `ns==1` 快路徑 | 中（熱路徑，要量） |
| `components/tasks/render_task/nc_render_task.c` | `pl->slots` 用推導值；`period_us` 用推導值；開機驗算＋log | 中 |
| `components/tasks/pixel_effect_task/nc_px_effect_task.c` | fps 用 `effect_fps`（唔再用 `CONFIG_…_FPS`） | 低 |
| `ports/project/**/data/config/system.json` | 加 `pixel` 節（先做 Show2400 一個 project 試） | 低 |

**向後兼容**：冇 `pixel` 節 → 完全照今日行為（Kconfig 三值 + system.json `frame_interval_ms`）。
所以可以一個 project 一個 project 遷移。

## 6. 驗證計劃

1. host test：`pixel_test` 加「`ordered` flag 由 init 傳入」的兩個 case（fifo／mailbox）。
2. 建置：Show2400（`dither_bits=10`）＋ Matrix-Show（`dither_bits=8`）兩邊都要過 ——
   確保 runtime 化之後兩條政策都編得入。
3. 上板（要板）：
   - `status pixel` 四個推導值對得上；
   - `0xF061`：`skipped` = **0**、`frames` ≈ `sub_hz × 秒`；
   - 故意把 `visual_fps` 調到 60（interval 4.17 ms < 線時間 4.5 ms）→ 要**大聲報錯**，
     唔可以照跑。
4. 量測：4 子幀 200 Hz 下 paint 的 `busy++`（背壓）出現頻率；effect CPU 應由
   「200 fps 白做」降到 50 fps。

## 7. 要拍板的問題

1. `dither_bits` 對映：8 → ns=1、10 → ns=4。要唔要支援 ns=2／8？（`NC_PX_MAX_SLOTS=6` 限制）
2. 越界行為：`visual_fps` 太高（線時間超週期）→ **拒絕開機**（lane 停用）定**自動降 fps**？
   建議前者（明示失敗），但你要決定。
3. `pixel` 節放 `system.json` 定另外開 `pixel.json`？（現在 pixel 相關散在 `system.json`
   的 `frame_interval_ms` 同 project 的 `sdkconfig.defaults`）
4. `ring_slots` 預設 `ns+1` 定 `ns+2`？（+1 最省記憶，+2 抗抖動好啲；9,600 px × 1 格 = 38 KB）

## 8. 名詞對照（避免再混淆）

| 名 | 意思 |
|---|---|
| 視覺幀率 `visual_fps` | 你眼見的畫面更新率（50 Hz） |
| 子幀率 `sub_hz` | 實際送線的頻率＝`visual_fps × ns`（200 Hz） |
| `interval_ms` | **一個 tick ＝ 一個子幀**（5 ms），不是視覺幀 |
| `ns`（subframes） | 一個視覺幀拆幾個子幀（4） |
| `ring_slots` | 環有幾格（5 ＝ 4 在飛 + 1 緩衝） |
| 線時間 | 一條 lane 真的把 `px` 顆燈送完要幾久（150 px → 4.5 ms） |
