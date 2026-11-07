#include "boot.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "boot";

#define BOOT_MAX_PHASES 16
static uint32_t g_us[BOOT_MAX_PHASES];   // 每個階段用咗幾多 **µs**（摘要行印成 ms）

int nc_boot_run(const nc_boot_phase_t *phases, size_t n)
{
    if (phases == NULL || n == 0) {
        return -1;
    }
    int first_fail = -1;
    for (size_t i = 0; i < n; i++) {
        if (phases[i].fn == NULL) {
            continue;
        }
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = phases[i].fn();
        if (i < BOOT_MAX_PHASES) {
            g_us[i] = (uint32_t)(esp_timer_get_time() - t0);
        }
        const int64_t dt = esp_timer_get_time() - t0;
        const unsigned pct = (unsigned)((dt * 100) / 1000);   // 0.1% 精度
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "boot[%u/%u] %-14s %3u.%u ms  ok", (unsigned)(i + 1),
                     (unsigned)n, phases[i].name, pct / 100, pct % 100);
        } else {
            ESP_LOGE(TAG, "boot[%u/%u] %-14s %3u.%u ms  FAIL %s", (unsigned)(i + 1),
                     (unsigned)n, phases[i].name, pct / 100, pct % 100,
                     esp_err_to_name(err));
            if (first_fail < 0) {
                first_fail = (int)i;
            }
        }
    }
    // 逐階段那幾行有機會喺 log 管道開始之前就寫落 ring（2 KB）而被擠走，
    // 所以最後再印一行**摘要**：一行睇完邊個階段幾慢。
    char line[220];
    size_t off = 0;
    off += (size_t)snprintf(line + off, sizeof(line) - off, "boot 摘要：");
    for (size_t i = 0; i < n && off < sizeof(line) - 24; i++) {
        off += (size_t)snprintf(line + off, sizeof(line) - off, "%s%u.%02u",
                                (i == 0) ? "" : " | ",
                                (unsigned)(g_us[i] / 1000), (unsigned)((g_us[i] % 1000) / 10));
    }
    ESP_LOGI(TAG, "%s ms（%u 個階段%s）", line, (unsigned)n,
             (first_fail < 0) ? "" : "，有失敗");
    ESP_LOGI(TAG, "boot 完成：%u 個階段%s", (unsigned)n,
             (first_fail < 0) ? "" : "（有失敗，見上面）");
    return first_fail;
}
