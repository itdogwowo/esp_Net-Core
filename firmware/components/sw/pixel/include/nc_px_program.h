// nc_px_program.h — DATA 平面的 program interpreter（CORE 的「畫波」）。
//
// 誰住這裡：把 `nc_px_asset_t` 的段落表**編譯**成每幀可用的預算值，再逐幀取等級。
// 它係**引擎的一部分**（`nc_px_engine_program_*`），但唔會改引擎狀態：
// 純函數、有界、無 malloc、無指標逃逸 —— 所以同一份 code 可以在 PC 上跑測試。
//
// 語義權威 = MicroPython 版的 `lib/sw/PixelMathMethod.py`（compile／value_at）
// ＋ `lib/sw/effect_core.py`（Effect.frame 的空間分布）。移植時刻意保留：
//   · step_phase = (65536·F) / 10 / fs（fs = end − prev，最少 1）
//   · phi4       = phi << 4；相位 = (phi4 + step_phase·rel) & 65535
//   · l_range    = l_max − l_lim（C 額外 clamp 到 >= 0，壞資料唔會出負值）
//   · pulse      = `(rel + phi4) % gap <= width`，gap = fs / F、width = pulse % gap
//   · 一幀的起點 g = ((t / speed)·step + offset) % total；每顆 px 之後 g += spacing
//   · 最後一段的 end_Time = 波長（total）
//
// 值域：全部 **12-bit（0..4095）**，同 MP 一樣；入 8.8 畫布時 `>> 4` 轉 0..255。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_px_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

// 2026-11-28：段數動態；seg 指向 compile 時配置的陣列（由 program_free 釋放）。

// 編譯後的一段：熱路徑唔再除、唔再 clamp（全部喺 compile 時算好）。
typedef struct {
    uint32_t start;         // 這段的第一幀（含）
    uint32_t end;           // 這段的最後一幀之後（不含）
    uint16_t l_range;       // l_max − l_lim（已 clamp >= 0）
    uint16_t l_lim;
    uint16_t phi4;          // phi << 4
    uint32_t step_phase;    // 每幀相位增量 (65536·F)/10/fs
    uint32_t gap;           // pulse 型：fs / F（最少 1）
    uint32_t width;         // pulse 型：pulse % gap
    uint16_t pulse;         // pulse_wave 的門檻（12-bit；缺省 2047）
    uint8_t  kind;          // nc_px_seg_type_t
} nc_px_prog_seg_t;

typedef struct {
    uint16_t n;                                  // 有幾段
    uint32_t total;                              // 波長（最後一段的 end）
    nc_px_prog_seg_t *seg;                       // 動態；NULL = 未編譯
} nc_px_prog_t;

// 播放參數（MP effects.json 的空間分布；`pixel_n` 由環決定，唔在這裡）。
typedef struct {
    uint16_t step;          // 每幀相位推進（時間軸）
    uint16_t spacing;       // 相鄰兩顆 px 的相位差（空間軸）
    uint16_t offset;        // 整條鏈的相位偏移
    uint16_t speed;         // 幾幀當一步（0 = 當 1，避免除零）
    bool     reverse;       // true = 波形倒轉方向
} nc_px_prog_play_t;

// ═══════════════════════ 編譯 ═══════════════════════
// 資產 → 預算段表。只收 kind=PROGRAM 且有段落、end_Time 嚴格遞增的資產
// （解析器已擋一次；這裡再擋，因為 struct 也可能由程式直接填）。
// 回 ESP_OK／ESP_ERR_INVALID_ARG；失敗時 `out` 被清空（n=0、total=0）。
esp_err_t nc_px_engine_program_compile(const nc_px_asset_t *a, nc_px_prog_t *out);
void      nc_px_engine_program_free(nc_px_prog_t *p);

// ═══════════════════════ 取值 ═══════════════════════
// 全域幀 g → 12-bit 等級。決定性、無狀態：同一個 g 永遠同一個值。
uint16_t nc_px_engine_program_value(const nc_px_prog_t *p, uint32_t g);

// 第 t 幀的起點相位（g0）。之後每顆 px：g += spacing % total（單次減法取模）。
uint32_t nc_px_engine_program_phase(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                    uint32_t t);

// 第 t 幀 → n 個 12-bit 等級（`lv` 由呼叫端提供，n 由環的事實決定，唔信 JSON）。
// `p` 無效／未 compile 時寫 0，安全。
void nc_px_engine_program_levels(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                 uint32_t t, uint16_t *lv, uint32_t n);

// 第 t 幀 → 8.8 顯示值畫布（px × 3）：等級先轉 0..255，乘 gain、過 gamma，
// 再乘指定的顏色。效果（`nc_effect_*`）用它，唔使自己做 12-bit 換算。
void nc_px_engine_program_canvas(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                 uint32_t t, uint32_t px,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t gain,
                                 uint16_t *dst);

#ifdef __cplusplus
}
#endif
