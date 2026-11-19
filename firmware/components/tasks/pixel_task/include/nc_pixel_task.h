// nc_pixel_task.h — 效果／Map 生產者任務（pixel）。
//
// 這是「處理效果／Map」的任務：按自己的節奏算一格 R,G,B,W，寫進 renderer 建立的環。
// 渲染（送燈／送 LCD）係另一個任務（tasks/render_task），兩者只透過環見面。
#pragma once

#include "nc_task.h"

#ifdef __cplusplus
extern "C" {
#endif

// main.c 在 CONFIG_NETCORE_PIXEL 之下登記；未開回 NULL。
const nc_task_t *nc_pixel_task(void);

#ifdef __cplusplus
}
#endif
