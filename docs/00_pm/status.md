
### 7.197 2026-11-24（送燈要分兩部份：CPU 提交 vs WS2812 線路等待）

用家指正：「物理送燈 18 ms」其實係兩件完全唔同嘅事，要分開講。

| 部份 | 時間 | 係咩 | 佔 CPU？ |
|---|---:|---|---|
| **CPU 提交** | **1.16 ms** | 把 86.8 KB packed 交去 esp_lcd（msync） | ✅ 佔 |
| **WS2812 線路等待** | **18 ms** | 600 顆 × 24 bit ÷ 800 kHz（協議硬性） | ❌ 唔佔（CPU 可做效果／配對） |

- 16×600（1 幀）：線路 18 ms；16×150（4 子幀）：每子幀 4.5 ms × 4 ＝ **同樣 18 ms**。
- 50 fps 的硬條件是 **線路 18 ms ≤ 20 ms**（剩 2 ms 餘量），唔是 CPU。
- CPU 每格只需 1.16 ms 提交；其餘時間可以做效果（5.03 ms）、配對（4–12 ms）等。
- 圖已改成三把尺：A 每格 CPU、B 每格線路等待、C 一次性。
  `docs/assets/pixel-pairing/extreme-bars.png`、`extreme-cards.png`。

### 7.198 2026-11-24（外部靈感：I80 波形合成、隨機配對）

**現況確認**：清畫布仍在程式碼（`nc_pair_task.c:225 memset(s_content.canvas…)`），
零拷貝亦未做；兩者都是計劃 Task 1。I80 波形合成 19.44 ms、配對完全隨機 12.29 ms
是兩個最值得投資的熱點。

#### A. I80 波形合成（19.44 ms）——參考 FastLED 的 LCD/I80 時鐘less 驅動

來源：
- [FastLED `channel_driver_lcd_clockless.h`](https://github.com/FastLED/FastLED/blob/2bd00c3299ed2bc7df0b65dd93c255609e8da250/src/platforms/esp/32/drivers/lcd_spi/channel_driver_lcd_clockless.h)
- [FastLED `wave3.h`](https://github.com/FastLED/FastLED/blob/master/src/fl/channels/wave3.h)
- [FastLED lcd_cam implementation notes](https://github.com/fastled/fastled/blob/6e37141d0f70d56e7ab73bb45ad51e859e8d4256/src/platforms/esp/32/drivers/lcd_cam/implementation_notes.md)

1. **Chunked streaming DMA（最大啟發）**：FastLED 用 3 個 DMA 槽的環，每個 chunk 約
   90 個輸入 byte（30 顆 LED／lane），ISR 在 chunk 完成時「轉置下一個 chunk（約 1–2 µs）
   並即刻提交」。即係**打包同送燈重疊**：總時間 ≈ max(送燈, 打包)，而唔係相加。
   我們現在是「整幀打包 19.4 ms → 一次提交 18 ms」＝ 37.4 ms 序列。
   若改成 chunked：16×600 有機會由 33 fps 升到 ~50 fps。
2. **wave3 nibble LUT**：16 個 u16（32 B）把 4 bit 展開成 12 個 tick；再對 2／4／8／16
   lane 做轉置。我們的 merge（8 lane）可以照這個做法，改成 16 lane 一次轉成 u16 字。
3. **bits_per_pixel = 16、1 bit／lane／PCLK**：整個 frame buffer 就是「每個 tick 一個
   16-bit 字」，同我們 packed 的概念一樣，但 FastLED 是**逐 chunk 產生**，不是整幀。

#### B. 隨機配對（12.29 ms）——9,598 段、每段 1 顆

觀察：runs 已按 dst 排序，所以寫入是**順序**的；成本在「每段一次」的固定開銷
（9,598 段 × ~1.2 µs）。

1. **run 表 16 B → 8 B**（dst／src u16、count u16、job u16）：表 153.6 KB → 76.8 KB，
   PSRAM 讀取即減半。
2. **每段預先算好通道起點**（c0／c1／c2）存入 run：省 3 次 `%` 與 2 次間接（job→aprog）。
3. **count == 1 特化**：完全隨機時全部是 1 顆段；用專用迴圈直接寫一個 u32，省掉內層
   for 與邊界檢查。
4. （可選）**chunk 重疊**：配對寫入下一個環槽時，DMA 正在送上一槽（環已支援 3 槽）。

預計：隨機配對 12.29 → **6–8 ms**；合併 zero-copy（省 4.5 ms 複製、2.0 ms 清畫布）後
9,600 px 的配對層可回到「幾 ms」級。

### 7.199 2026-11-24（更正：I80 已經是 async，打包與送燈本來就重疊）

用家指正：DMA 一早已經開，為何會是 37.4 ms 序列？查證後確認**用家正確**：

- `nc_i80_drv_submit()` 用 `esp_lcd_panel_io_tx_color()`：**排進佇列即返回**（`tx_us` 1.16 ms
  只是 cache writeback，不是等 18 ms）。
- 完成由 ISR 回呼 `on_color_done()` 標記（每個槽有單調 order，避免回收後認錯格）。
- 3 個 DMA 槽 + `reclaim()`：上一幀傳輸中，下一幀可以同時 encode。
- 所以 throughput ≈ **max(encode, 線路)**，唔係相加。

**修正後的預期（16×600、現行程式碼）**：

| 項目 | 舊量測（§7.123，async 之前） | 現行預期 |
|---|---:|---:|
| encode | 19.4 ms | 19.4 ms |
| 線路 | 18 ms | 18 ms |
| frame | 37.9–39.6 ms（33 fps） | **≈ max(19.4, 18) ≈ 19.4–21 ms（48–50 fps）** |

⚠️ 這個預期**未在 16-lane 板重測**（Zero 只有 1 條 data pin，起唔到 16 lane）。
下次上 SuperMini/16-lane 板要用現行固件重測 `frame_us`／`enc_us`／`tx_us`。

**對計劃的影響**：Task 6（chunked streaming）由「必需」降為「可選」——
async 已經重疊，chunked 只在「要再削 encode（19.4 ms）或降延遲」時才做。
真正要盯的是：encode 19.4 ms 距離 20 ms 預算只剩 0.6 ms，而且同 core 的 pair 要分開核。

### 7.200 2026-11-24（原型實測：隨機配對 12.28 → 6.22 ms；波形合成可加速 5–8 倍）

**A. 隨機配對原型（0xF077 variant 9）**

在 bench 先建立「平面 8 B run 表」（每顆：dst、c0、c1、c2 都已預先算好），
再跑「讀 8 B、寫 4 B」的最小迴圈：

| 做法 | 時間（9,600 px、完全隨機 9,598 段） |
|---|---:|
| 現在 `nc_px_layout_apply`（16 B run、job→aprog、3 次 %） | **12.28 ms** |
| **平面 8 B＋預算通道起點** | **6.22 ms（−49%）** |
| 平面表建立（一次性，跟組合建立／快取一起做） | 14.8 ms |

→ 證實計劃 Task 7 的方向；混合做法（count>1 保留 16 B、count==1 用平面表）亦可。

**B. 波形合成：FastLED 唔係上限**

| 做法 | 狀態 | 時間／估算 |
|---|---|---:|
| 現在：逐 lane `wave_pack` → `merge8` 逐 byte 移位／OR | 實測 | **19.44 ms**（86.4 KB → 4.5 MB/s，CPU-bound） |
| 8×8 SWAR 轉置（64-bit 三步交換） | 估算 | **3–6 ms**（5–8 倍） |
| FastLED wave3 nibble LUT（16×u16）＋ 16-lane 轉置 | 參考 | 同類做法，LUT 32 B 常駐 cache |
| chunked DMA（3 槽、每 chunk ~30 顆） | 參考 | async 已存在；只降單次 encode 延遲 |
| RMT | **不可行** | ESP32-S3 RMT 冇 DMA；16×600 一定要 LCD_CAM（I80） |

- 參考：[FastLED lcd clockless](https://github.com/FastLED/FastLED/blob/2bd00c3299ed2bc7df0b65dd93c255609e8da250/src/platforms/esp/32/drivers/lcd_spi/channel_driver_lcd_clockless.h)、
  [wave3.h](https://github.com/FastLED/FastLED/blob/master/src/fl/channels/wave3.h)、
  [ESP RMT FAQ（S3 冇 DMA）](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/rmt.html)。
- 圖：`docs/assets/pixel-pairing/approach-now-vs-plan.png`（現在 vs 打算）。

**下一步**：SWAR 轉置要先寫一個獨立原型（bench）對照 `merge8`，量到真實倍數才改 `nc_i80_out.c`。

### 7.201 2026-11-24（USB 實測推翻估算：SWAR 轉置只有 1.22×，不是 5–8×）

用家要求直接用 USB（OTA＋nc4）驗證，唔好講估算。做法：0xF077 variant 10，
在板上建 16 條 lane 的 packed 資料（每條 600 px → 5,400 B），比較：

| 做法 | 實測（3 次，min） | 正確性 |
|---|---:|---|
| 現況 `merge8_store` ＋ `merge8`（兩組 8 lane） | **22.52 ms** | — |
| 8×8 SWAR 轉置（64-bit 三步交換） | **18.51 ms** | **match=1（輸出逐 byte 相同）** |

即 **1.22×**，不是我之前估的 5–8×。原始回覆（USB nc4）：
`merge8=22515us swar=18516us match=1`。

**為什麼估算錯**：瓶頸不是轉置算術，是 **gather**——每個 byte index 要由 16 個
不同 lane 緩衝各讀 1 byte（PSRAM／cache），輸出 86.4 KB；算術只佔少數 cycle。
SWAR 每輪仍要做 16 次 load ＋ 組 u64 的移位，所以只得 22% 改善。

**下一步（改寫計劃 Task 6）**：真正有機會的是「**展開＋轉置一次過**」——
FastLED 的 wave3 正是每 16 個輸入 byte 直接產生 24 個 u16（48 bytes），
唔需要先 `wave_pack` 成 lane-major 再做第二次 merge。要先用原型量到真實倍數，
唔再靠估算。

### 7.202 2026-11-24（隨機配對三步實測：12.29 → 6.23 → 4.53 ms）

用家要求逐步解釋＋繼續測試。**「B」＝ byte（位元組）**：16 B ＝ 16 個位元組，唔係 16 bit。

| 做法 | 每顆資料 | 每顆要做什麼 | 實測 |
|---|---|---|---:|
| 現在 | 每「段」16 B：dst 4B／src 4B／count 4B／job 4B | 查 job → 查 aprog（兩次間接）、每顆 3 次 `%` | **12.29 ms** |
| 平面 8 B | dst 2B／c0 2B／c1 2B／c2 2B | 讀 8 B、寫 4 B；冇間接、冇 `%` | **6.23 ms** |
| **平面 4 B** | dst 2B／src 2B | 讀 4 B、3 次 `%`、寫 4 B | **4.53 ms（−63%）** |
| 平面 8 B 只讀表（唔寫入） | — | 量「讀表＋算值」成本 | 3.95 ms |

- 6.23 ms 的分解：**讀表＋算值 3.95 ms、寫入約 2.3 ms**。
- 4 B 版最快：表 76.8 → 38.4 KB，PSRAM 讀取減半；3 次 `%` 比讀 4 B 便宜。
- 建表成本 14.8 ms（一次性，跟組合建立／快取一起做）。
- 圖：`docs/assets/pixel-pairing/pairing-explained.png`。

### 7.203 2026-11-24（三種表在四種排列下的完整實測）

| 排列 | run 16 B（每段） | 平面 8 B（每顆） | 平面 4 B（每顆） |
|---|---:|---:|---:|
| 1 段（9,600 px） | 2.98 ms | 3.50 ms | **2.28 ms** |
| 500 組（500 段） | 3.73 ms | 3.29 ms | **2.38 ms** |
| 500 組（1,000 段） | 4.10 ms | — | — |
| 完全隨機（9,598 段） | 12.29 ms | 6.23 ms | **4.50 ms** |

**用家的模型檢討**：
1. 「組合內部越多越便宜」——對**表的大小**成立（段數 × 16 B），但對**執行時間**唔完全成立：
   run 版每段有固定開銷（查 job／aprog、3 次 `%`、內層 setup），所以就算 1 段都要 2.98 ms。
2. 「平面 8 B＝每顆都有自己的固定支出」——正確；但佢在「1 段」時反而慢過 run（3.50 vs 2.98），
   因為表由 16 B 變成 76.8 KB。
3. **第三個方案（4 B／顆）**：都係每顆固定支出，但只存 dst＋src，通道起點即時算；
   表 38.4 KB。**四種排列全部最快**（2.28 / 2.38 / 4.50 ms）。

**結論**：4 B 平面表係目前最佳；建表 14.8 ms 一次性、可入快取。

### 7.204 2026-11-24（平面 4 B／顆犧牲了什麼）

4 B 版最快（1 段 2.28、500 組 2.38、隨機 4.50 ms），但係用「通用性」換返嚟：

| 能力 | run 16 B | 平面 4 B |
|---|---|---|
| 每段不同**寫入型別**（rgb／rgbw／單通道／ww） | ✅ 有 `job` 指去 job（含 write） | ❌ 只支援 RGB（或每種型別一張表） |
| 一個 mode **多個效果**（各自值流） | ✅ `job`→`aprog` | ❌ 只有一個 `src`，冇 aprog 編號 |
| 覆蓋範圍／反轉／stride／跨型別 slice | ✅ 建表時展開 | ✅ 一樣（建表時已展開） |
| 效果值 12-bit | ✅ | ✅（表只存索引） |
| 像素上限 | u32（40 億） | **u16（65,535）** |
| 值索引上限 | u32 | **u16（65,535）** |
| 表大小 | 16 B × 段數（1 段 ＝ 16 B） | **4 B × 像素**（9,600 → 38.4 KB） |
| 快取檔 | 12.5 KB → 讀 10 ms | 38.4 KB → 讀約 25–40 ms（推算） |
| 建表 | 40–137 ms | **+14.8 ms**（一次性） |

**取捨**：
1. **寫入型別**：run 版每段可以是 rgb／rgbw／單通道／ww；4 B 版冇 job，只能 RGB。
2. **多效果混合**：run 版可同時引用多個效果（每段一個 aprog）；4 B 版只有一個 src，
   要合併值流（索引可能爆 65,535）或者加 byte。
3. **範圍**：dst／src 都係 2 B → 65,535。今日 9,600 px／38,400 值冇問題；
   將來超過就要 4 B 欄位（變 8 B／顆）。
4. **記憶體與快取**：表按像素增长（38.4 KB），run 表按段（1 段 16 B）。
   快取檔變大、讀取變慢；或者選擇唔快取、每次重建 14.8 ms。

**建議（保留能力）**：run 表仍然是「編譯後的正本」；載入時如果平均段長 < 門檻（例如 < 3 顆），
才展開成 4 B 平面表（+14.8 ms，可選快取）。rgbw／多效果維持 run 路徑。
如果想兩者兼得，可以試 **6 B／顆**（dst 2、src 2、aprog 1、write 1）——待原型。

### 7.205 2026-11-24（三表對比；用家指正：寫入型別重要、多效果要講清楚）

**用家指正**：
1. 「每段不同寫入型別（rgb／rgbw／單通道／ww）」是重要特性，不能丟。
2. 「一個 mode 多個效果」要講清楚：正常效果是**輪流播放**，不應該重疊；
   一個 mode 的 `map[]` 每條 = group ＋ effect ＋ write。

**釐清後的結論**：4 B 平面表可以**保留**這兩樣——只要「**每個 entry 一張表**」，
表頭帶 `write` 同 `aprog`，值流各自。所以：

| 能力 | 16 B run | 8 B／顆 | 4 B／顆 |
|---|---|---|---|
| 每段不同寫入型別 | 得（job→write） | 要分表（每表一型別） | 要分表（每表一型別） |
| 一個 mode 多個效果 | 得（job→aprog） | 要分表（每 effect 一張） | 要分表（每 effect 一張） |
| 覆蓋／反轉／stride／slice | 得 | 得 | 得 |
| 12-bit 值 | 得 | 得 | 得 |
| 像素／值索引上限 | u32 | u16（65,535） | u16（65,535） |
| 表大小（9,600 px） | 16 B × 段數 | 76.8 KB | **38.4 KB** |
| 快取讀取（推算） | 12.5 KB／10 ms | 76.8 KB／50–80 ms | 38.4 KB／25–40 ms |
| 建表 | 40–137 ms | +14.8 ms | +14.8 ms |

**實測速度**：1 段 → 2.98／3.50／**2.28**；500 組 → 3.73／3.29／**2.38**；
完全隨機 → 12.29／6.23／**4.50** ms。

**真正犧牲**（不是寫入型別／多效果）：
1. **範圍**：u16 → 65,535（今日 9,600／38,400 冇問題）。
2. **記憶體**：4 B × 像素（38.4 KB），run 表只 16 B × 段數。
3. **快取**：檔由 12.5 KB 變 38.4 KB，讀取變慢（推算 25–40 ms）。
4. **建表**：+14.8 ms（一次性）。
5. **8 B 版被 4 B 支配**：更大、冇多能力、1 段時更慢。

### 7.206 2026-11-24（重疊行為實測；分表成本實測）

**A. 群組重疊（A=0:4、B=2:5，重疊 px2、px3）**

host 測試（pxLayout_test [9]）：A 全部 0x100（>>4 = 16）、B 全部 0x200（>>4 = 32）：

| 像素 | 結果 |
|---|---|
| px0、px1（只有 A） | 16 |
| **px2、px3（A＋B）** | **32** |
| px4（只有 B） | 32 |

→ 現行行為是**後面的 entry 贏**（不是相加；相加會是 48）。所以用家想要的「最新／最後套用」**已經是現況**，
唔需要改；只需寫入文件（同一通道：後者覆蓋；不同通道：共存）。

**B. 分表成本（兩個 entry 各自一張 4 B 表）**

| 做法 | 實測（9,600 px、完全隨機） |
|---|---:|
| 單表 4 B（1 entry） | **4.50 ms** |
| 兩張表 4 B（2 entry，切半） | **6.23 ms（+1.7 ms）** |

→ 分表**唔係免費**：原型量到 +38%。原因未確定（兩個緊迴圈、同樣總寫入量），
所以「每個 entry 一張表」暫時唔可以當零成本；要再改原型（例如一次掃兩張表、共用一個迴圈）
先可以判斷係實質成本定係原型寫法造成。

**C. 8 B vs 4 B 的能力**：完全一樣（都係每顆一筆、都要分表）；4 B 只係細一半、快啲。
所以 8 B 被 4 B 支配（除非將來想要「免計算」而犧牲記憶體）。

### 7.207 2026-11-24（釐清 c0/c1/c2、寫入型別、多效果在 4 B 表的位置）

**c0/c1/c2 係咩**：現行 `nc_px_layout_apply` 只用一個 `src`，再按 write 型別算通道索引：

| write | 通道值索引 |
|---|---|
| rgb | `3×src`、`+1`、`+2`（每顆 3 個值） |
| rgbw | `4×src`、`+1`、`+2`、`+3`（每顆 4 個值） |

所以「9,600 顆 × 4 通道各自有值」**係效果層的值流**，配對層只需要「呢顆用第幾個 src」。
4 B（dst＋src）**保留這個特性**：stride 由表頭的 write 決定，src 逐顆前進。

**4 B 做不到的**：每顆的四個通道用**任意、唔連續**的值索引（例如 R 用 5、G 用 900）。
但注意：**現行 run 表都做唔到**（run 只有一個 src），所以 4 B 冇失去現有能力；
8 B（c0/c1/c2）可以做到，但我們建表時都係連續的，冇用到。

**寫入型別／多效果去邊**：搬去**表頭**（每個 entry 一張表）：
`{ write, aprog, nv, base }` ＋ 每顆 `{ dst 2B, src 2B }`。
所以 per-pixel 的 `job`／`aprog` 可以移除（用家講的「把記這個位的地方移除」＝對）。

**如果將來要「同一個 entry 內混合寫入型別」**：要加 1 B（5 B／顆），或者把群組拆開。

**未解決**：分表量到 +1.7 ms（4.50 → 6.23），要先查清楚係實質成本定係原型寫法。

### 7.208 2026-11-24（用測試決定：4 B 單表 vs 6 B 單表 vs 分表）

| 排列 | 4 B 單表（1 entry） | 4 B 兩表（2 entry） | 6 B 單表（write+aprog） |
|---|---:|---:|---:|
| 1 段（9,600 px） | **2.28 ms** | — | 2.98 ms |
| 500 組（500 段） | **2.38 ms** | — | 2.86 ms |
| 完全隨機（9,598 段） | **4.50 ms** | **6.23 ms** | **5.34 ms** |

**結論**：
1. **單 entry**（一個效果、一種 write）→ **4 B 表最快**（4.50 ms），write 放表頭。
2. **多 entry**（多效果／多 write）→ **6 B 單表**（5.34 ms）**贏過分表**（6.23 ms），
   而且 write／aprog 直接放每顆，冇分表切換。
3. 所以按 mode 決定：`nentry == 1` 用 4 B；`nentry > 1` 用 6 B。
4. 兩者都快過 run 表（run：2.98／3.73／12.29 ms，隨機最差）。

**記賬**：6 B 比 4 B 慢 0.5–0.85 ms（多讀 2 B／顆），但換來「一張表處理混合 write／多效果」。

### 7.209 2026-11-24（定案：統一用 6 B 表；4 B 只留文件做隱藏優化）

用家決定：**唔做複雜分層，統一用 6 B**（兼容性要緊；只差零點幾 ms）。
4 B 的測試只寫入文件，將來真的需要再查。

**「兩種情況」其實係咁講**（之前講得太複雜）：

| 情況 | 意思 | 之前建議 |
|---|---|---|
| 一個 mode 只有 **一個效果** | mode 的 `map[]` 只有一條 entry | 4 B 表（快 0.5–0.85 ms） |
| 一個 mode 有 **多個效果** | `map[]` 有多條 entry（不同 group／write） | 6 B 表 |

**定案**：無論幾多個效果，**一律用 6 B 單表**：
`{ dst 2B, src 2B, write 1B, aprog 1B }` ＋ 一次掃完。
- 實測（完全隨機）：**5.34 ms**（4 B 是 4.50、分表 6.23、run 12.29）。
- 兼容性：write／aprog 逐顆都有，唔需要分表。
- ⚠️ 實作注意：5.34 ms 係「全部 RGB」的量測；真正有混合 write 時，
  應該**按 write 分組**（entry 排序或預先分桶），避免每顆一個 switch。

**4 B 隱藏方法（文件備查，唔實作）**：單 entry 時 4 B 可省 0.5–0.85 ms；
若將來要追極限，查本節與 §7.208。

### 7.210 2026-11-24（為什麼 entry 上限是 8？來源與現況）

`NC_PX_MODE_ENTRY_MAX 8` 的來源：

- 它驅動 `NC_PX_JOB_MAX` → `nc_px_plan_t.job[8]`，每個 `nc_px_job_t` = **552 B**
  （含已編譯 program），所以 `nc_px_plan_t` = **4,424 B**（8 × 552 ＋ 表頭）。
- 這是**舊的 show 路徑**（`nc_px_show_plan()`）用的固定陣列；現時 grep 顯示
  **production 冇呼叫**，只有 host 測試（pxMap_bench／pxShow_test／pxLayout_test）用。

**production 實際用的 layout 路徑**（`nc_px_layout_build`）冇 8 這個限制：
entry 係動態 pool，真正的上限是：

| 項目 | 上限 | 原因 |
|---|---:|---|
| 一個 mode 的 distinct 效果（aprog） | **32** | `NC_PX_LAYOUT_ASSET_MAX`；每個 aprog 536 B（32 × 536 ≈ 17 KB） |
| 整棵樹的效果（assets） | **32** | `NC_PX_ASSET_MAX` |
| group 數 | 65,535 | `NC_PX_GROUP_MAX`（u16） |
| 像素 | 65,535 | 6 B 表的 dst 是 u16 |
| entry 數（同一效果可重複） | 動態 | 由 pool 容量／記憶體決定 |

**結論**：8 係舊路徑的殘留，唔係 production 限制；要更多效果，先要提
`NC_PX_ASSET_MAX`／`NC_PX_LAYOUT_ASSET_MAX`（aprog 536 B／個，32 個 ≈ 17 KB）。
舊 `nc_px_show_plan()` 如果冇用，可以刪或把上限改成動態。

### 7.211 2026-11-24（assets 與 aprog 是什麼；為什麼有 32／8）

**兩個名詞**：

| 名詞 | 意思 | 大小 | 現在上限 |
|---|---|---|---:|
| **assets** | `effects/effects.json` 裡的效果定義（program、pixel_n、step…）——整棵樹共用 | 262 B／個 | **32**（`item[32]`，`n` 是 uint8） |
| **aprog** | 某個 mode 引用某個效果時，**編譯出來的可執行副本**（536 B）；同一效果在同一 mode 只編一次，多條 entry 共用 | 536 B／個 | **32**（`NC_PX_LAYOUT_ASSET_MAX`） |

- 32 個 assets ≈ **8.4 KB**；32 個 aprog ≈ **17.2 KB**。
- 舊 `NC_PX_JOB_MAX 8`（plan 的 8 × 552 B = 4.4 KB）production 冇用，只有 host 測試。

**用家的原則：唔要魔法數字，用足欄位闊度**。照這個原則改：

| 項目 | 改成 | 新上限 |
|---|---|---|
| assets | 掃描 `effects.json` 數量後 lalloc | `n` uint16 → 65,535；實際由記憶體決定（262 B × N；255 個 ≈ 67 KB） |
| aprog | build 先數 distinct 效果，再 lalloc | `job.aprog` u16 → 65,535；記憶體 536 B × N（255 個 ≈ 137 KB） |
| 6 B 表 aprog | 1 B 欄位 | **255**（欄位闊度，不是魔法數字） |
| 舊 plan | 刪除或改動態 | 不再有 8 |

**耗盡時**：要明確 log（記憶體不足／欄位上限），唔好靜靜跳過。

### 7.212 2026-11-24（動態配置：寫幾多個就開幾多個）

用家要求：唔要固定 32，**寫幾個就開幾個**。查完現況：

| 位置 | 現況 | 大小 | 改成 |
|---|---|---:|---|
| `nc_px_asset.c` 的 `static nc_px_asset_tbl_t s_tbl` | 固定 32 | **8,386 B BSS** | 掃 count → lalloc（262 B × N） |
| pair 的 `s_content.assets` | 固定 32 | **8,386 B PSRAM** | 依 mode 的 distinct 效果數（最多 nentry × 262 B） |
| `layout_build` 的 aprog | 固定 32 | **17,152 B PSRAM** | 先數 distinct 效果 → 精確 lalloc（536 B × N） |
| 舊 `nc_px_plan_t` | 固定 8 | 4,424 B | 刪除（production 冇用） |

**做法**：`nc_px_asset_tbl_t` 改成 flexible array：

```c
typedef struct {
    uint16_t n;
    nc_px_asset_t item[];      // 實際數量由載入時決定
} nc_px_asset_tbl_t;

// 載入：第一次掃數，第二次 lalloc(n * sizeof(item)) 再填
```

**省幾多（常見情況）**：
- 整棵樹只有 2 個效果：8,386 → **524 B**（省 7.8 KB BSS）
- 一個 mode 用 3 個效果：8,386 → **786 B**（省 7.6 KB PSRAM）
- aprog 3 個：17,152 → **1,608 B**（省 15.5 KB PSRAM）

**上限**：`n` 用 uint16 → 65,535；實際由記憶體回答（lalloc 失敗 → 明確 log）。
**注意**：改成 flexible array 會改 ABI（`sizeof(nc_px_asset_tbl_t)` 由 8,386 變 2），
所有 `lalloc(sizeof(nc_px_asset_tbl_t))` 要改成 `sizeof + n * sizeof(item)`。

### 7.213 2026-11-24（夜間執行：零拷貝、開機預建、aprog 動態、抖動路徑）

依計劃逐個 Task 做，詳見 `docs/00_pm/night-report-2026-11-24.md`。

**完成並驗證**：
1. **零拷貝**：`ring_begin → apply → commit`，無 canvas、無 memset、無複製；
   64 px 上板 `clear=0`、`apply 31 µs`、`commit 7 µs`、**49.9 fps**；0xF06B 預覽正常。
2. **開機預建組合表**：首次缺 1 個 → `built=1, 269 ms`；第二次 → `built=0, 7 ms`；
   結果經 0xF079 的 `prebuild` 讀回。
3. **aprog 動態**：先數 distinct 效果再配置（移除固定 32）；suite 只剩兩個既有失敗。
4. **抖動**：`apply4`（多輸出）＋ `apply_dither`（逐子幀）；host 測試 [11] 過；
   2400 px × 4 子幀上板：`apply 9.06 ms`／視覺幀。
5. **Kconfig**：`SUBFRAMES`／`RING_SLOTS`／`MOCK_ONLY`；新 project Show2400 可建置。

**未完成**：mock-only lane 未生效（render 仍走 i80 → fps 13.9 係線路限制）；
assets 動態（仍 32）；6 B 單表；render 子幀接線完整驗證；chunked DMA（可選）。

**重要**：
- `pxEngine_test`／`pxProgram_test` 失敗係 **HEAD 既有**（`07cbc0c`），
  之前被舊 binary 遮住；`run_all.sh` 已改成每次強制重建。
- 裝置 FAT 寫入被鎖（`err_busy`），OTA 正常；要**斷電重插**，
  之後重新上傳 Zero-Show 的 `/pixel` 內容（目前板上係 2400 測試集）。

**補測（mock lane 生效後，2,400 px × 4 子幀）**：render **199.9 fps**（200 Hz 子幀、
視覺 50 fps）、apply **6.03 ms**／視覺幀、commit 12 µs、CPU ≈6.3 ms（31.5%）。
之前「mock lane 未生效」係 OTA 失敗、板跑舊映像，非程式問題。
