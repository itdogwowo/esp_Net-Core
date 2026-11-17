// WAMR 執行引擎：ncm 的 backend 實作（外部任務／效果模組真的「跑」起來那一層）。
//
// 用法（main.c 的 ncm task on_start）：
//     nc_mod_init(arena, len);
//     nc_mod_set_backend(nc_mod_wamr_backend());
// 之後 nc_mod_load() 就會把模組實例化；nc_mod_tick() 每 tick 叫一次
// task_tick()（任務）或 fx_render()（效果）。
#pragma once

#include "nc_mod.h"

#ifdef __cplusplus
extern "C" {
#endif

const nc_mod_backend_t *nc_mod_wamr_backend(void);

#ifdef __cplusplus
}
#endif
