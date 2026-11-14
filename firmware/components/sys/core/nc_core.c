#include "nc_core.h"

#include "esp_ipc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "core";

// 0 = 未探測。開機只會探測一次，之後全部是唯讀查表。
static int s_cores;

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
#if CONFIG_FREERTOS_UNICORE
    // 單核（C3／C6／H2）：沒有「另一個核」，也沒有 esp_ipc。
    // 上面那個分支一定會命中（core 只可能是 0），這裡只是把 esp_ipc 擋在編譯期外。
    (void)core;
    return fn(arg);
#else
    core_job_t j = { .fn = fn, .arg = arg, .ret = ESP_FAIL };
    const esp_err_t e = esp_ipc_call_blocking((uint32_t)core, core_job, &j);
    return e == ESP_OK ? j.ret : e;     // 只喺開機 init 用
#endif
}

// ───────────────────────── 核數 ─────────────────────────

int nc_core_probe(void)
{
    if (s_cores > 0) {
        return s_cores;
    }
    int n = (int)CONFIG_FREERTOS_NUMBER_OF_CORES;
    if (n < 1) {
        n = 1;                          // 理論上唔會發生；當單核最保守
    }
    if (n > NC_CORES_MAX) {
        n = NC_CORES_MAX;               // 將來的 3 核也先夾住：框架只有 decode0/1
    }
    s_cores = n;
    return s_cores;
}

int nc_core_count(void)
{
    return (s_cores > 0) ? s_cores : nc_core_probe();
}

bool nc_core_has(int core)
{
    return core >= 0 && core < nc_core_count();
}

// 核心組落點（設計理念見 docs/03_notes/system-model.md §4）：
// 所有「要求某個核」的地方都經過這裡。
// 核唔存在 → 該核的整組（解碼器＋任務）喺仍然存在嘅核建立（現時 = core0）。
// 同一個非法核只警告一次 —— 開機時同一個 config 錯誤可能被問好幾次，
// 每次都印會把真正的第一因淹沒。
int nc_core_clamp(int core)
{
    static uint8_t s_warned;            // bit0 = core0、bit1 = core1 …（boot only）

    if (core == NC_CORE_ANY) {
        return core;                    // 唔指定：唔關核數事
    }
    if (nc_core_has(core)) {
        return core;
    }
    if (core >= 0 && core < 8 && (s_warned & (1u << core)) == 0) {
        s_warned |= (uint8_t)(1u << core);
        ESP_LOGW(TAG, "要求 core %d，但本機只有 %d 核 → 收斂到 core 0"
                      "（單核板上 system.json／tasks.json 寫了 core1）",
                 core, nc_core_count());
    }
    return 0;
}
