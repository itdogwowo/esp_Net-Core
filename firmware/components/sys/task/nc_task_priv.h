// 任務管理器的內部結構（不对外）。只有 nc_task.c 與 nc_task_loop.c 會 include。
#pragma once

#include "nc_task.h"

// 一個 loop 的執行期狀態。由管理器持有，trampoline 更新量測欄位。
typedef struct {
    const nc_task_loop_t *def;
    TaskHandle_t handle;
    int core;                 // 執行期核心（CORE_ANY 在 start 時定案；沒有 set_core）
    bool started;
    bool exited;
    bool enabled;
    volatile bool exit_req;

    // 量測（只有自己的 loop 會寫；status 讀到半新半舊最多是數字不準，不會越界）
    uint32_t count;           // 跑了幾輪
    uint32_t last_us;         // 上一輪「使用者 fn」花多久
    uint32_t max_us;          // 最久的一輪（fn）
    uint64_t total_us;        // 累計（fn）
    uint32_t over;            // 超預算次數

    // 管理器自己的成本：從進迴圈到記完帳（扣掉 fn）。
    // 這是「被登記成任務」與「自己寫一個 for(;;) 直接跑」的差距。
    uint32_t last_wrap_us;
    uint64_t total_wrap_us;

    // 每核「大循環」量測的 token（nc_epoch）。-1 = 未登記。
    int epoch_token;
} nc_task_rt_t;

// 建立 / 停止 FreeRTOS task。arg 是 loop 的 arg；exit 用 nc_task_exit_requested()。
esp_err_t nc_task_rt_start(nc_task_rt_t *rt);
esp_err_t nc_task_rt_stop(nc_task_rt_t *rt, uint32_t wait_ms);
bool nc_task_rt_exit_requested(void);
uint32_t nc_task_rt_avg_us(const nc_task_rt_t *rt);
uint32_t nc_task_rt_wrap_us(const nc_task_rt_t *rt);      // 上一輪的管理器成本
uint32_t nc_task_rt_wrap_avg_us(const nc_task_rt_t *rt);  // 平均
