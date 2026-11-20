// nc_effect_zone1.c — 第一區（第 0 橫行）由暗到亮的 8 級；其餘區全黑。
//
// 用途：對照絲印／接線次序，確認全部顆數都食到新資料（有漏顆／沾色即刻見到）。
// 呢個係**實作**，住 components/effects/；引擎住 sw/pixel，只經 nc_px_engine.h 見面。
#include "nc_px_engine.h"

static void render_zone1(void *st, const nc_px_engine_in_t *in, uint16_t *dst)
{
    (void)st;
    if (in->zone != 0) {
        return;                         // 引擎已經清黑，唔使再寫
    }
    const nc_px_engine_params_t *p = in->params;
    for (uint32_t x = 0; x < in->px; x++) {
        const uint16_t lin = nc_px_engine_lin(p, 80u + x * 21u);   // 顯示值約 20..200
        dst[x * 3 + 0] = nc_px_engine_gamma(lin);
        dst[x * 3 + 1] = nc_px_engine_gamma((uint16_t)(lin / 3u));
        dst[x * 3 + 2] = nc_px_engine_gamma((uint16_t)(lin / 6u));
    }
}

const nc_px_engine_desc_t nc_effect_zone1 = {
    .name = "zone1",
    .id = NC_PX_EFF_ZONE1,
    .render = render_zone1,
};
