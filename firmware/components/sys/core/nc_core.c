#include "nc_core.h"

#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    esp_err_t (*fn)(void *);
    void *arg;
    esp_err_t ret;
} core_job_t;

static void core_job(void *arg)
{
    core_job_t *j = (core_job_t *)arg;
    j->ret = j->fn(j->arg);
}

esp_err_t nc_core_run_on(int core, esp_err_t (*fn)(void *), void *arg)
{
    if (fn == NULL || core < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (core == (int)xPortGetCoreID()) {
        return fn(arg);                 // 已經喺正確的核：直接跑，零成本
    }
    core_job_t j = { .fn = fn, .arg = arg, .ret = ESP_FAIL };
    const esp_err_t e = esp_ipc_call_blocking((uint32_t)core, core_job, &j);
    return e == ESP_OK ? j.ret : e;     // 只喺開機 init 用
}
