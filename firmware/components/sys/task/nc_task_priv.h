// 任務管理器的內部結構（不对外）。只有 nc_task.c 與 nc_task_loop.c 會 include。
#pragma once

#include "nc_task.h"

// 一個 loop 的執行期狀態。由管理器持有。
typedef struct {
    const nc_task_loop_t *def;
    TaskHandle_t handle;
    int core;                 // 執行期核心（CORE_ANY 在 start 時定案；沒有 set_core）
    bool started;
    bool exited;
    bool enabled;
    volatile bool exit_req;
} nc_task_rt_t;

// 建立 / 停止 FreeRTOS task。arg 是 loop 的 arg；exit 用 nc_task_exit_requested()。
esp_err_t nc_task_rt_start(nc_task_rt_t *rt);
esp_err_t nc_task_rt_stop(nc_task_rt_t *rt, uint32_t wait_ms);
bool nc_task_rt_exit_requested(void);
