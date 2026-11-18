// 測試用 nc_task.h shim：核心（環／編碼器）與 mock 後端都不需要任務管理器，
// 但 firmware 的 nc_pixel.h 會 include 真正的 nc_task.h（為了 nc_task_t）。
#pragma once
typedef struct nc_task {
    const char *name;
    int         kind;
    int         level;
} nc_task_t;
