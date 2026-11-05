// hello：最小框架自測任務 —— 每秒印一次，證明任務管理器 + tasks.json 這條路通。
//
// 這個檔案是「一個任務一個 component」的樣板：只放宣告與 loop 本體，
// 不碰 xTaskCreate、不碰 registry（那是 task_manager 的事）。
#include "nc_hello.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hello";

static void hello_loop(void *arg)
{
    (void)arg;
    static uint32_t n;
    n++;
    ESP_LOGI(TAG, "Hello World #%u（core %d）", (unsigned)n, (int)xPortGetCoreID());
}

static const nc_task_loop_t s_hello_loops[] = {
    { .name = "hello", .fn = hello_loop, .arg = NULL,
      .stack = 3072, .prio = 1, .core = 0, .layer = 6, .period_ms = 1000 },
};

static const nc_task_t s_task_hello = {
    .name = "hello", .kind = NC_TASK_NORMAL, .level = 6,
    .loops = s_hello_loops,
    .nloops = sizeof(s_hello_loops) / sizeof(s_hello_loops[0]),
};

const nc_task_t *nc_hello_task(void)
{
    return &s_task_hello;
}
