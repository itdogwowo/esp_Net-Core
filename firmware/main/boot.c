// 開機執行器：驗依賴 → 呼叫 → 計時 → 記錄 → 摘要。
//
// 這一層刻意保持「零業務知識」：它唔知 config 怎麼讀、USB 怎麼起，
// 只知每個階段自己宣告的 needs／gives，以及 ctx 由階段填、由階段讀。
// 所以加一個階段 = 在 main.c 的表加一行；改順序 = 移動一行（契約會自己喊）。
#include "boot.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "boot";

// 這次開機的決定 + 帳。單例：階段與讀者都經 nc_boot_ctx() 拿，唔會有第二份。
static nc_boot_ctx_t s_ctx;
static nc_boot_result_t s_res[NC_BOOT_MAX_STAGES];
static size_t s_res_n;

nc_boot_ctx_t *nc_boot_ctx(void)
{
    return &s_ctx;
}

const char *nc_usb_mode_name(nc_usb_mode_t mode)
{
    switch (mode) {
    case NC_USB_OTG:  return "otg";
    case NC_USB_JTAG: return "jtag";
    default:          return "none";
    }
}

// ─────────────── 依賴位元 ↔ 名字（log 要講得出「缺什麼」）───────────────
static const struct {
    uint16_t bit;
    const char *name;
} k_deps[] = {
    { NC_CAP_NVS,      "nvs"      },
    { NC_CAP_STORAGE,  "storage"  },
    { NC_CAP_CONFIG,   "config"   },
    { NC_CAP_CORES,    "cores"    },
    { NC_CAP_USB,      "usb"      },
    { NC_CAP_ROUTER,   "router"   },
    { NC_CAP_DECLARED, "declared" },
    { NC_CAP_PLAN,     "plan"     },
    { NC_CAP_TASKS,    "tasks"    },
    { NC_CAP_CHANNELS, "channels" },
};
#define K_DEPS (sizeof(k_deps) / sizeof(k_deps[0]))

const char *nc_cap_name(uint16_t bit)
{
    for (size_t i = 0; i < K_DEPS; i++) {
        if (k_deps[i].bit == bit) {
            return k_deps[i].name;
        }
    }
    return "?";
}

// "config|usb"：缺依賴時要一眼看得出缺哪幾樣（唔好只印一個 0x0014）。
static void deps_str(uint16_t bits, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < K_DEPS && off + 1 < cap; i++) {
        if ((bits & k_deps[i].bit) == 0) {
            continue;
        }
        const int w = snprintf(out + off, cap - off, "%s%s",
                               (off > 0) ? "|" : "", k_deps[i].name);
        if (w < 0) {
            break;
        }
        off += ((size_t)w < cap - off) ? (size_t)w : (cap - off - 1);
    }
}

// 追加式格式化：滿咗就停，唔會越界（摘要那行是固定 220 B）。
static void appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    if (buf == NULL || off == NULL || *off + 1 >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return;
    }
    *off += ((size_t)w < cap - *off) ? (size_t)w : (cap - *off - 1);
}

// ───────────────────────── run ─────────────────────────

int nc_boot_run(const nc_boot_stage_t *stages, size_t n)
{
    if (stages == NULL || n == 0) {
        return -1;
    }
    s_res_n = (n > NC_BOOT_MAX_STAGES) ? NC_BOOT_MAX_STAGES : n;
    memset(s_res, 0, sizeof(s_res));
    s_ctx.ready = 0;              // 每次 run 都係新的一輪（目前只會跑一次）

    int first_fail = -1;
    int n_skip = 0;
    int n_fail = 0;
    int n_warn = 0;

    for (size_t i = 0; i < n; i++) {
        const nc_boot_stage_t *s = &stages[i];
        nc_boot_result_t *r = (i < s_res_n) ? &s_res[i] : NULL;
        if (r != NULL) {
            r->id = (s->id != NULL) ? s->id : "B??";
            r->name = s->name;
            r->state = NC_BOOT_UNRUN;
            r->err = ESP_OK;
        }

        // ① 驗依賴：缺就唔跑，並寫明缺什麼。
        //    以前係「照跑，然後下游各自壞」——症狀散在十幾個地方。
        const uint16_t missing = (uint16_t)(s->needs & (uint16_t)~s_ctx.ready);
        if (missing != 0) {
            char want[64];
            deps_str(missing, want, sizeof(want));
            ESP_LOGW(TAG, "boot[%s] %-15s SKIP（缺 %s）",
                     (s->id != NULL) ? s->id : "B??", s->name, want);
            if (r != NULL) {
                r->state = NC_BOOT_SKIP;
                r->err = ESP_ERR_INVALID_STATE;
            }
            n_skip++;
            continue;
        }

        // ② 跑（NULL 當成成功：預留位，方便先把表排好再填實作）
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = (s->fn != NULL) ? s->fn() : ESP_OK;
        const int64_t dt = esp_timer_get_time() - t0;
        const unsigned tenth = (unsigned)(dt / 10);        // 0.1 ms 為單位
        if (r != NULL) {
            r->us = (uint32_t)dt;
            r->err = err;
        }

        if (err == ESP_OK) {
            s_ctx.ready |= s->gives;                       // ③ 記帳：後面的人可以驗
            if (r != NULL) {
                r->state = NC_BOOT_OK;
            }
            ESP_LOGI(TAG, "boot[%s] %-15s %3u.%02u ms  ok",
                     (s->id != NULL) ? s->id : "B??", s->name,
                     tenth / 100, tenth % 100);
        } else if (s->optional) {
            if (r != NULL) {
                r->state = NC_BOOT_WARN;
            }
            n_warn++;
            ESP_LOGW(TAG, "boot[%s] %-15s %3u.%02u ms  WARN %s（可選，唔阻開機）",
                     (s->id != NULL) ? s->id : "B??", s->name,
                     tenth / 100, tenth % 100, esp_err_to_name(err));
        } else {
            if (r != NULL) {
                r->state = NC_BOOT_FAIL;
            }
            n_fail++;
            if (first_fail < 0) {
                first_fail = (int)i;
            }
            // 唔停：後面的階段靠 needs 自己決定要唔要跑（缺 gives 的自然 SKIP）。
            ESP_LOGE(TAG, "boot[%s] %-15s %3u.%02u ms  FAIL %s",
                     (s->id != NULL) ? s->id : "B??", s->name,
                     tenth / 100, tenth % 100, esp_err_to_name(err));
        }
    }

    // ④ 摘要：逐階段那幾行有機會喺 log 管道開始之前就寫落 ring（2 KB）而被擠走，
    //    所以最後再印一行 —— 一行睇完邊個階段幾重、幾個冇跑。
    char line[220];
    size_t off = (size_t)snprintf(line, sizeof(line), "boot 摘要：");
    for (size_t i = 0; i < s_res_n; i++) {
        appendf(line, sizeof(line), &off, "%s%s %u.%02u", (i == 0) ? "" : " | ",
                s_res[i].id, (unsigned)(s_res[i].us / 1000),
                (unsigned)((s_res[i].us % 1000) / 10));
    }
    ESP_LOGI(TAG, "%s ms（%u 階：%d 失敗、%d 跳過、%d 可選失敗）",
             line, (unsigned)n, n_fail, n_skip, n_warn);
    ESP_LOGI(TAG, "boot 完成：已具備 0x%04x%s", (unsigned)s_ctx.ready,
             (first_fail < 0) ? "" : "；有階段失敗（見上面）");
    return first_fail;
}

const nc_boot_result_t *nc_boot_results(size_t *n)
{
    if (n != NULL) {
        *n = s_res_n;
    }
    return s_res;
}
