// nc_pixel_effects.h — CORE 內建效果（實作）的登記入口。
//
// 實作住 `components/effects/pixel_effects/`；引擎住 `components/sw/pixel/`。
// 兩邊只經 `nc_px_engine.h` 的契約見面：實作唔 include 引擎內部、唔開 task、唔碰硬件。
//
// 誰呼叫 `nc_pixel_effects_register()`：pixel 任務的 on_start（冪等，重呼叫安全）。
#pragma once

#include "nc_px_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把內建效果（zone1／breath／wave／solid／ramp）交去引擎 registry。
void nc_pixel_effects_register(void);

// 個別效果（測試可以直接拿，唔一定要經 registry）。
extern const nc_px_engine_desc_t nc_effect_zone1;
extern const nc_px_engine_desc_t nc_effect_breath;
extern const nc_px_engine_desc_t nc_effect_wave;
extern const nc_px_engine_desc_t nc_effect_solid;
extern const nc_px_engine_desc_t nc_effect_ramp;

#ifdef __cplusplus
}
#endif
