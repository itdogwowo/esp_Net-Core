#include "nc_hub.h"

#include <stdatomic.h>
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
#include "esp_cpu.h"
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

#define NC_HUB_MAX 32          // 多任務 mesh（6 節點 = 30 條單向鏈）
#define NC_HUB_CORE_NONE 0xFFu

// 只有 host 測試不需要 FreeRTOS／esp_timer；其他平台一律走真實值。
static inline int hub_core_now(void)
{
#ifdef NC_HUB_HOST_TEST
    return 0;
#else
    return (int)esp_cpu_get_core_id();
#endif
}

static struct nc_hub *s_registry[NC_HUB_MAX];

struct nc_hub {
    const char *owner;
    uint8_t *slots;
    // state 用 32-bit atomic：Xtensa 的 S32C1I 只支援 32-bit，
    // 1-byte atomic 會走 libatomic 鎖，成本高一個數量級。
    _Atomic uint32_t *state;
    uint32_t *slot_gen;          // 每格最後一次 publish 的 generation
    size_t slot_size;
    size_t n;
    size_t w;                    // 下一個要寫的位置（writer 專用）
    size_t r;                    // 下一個要讀的位置（reader 專用）
    int last_read;               // reader 目前持有哪一格（-1 = 沒有）
    int busy_write;              // writer 目前正在寫哪一格（-1 = 沒有）
    bool owns_region;            // true = slots 由 nc_hub_create() 配置
    _Atomic uint32_t gen;        // 最後一次 publish 的 generation
    _Atomic uint32_t publishes;  // 成功 publish 次數
    _Atomic uint8_t writer_core; // 第一次 write_begin 的核心（0xFF = 未用）
    _Atomic uint8_t reader_core;
    _Atomic uint32_t core_violations;   // 從非預期核心呼叫的次數
};

static nc_hub_t *hub_init(void *region, size_t region_bytes, const char *owner,
                          size_t slot_size, size_t n, size_t align)
{
    if (region == NULL || slot_size == 0 || n == 0 || n > 256) {
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
    h->state = calloc(n, sizeof(*h->state));
    h->slot_gen = calloc(n, sizeof(*h->slot_gen));
    if (h->state == NULL || h->slot_gen == NULL) {
        free(h->state);
        free(h->slot_gen);
        free(h);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        atomic_init(&h->state[i], NC_HUB_IDLE);
    }
    atomic_init(&h->gen, 0);
    atomic_init(&h->publishes, 0);
    atomic_init(&h->writer_core, NC_HUB_CORE_NONE);
    atomic_init(&h->reader_core, NC_HUB_CORE_NONE);
    atomic_init(&h->core_violations, 0);
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
        const uint8_t wc = atomic_load_explicit(&h->writer_core, memory_order_relaxed);
        const uint8_t rc = atomic_load_explicit(&h->reader_core, memory_order_relaxed);
        off += snprintf(buf + off, cap - off,
                        "%s{\"owner\":\"%s\",\"slot\":%u,\"n\":%u,\"fill\":%u,"
                        "\"gen\":%u,\"wc\":%d,\"rc\":%d,\"viol\":%u}",
                        first ? "" : ",", h->owner ? h->owner : "?",
                        (unsigned)h->slot_size, (unsigned)h->n, (unsigned)nc_hub_fill(h),
                        (unsigned)atomic_load_explicit(&h->gen, memory_order_relaxed),
                        wc == NC_HUB_CORE_NONE ? -1 : (int)wc,
                        rc == NC_HUB_CORE_NONE ? -1 : (int)rc,
                        (unsigned)atomic_load_explicit(&h->core_violations, memory_order_relaxed));
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
    h->owns_region = true;
    HUB_LOG("hub %-10s %u x %u B（%s）", owner, (unsigned)n, (unsigned)slot_size,
            cls == NC_HUB_SRAM ? "SRAM" : "PSRAM");
    return h;
}

// ── 核心歸屬：只記錄第一次，之後換核只計數（SPSC 前提被破壞要看得見）──
static void hub_note_core(nc_hub_t *hub, _Atomic uint8_t *slot, int core)
{
    // 熱路徑只做一次 load；同核（正常情況）直接返回，不再 CAS。
    const uint8_t cur = atomic_load_explicit(slot, memory_order_relaxed);
    if ((int)cur == core) {
        return;
    }
    if (cur == NC_HUB_CORE_NONE) {
        uint8_t expect = NC_HUB_CORE_NONE;
        if (atomic_compare_exchange_strong_explicit(slot, &expect, (uint8_t)core,
                                                    memory_order_relaxed,
                                                    memory_order_relaxed)) {
            return;
        }
        if ((int)expect == core) {
            return;
        }
    }
    atomic_fetch_add_explicit(&hub->core_violations, 1, memory_order_relaxed);
}

void *nc_hub_write_begin(nc_hub_t *hub)
{
    if (hub == NULL || hub->busy_write >= 0) {
        return NULL;   // 已經有一次沒 commit
    }
    hub_note_core(hub, &hub->writer_core, hub_core_now());

    // 嚴格 FIFO：只寫 w。若 w 不是 IDLE（環滿），回 NULL，
    // 不可跳到後面的空槽 —— 那會讓較新的資料越過尚未讀取的舊資料。
    const size_t idx = hub->w;
    uint32_t st = NC_HUB_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&hub->state[idx], &st, NC_HUB_WRITING,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed)) {
        return NULL;
    }
    hub->busy_write = (int)idx;
    return hub->slots + idx * hub->slot_size;
}

esp_err_t nc_hub_write_commit(nc_hub_t *hub)
{
    if (hub == NULL || hub->busy_write < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t idx = (size_t)hub->busy_write;
    const uint32_t next = atomic_load_explicit(&hub->gen, memory_order_relaxed) + 1;

    // generation 先落地，再用 release 發佈 READY；reader acquire 後才讀得到。
    hub->slot_gen[idx] = next;
    atomic_store_explicit(&hub->gen, next, memory_order_relaxed);
    atomic_store_explicit(&hub->state[idx], NC_HUB_READY, memory_order_release);
    atomic_fetch_add_explicit(&hub->publishes, 1, memory_order_relaxed);

    hub->busy_write = -1;
    hub->w = (idx + 1) % hub->n;
    return ESP_OK;
}

static void hub_release_read(nc_hub_t *hub)
{
    const int idx = hub->last_read;
    if (idx < 0) {
        return;
    }
    atomic_store_explicit(&hub->state[idx], NC_HUB_IDLE, memory_order_release);
    hub->last_read = -1;
}

static void *hub_read_begin_gen(nc_hub_t *hub, uint32_t *gen)
{
    if (hub == NULL) {
        return NULL;
    }
    hub_note_core(hub, &hub->reader_core, hub_core_now());

    // 先歸還上一次讀的那格（reader 同時最多持有一格，語義不變）
    hub_release_read(hub);

    // 嚴格 FIFO：只讀 r。r 不是 READY 就回 NULL。
    // 壓力測試證實：只要 reader 跳格（即使 writer 嚴格），N=2 仍會出現亂序，
    // 因為跳過的格子會被 writer 重填並在稍後才被讀到。
    const size_t idx = hub->r;
    uint32_t st = NC_HUB_READY;
    if (!atomic_compare_exchange_strong_explicit(&hub->state[idx], &st, NC_HUB_READING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return NULL;
    }
    hub->last_read = (int)idx;
    hub->r = (idx + 1) % hub->n;
    if (gen != NULL) {
        *gen = hub->slot_gen[idx];
    }
    return hub->slots + idx * hub->slot_size;
}

void *nc_hub_read_begin(nc_hub_t *hub)
{
    return hub_read_begin_gen(hub, NULL);
}

void *nc_hub_read_begin_gen(nc_hub_t *hub, uint32_t *gen)
{
    return hub_read_begin_gen(hub, gen);
}

void nc_hub_read_end(nc_hub_t *hub)
{
    if (hub == NULL) {
        return;
    }
    hub_release_read(hub);
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
        if (atomic_load_explicit(&hub->state[i], memory_order_acquire) == NC_HUB_READY) {
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

void nc_hub_destroy(nc_hub_t *hub)
{
    if (hub == NULL) {
        return;
    }
    for (int i = 0; i < NC_HUB_MAX; i++) {
        if (s_registry[i] == hub) {
            s_registry[i] = NULL;
        }
    }
    if (hub->owns_region) {
        free(hub->slots);
    }
    free(hub->state);
    free(hub->slot_gen);
    free(hub);
}

uint32_t nc_hub_generation(const nc_hub_t *hub)
{
    return hub ? atomic_load_explicit(&hub->gen, memory_order_relaxed) : 0;
}

void nc_hub_stats(const nc_hub_t *hub, nc_hub_stats_t *out)
{
    if (hub == NULL || out == NULL) {
        return;
    }
    const uint8_t wc = atomic_load_explicit(&hub->writer_core, memory_order_relaxed);
    const uint8_t rc = atomic_load_explicit(&hub->reader_core, memory_order_relaxed);
    out->generation = atomic_load_explicit(&hub->gen, memory_order_relaxed);
    out->fill = nc_hub_fill(hub);
    out->writer_core = (wc == NC_HUB_CORE_NONE) ? -1 : (int)wc;
    out->reader_core = (rc == NC_HUB_CORE_NONE) ? -1 : (int)rc;
    out->core_violations = atomic_load_explicit(&hub->core_violations, memory_order_relaxed);
    out->publishes = atomic_load_explicit(&hub->publishes, memory_order_relaxed);
}
