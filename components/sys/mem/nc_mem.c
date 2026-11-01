#include "nc_mem.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mem";

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

// ───────────────────────── POOL ─────────────────────────

struct nc_mem_pool {
    const char *name;
    uint8_t *base;
    bool owns;
    size_t block_size;
    size_t count;
    size_t used;
    size_t peak;
    size_t fails;
    void *free_list;          // 侵入式：用區塊本身存 next
    portMUX_TYPE mux;
};

nc_mem_pool_t *nc_mem_pool_create(const char *name, nc_mem_class_t cls,
                                  size_t block_size, size_t count)
{
    if (count == 0) {
        return NULL;
    }
    if (block_size < sizeof(void *)) {
        block_size = sizeof(void *);
    }
    block_size = (block_size + 3) & ~(size_t)3;   // 4-byte 對齊

    nc_mem_pool_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }

    p->base = heap_caps_aligned_alloc(16, block_size * count, caps_for(cls));
    if (p->base == NULL) {
        ESP_LOGE(TAG, "pool %s: 要不到 %u x %u B", name,
                 (unsigned)count, (unsigned)block_size);
        free(p);
        return NULL;
    }
    p->owns = true;

    p->name = name;
    p->block_size = block_size;
    p->count = count;
    p->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    // 串成 free list
    for (size_t i = 0; i < count; i++) {
        void *blk = p->base + i * block_size;
        *(void **)blk = p->free_list;
        p->free_list = blk;
    }

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "pool %-8s %u x %u B（%u KB），內部剩 %u KB",
             name, (unsigned)count, (unsigned)block_size,
             (unsigned)(block_size * count / 1024), (unsigned)(free_heap / 1024));
    return p;
}

nc_mem_pool_t *nc_mem_pool_init(void *region, size_t region_bytes, const char *name,
                                nc_mem_class_t cls, size_t block_size, size_t count)
{
    (void)cls;
    if (region == NULL || count == 0 || block_size < sizeof(void *)) {
        return NULL;
    }
    if (block_size * count > region_bytes) {
        ESP_LOGE(TAG, "pool %s: 區域太小（要 %u，給 %u）", name,
                 (unsigned)(block_size * count), (unsigned)region_bytes);
        return NULL;
    }
    nc_mem_pool_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->base = region;
    p->owns = false;
    p->name = name;
    p->block_size = block_size;
    p->count = count;
    p->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    for (size_t i = 0; i < count; i++) {
        void *blk = p->base + i * block_size;
        *(void **)blk = p->free_list;
        p->free_list = blk;
    }
    ESP_LOGI(TAG, "pool %-8s %u x %u B（外部區域）", name, (unsigned)count, (unsigned)block_size);
    return p;
}

void *nc_mem_pool_get(nc_mem_pool_t *pool, uint32_t timeout_ms)
{
    if (pool == NULL) {
        return NULL;
    }
    const TickType_t step = pdMS_TO_TICKS(1);
    uint32_t waited = 0;

    for (;;) {
        void *blk = NULL;
        taskENTER_CRITICAL(&pool->mux);
        if (pool->free_list != NULL) {
            blk = pool->free_list;
            pool->free_list = *(void **)blk;
            pool->used++;
            if (pool->used > pool->peak) {
                pool->peak = pool->used;
            }
        }
        taskEXIT_CRITICAL(&pool->mux);

        if (blk != NULL) {
            return blk;
        }

        pool->fails++;
        if (waited >= timeout_ms) {
            return NULL;
        }
        vTaskDelay(step);
        waited++;
    }
}

void nc_mem_pool_put(nc_mem_pool_t *pool, void *block)
{
    if (pool == NULL || block == NULL) {
        return;
    }
    if ((uint8_t *)block < pool->base ||
        (uint8_t *)block >= pool->base + pool->block_size * pool->count) {
        ESP_LOGE(TAG, "pool %s: put 了不屬於它的指標", pool->name);
        return;
    }

    taskENTER_CRITICAL(&pool->mux);
    *(void **)block = pool->free_list;
    pool->free_list = block;
    if (pool->used > 0) {
        pool->used--;
    }
    taskEXIT_CRITICAL(&pool->mux);
}

void nc_mem_pool_stats(const nc_mem_pool_t *pool, nc_mem_pool_stats_t *out)
{
    if (pool == NULL || out == NULL) {
        return;
    }
    out->block_size = pool->block_size;
    out->count = pool->count;
    out->used = pool->used;
    out->peak = pool->peak;
    out->fails = pool->fails;
}

// ───────────────────────── ARENA ─────────────────────────

struct nc_mem_arena {
    const char *name;
    uint8_t *base;
    bool owns;
    size_t size;
    size_t used;
};

nc_mem_arena_t *nc_mem_arena_create(const char *name, nc_mem_class_t cls, size_t size)
{
    if (size == 0) {
        return NULL;
    }
    nc_mem_arena_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }
    a->owns = true;
    a->base = heap_caps_aligned_alloc(16, size, caps_for(cls));
    if (a->base == NULL) {
        ESP_LOGW(TAG, "arena %s: 要不到 %u KB（class=%d）", name,
                 (unsigned)(size / 1024), (int)cls);
        free(a);
        return NULL;
    }
    a->name = name;
    a->size = size;
    ESP_LOGI(TAG, "arena %-8s %u KB", name, (unsigned)(size / 1024));
    return a;
}

nc_mem_arena_t *nc_mem_arena_init(void *region, size_t region_bytes, const char *name,
                                  nc_mem_class_t cls)
{
    (void)cls;
    if (region == NULL || region_bytes == 0) {
        return NULL;
    }
    nc_mem_arena_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }
    a->base = region;
    a->owns = false;
    a->name = name;
    a->size = region_bytes;
    ESP_LOGI(TAG, "arena %-8s %u KB（外部區域）", name, (unsigned)(region_bytes / 1024));
    return a;
}

void *nc_mem_arena_alloc(nc_mem_arena_t *arena, size_t size, size_t align)
{
    if (arena == NULL || size == 0) {
        return NULL;
    }
    if (align == 0) {
        align = 4;
    }

    size_t off = (arena->used + align - 1) & ~(align - 1);
    if (off + size > arena->size) {
        ESP_LOGW(TAG, "arena %s: 滿了（要 %u，剩 %u）", arena->name,
                 (unsigned)size, (unsigned)(arena->size - arena->used));
        return NULL;
    }
    arena->used = off + size;
    return arena->base + off;
}

void nc_mem_arena_reset(nc_mem_arena_t *arena)
{
    if (arena != NULL) {
        arena->used = 0;
    }
}

size_t nc_mem_arena_used(const nc_mem_arena_t *arena)
{
    return arena ? arena->used : 0;
}

size_t nc_mem_arena_size(const nc_mem_arena_t *arena)
{
    return arena ? arena->size : 0;
}
