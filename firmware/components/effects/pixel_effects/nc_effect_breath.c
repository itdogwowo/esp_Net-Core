// nc_effect_breath.c — RGB 輪流呼吸（全板同步）：每 breath_ms 一段，R → G → B。
#include "nc_px_engine.h"

static void render_breath(void *st, const nc_px_engine_in_t *in, uint16_t *dst)
{
    (void)st;
    const nc_px_engine_params_t *p = in->params;
    uint32_t seg_us = (uint32_t)p->breath_ms * 1000u;
    if (seg_us == 0) {
        seg_us = 1u;                        // 防 0：未設過參數都唔會除零
    }
    const uint32_t t = (uint32_t)((uint64_t)in->now_us % ((uint64_t)seg_us * 3u));
    const int seg = (int)(t / seg_us);
    // 8.8 相位：一段 = 0..511（半個 sine 週期）；64-bit 因為 12 s 一段 × 512 會爆 32-bit
    const uint32_t ph = (uint32_t)(((uint64_t)(t % seg_us) * 512u) / seg_us);
    // 引擎只有一個正弦（sin_lin，0..4095）。這裡要 Q15 有號值 → 自己做單位換算。
    const int32_t s = ((int32_t)nc_px_engine_sin_lin((ph & 1023u) << 6) - 2048) << 4;
    const uint16_t lin = (uint16_t)(((uint32_t)s * 2u * p->gain) / 255u);
    const uint16_t v = nc_px_engine_gamma(lin);
    for (uint32_t i = 0; i < in->px; i++) {
        dst[i * 3 + 0] = 0;
        dst[i * 3 + 1] = 0;
        dst[i * 3 + 2] = 0;
        dst[i * 3 + seg] = v;
    }
}

const nc_px_engine_desc_t nc_effect_breath = {
    .name = "breath",
    .id = NC_PX_EFF_BREATH,
    .render = render_breath,
};
