// nc_px_engine_program.c — program interpreter：段落表 → 每幀 12-bit 等級 → 畫布。
//
// 移植自 MicroPython 版（權威）：
//   `lib/sw/PixelMathMethod.py` 的 compile()／value_at()（波形）
//   `lib/sw/effect_core.py` 的 Effect.frame()（時間 × 空間的索引方式）
// 兩邊用同一組 golden 對過（tools/hosttest/pxProgram_test，期望值由 Python 跑出來）。
//
// 為什麼要「先 compile」：熱路徑（每幀 × 每顆 px）唔可以再做除法／clamp／查字段；
// MP 的註解講得好清楚 —— 除法同 clamp 只喺 compile 時算一次，之後乘數變加數。
// 這裡照做：compile 完只剩陣列索引＋加減＋一個正弦。
#include "nc_px_program.h"

#include <stdlib.h>
#include <string.h>

#include "nc_px_engine.h"
#include "sdkconfig.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

static nc_px_prog_seg_t *prog_seg_alloc(uint32_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc((size_t)n * sizeof(nc_px_prog_seg_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return (nc_px_prog_seg_t *)p;
    }
#endif
    return (nc_px_prog_seg_t *)malloc((size_t)n * sizeof(nc_px_prog_seg_t));
}

#if CONFIG_NETCORE_PIXEL

static uint16_t clamp12(uint32_t v)
{
    return (uint16_t)((v > 4095u) ? 4095u : v);
}

esp_err_t nc_px_engine_program_compile(const nc_px_asset_t *a, nc_px_prog_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (a == NULL || a->kind != NC_PX_ASSET_PROGRAM || a->nseg == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out->seg = prog_seg_alloc(a->nseg);
    if (out->seg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t prev = 0;
    for (uint16_t i = 0; i < a->nseg; i++) {
        const nc_px_seg_t *s = nc_px_asset_seg(a, i);
        if (s == NULL) {
            nc_px_engine_program_free(out);
            return ESP_ERR_INVALID_ARG;
        }
        const uint32_t end = s->end_f;
        if (end <= prev) {                       // end_Time 必須嚴格遞增（0 亦唔收）
            nc_px_engine_program_free(out);
            return ESP_ERR_INVALID_ARG;
        }
        const uint32_t fs = end - prev;          // 這段幾幀
        const uint32_t f = s->f;

        nc_px_prog_seg_t *c = &out->seg[i];
        c->start = prev;
        c->end = end;
        c->l_lim = clamp12(s->l_lim);
        c->l_range = (s->l_max > c->l_lim) ? (uint16_t)(clamp12(s->l_max) - c->l_lim) : 0u;
        c->phi4 = (uint16_t)(clamp12(s->phi) << 4);
        // MP：(65536 * F) // 10 // fs —— 兩個整除的次序有意義，唔可以合併成一次除。
        c->step_phase = (uint32_t)(((uint64_t)65536u * f) / 10u / fs);
        c->gap = f ? (fs / f) : 1u;
        if (c->gap == 0u) {
            c->gap = 1u;
        }
        c->width = (uint32_t)(s->pulse % c->gap);
        c->pulse = s->pulse;
        c->kind = s->type;
        prev = end;
    }
    out->n = a->nseg;
    out->total = prev;
    return ESP_OK;
}

void nc_px_engine_program_free(nc_px_prog_t *p)
{
    if (p == NULL) {
        return;
    }
    free(p->seg);
    memset(p, 0, sizeof(*p));
}

uint16_t nc_px_engine_program_value(const nc_px_prog_t *p, uint32_t g)
{
    if (p == NULL || p->n == 0 || p->total == 0) {
        return 0;
    }
    g %= p->total;

    for (uint16_t i = 0; i < p->n; i++) {
        const nc_px_prog_seg_t *s = &p->seg[i];
        if (g >= s->end) {
            continue;                            // 唔喺這段
        }
        switch (s->kind) {
        case NC_PX_SEG_KEEP:
            return (uint16_t)(s->l_range + s->l_lim);
        case NC_PX_SEG_STARTER:
            return 0;
        case NC_PX_SEG_PULSE: {
            const uint32_t rel = g - s->start;
            const uint32_t on = ((rel + s->phi4) % s->gap) <= s->width;
            return (uint16_t)(s->l_lim + (on ? s->l_range : 0u));
        }
        default: {
            const uint32_t rel = g - s->start;
            // 相位只取低 16 位；乘積用 64-bit 避免 step_phase × rel 溢出（MP 是大整數）。
            const uint32_t ph = (uint32_t)(((uint64_t)s->phi4 +
                                            (uint64_t)s->step_phase * rel) & 65535u);
            const uint16_t w = nc_px_engine_sin_lin(ph);
            uint32_t v;
            if (s->kind == NC_PX_SEG_MATH_NOW) {
                v = ((uint32_t)w * s->l_range) >> 12;
            } else if (s->kind == NC_PX_SEG_SQUARE_WAVE_NOW) {
                v = (w >= 2048u) ? s->l_range : 0u;
            } else {                             // NC_PX_SEG_PULSE_WAVE
                v = (w >= s->pulse) ? s->l_range : 0u;
            }
            return (uint16_t)(v + s->l_lim);
        }
        }
    }
    return 0;                                    // 正常唔會到（g < total = 最後一段 end）
}

uint32_t nc_px_engine_program_phase(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                    uint32_t t)
{
    if (p == NULL || p->total == 0) {
        return 0;
    }
    const uint32_t speed = (pl != NULL && pl->speed != 0) ? pl->speed : 1u;
    const uint32_t step = (pl != NULL) ? pl->step : 0u;
    const uint32_t offset = (pl != NULL) ? pl->offset : 0u;
    return (uint32_t)((((uint64_t)(t / speed)) * step + offset) % p->total);
}

void nc_px_engine_program_levels(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                 uint32_t t, uint16_t *lv, uint32_t n)
{
    if (lv == NULL || n == 0) {
        return;
    }
    if (p == NULL || p->total == 0 || p->n == 0) {
        memset(lv, 0, (size_t)n * sizeof(lv[0]));
        return;
    }
    // spacing 先取模（MP 的 _spacing_mod）：之後每步最多加一次、減一次就完成取模。
    const uint32_t sp = (pl != NULL) ? ((uint32_t)pl->spacing % p->total) : 0u;
    const bool rev = (pl != NULL) && pl->reverse;
    uint32_t g = nc_px_engine_program_phase(p, pl, t);

    for (uint32_t i = 0; i < n; i++) {
        lv[rev ? (n - 1u - i) : i] = nc_px_engine_program_value(p, g);
        g += sp;
        if (g >= p->total) {
            g -= p->total;
        }
    }
}

void nc_px_engine_program_canvas(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                 uint32_t t, uint32_t px,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t gain,
                                 uint16_t *dst)
{
    if (dst == NULL || px == 0) {
        return;
    }
    if (p == NULL || p->total == 0 || p->n == 0) {
        memset(dst, 0, (size_t)px * 3u * sizeof(dst[0]));
        return;
    }

    const uint32_t sp = (pl != NULL) ? ((uint32_t)pl->spacing % p->total) : 0u;
    const bool rev = (pl != NULL) && pl->reverse;
    uint32_t ph = nc_px_engine_program_phase(p, pl, t);

    for (uint32_t i = 0; i < px; i++) {
        const uint16_t lv = nc_px_engine_program_value(p, ph);
        ph += sp;
        if (ph >= p->total) {
            ph -= p->total;
        }

        // 12-bit 等級 → 0..255（`>>4`，同 MP 的 array('H') → 8-bit 一致），
        // 再行引擎既有的 gain／gamma 路徑，最後乘顏色（色度，唔影響亮度曲線）。
        const uint32_t level255 = (uint32_t)lv >> 4;
        const uint16_t lin = (uint16_t)((level255 * (uint32_t)gain * 257u) / 255u);
        const uint16_t v88 = nc_px_engine_gamma(lin);

        uint16_t *o = &dst[(rev ? (px - 1u - i) : i) * 3u];
        o[0] = (uint16_t)(((uint32_t)v88 * r) / 255u);
        o[1] = (uint16_t)(((uint32_t)v88 * g) / 255u);
        o[2] = (uint16_t)(((uint32_t)v88 * b) / 255u);
    }
}

#else  // !CONFIG_NETCORE_PIXEL — 契約仍在，實現係 no-op

esp_err_t nc_px_engine_program_compile(const nc_px_asset_t *a, nc_px_prog_t *out)
{
    (void)a;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

void nc_px_engine_program_free(nc_px_prog_t *p)
{
    (void)p;
}

uint16_t nc_px_engine_program_value(const nc_px_prog_t *p, uint32_t g)
{
    (void)p;
    (void)g;
    return 0;
}

uint32_t nc_px_engine_program_phase(const nc_px_prog_t *p, const nc_px_prog_play_t *pl, uint32_t t)
{
    (void)p;
    (void)pl;
    (void)t;
    return 0;
}

void nc_px_engine_program_levels(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                 uint32_t t, uint16_t *lv, uint32_t n)
{
    (void)p;
    (void)pl;
    (void)t;
    (void)lv;
    (void)n;
}

void nc_px_engine_program_canvas(const nc_px_prog_t *p, const nc_px_prog_play_t *pl,
                                 uint32_t t, uint32_t px, uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t gain, uint16_t *dst)
{
    (void)p;
    (void)pl;
    (void)t;
    (void)px;
    (void)r;
    (void)g;
    (void)b;
    (void)gain;
    (void)dst;
}

#endif  // CONFIG_NETCORE_PIXEL
