// nc_effect_table.c — 內建效果登記表：把實作交去引擎 registry。
//
// 一個域一個 component：`components/effects/pixel_effects/`。
// 加一個 C 效果 = 加一個 `nc_effect_<name>.c` ＋ 這張表加一行（引擎一行都唔使改）。
#include "nc_pixel_effects.h"

static bool s_done;

void nc_pixel_effects_register(void)
{
    if (s_done) {
        return;                         // 冪等：任務重啟／多次呼叫都安全
    }
    s_done = true;
    nc_px_engine_register(&nc_effect_zone1);
    nc_px_engine_register(&nc_effect_breath);
    nc_px_engine_register(&nc_effect_wave);
    nc_px_engine_register(&nc_effect_solid);
    nc_px_engine_register(&nc_effect_ramp);
}
