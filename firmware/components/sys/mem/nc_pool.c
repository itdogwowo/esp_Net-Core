#include "nc_pool.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pool";

typedef struct {
    nc_pool_class_cfg_t cfg;
    uint8_t *base;
    size_t   used;
    size_t   peak;
    size_t   fails;
    nc_pool_slot_t *free_list;
} pool_class_t;

static pool_class_t s_cls[NC_POOL_CLASS_MAX];
static nc_pool_slot_t s_slots[NC_POOL_MAX_SLOTS];
static size_t s_slots_used;
static bool s_inited;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t caps_for(nc_mem_class_t cls)
{
    switch (cls) {
    case NC_MEM_DMA:
        return MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    case NC_MEM_PSRAM:
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    case NC_MEM_INTERNAL:
    default:
        return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
}

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

void nc_pool_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_cls, 0, sizeof(s_cls));
    memset(s_slots, 0, sizeof(s_slots));
    s_slots_used = 0;
    s_inited = false;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t nc_pool_init(const nc_pool_class_cfg_t *cfg, int n)
{
    if (cfg == NULL || n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_cls, 0, sizeof(s_cls));
    memset(s_slots, 0, sizeof(s_slots));
    s_slots_used = 0;

    int ok = 0;
    for (int i = 0; i < n && i < NC_POOL_CLASS_MAX; i++) {
        if (!cfg[i].enable || cfg[i].size == 0 || cfg[i].count == 0) {
            continue;
        }
        const size_t size = align_up(cfg[i].size, 64);
        const size_t count = cfg[i].count;
        if (s_slots_used + count > NC_POOL_MAX_SLOTS) {
            ESP_LOGE(TAG, "class %s：descriptor 不夠（要 %u，剩 %u）",
                     cfg[i].name, (unsigned)count,
                     (unsigned)(NC_POOL_MAX_SLOTS - s_slots_used));
            continue;
        }
        uint8_t *base = heap_caps_aligned_alloc(64, size * count, caps_for(cfg[i].where));
        if (base == NULL) {
            ESP_LOGE(TAG, "class %s：要不到 %u × %u B（%s）", cfg[i].name,
                     (unsigned)count, (unsigned)size,
                     cfg[i].where == NC_MEM_PSRAM ? "PSRAM" : "internal");
            continue;
        }
        pool_class_t *c = &s_cls[i];
        c->cfg = cfg[i];
        c->cfg.size = (uint16_t)size;
        c->base = base;
        c->free_list = NULL;
        for (size_t k = 0; k < count; k++) {
            nc_pool_slot_t *s = &s_slots[s_slots_used++];
            memset(s, 0, sizeof(*s));
            s->data = base + k * size;
            s->cap = (uint16_t)size;
            s->cls = (uint8_t)i;
            s->state = NC_POOL_ST_FREE;
            s->dst_link = 0xFF;
            s->next = c->free_list;
            c->free_list = s;
        }
        ok++;
        ESP_LOGI(TAG, "class %-7s %u × %u B（%s，合計 %u KB）", c->cfg.name,
                 (unsigned)count, (unsigned)size,
                 cfg[i].where == NC_MEM_PSRAM ? "PSRAM" : "internal",
                 (unsigned)(size * count / 1024));
    }
    s_inited = (ok > 0);
    ESP_LOGI(TAG, "pool：%d 個 class、%u 格 descriptor（%u B）", ok,
             (unsigned)s_slots_used, (unsigned)sizeof(s_slots));
    return s_inited ? ESP_OK : ESP_ERR_NO_MEM;
}

nc_pool_slot_t *nc_pool_alloc(nc_pool_class_t cls, uint8_t src_link)
{
    if (cls >= NC_POOL_CLASS_MAX) {
        return NULL;
    }
    pool_class_t *c = &s_cls[cls];
    nc_pool_slot_t *s = NULL;

    portENTER_CRITICAL(&s_mux);
    s = c->free_list;
    if (s != NULL) {
        c->free_list = s->next;
        s->next = NULL;
        s->state = NC_POOL_ST_WRITING;
        s->len = 0;
        s->src_link = src_link;
        s->dst_link = 0xFF;
        s->refcnt = 1;          // 呼叫者是第一個 owner
        s->gen++;
        s->ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
        c->used++;
        if (c->used > c->peak) {
            c->peak = c->used;
        }
    } else {
        c->fails++;
    }
    portEXIT_CRITICAL(&s_mux);
    return s;
}

void nc_pool_commit(nc_pool_slot_t *slot, uint16_t len)
{
    if (slot == NULL || slot->state != NC_POOL_ST_WRITING) {
        return;
    }
    if (len > slot->cap) {
        len = slot->cap;
    }
    portENTER_CRITICAL(&s_mux);
    slot->len = len;
    slot->state = NC_POOL_ST_READY;
    portEXIT_CRITICAL(&s_mux);
}

void nc_pool_retain(nc_pool_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (slot->refcnt < 0xFFFF) {
        slot->refcnt++;
    }
    portEXIT_CRITICAL(&s_mux);
}

void nc_pool_release(nc_pool_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (slot->refcnt > 0) {
        slot->refcnt--;
    }
    if (slot->refcnt == 0 && slot->state != NC_POOL_ST_FREE) {
        pool_class_t *c = &s_cls[slot->cls];
        slot->state = NC_POOL_ST_FREE;
        slot->len = 0;
        slot->dst_link = 0xFF;
        if (c->used > 0) {
            c->used--;
        }
        slot->next = c->free_list;
        c->free_list = slot;
    }
    portEXIT_CRITICAL(&s_mux);
}

void nc_pool_stats(int cls, uint32_t *count, uint32_t *used, uint32_t *peak, uint32_t *fails)
{
    if (cls < 0 || cls >= NC_POOL_CLASS_MAX) {
        return;
    }
    const pool_class_t *c = &s_cls[cls];
    if (count != NULL) {
        *count = c->cfg.enable ? c->cfg.count : 0;
    }
    if (used != NULL) {
        *used = (uint32_t)c->used;
    }
    if (peak != NULL) {
        *peak = (uint32_t)c->peak;
    }
    if (fails != NULL) {
        *fails = (uint32_t)c->fails;
    }
}

// 追加式格式化（永遠 NUL 結尾；滿了就停）
static size_t jappend(char *buf, size_t cap, size_t len, const char *fmt, ...)
{
    if (len + 1 >= cap) {
        return len;
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return len;
    }
    const size_t n = (size_t)w;
    return (n >= cap - len) ? (cap - 1) : (len + n);
}

size_t nc_pool_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t len = 0;
    len = jappend(buf, cap, len, "{\"init\":%d,\"classes\":[", s_inited ? 1 : 0);
    bool first = true;
    for (int i = 0; i < NC_POOL_CLASS_MAX; i++) {
        const pool_class_t *c = &s_cls[i];
        if (!c->cfg.enable) {
            continue;
        }
        len = jappend(buf, cap, len,
                      "%s{\"name\":\"%s\",\"size\":%u,\"count\":%u,"
                      "\"used\":%u,\"peak\":%u,\"fails\":%u}",
                      first ? "" : ",", c->cfg.name, (unsigned)c->cfg.size,
                      (unsigned)c->cfg.count, (unsigned)c->used,
                      (unsigned)c->peak, (unsigned)c->fails);
        first = false;
    }
    len = jappend(buf, cap, len, "]}");
    return len;
}
