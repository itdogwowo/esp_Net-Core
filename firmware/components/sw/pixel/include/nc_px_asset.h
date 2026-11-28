// nc_px_asset.h — DATA 平面：效果資產的結構體（JSON 解析後的正本）。
//
// 為什麼要這一層：`/data/pixel/effects/` 是使用者地盤（`effects.json`、`*.json`、`*.nce`），
// 但熱路徑唔可以逐幀 parse JSON。所以：**載入時解析一次、入結構體**，之後只行 struct。
//
// 規則（權威：docs/03_notes/behavior-taxonomy.md §2.0／§3.2、config-layers.md §5.1）：
//   · 三種載體掃入同一張表，key = `name`；掃描順序 effects.json → *.json（檔名排序）→ *.nce；
//     撞名 = 先到者勝 + log。
//   · 資產「存在」≠「會播」；會不會播由 registry.json（nc_px_show_t，下一支）決定。
//   · 這裡只放「宣告」，唔放程式碼：畫波由 CORE 的 program interpreter 解釋（有界，冇指標）。
//
// 數值域：program 段係 12-bit（0..4095，MP 沿用）；入 8.8 畫布時 `<<4`。
// 段型別只收 MP 的通用的六種；`uart_motor_*` 那類「畫波寫唔出」的效果唔入 program，走 `.nce`。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PX_NAME_MAX   24

// ═══════════════════════ 波形段（program）═══════════════════════
// 對應 MP effects.json 的 program[]：{"type","F","l_max","l_lim","phi","end_Time"}。
typedef enum {
    NC_PX_SEG_KEEP = 0,          // 保持上一段的值
    NC_PX_SEG_MATH_NOW,          // 正弦（nc_px_engine_sin_lin，0..4095）
    NC_PX_SEG_SQUARE_WAVE_NOW,   // 方波
    NC_PX_SEG_PULSE_WAVE,        // 脈衝串
    NC_PX_SEG_PULSE,             // 單脈衝
    NC_PX_SEG_STARTER,           // 起始段
    NC_PX_SEG_TYPE_MAX
} nc_px_seg_type_t;

typedef struct {
    uint8_t  type;      // nc_px_seg_type_t
    uint16_t f;         // 每步相位增量（MP 的 F）
    uint16_t l_max;     // 0..4095
    uint16_t l_lim;     // 0..4095
    uint16_t phi;       // 相位偏移 0..4095
    uint16_t end_f;     // MP 的 end_Time：**絕對幀、必須嚴格遞增**；最後一段 = 波長
    uint16_t pulse;     // pulse／pulse_wave 的 12-bit 門檻（缺省 2047，與 MP 一致）
} nc_px_seg_t;

// ═══════════════════════ 一個效果資產 ═══════════════════════
// 2026-11-28：program 段數改為動態；seg 只存「全域 segment pool 的 offset」。
// accessor：nc_px_asset_seg()（唯讀）／nc_px_asset_seg_add()（parser 用）。

typedef enum {
    NC_PX_ASSET_PROGRAM = 0,       // DATA：program（JSON 波形）
    NC_PX_ASSET_NCE,               // USER：`.nce` 外掛（manifest 自帶 name／px）
} nc_px_asset_kind_t;

typedef struct {
    char        name[NC_PX_NAME_MAX];   // key；JSON 冇寫 = 檔名
    uint8_t     kind;                   // nc_px_asset_kind_t
    uint16_t    nseg;                   // kind=PROGRAM 時有效（動態）
    uint16_t    seg_off;                // 第一個 segment 在全域 pool 的 index
    uint16_t    pixel_hint;             // 只作提示：真正 px 由環的事實決定，唔信呢個
    // 空間／時間分布（同 MP effects.json 同名）：phase = (t/speed)*step + i*spacing + offset
    uint16_t    step;                   // 每次時間推進幾格（預設 1）
    uint16_t    spacing;                // 相鄰 pixel 相位差（預設 0）
    uint16_t    offset;                 // 整體相位偏移
    uint16_t    speed;                  // 幾幀一步（0 當 1）
    bool        reverse;                // 反向輸出
} nc_px_asset_t;

// 效果表：**唔再係固定 32 個**（舊版 32 × 262 B ＝ 8,386 B 食死在 BSS，開機就冇）。
// `n` 已用、`cap` 容量、`item` 由 `nc_px_asset_tbl_*` 管理（板上有 PSRAM 就用 PSRAM）。
// 呼叫端亦可以自己塞緩衝（`nc_px_asset_tbl_use`，owned = 0 → 本層唔會 realloc／free 它）。
typedef struct {
    uint16_t       n;
    uint16_t       cap;
    uint8_t        owned;
    nc_px_asset_t *item;
} nc_px_asset_tbl_t;

typedef struct {
    uint16_t loaded;      // 入表幾個
    uint16_t rejected;    // 解析失敗／未知段型別／表滿
    uint16_t dup;         // 撞名被忽略（保留第一個）
} nc_px_asset_stat_t;

// ═══════════════════════ 表本體（按實際數量配置，冇上限）═══════════════════════
// 為什麽要動態：效果數係用戶 DATA（`effects.json` ＋ 一堆檔），固定 32 就係「隱藏上限」。
// `alloc` 連表頭一齊配置；`use` 用呼叫端自己的緩衝（host 測試／臨時表）；
// `reserve` 唔夠位就倍增（8 → 16 → …），舊內容自動帶過去。
nc_px_asset_tbl_t *nc_px_asset_tbl_alloc(uint16_t cap);   // 回 NULL = 記憶體不足

// 動態 segment pool accessor（asset 可能被 copy，offset 仍然有效）。
const nc_px_seg_t *nc_px_asset_seg(const nc_px_asset_t *a, uint32_t i);
nc_px_seg_t       *nc_px_asset_seg_add(nc_px_asset_t *a);   // 追加一段並回傳可寫 pointer
void               nc_px_asset_tbl_free(nc_px_asset_tbl_t *t);
void               nc_px_asset_tbl_use(nc_px_asset_tbl_t *t, nc_px_asset_t *buf, uint16_t cap);
esp_err_t          nc_px_asset_tbl_reserve(nc_px_asset_tbl_t *t, uint16_t need);

// ═══════════════════════ API ═══════════════════════
// **解析核心**（這一支唔碰檔案系統，所以可以直接在 PC 上測）：
//   JSON 文字 → 一個（`{...}`）或一批（`{"effects":[...]}`）。
//   name_hint = 檔名（單一效果的檔冇寫 `name` 時用它；`effects.json` 的陣列元素不用）。
//   回 ESP_OK = 至少加入一個；ESP_ERR_INVALID_ARG = JSON／schema 壞；
//   ESP_ERR_INVALID_STATE = 撞名；ESP_ERR_NO_MEM = 表滿。
esp_err_t nc_px_asset_add_json(const char *json, size_t len, const char *name_hint);

// 掃描一個目錄（開機一次、ASSET_RELOAD 再一次）。壞檔／缺字段／超上限：
//   跳過 + log，保留 last-good，唔會令整張表失效（回 ESP_OK）。
esp_err_t nc_px_asset_scan(const char *dir);

void                  nc_px_asset_clear(void);
int                   nc_px_asset_count(void);
const nc_px_asset_t  *nc_px_asset_at(int i);
const nc_px_asset_t  *nc_px_asset_find(const char *name);
void                  nc_px_asset_stats(nc_px_asset_stat_t *out);

// 直接加入一個已填好的資產（掃到 `.nce` 時用；`name` 空白 = 拒絕）。
esp_err_t nc_px_asset_add(const nc_px_asset_t *a);

// 掃描外殼（nc_px_asset_scan.c）遇到讀唔到／過大時呼叫：計入 rejected。
void nc_px_asset_note_reject(void);

// 狀態 JSON（0xF0xx 回覆用）：載入幾個、邊個被拒、為咩被拒。截斷回 0。
size_t nc_px_asset_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
