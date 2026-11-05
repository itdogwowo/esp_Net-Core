// probe：開機自測（hub 交換 4 格）。測試用，受 NC_ENABLE_TESTS gate。
#include "nc_probe.h"

#include <string.h>

#include "esp_log.h"
#include "nc_hub.h"
#include "nc_plan.h"

static const char *TAG = "probe";
static nc_hub_t *s_probe_hub;

// ── probe：開機自測（hub 交換 4 格）──
static int task_probe_declare(nc_task_res_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_task_res_t){
        .name = "probe.hub", .bytes = 4 * 128, .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
}

static esp_err_t task_probe_start(const nc_task_res_view_t *res, int n)
{
    (void)n;
    s_probe_hub = nc_hub_create_in(res[0].ptr, res[0].len, "probe", 128, 4, 32);
    if (s_probe_hub == NULL) {
        return ESP_FAIL;
    }

    char *w = nc_hub_write_begin(s_probe_hub);
    bool ok = (w != NULL);
    if (ok) {
        memset(w, 0x5A, 128);
        ok = (nc_hub_write_commit(s_probe_hub) == ESP_OK);
        char *r = nc_hub_read_begin(s_probe_hub);
        ok = ok && (r == w) && (r[0] == 0x5A) && (r[127] == 0x5A);
        nc_hub_read_end(s_probe_hub);
    }
    ESP_LOGI(TAG, "svc probe: hub 自測 %s", ok ? "OK" : "FAIL");
    return ok ? ESP_OK : ESP_FAIL;
}

// ⑤ probe（Normal L3）：開機自測（hub 交換 4 格）
static const nc_task_t s_task_probe = {
    .name = "probe", .kind = NC_TASK_NORMAL, .level = 3,
    .declare = task_probe_declare, .on_start = task_probe_start,
};


const nc_task_t *nc_test_probe_task(void)
{
    return &s_task_probe;
}
