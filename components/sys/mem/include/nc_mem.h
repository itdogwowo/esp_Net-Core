// 記憶體管理：兩個原語，對應兩種壽命
//
//   POOL   固定大小區塊，alloc/free 成對。  → 幀緩衝、DMA 緩衝、封包暫存（hot path）
//   ARENA  一次配一堆，全部一起放。          → assets / config / 解析結果（reload 才換）
//
// 為什麼不直接用 malloc：
//   1. 長時間跑（燈效 50 FPS）時 malloc/free 會把 heap 切碎，某天就配不出連續塊。
//   2. 熱路徑的配置時間不確定（PSRAM 更明顯），會直接變成掉幀。
//   3. 出問題時看不出「誰吃掉了記憶體」——pool 有名稱與統計。
//
// class 決定記憶體從哪裡來（由硬體限制決定，不是偏好）：
//   NC_MEM_DMA      內部 RAM，DMA 可用。RMT / SPI / I2S 的緩衝必須用這個。
//   NC_MEM_INTERNAL 內部 RAM，一般用途。快，但只有幾百 KB。
//   NC_MEM_PSRAM    外部 PSRAM。大，慢，部分周邊不能 DMA。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_MEM_DMA = 0,
    NC_MEM_INTERNAL,
    NC_MEM_PSRAM,
} nc_mem_class_t;

// ───────────────────────── POOL ─────────────────────────

typedef struct nc_mem_pool nc_mem_pool_t;

// count 個 block_size 的區塊，一次向系統要。block_size 至少要是指標大小。
nc_mem_pool_t *nc_mem_pool_create(const char *name, nc_mem_class_t cls,
                                  size_t block_size, size_t count);

// 取一塊；timeout_ms 內拿不到就回 NULL（0 = 不等）
void *nc_mem_pool_get(nc_mem_pool_t *pool, uint32_t timeout_ms);

// 還回去。可在 ISR 呼叫（只做臨界區操作，不阻塞）。
void nc_mem_pool_put(nc_mem_pool_t *pool, void *block);

typedef struct {
    size_t block_size;
    size_t count;
    size_t used;
    size_t peak;
    size_t fails;      // 取不到的次數（開發期的關鍵指標）
} nc_mem_pool_stats_t;

void nc_mem_pool_stats(const nc_mem_pool_t *pool, nc_mem_pool_stats_t *out);

// ───────────────────────── ARENA ─────────────────────────

typedef struct nc_mem_arena nc_mem_arena_t;

// 建立時就把整塊要下來，之後不再向系統要記憶體
nc_mem_arena_t *nc_mem_arena_create(const char *name, nc_mem_class_t cls, size_t size);

// bump 配置；align = 0 → 4 bytes。放不下回 NULL（不會分割、不會回收單塊）
void *nc_mem_arena_alloc(nc_mem_arena_t *arena, size_t size, size_t align);

// 一次釋放全部（例如 ASSET_RELOAD：新的一份驗證通過才 reset 舊的）
void nc_mem_arena_reset(nc_mem_arena_t *arena);

size_t nc_mem_arena_used(const nc_mem_arena_t *arena);
size_t nc_mem_arena_size(const nc_mem_arena_t *arena);

#ifdef __cplusplus
}
#endif
