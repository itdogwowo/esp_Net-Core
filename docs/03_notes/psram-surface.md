# PSRAM surface／pass 管理器（設計）

> 2026-11-09。回答「怎樣才算用對 PSRAM」、「需不需要一個管理器做中間層」。
> 實測數據：[buffer-pool.md](buffer-pool.md) §4；指令 `0xF01C`／`0xF01D`。

## 0. 先定義「用對方法」——不是用 tile 就會快

PSRAM 是 **bus-bound**：38.4 KB 一幀，寫一次 ~1.2 ms、讀一次 ~1.2 ms、copy ~1.8–1.9 ms、
flush ~0.4 ms。`0xF01D` 量到：新鮮 40 KB 分塊 copy 只有 **16 MB/s**（整幀 12 MB/s），
tiling 只快 **1.3×**，不是 6×。**因為每個 byte 最終都要寫入 PSRAM。**

所以「用對方法」的判準是：

| # | 判準 | 為什麼 |
|---|---|---|
| 1 | **每幀 ≤1 次全幀寫 + ≤1 次全幀讀**（目標 2.8 ms） | 每多一個全幀 pass 多 ~1.2 ms |
| 2 | **多 pass 效果在 internal tile（≤16 KB）做**，唔好每 pass 掃 PSRAM | 同一塊在 cache 內重複用才有 100+ MB/s |
| 3 | 分塊係用來 **bound latency**（flush 9.6 KB ~0.1–0.2 ms）＋ **multi-pass 局部性** | 不是用來提升單 pass 頻寬 |
| 4 | 64 B 對齊、順序 row-major、避免隨機跳躍 | cache line／burst |
| 5 | 雙／三緩衝；**唔好寫 DMA 正在讀的那塊** | 撕裂／cache flush |
| 6 | 量 `passes`、`frame_us`、`tile_us`、`flush_us`；有效 <20 MB/s 代表 pass 太多 | 用數字，不用感覺 |
| 7 | **ISR 不碰 PSRAM**；OTA/flash 停頓要 pause 或接受 | flash 停 cache 時 PSRAM 一齊停 |

一句話：**「對」= 數 pass、把多 pass 搬入 internal tile、分塊 bound latency；**
不是「切 16 KB 就會變 150 MB/s」。

## 1. 需不需要管理器？——要，但係「surface 管理器」，不是通用 heap

| 情境 | 管理器？ |
|---|---|
| 一個效果、一個 display、冇 audio | 唔一定要；30 行 double buffer＋chunk loop 已夠 |
| 多效果／audio／OTA／要統計 | **要**：否則每個效果作者都會不小心做全幀多 pass |
| 多個 surface（LED、面板、audio） | 要，但每個 surface **固定 buffer**，不要每次都 malloc |

> **不要和 `nc_pool` 混在一起**：`nc_pool` 管的是 **bus frame**（RX／forward／bulk），
> 生命週期是「一幀」；`nc_surface` 管的是 **pixel/audio 的長壽 buffer**（2–3 幀、tile）。
> 兩者用同一個 pool 會令「一幀 release」和「一個 surface 存在幾秒」互相打架。

管理器只做四件事：**buffer 擁有權、tile 順序、pass 計算、sink 交接**。
不做：per-frame malloc、generic heap、full-frame copy、ISR 工作、效果邏輯。

## 2. 架構

```
effect（producer）
   │  nc_surface_begin / next_tile / commit_tile / end
   ▼
nc_surface（fixed buffers ×2–3、tile iterator、internal scratch、pass counter）
   │  sink->push_tile(data,len) / sink->frame_done()
   ▼
display sink
   ├─ direct：I80 tx_color 直接讀 PSRAM tile（driver flush per tile）
   └─ staging：copy tile → internal 2×tile ping-pong → I80 讀 internal
audio：nc_audio_ring（PSRAM）→ 4 KB chunk → internal I2S DMA ping-pong
```

- buffer 狀態：`FREE → WRITING → READY → PRESENTING → FREE`；2 塊可跑，3 塊可重疊。
- tile：預設 **4 rows × 600 px × 4 B = 9.6 KB**（≤16 KB，留一半 cache 給其他資料）。
- internal scratch：2 × tile（9.6 KB）或 1 × 16 KB；multi-pass 效果 copy 入去再做。
- sink 二選一：
  - **direct**：`esp_lcd_panel_io_tx_color(io, cmd, tile, 9,600)`；driver 會 flush 該 tile（~0.1–0.2 ms），
    GDMA 直接讀 PSRAM。最少內部 RAM。
  - **staging**：copy tile 去內部 ping-pong（9.6 KB ≈ 0.1 ms），DMA 只讀內部；時序最穩，多 19.2 KB 內部。
- audio 是獨立 ring，不經 surface；只共用 PSRAM 頻寬，所以要有 **bandwidth budget**。

## 3. API（草案）

```c
typedef struct {
    uint16_t w, h, bpp;
    uint16_t tile_rows;       // 4
    uint8_t  buffers;         // 2–3
    nc_mem_class_t where;     // PSRAM
    size_t   scratch_bytes;   // 0 = 不用 internal scratch；否則 2 × tile
} nc_surface_cfg_t;

typedef struct {
    uint8_t *data;            // PSRAM tile（或 internal scratch）
    uint16_t x, y, w, h;
    size_t   bytes;
    uint8_t  flags;           // INTERNAL / DIRTY
} nc_tile_t;

nc_surface_t *nc_surface_create(const nc_surface_cfg_t *cfg);
int  nc_surface_begin(nc_surface_t *s);                 // 等 FREE buffer，回 index
bool nc_surface_next_tile(nc_surface_t *s, nc_tile_t *t); // 順序 row-major
void nc_surface_commit_tile(nc_surface_t *s, nc_tile_t *t); // 交給 sink（可立即 push）
void nc_surface_end(nc_surface_t *s);                   // frame done
void nc_surface_wait(nc_surface_t *s, uint32_t timeout_ms); // 等上一個 present
void nc_surface_tile_internal(nc_surface_t *s, nc_tile_t *t); // copy PSRAM→scratch（multi-pass）
void nc_surface_tile_flush(nc_surface_t *s, nc_tile_t *t);    // scratch→PSRAM

typedef struct {
    uint32_t frames, passes;
    uint32_t tile_us_avg, tile_us_max;
    uint32_t frame_us_avg, frame_us_max;
    uint32_t flush_us_avg, underruns;
    uint32_t mbps;            // 有效
} nc_surface_stats_t;
```

- `begin` 會等 `FREE`（3 塊時通常 immediate）；`wait` 等 `PRESENTING` 完成。
- `commit_tile` 可以選擇「立即 push」或「frame end 才 push」；LED/I80 建議立即推，保持 tile 熱。
- `tile_internal`／`tile_flush` 只給 multi-pass 效果；單 pass 效果直接寫 PSRAM tile。
- 所有 buffer 在 `create` 時一次配置（`nc_pool` 或 `nc_mem_pool`），之後不再 malloc。

## 4. 設定

```json
"pixel": {
  "surface": {
    "w": 600, "h": 16, "bpp": 4,
    "buffers": 3,
    "tile_rows": 4,
    "where": "psram",
    "scratch_kb": 20
  },
  "display": {
    "kind": "i80",
    "mode": "direct",
    "chunk_kb": 8
  }
},
"audio": {
  "ring_ms": 200,
  "chunk_kb": 4
}
```

- 3 buffers = 115.2 KB PSRAM；tile 9.6 KB；scratch 2 × 9.6 KB internal。
- `mode=direct` 先試；量到 jitter 才改 `staging`。
- `pass_budget` 可選：超過就 `LOGE`＋統計（不是丟幀）。

## 5. 預算（38.4 KB frame、60 fps）

| 項目 | 時間 |
|---|---:|
| 寫一次 PSRAM | 1.2 ms |
| 讀一次（DMA 或 CPU） | 1.2 ms |
| flush（direct，分塊） | 0.4 ms |
| **合計（1 write + 1 read + flush）** | **~2.8 ms（17%）** |
| 每多一個全幀 read pass | +1.2 ms |
| 每多一個全幀 write pass | +1.2 ms |
| internal tile 多 pass（copy in + N×內部 + copy out） | ~2.5–3 ms |

- 60 fps 仍有 13.9 ms 做效果／網路；3 緩衝可以 overlap「讀 frame N、寫 frame N+1」。
- audio：192 KB/s；4 KB chunk 每 20.8 ms；copy 0.05–0.1 ms（PSRAM 4 KB 在 cache 內）。
- 如果效果需要 3 個全幀 pass（例如 blur + blend + dither），總數會到 6–8 ms（36–48%），
  這時應該 **copy tile 入 internal 做**，而不是掃 PSRAM 三次。

## 6. 決策矩陣

| 情境 | 建議 |
|---|---|
| 單效果、單 display、冇 audio | 唔用 manager；double buffer＋chunk loop，照 §0 判準 |
| 多效果、audio、OTA、要統計 | **用 `nc_surface`**（固定 buffer、tile、pass 計數） |
| 多個 display／surface | 每個 surface 一個 manager instance；buffer 固定，不共用 |
| 效果需要多次讀寫 | manager 的 `tile_internal`／`tile_flush`；唔好每 pass 掃 PSRAM |
| 想省內部 RAM | `mode=direct`；scratch 0；用 2 buffers |
| 想要最穩時序 | `mode=staging`＋2 × tile internal；多 19.2 KB |
| PSRAM 與 flash 同時 | OTA 時 pause surface（或接受花屏） |

## 7. 落地順序

| 階段 | 內容 | 驗收 |
|---|---|---|
| P0 | `nc_surface` 2 buffers、tile iterator、direct sink（I80 未接前用 dummy sink） | hosttest＋上板 tile bench |
| P1 | internal scratch（multi-pass）、staging sink、stats | `tile_us`／`flush_us` 量得到 |
| P2 | `nc_audio_ring`（PSRAM ring → internal DMA ping-pong） | 192 KB/s 不 underrun |
| P3 | config＋效果遷移；pass budget | 38.4 KB frame < 5 ms（1 write+1 read） |

## 8. 不要做

- ❌ 每幀 malloc／free（碎片、時間不確定）
- ❌ 全幀 copy 一次再讀（多 1.8–1.9 ms）
- ❌ 用 tile 當「萬能加速」——單 pass 仍然 bus-bound
- ❌ 在 ISR 碰 PSRAM
- ❌ 寫 DMA 正在讀的 buffer
- ❌ 4 個以上全幀 pass 而不改 internal tile

## 9. 未定問題

1. I80 面板是否要「整幀 latch」？如果要，tile 只 bound latency，不會減少 pass。
2. tile 4 rows（9.6 KB）vs 6 rows（14.4 KB）vs 8 rows（19.2 KB > 16 KB）：先 4，量到 flush 太碎再調。
3. 2 vs 3 buffers：3 可以 overlap 寫 N+1／讀 N；2 需要等。
4. audio ring 200 ms（38.4 KB）夠不夠？P4 32 MB PSRAM 可以更大。
5. 是否要一個全域 bandwidth budget（surface＋audio＋network）？現在只有統計，沒有強制。
