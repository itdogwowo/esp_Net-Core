// nc_px_engine_test.h — pxEngine_test 與「效果引擎」之間的唯一接縫。
//
// 為什麼要這一支：引擎 API／型別改名時，只改這裡，測試本文、案例、golden 全部不動，
// 「重構前後逐 byte 一樣」才是用同一份測試證明的，不是兩份互相對照的程式。
//
// 命名：`pxEngine_test` 係測試名（tools/hosttest/），`nc_px_engine_*` 係引擎的公開名字；
// 兩者對得住 `sw/pixel/include/nc_px_engine.h`。`fx` 已退役（status.md §7.130）。
#pragma once

#include "nc_px_engine.h"          // 引擎契約＋API（sw/pixel）
#include "nc_pixel_effects.h"      // 內建效果登記（components/effects/pixel_effects）

typedef nc_px_engine_cfg_t nc_px_test_cfg_t;

#define NC_PX_TEST_ZONES       NC_PX_CANVAS_ZONES

#define nc_px_test_register()          nc_pixel_effects_register()
#define nc_px_test_defaults()          nc_px_engine_defaults()
#define nc_px_test_cfg()               nc_px_engine_cfg()
#define nc_px_test_set_cfg(c)          nc_px_engine_set_cfg(c)
#define nc_px_test_bind(px)            nc_px_engine_bind(px)
#define nc_px_test_reset()             nc_px_engine_reset()
#define nc_px_test_frame(dst, hdr, t)  nc_px_engine_frame((dst), (hdr), (t))
