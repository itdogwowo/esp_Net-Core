# PSRAM 單一服務管線：決策、實測、500 組模型

> 2026-11-22。這頁是 effect→pair→render 在 **固定 PSRAM 服務路徑** 下的完整紀錄：
> 為什麼改、量到什麼、500 個可變長度連續 group 的 8-bit／12-bit 成本、
> table 設計、production 仍未做的部分。
>
> 相關頁：
> [show-pipeline.md](show-pipeline.md)（三任務資料流）、
> [../03_notes/buffers.md](../03_notes/buffers.md)（HOT／BULK／DMA 政策）、
> [status.md](status.md)（§7.160–7.171）。

---

## 1. 決策摘要

用家要求：**穩定服務，無論 LED 有多少都走同一條流程；有 PSRAM 環節就全部走 PSRAM；不要 runtime 智能選擇。**

已落實：

| 類別 | 固定位置 | 內容 |
|---|---|---|
| HOT | internal SRAM | plan、asset、wave table、run／offset table |
| BULK | **PSRAM（任何 LED 數）** | content/pair canvas、Show I80 packed 波形、12-bit canvas、大 JSON |
| DMA | internal／DMA-capable | I80 真正 DMA 讀的 buffer |

- `content/pair` canvas 已 **hardcode `MALLOC_CAP_SPIRAM`**，64 px 與 9,600 px 都一樣。
- Show project 固定 `CONFIG_NETCORE_PIXEL_PACK_PSRAM=y`，packed 波形全部 PSRAM。
- **沒有**「internal 失敗才退 PSRAM」、沒有 free heap 判斷、沒有 size-based 分支。
- allocation 失敗就 log／task 失敗，不會偷偷換位置。
- `NETCORE_PIXEL_LEDS` range 已擴到 **16384**。

---

## 2. 實測環境

| 項目 | 值 |
|---|---|
| Board | Waveshare ESP32-S3-Zero，ESP32-S3 240 MHz，2 MB PSRAM |
| Production app | Zero-Show `d6837876`、image **705,568 B** |
| Production tasks | `render`／`pair`／`effect`（沒有 pixel／content／effect_bench） |
| Production 內容 | `/pixel` breathe、64 px、white=0 |
| Probe firmware | ESP32-S3-Matrix-EffectPerf（同一 S3；只量 CPU／記憶體，不送 9,600 燈） |
| Probe command | `0xF069`（EffectPerf） |
| Stats command | `0xF071`；burn `0xF073`；reset `0xF075` |

**重要區分**：

- 64 px 是 production 真機輸出。
- 9,600 px 是 **CPU／memory probe**，不是 production 9,600 燈輸出。

---

## 3. 64 px production（全 PSRAM）

固定政策後重新量：

| 段 | avg | max |
|---|---:|---:|
| effect | **8 µs** | 23 µs |
| pair | **35 µs** | 86 µs |
| ├ apply | 25 µs | 47 µs |
| ├ white | 1 µs | 3 µs |
| └ commit | 9 µs | 36 µs |
| effect→pair latency | 482 µs | 491 µs |
| render encode | **363 µs** | — |
| render tx | 24 µs | — |
| render fps | **50.0** | frame 20.0 ms |

Burn 2 秒：

| 段 | 結果 |
|---|---|
| effect | **55,656 幀 / ~18,500 fps**；avg 6 µs、max 77 µs |
| pair | 257 幀（ring 限流） |
| ring full | 2,006 |
| render | 50.0 fps、enc 362 µs、tx 21 µs |

結論：64 px 走全 PSRAM **沒有破壞 50 fps**。

---

## 4. Wave table：做一次、重用

問題：之前每幀每個值都跑 program interpreter。

做法：plan 建立時預算：

```c
uint16_t wave[total];       // total 通常 256，最多 4096
wave[p] = nc_px_engine_program_value(&prog, p);
```

每幀只做 index：

```c
value = wave[(phase + i * spacing) % total];
```

| 項目 | 無 wave table | 有 wave table |
|---|---:|---:|
| 64 px effect | 46 µs | **7–8 µs** |
| 9,600 RGB gen（28,800 值） | 15.6 ms | **2.0 ms** |
| 9,600 full8 internal | 16.59 ms | **4.42 ms** |

- wave table 只在 `plan_gen` 改變時重建。
- 這是「做一次、之後重用」最值錢的一項。

---

## 5. 9,600 RGB 極限 probe

設定：

- `px=9600`
- `nv=28800`（3 值/LED，`write=rgb`）
- random permutation `table_rnd[k] = k*7919 % 9600`
- `frames=2`，每項取多次最好值
- 三次獨立 run

### 5.1 三次結果

| 項目 | run 1 | run 2 | run 3 |
|---|---:|---:|---:|
| gen（interpreter） | 15.614 ms | 15.6 ms | 15.6 ms |
| gen（wave table） | 1.996 ms | 1.99 ms | 1.99 ms |
| pair random12 | 19.968 ms | 19.967 ms | 19.968 ms |
| pair packed12 | 20.599 ms | 20.601 ms | 20.599 ms |
| pair random8（internal） | 0.971 ms | 0.971 ms | 0.971 ms |
| pair inverse8（internal） | 930 µs | 930 µs | 930 µs |
| pair block8（internal） | 967 µs | 967 µs | 967 µs |
| full random12 | 37.880 ms | 37.862 ms | 37.862 ms |
| full random8（internal） | 16.586 ms | 16.595 ms | 16.591 ms |
| **full cached8（internal）** | **4.426 ms** | **4.425 ms** | **4.409 ms** |
| mem | c12 PSRAM／c8 internal | 同左 | 同左 |

三次穩定，沒有 free heap 造成的波動。

### 5.2 為什麼 12-bit random 慢 20×

用 `esp_ptr_internal()`／`esp_ptr_external_ram()` 量到：

```text
c12_int=0  c12_ps=1   → 12-bit canvas 在 PSRAM
c8_int=1   c8_ps=0    → 8-bit canvas 在 internal（當時）
```

原因：

- 9,600 RGB 的 vals 57.6 KB ＋ c8 38.4 KB ＋ tables 38.4 KB 已佔用 internal；
  再要 76.8 KB 的 12-bit canvas 時 internal 不足，fallback 去 PSRAM。
- **不是**「四個 channel 連續但分四次 store」。
- `packed12`（2×32-bit store/px）仍是 **20.599 ms**，證明 store 數不是主因。
- 主因是 **PSRAM random write**：12-bit random 19.967 ms vs 8-bit internal random 0.970 ms。

### 5.3 固定 c8 在 PSRAM 後

| 項目 | c8 internal | c8 PSRAM |
|---|---:|---:|
| pair random8 | 0.972 ms | **10.647 ms** |
| pair inverse8 | 0.931 ms | **1.872 ms** |
| pair block8（16 px） | 0.966 ms | **1.864 ms** |
| full cached random8 | 4.423 ms | **14.060 ms** |
| **full cached inverse8** | **3.648 ms** | **4.586 ms** |
| full cached block8 | 3.689 ms | **4.583 ms** |

**固定 PSRAM 的關鍵是唔好 random write；用 destination 順序的 inverse／block，full 由 14.06 → 4.59 ms。**

---

## 6. 真實情況：500 個可變長度連續 group

用家澄清：

> 500 個隨機連續的組合，長度可能是 2–3 px、幾百 px、十幾 px；group 內連續，group 之間任意跳。

即係 **500 條 run**，總共 9,600 px，平均 19.2 px/run。

### 6.1 用 run table，不是 per-pixel table

```c
typedef struct {
    uint16_t src_start;
    uint16_t dst_start;
    uint16_t count;      // 2、3、十幾、幾百
} run_t;

run_t runs[500];         // 500 × 6 B = 3 KB
```

- 建立一次（mode／map 載入），之後每幀重用。
- group 長度只是 `count`，不需要為 2 px 或幾百 px 寫特別路徑。
- 建議按 `dst_start` 排序：canvas 寫入順序化，只有 500 次 source 跳轉。
- 總寫入仍然 9,600 px；group 跳轉只有 500 次（完全 random 是 9,600 次）。

### 6.2 用 600 組 ×16 px 做保守 proxy

600 組比 500 組更多跳轉，所以量出嚟偏慢、保守：

| 情況 | pair | full（wave gen + pair） |
|---|---:|---:|
| 600×16，8-bit internal | 0.966 ms | ~3.0 ms |
| 600×16，8-bit PSRAM | 1.864 ms | ~3.9 ms |
| 完全 random，8-bit PSRAM | 10.647 ms | 14.060 ms |

500 條、平均 19.2 px 的估算：

| 路線 | pair（估） | full CPU（估） | 50 fps 佔用 |
|---|---:|---:|---:|
| **8-bit canvas（PSRAM）** | **1.5–1.9 ms** | **3.5–3.9 ms** | **18–20%** |
| **12-bit canvas + conv（PSRAM）** | **5.0–5.3 ms** | **10.4–10.7 ms** | **52–54%** |
| 12-bit values → 直接 8-bit canvas | 1.5–1.9 ms | 3.5–3.9 ms | 18–20% |

12-bit canvas 的 5.0–5.3 ms 是從 12-bit sequential range 4.9 ms + 500 run overhead 推算；
exact 500 可變長度 probe 尚未接入，這是 estimate，不是正式量測。

### 6.3 為什麼 12-bit canvas 不值得

WS2812 只有 8-bit PWM。12-bit canvas 只是中間暫存：

- 12-bit canvas = 76.8 KB PSRAM，4 次 16-bit store/px。
- 之後仍要 `nc_px_canvas12_to_8()`：**3.4 ms**。
- 12-bit canvas 版本 full **10.4–10.7 ms**，比直接 8-bit **3.5–3.9 ms** 多 6–7 ms。
- 對 WS2812 沒有好處。

**Production 正確路線：effect 12-bit values → pair 直接 `>>4` 寫 8-bit canvas。**

### 6.4 三種 table

| table | 大小 | 用途 |
|---|---:|---|
| **run table** | 500 × 6 B ≈ 3 KB | 500 個可變長度連續 group；首選 |
| per-pixel dest table | 9,600 × 2 B = 19.2 KB | 完全 random；worst-case |
| per-pixel inverse table | 9,600 × 2 B = 19.2 KB | destination 順序寫、source 隨機讀 |
| wave table | 256 × 2 B = 512 B | 效果波形，不變 |

---

## 7. Render 的物理限制

9,600 顆 WS2812、單 lane 800 kHz：

```text
9,600 × 24 bit / 800,000 = 288 ms/幀 = 3.47 fps
```

- 要 50 fps：需要約 **15 條並行 800 kHz lane**。
- 這是物理輸出限制，不是 CPU pair。
- 9,600 要 50 fps 的資料率：9,600×24×50 = 11.52 Mbit/s。
- 現時 Zero 只接一條 GPIO1 lane。

---

## 8. 目前 production 的限制

| 限制 | 現況 | 影響 |
|---|---|---|
| effect frame | 每 job cap **256 值** | 9,600 RGB 要 28,800 值，未支援 |
| pair plan | 最多 **8 mode entry／16 map group** | 500 組未支援，要改 run table |
| 9,600 random table | 只在 probe | production 未接 |
| render | 單 lane | 9,600 只有 3.47 fps |
| 64 px | 已全 PSRAM production | 50 fps 正常 |

**所以：**

- 64 px：production，effect 8 µs、pair 35 µs、render 363 µs、50 fps。
- 9,600：CPU probe；8-bit 500-run 估算 pair 1.5–1.9 ms、full 3.5–3.9 ms；
  但 production 接線（frame／run table／多 lane）未完成。

---

## 9. 指令與重現

| 指令 | 用途 |
|---|---|
| `0xF071` | 讀三任務分段計時 |
| `0xF073 [秒]` | burn（effect 全速） |
| `0xF075` | reset stats |
| `0xF069` | EffectPerf 9,600 probe |
| `0xF06B` | 讀 production 一幀 |
| `0xF06D [id]` | 切 mode |
| `0xF06F` | reload `/pixel` |

工具：

```bash
python3 tools/pipeline_stats.py --via http://127.0.0.1:8011
python3 tools/pipe_bench.py --via http://127.0.0.1:8011 --px 9600 --frames 2 --mode rgb
python3 tools/pipe_bench.py --via http://127.0.0.1:8011 --px 9600 --frames 2 --mode rgb --c8-psram
```

---

## 10. Build／測試紀錄

| 項目 | 結果 |
|---|---|
| Zero-Show | **705,568 B**、app `d6837876` |
| Matrix-Show | 689.5 KB |
| EffectPerf | 679.1 KB |
| Host tests | 20 套全綠 |
| WebMaster | 296 passed |

功能：

- 0xF06D 切 color／breathe 成功（apply 225–380 µs，視模式）
- 0xF06F reload 成功
- status tasks：只有 `render`／`pair`／`effect`

---

## 11. 一句話結論

> 所有 LED 數都走同一條 PSRAM 服務路徑：effect wave table → pair 8-bit PSRAM canvas → render PSRAM packed。  
> 64 px 實測仍然 50 fps（effect 8 µs、pair 35 µs、render 363 µs）。  
> 500 個可變長度 run 的 9,600 px，用 3 KB run table、12→8 直接寫，估 pair 1.5–1.9 ms、full 3.5–3.9 ms。  
> 12-bit canvas 版本約 10.4–10.7 ms，對 8-bit WS2812 沒有好處；完全 random 的 19.97 ms 只是 worst-case。  
> 9,600 真正的瓶頸是單 lane 288 ms/幀（3.47 fps），要 15 lane 才能 50 fps。
