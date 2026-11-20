// nc_effect_wave.c — 純色波浪呼吸（整數）：
//   · 顏色：R → G → B 純色輪流（每 breath_ms 一段），唔用彩虹色相；
//   · 亮度：全局呼吸包絡（0.25..1.0）＋ 斜向空間波浪。
#include "nc_px_engine.h"

static void render_wave(void *st, const nc_px_engine_in_t *in, uint16_t *dst)
{
    (void)st;
    const nc_px_engine_params_t *p = in->params;
    const int32_t wms = (p->wave_ms >= 200) ? (int32_t)p->wave_ms : 3000;
    const int32_t t_ms = (int32_t)(in->now_us / 1000);
    const int32_t spat = (int32_t)(((int64_t)t_ms * 1024) / wms);
    const uint32_t seg_us = (uint32_t)p->breath_ms * 1000u;
    const int seg = seg_us ? (int)(((uint64_t)in->now_us / seg_us) % 3u) : 0;
    const uint32_t ph = seg_us ? (uint32_t)(((uint64_t)(in->now_us % seg_us) * 512u) / seg_us) : 0;
    const int32_t env = 8192 + (nc_px_engine_sin(ph & 1023u) >> 2);   // 0.25 .. 1.0
    for (uint32_t x = 0; x < in->w; x++) {
        const uint32_t wph = (uint32_t)((int32_t)(x * 128u) + (int32_t)in->zone * 96 - spat) & 1023u;
        int32_t w = 16384 + (nc_px_engine_sin(wph) >> 1);              // 0..32767
        w = (w * w) >> 15;                                             // 暗部佔多數
        w = (int32_t)(((uint32_t)w * (uint32_t)env) >> 15);            // × 呼吸包絡
        const uint16_t lin = (uint16_t)(((uint32_t)w * 2u * p->gain) / 255u);
        const uint16_t v = nc_px_engine_gamma(lin);
        for (int c = 0; c < 3; c++) {
            dst[x * 3 + c] = (c == seg) ? v : 0;                       // 純色：只亮一個通道
        }
    }
}

const nc_px_engine_desc_t nc_effect_wave = {
    .name = "wave",
    .id = NC_PX_EFF_WAVE,
    .render = render_wave,
};
