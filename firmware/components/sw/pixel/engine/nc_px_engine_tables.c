// nc_px_engine_tables.c — 引擎的查表（gamma／sine／量化）。
//
// 為什麼要查表：熱路徑（60 fps × 64 px × 3 ch）唔可以行 powf/sinf。
// 表在 bind 時建一次；效果只經 nc_px_engine_*() 純函數用，唔可以直接掂表。
#include "nc_px_engine.h"

#include <math.h>

#define RGB_PI_F 3.14159265358979f

static uint16_t s_gam[1024];              // gamma：(0..1023) linear → 8.8 顯示
static int16_t  s_sin[1024];              // Q15 sine

void nc_px_engine_tables_init(void)
{
    if (s_gam[1023] != 0) {
        return;                           // 已經建過
    }
    for (int i = 0; i < 1024; i++) {
        const float x = (float)i / 1023.0f;
        float y = powf(x, 2.2f) * 255.0f * 256.0f;   // 8.8
        if (y > 65535.0f) {
            y = 65535.0f;
        }
        s_gam[i] = (uint16_t)(y + 0.5f);
        s_sin[i] = (int16_t)lroundf(sinf(2.0f * RGB_PI_F * (float)i / 1024.0f) * 32767.0f);
    }
}

uint16_t nc_px_engine_gamma(uint16_t lin)
{
    uint32_t idx = (uint32_t)lin >> 6;          // 0..1023
    const uint32_t frac = (uint32_t)lin & 63u;
    if (idx >= 1023u) {
        return s_gam[1023];
    }
    const uint32_t a = s_gam[idx];
    const uint32_t b = s_gam[idx + 1u];
    return (uint16_t)(a + (((b - a) * frac) >> 6));
}

uint16_t nc_px_engine_lin(const nc_px_engine_params_t *p, uint32_t level255)
{
    return (uint16_t)((level255 * (uint32_t)p->gain * 257u) / 255u);
}

int32_t nc_px_engine_sin(uint32_t phase)
{
    return s_sin[phase & 1023u];
}

// 把 8.8 的目標量化到 bits 位（8..16）。例：bits=12 ⇒ 每級 16/256 = 1/16 個 8-bit 級。
uint16_t nc_px_engine_quant(uint16_t tgt, uint8_t bits)
{
    if (bits >= 16) {
        return tgt;
    }
    if (bits < 8) {
        bits = 8;
    }
    const uint32_t step = 1u << (16u - bits);
    return (uint16_t)((((uint32_t)tgt + step / 2u) / step) * step);
}
