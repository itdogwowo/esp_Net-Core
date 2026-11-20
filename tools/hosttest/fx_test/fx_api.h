// fx_api.h — fx_test 與「效果引擎」之間的唯一接縫。
//
// M2 重構時只改這一支：測試本文、案例、golden 全部不變，
// 所以「逐 byte 一樣」係用同一份測試證明的，不是兩份互相對照的程式。
#pragma once

#include "nc_px_engine.h"          // 引擎契約＋API（sw/pixel）
#include "nc_pixel_effects.h"      // 內建效果登記（components/effects/pixel_effects）

typedef nc_px_engine_cfg_t fx_cfg_t;

#define FX_ZONES               NC_PX_CANVAS_ZONES

#define fx_register()          nc_pixel_effects_register()
#define fx_defaults()          nc_px_engine_defaults()
#define fx_cfg()               nc_px_engine_cfg()
#define fx_set_cfg(c)          nc_px_engine_set_cfg(c)
#define fx_bind(px)            nc_px_engine_bind(px)
#define fx_reset()             nc_px_engine_reset()
#define fx_frame(dst, hdr, t)  nc_px_engine_frame((dst), (hdr), (t))
