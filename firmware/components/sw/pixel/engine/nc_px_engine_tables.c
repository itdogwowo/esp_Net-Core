// nc_px_engine_tables.c — 引擎的查表（gamma／sine／量化）。
//
// 為什麼要查表：熱路徑（60 fps × 64 px × 3 ch）唔可以行 powf/sinf。
// 表在 bind 時建一次；效果只經 nc_px_engine_*() 純函數用，唔可以直接掂表。
#include "nc_px_engine.h"

#include <math.h>

#define RGB_PI_F 3.14159265358979f

static uint16_t s_gam[1024];              // gamma：(0..1023) linear → 8.8 顯示
// 正弦：**只有一個實作**（混合正弦：256 錨點 Q15＋線性插值，514 B）。
// 舊的 1024 點 Q15 表（s_sin）與 MP 多項式版已經刪除，見 nc_px_engine.h 的說明。
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

// 混合正弦：**錨點表（256 個）＋節點之間線性插值**。
// 表只存正弦本身（Q15），插值係一次式：a + ((b−a)·f >> 8)。
// 量測（tools/hosttest/sin_bench，host x86-64）：2.4× 快過多項式、誤差 0.23 LSB
// （多項式 3.74、1024 點查表 12.38）；表 514 B。詳細數字見 status.md §7.135。
// **這是引擎唯一的正弦實作**（舊的 Q15 查表與多項式版已刪）。
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
