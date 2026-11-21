// nc_effect_task.c — 三任務管線的第一段：只生產波形值。
//
// 唔讀 /data、唔碰 map／write／ring；plan 由 pair 經 nc_pipe_plan_snapshot() 提供。
// 每幀把 NC_PX_JOB_MAX 個 job 的 value 寫入 fixed frame，pair 再配對。
#include "sdkconfig.h"

#if CONFIG_NETCORE_CONTENT

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "nc_pipeline.h"
#include "nc_prio.h"
#include "nc_task.h"

static const char *TAG = "effect";

static nc_pipe_effect_frame_t s_frames[2];
static volatile uint8_t s_pub;
static volatile uint32_t s_seq;
static SemaphoreHandle_t s_mtx;

static nc_px_plan_t s_local_plan;
static nc_px_asset_t s_local_assets[NC_PX_JOB_MAX];
static uint32_t s_local_gen;
static uint32_t s_step;

bool nc_pipe_effect_copy(nc_pipe_effect_frame_t *out)
{
    if (out == NULL || s_mtx == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_mtx, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool ok = (s_seq != 0);
    if (ok) {
        *out = s_frames[s_pub];
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

static void effect_loop(void *arg)
{
    (void)arg;
    const uint32_t fps = (CONFIG_NETCORE_PIXEL_FPS > 0) ? CONFIG_NETCORE_PIXEL_FPS : 50;
    const TickType_t period = pdMS_TO_TICKS(1000u / fps);
    TickType_t last = xTaskGetTickCount();

    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
    }
    while (!nc_task_exit_requested()) {
        uint32_t gen = 0;
        if (s_mtx == NULL ||
            !nc_pipe_plan_snapshot(&s_local_plan, s_local_assets,
                                   NC_PX_JOB_MAX, &gen)) {
            nc_task_beat();
            vTaskDelay(pdMS_TO_TICKS(10));
            last = xTaskGetTickCount();
            continue;
        }
        if (gen != s_local_gen) {
            s_local_gen = gen;
            s_step = 0;
            ESP_LOGI(TAG, "plan snapshot gen=%u jobs=%u", (unsigned)gen,
                     (unsigned)s_local_plan.njobs);
        }

        const uint8_t slot = s_pub ^ 1u;
        nc_pipe_effect_frame_t *f = &s_frames[slot];
        const int64_t t0 = esp_timer_get_time();
        f->plan_gen = s_local_gen;
        f->njobs = s_local_plan.njobs;
        for (uint8_t j = 0; j < s_local_plan.njobs; j++) {
            f->nv[j] = nc_px_job_vals(&s_local_plan.job[j], s_step,
                                      f->vals[j], NC_PIPE_VAL_MAX);
        }
        const int64_t t1 = esp_timer_get_time();
        nc_pipe_stats_effect((uint32_t)(t1 - t0));
        s_step++;
        f->t_pub_us = (uint32_t)t1;
        f->seq = ++s_seq;

        if (xSemaphoreTake(s_mtx, portMAX_DELAY) == pdTRUE) {
            s_pub = slot;
            xSemaphoreGive(s_mtx);
        }
        nc_task_beat();
        if (nc_pipe_stats_burn_active()) {
            taskYIELD();                  // 極限：唔等 50 fps，effect 全速跑
        } else {
            vTaskDelayUntil(&last, period);
        }
    }
}

static const nc_task_loop_t s_loops[] = {
    {
        .name = "effect",
        .fn = effect_loop,
        .stack = 4096,
        .prio = NC_PRIO_OTHERS_MAX - 1,
        .core = 0,
        .layer = 5,
        .period_ms = 0,
    },
};

static const nc_task_t s_task = {
    .name = "effect",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 22,
    .on_start = NULL,
    .on_stop = NULL,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(effect, &s_task);

#endif  // CONFIG_NETCORE_CONTENT
