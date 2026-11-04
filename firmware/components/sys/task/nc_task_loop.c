// loop 的執行層：建立 FreeRTOS task、跑使用者迴圈、量測、停止。
// 這是「任務管理器代管」的實際所在 —— 使用者只寫 fn(arg)，不碰 xTaskCreate。
#include "nc_task_priv.h"

#include <string.h>

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nc_epoch.h"

static const char *TAG = "task";

// 讓 nc_task_exit_requested() 能用「目前 task 的 handle」反查。
static nc_task_rt_t *volatile s_active[NC_TASK_MAX * NC_TASK_LOOPS_MAX];

static void active_put(nc_task_rt_t *rt)
{
    for (size_t i = 0; i < sizeof(s_active) / sizeof(s_active[0]); i++) {
        if (s_active[i] == NULL) {
            s_active[i] = rt;
            return;
        }
    }
}

static void active_del(nc_task_rt_t *rt)
{
    for (size_t i = 0; i < sizeof(s_active) / sizeof(s_active[0]); i++) {
        if (s_active[i] == rt) {
            s_active[i] = NULL;
            return;
        }
    }
}

static nc_task_rt_t *active_by_handle(TaskHandle_t h)
{
    for (size_t i = 0; i < sizeof(s_active) / sizeof(s_active[0]); i++) {
        nc_task_rt_t *rt = s_active[i];
        if (rt != NULL && rt->handle == h) {
            return rt;
        }
    }
    return NULL;
}

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

// 每一輪：量時間 → 記帳 → 依 period 延遲 → 看有沒有被要求結束。
// 使用者要停止就在自己的 fn 裡呼叫 nc_task_exit_requested() 後返回。
static void trampoline(void *arg)
{
    nc_task_rt_t *rt = (nc_task_rt_t *)arg;
    const nc_task_loop_t *d = rt->def;

    while (!rt->exit_req) {
        const int64_t iter0 = now_us();     // 這一輪的起點（含管理器自己的成本）
        if (rt->enabled && d->fn != NULL) {
            const int64_t t0 = now_us();
            d->fn(d->arg);
            const int64_t t1 = now_us();
            const uint32_t dt = (uint32_t)(t1 - t0);

            rt->count++;
            rt->last_us = dt;
            rt->total_us += dt;
            if (dt > rt->max_us) {
                rt->max_us = dt;
            }
            if (d->budget_us != 0 && dt > d->budget_us) {
                rt->over++;
            }
            // 管理器成本 = 整輪 - fn。內含兩次 esp_timer_get_time()，
            // 所以這是「含量測」的上界；要看純 overhead 就用同一個數字做相對比較。
            const uint32_t wrap = (uint32_t)(now_us() - iter0) - dt;
            rt->last_wrap_us = wrap;
            rt->total_wrap_us += wrap;

            // 每核大循環：同一次 timestamp 餵埋量測，避免多一次 esp_timer。
            if (rt->epoch_token >= 0) {
                nc_epoch_mark(rt->core, rt->epoch_token, t1,
                              (uint32_t)esp_cpu_get_cycle_count(), dt);
            }
        } else if (rt->epoch_token >= 0) {
            // 停用的 loop 都要參與（exec=0），否則同核其他 loop 的大循環永遠等它。
            nc_epoch_mark(rt->core, rt->epoch_token, now_us(),
                          (uint32_t)esp_cpu_get_cycle_count(), 0);
        }
        if (d->run_once) {
            break;
        }
        if (d->period_ms != 0) {
            vTaskDelay(pdMS_TO_TICKS(d->period_ms));
        } else if (!rt->enabled) {
            vTaskDelay(pdMS_TO_TICKS(10));   // 停用又沒有節奏 → 不要空轉
        }
    }

    if (rt->epoch_token >= 0) {
        nc_epoch_remove(rt->core, rt->epoch_token);
        rt->epoch_token = -1;
    }
    active_del(rt);
    rt->started = false;
    rt->exited = true;
    rt->handle = NULL;
    ESP_LOGI(TAG, "loop [%s] 結束（%u 輪）", d->name ? d->name : "?",
             (unsigned)rt->count);
    vTaskDelete(NULL);
}

esp_err_t nc_task_rt_start(nc_task_rt_t *rt)
{
    if (rt == NULL || rt->def == NULL || rt->def->name == NULL || rt->def->fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rt->started) {
        return ESP_OK;
    }

    rt->exit_req = false;
    rt->exited = false;
    active_put(rt);

    BaseType_t ok;
    if (rt->core == NC_TASK_CORE_ANY) {
        ok = xTaskCreate(trampoline, rt->def->name, rt->def->stack, rt,
                         rt->def->prio, &rt->handle);
    } else {
        ok = xTaskCreatePinnedToCore(trampoline, rt->def->name, rt->def->stack, rt,
                                     rt->def->prio, &rt->handle, rt->core);
    }
    if (ok != pdPASS) {
        active_del(rt);
        rt->handle = NULL;
        ESP_LOGE(TAG, "loop [%s] 建立失敗（stack=%u）", rt->def->name,
                 (unsigned)rt->def->stack);
        return ESP_ERR_NO_MEM;
    }
    rt->started = true;
    rt->core = (int)xTaskGetCoreID(rt->handle);   // CORE_ANY 喺呢度定案
    rt->epoch_token = nc_epoch_add(rt->core);
    ESP_LOGI(TAG, "loop [%s] L%d → core %d", rt->def->name, rt->def->layer,
             rt->core);
    return ESP_OK;
}

esp_err_t nc_task_rt_stop(nc_task_rt_t *rt, uint32_t wait_ms)
{
    if (rt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rt->started) {
        return ESP_OK;
    }
    rt->exit_req = true;

    uint32_t waited = 0;
    while (rt->started && waited < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (rt->started) {
        ESP_LOGW(TAG, "loop [%s] 沒有在 %u ms 內結束（沒有呼叫 nc_task_exit_requested？）",
                 rt->def->name ? rt->def->name : "?", (unsigned)wait_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool nc_task_rt_exit_requested(void)
{
    nc_task_rt_t *rt = active_by_handle(xTaskGetCurrentTaskHandle());
    return rt != NULL && rt->exit_req;
}

uint32_t nc_task_rt_avg_us(const nc_task_rt_t *rt)
{
    if (rt == NULL || rt->count == 0) {
        return 0;
    }
    return (uint32_t)(rt->total_us / rt->count);
}

uint32_t nc_task_rt_wrap_us(const nc_task_rt_t *rt)
{
    return rt ? rt->last_wrap_us : 0;
}

uint32_t nc_task_rt_wrap_avg_us(const nc_task_rt_t *rt)
{
    if (rt == NULL || rt->count == 0) {
        return 0;
    }
    return (uint32_t)(rt->total_wrap_us / rt->count);
}
