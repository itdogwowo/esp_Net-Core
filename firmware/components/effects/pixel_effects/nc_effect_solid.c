// nc_effect_solid.c — 單色：r/g/b 係「顯示值」0..255，frac 再補 1/256 級（唔經 gamma）。
#include "nc_px_engine.h"

static uint16_t solid_tgt(const nc_px_engine_params_t *p, uint8_t c)
{
    if (c == 0) {
        return 0;
    }
    const uint32_t v = ((uint32_t)c * 256u * p->gain) / 255u;
    return (uint16_t)(v + p->frac);
}

static void render_solid(void *st, const nc_px_engine_in_t *in, uint16_t *dst)
{
    (void)st;
    const nc_px_engine_params_t *p = in->params;
    const uint16_t r = solid_tgt(p, p->r);
    const uint16_t g = solid_tgt(p, p->g);
    const uint16_t b = solid_tgt(p, p->b);
    for (uint32_t i = 0; i < in->px; i++) {
        dst[i * 3 + 0] = r;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = b;
    }
}

const nc_px_engine_desc_t nc_effect_solid = {
    .name = "solid",
    .id = NC_PX_EFF_SOLID,
    .render = render_solid,
};
