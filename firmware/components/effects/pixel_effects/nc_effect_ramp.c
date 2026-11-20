// nc_effect_ramp.c — 全部顆數由 0 到 252（linear）一條線：抖動開／關一眼看得出分別。
#include "nc_px_engine.h"

static void render_ramp(void *st, const nc_px_engine_in_t *in, uint16_t *dst)
{
    (void)st;
    const nc_px_engine_params_t *p = in->params;
    for (uint32_t i = 0; i < in->px; i++) {
        const uint32_t g = (uint32_t)in->zone * in->px + i;     // 全鏈位置（跨區連續）
        const uint16_t v = nc_px_engine_gamma(nc_px_engine_lin(p, g * 4u));
        dst[i * 3 + 0] = v;
        dst[i * 3 + 1] = v;
        dst[i * 3 + 2] = v;
    }
}

const nc_px_engine_desc_t nc_effect_ramp = {
    .name = "ramp",
    .id = NC_PX_EFF_RAMP,
    .render = render_ramp,
};
