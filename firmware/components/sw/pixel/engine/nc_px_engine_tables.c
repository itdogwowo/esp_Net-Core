// nc_px_engine_tables.c — 引擎的查表（gamma／sine／量化）。
//
// 為什麼要查表：熱路徑（60 fps × 64 px × 3 ch）唔可以行 powf/sinf。
// 表在 bind 時建一次；效果只經 nc_px_engine_*() 純函數用，唔可以直接掂表。
#include "nc_px_engine.h"

#include <math.h>

#define RGB_PI_F 3.14159265358979f

static uint16_t s_gam[1024];              // gamma：(0..1023) linear → 8.8 顯示
static int16_t  s_sin[1024];              // Q15 sine
#define NC_PX_SIN_LIN_N 256
static int16_t  s_sin_lin[NC_PX_SIN_LIN_N + 1];   // 混合正弦的錨點（Q15）

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
    for (int i = 0; i <= NC_PX_SIN_LIN_N; i++) {
        s_sin_lin[i] = (int16_t)lroundf(sinf(2.0f * RGB_PI_F * (float)i / (float)NC_PX_SIN_LIN_N) * 32767.0f);
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

// 混合正弦：**錨點表（256 個）＋節點之間線性插值**。
// 表只存正弦本身（Q15），插值係一次式：a + ((b−a)·f >> 8)。
// 量測（tools/hosttest/sin_bench，host x86-64）：2.4× 快過多項式、誤差 0.23 LSB
// （多項式 3.74、1024 點查表 12.38）；表 514 B。詳細數字見 status.md §7.135。
uint16_t nc_px_engine_sin_lin(uint32_t phase)
{
    const uint32_t i = (phase >> 8) & (NC_PX_SIN_LIN_N - 1u);
    const uint32_t f = phase & 255u;
    const int32_t a = s_sin_lin[i];
    const int32_t b = s_sin_lin[i + 1];
    const int32_t v = a + (((b - a) * (int32_t)f) >> 8);      // Q15 有號
    // Q15 → 0..4095 **四捨五入**（+8 再 >>4）。截斷會偏 -0.5 LSB：
    // 量測（sin_bench 全相位掃描）max 誤差 1.23 → 0.72 LSB。
    const int32_t w = ((v + 8) >> 4) + 2048;
    return (uint16_t)((w > 4095) ? 4095 : (w < 0 ? 0 : w));    // 峰值 32767 會掂到 4096
}

// 0..65535 相位 → 0..4095 的正弦：拋物線基底＋二次修正（`922*(y²−y)>>12`），
// 同 MicroPython 的 PixelMathMethod 一樣，**無表、無浮點**。
// 精度：max 3.62 LSB（0.09% FS），比 1024 點的 s_sin 表好（實測見 tools/hosttest/sin_bench）。
// 用途：DATA program 的 12-bit 波形（keep/math_now/…），亦可以做細週期的相位換算。
uint16_t nc_px_engine_sin_q12(uint32_t phase)
{
    const uint32_t p = phase & 65535u;
    int32_t x, sgn;
    if (p < 32768u) {
        x = (int32_t)p;
        sgn = 1;
    } else {
        x = (int32_t)(p - 32768u);
        sgn = -1;
    }
    int32_t s = (int32_t)(((uint32_t)x * (uint32_t)(32768 - x)) >> 16);   // 0..4096 拋物線
    const int32_t s2 = (s * s) >> 12;
    s += (922 * (s2 - s)) >> 12;                                          // 二次修正
    if (sgn < 0) {
        s = -s;
    }
    const int32_t w = (s + 4096) >> 1;
    return (uint16_t)((w > 4095) ? 4095 : w);
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
