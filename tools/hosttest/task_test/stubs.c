// 四個子系統（plan / loop runtime / nc4 / log）的假實作。
// 目的不是模擬行為，而是讓 nc_task.c 在 host 上真的跑起來：
// 驗順序、驗帳本、驗 attach/detach。
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nc4.h"
#include "nc_plan.h"
#include "nc_task_priv.h"

#define STUB_MAX 64

// ── log ──
static void vlog(const char *lvl, const char *tag, const char *fmt, va_list ap)
{
    printf("%s[%s] ", lvl, tag ? tag : "?");
    vprintf(fmt, ap);
    printf("\n");
}

void mod_host_log_e(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("E", tag, fmt, ap);
    va_end(ap);
}

void mod_host_log_w(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("W", tag, fmt, ap);
    va_end(ap);
}

void mod_host_log_i(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("I", tag, fmt, ap);
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_ERR_?";
    }
}

// ── plan ──
static struct {
    char name[32];
    size_t bytes;
    nc_plan_class_t cls;
    int level;
} s_plan[STUB_MAX];
static int s_plan_n;

esp_err_t nc_plan_declare(const nc_plan_decl_t *decl)
{
    if (decl == NULL || decl->name == NULL || decl->bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_plan_n; i++) {
        if (strcmp(s_plan[i].name, decl->name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_plan_n >= STUB_MAX) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_plan[s_plan_n].name, sizeof(s_plan[s_plan_n].name), "%s", decl->name);
    s_plan[s_plan_n].bytes = decl->bytes;
    s_plan[s_plan_n].cls = decl->cls;
    s_plan[s_plan_n].level = decl->level;
    s_plan_n++;
    return ESP_OK;
}

void *nc_plan_get(const char *name)
{
    for (int i = 0; i < s_plan_n; i++) {
        if (strcmp(s_plan[i].name, name) == 0) {
            static char arena[1024];
            return arena + (i % 8) * 64;
        }
    }
    return NULL;
}

int mod_stub_plan_count(void) { return s_plan_n; }
const char *mod_stub_plan_name(int i) { return (i >= 0 && i < s_plan_n) ? s_plan[i].name : NULL; }
void mod_stub_plan_dump(void)
{
    for (int i = 0; i < s_plan_n; i++) {
        printf("    plan[%d] %s %u B cls=%d\n", i, s_plan[i].name,
               (unsigned)s_plan[i].bytes, (int)s_plan[i].cls);
    }
}

// ── loop runtime（真的 FreeRTOS task 在 host 上不存在，這裡只記狀態）──
#define LOOP_MAX (NC_TASK_MAX * NC_TASK_LOOPS_MAX)
static nc_task_rt_t *s_running[LOOP_MAX];

esp_err_t nc_task_rt_start(nc_task_rt_t *rt)
{
    if (rt == NULL || rt->def == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rt->started = true;
    rt->exited = false;
    rt->exit_req = false;
    for (int i = 0; i < LOOP_MAX; i++) {
        if (s_running[i] == NULL) {
            s_running[i] = rt;
            break;
        }
    }
    return ESP_OK;
}

esp_err_t nc_task_rt_stop(nc_task_rt_t *rt, uint32_t wait_ms)
{
    (void)wait_ms;
    if (rt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rt->started = false;
    rt->exited = true;
    for (int i = 0; i < LOOP_MAX; i++) {
        if (s_running[i] == rt) {
            s_running[i] = NULL;
            break;
        }
    }
    return ESP_OK;
}

bool nc_task_rt_exit_requested(void)
{
    return false;
}

bool mod_stub_loop_running(const char *name)
{
    for (int i = 0; i < LOOP_MAX; i++) {
        if (s_running[i] != NULL && s_running[i]->def != NULL &&
            s_running[i]->def->name != NULL &&
            strcmp(s_running[i]->def->name, name) == 0) {
            return true;
        }
    }
    return false;
}

// nc_task_reset() 之後 slot 會被清空，s_running 裡的指標就懸空了 —— 測試換 case
// 時要先叫這個，否則下一次查詢會踩到已清空的 def。
void mod_stub_loop_reset(void)
{
    for (int i = 0; i < LOOP_MAX; i++) {
        s_running[i] = NULL;
    }
}

// ── nc4 decoder ──
struct nc4_decoder {
    struct {
        uint16_t cmd;
        const char *owner;
    } t[NC4_MAX_HANDLERS];
    size_t n;
};

esp_err_t nc4_decoder_on_owner(nc4_decoder_t *decoder, uint16_t cmd,
                               nc4_handler_t handler, const char *owner)
{
    (void)handler;
    if (decoder == NULL || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < decoder->n; i++) {
        if (decoder->t[i].cmd == cmd) {
            decoder->t[i].owner = owner;
            return ESP_OK;
        }
    }
    if (decoder->n >= NC4_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    decoder->t[decoder->n].cmd = cmd;
    decoder->t[decoder->n].owner = owner;
    decoder->n++;
    return ESP_OK;
}

int nc4_decoder_off_owner(nc4_decoder_t *decoder, const char *owner)
{
    if (decoder == NULL || owner == NULL) {
        return 0;
    }
    int removed = 0;
    for (size_t i = 0; i < decoder->n;) {
        if (decoder->t[i].owner != NULL && strcmp(decoder->t[i].owner, owner) == 0) {
            memmove(&decoder->t[i], &decoder->t[i + 1],
                    (decoder->n - i - 1) * sizeof(decoder->t[0]));
            decoder->n--;
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

bool nc4_decoder_has(const nc4_decoder_t *decoder, uint16_t cmd)
{
    if (decoder == NULL) {
        return false;
    }
    for (size_t i = 0; i < decoder->n; i++) {
        if (decoder->t[i].cmd == cmd) {
            return true;
        }
    }
    return false;
}

nc4_decoder_t *nc4_decoder_create(void)
{
    static struct nc4_decoder d;
    memset(&d, 0, sizeof(d));
    return &d;
}

const char *mod_stub_decoder_owner(const nc4_decoder_t *decoder, uint16_t cmd)
{
    for (size_t i = 0; i < decoder->n; i++) {
        if (decoder->t[i].cmd == cmd) {
            return decoder->t[i].owner;
        }
    }
    return NULL;
}

size_t mod_stub_decoder_count(const nc4_decoder_t *decoder)
{
    return decoder ? decoder->n : 0;
}
