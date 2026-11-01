#include "nc_hub.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef NC_HUB_HOST_TEST
#define HUB_LOG(...) ((void)0)
static void *hub_alloc(size_t bytes, size_t align, nc_hub_class_t cls)
{
    (void)cls;
    void *p = NULL;
    if (posix_memalign(&p, align, bytes) != 0) {
        return NULL;
    }
    return p;
}
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "hub";
#define HUB_LOG(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)

static void *hub_alloc(size_t bytes, size_t align, nc_hub_class_t cls)
{
    uint32_t caps = MALLOC_CAP_8BIT;
    if (cls == NC_HUB_SRAM) {
        caps |= MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    } else {
        caps |= MALLOC_CAP_SPIRAM;
    }
    return heap_caps_aligned_alloc(align, bytes, caps);
}
#endif

#define NC_HUB_MAX 8
static struct nc_hub *s_registry[NC_HUB_MAX];

struct nc_hub {
    const char *owner;
    uint8_t *slots;
    nc_hub_state_t *state;
    size_t slot_size;
    size_t n;
    size_t w;          // 下一個要寫的位置
    size_t r;          // 下一個要讀的位置
    int last_read;     // reader 目前持有哪一格（-1 = 沒有）
    int busy_write;    // writer 目前正在寫哪一格（-1 = 沒有）
};

static nc_hub_t *hub_init(void *region, size_t region_bytes, const char *owner,
                          size_t slot_size, size_t n, size_t align)
{
    if (region == NULL || slot_size == 0 || n == 0 || n > 64) {
        return NULL;
    }
    if (slot_size * n > region_bytes) {
        return NULL;
    }
    if (align == 0) {
        align = 32;
    }
    (void)align;

    nc_hub_t *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    h->state = calloc(n, sizeof(nc_hub_state_t));
    if (h->state == NULL) {
        free(h);
        return NULL;
    }
    h->owner = owner;
    h->slots = region;
    h->slot_size = slot_size;
    h->n = n;
    h->last_read = -1;
    h->busy_write = -1;
    for (int i = 0; i < NC_HUB_MAX; i++) {
        if (s_registry[i] == NULL) {
            s_registry[i] = h;
            break;
        }
    }
    return h;
}

size_t nc_hub_registry_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += snprintf(buf + off, cap - off, "[");
    bool first = true;
    for (int i = 0; i < NC_HUB_MAX && off < cap; i++) {
        const nc_hub_t *h = s_registry[i];
        if (h == NULL) {
            continue;
        }
        off += snprintf(buf + off, cap - off,
                        "%s{\"owner\":\"%s\",\"slot\":%u,\"n\":%u,\"fill\":%u}",
                        first ? "" : ",", h->owner ? h->owner : "?",
                        (unsigned)h->slot_size, (unsigned)h->n, (unsigned)nc_hub_fill(h));
        first = false;
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]");
    }
    return off;
}

nc_hub_t *nc_hub_create_in(void *region, size_t region_bytes, const char *owner,
                           size_t slot_size, size_t n, size_t align)
{
    return hub_init(region, region_bytes, owner, slot_size, n, align);
}

nc_hub_t *nc_hub_create(const char *owner, size_t slot_size, size_t n,
                        nc_hub_class_t cls, size_t align)
{
    if (align == 0) {
        align = 32;
    }
    const size_t bytes = slot_size * n;
    void *region = hub_alloc(bytes, align, cls);
    if (region == NULL) {
        HUB_LOG("hub %s: 要不到 %u B", owner, (unsigned)bytes);
        return NULL;
    }
    nc_hub_t *h = hub_init(region, bytes, owner, slot_size, n, align);
    if (h == NULL) {
        free(region);
        return NULL;
    }
    HUB_LOG("hub %-10s %u x %u B（%s）", owner, (unsigned)n, (unsigned)slot_size,
            cls == NC_HUB_SRAM ? "SRAM" : "PSRAM");
    return h;
}

void *nc_hub_write_begin(nc_hub_t *hub)
{
    if (hub == NULL || hub->busy_write >= 0) {
        return NULL;   // 已經有一次沒 commit
    }
    // 找一格 IDLE（指標順序前進，讓 slot 輪流使用）
    for (size_t i = 0; i < hub->n; i++) {
        size_t idx = (hub->w + i) % hub->n;
        if (hub->state[idx] == NC_HUB_IDLE) {
            hub->state[idx] = NC_HUB_WRITING;
            hub->busy_write = (int)idx;
            return hub->slots + idx * hub->slot_size;
        }
    }
    return NULL;       // 沒有空位：呼叫端自己跳過這一輪
}

esp_err_t nc_hub_write_commit(nc_hub_t *hub)
{
    if (hub == NULL || hub->busy_write < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t idx = (size_t)hub->busy_write;
    hub->state[idx] = NC_HUB_READY;
    hub->busy_write = -1;
    hub->w = (idx + 1) % hub->n;
    return ESP_OK;
}

void *nc_hub_read_begin(nc_hub_t *hub)
{
    if (hub == NULL) {
        return NULL;
    }
    // 先歸還上一次讀的那格（reader 同時最多持有一格）
    if (hub->last_read >= 0) {
        hub->state[hub->last_read] = NC_HUB_IDLE;
        hub->last_read = -1;
    }
    for (size_t i = 0; i < hub->n; i++) {
        size_t idx = (hub->r + i) % hub->n;
        if (hub->state[idx] == NC_HUB_READY) {
            hub->state[idx] = NC_HUB_READING;
            hub->last_read = (int)idx;
            hub->r = (idx + 1) % hub->n;
            return hub->slots + idx * hub->slot_size;
        }
    }
    return NULL;
}

void nc_hub_read_end(nc_hub_t *hub)
{
    if (hub == NULL || hub->last_read < 0) {
        return;
    }
    hub->state[hub->last_read] = NC_HUB_IDLE;
    hub->last_read = -1;
}

bool nc_hub_dirty(const nc_hub_t *hub)
{
    return hub != NULL && nc_hub_fill(hub) > 0;
}

size_t nc_hub_fill(const nc_hub_t *hub)
{
    if (hub == NULL) {
        return 0;
    }
    size_t cnt = 0;
    for (size_t i = 0; i < hub->n; i++) {
        if (hub->state[i] == NC_HUB_READY) {
            cnt++;
        }
    }
    return cnt;
}

size_t nc_hub_depth(const nc_hub_t *hub)
{
    return hub ? hub->n : 0;
}

size_t nc_hub_slot_size(const nc_hub_t *hub)
{
    return hub ? hub->slot_size : 0;
}

const char *nc_hub_owner(const nc_hub_t *hub)
{
    return hub ? hub->owner : NULL;
}
