// Hub：N 個等長 slot + 每格狀態 + 讀寫位置
//
// C 版的 AtomicStreamHub。單寫入者 / 單讀取者（SPSC），無鎖、無等待。
//
// 狀態機（每格）：
//   IDLE ──write_begin──► WRITING ──commit──► READY ──read_begin──► READING ──read_end──► IDLE
//
// 三種深度的語義（N 是參數，不是分支）：
//   N = 1  一次性轉交：reader 取得後長期持有，writer 不再寫入。
//          適合「開機算好一份，之後只讀」。要循環使用請用 N >= 2。
//   N = 2  嚴格輪替：writer 寫 A，reader 讀 A 並歸還，writer 才能寫 A。
//   N >=3  吸收抖動：writer 永遠有空位，reader 慢慢讀也不影響。
//
// 本模組永遠不等待、不丟棄任何已 commit 的資料：
//   · write_begin 沒有 IDLE 的格子 → 回 NULL（呼叫端自己跳過這一輪）
//   · read_begin  沒有 READY 的格子 → 回 NULL（呼叫端自己跳過這一輪）
//
// 誰負責什麼：
//   管理器：格子數量、狀態、指標、owner 記錄
//   用家：要不要寫、要不要讀、跳過幾次（記在自己的 status）

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_HUB_IDLE = 0,
    NC_HUB_WRITING,
    NC_HUB_READY,
    NC_HUB_READING,
} nc_hub_state_t;

typedef enum {
    NC_HUB_SRAM = 0,
    NC_HUB_PSRAM,
} nc_hub_class_t;

typedef struct nc_hub nc_hub_t;

// 建立 hub：owner 只是標記（報表用），slot 一次配置好。
// align 0 → 32（cache line / DMA）。
nc_hub_t *nc_hub_create(const char *owner, size_t slot_size, size_t n,
                        nc_hub_class_t cls, size_t align);

// 在「外部已配好的區域」上建立（plan 給的記憶體走這條）
nc_hub_t *nc_hub_create_in(void *region, size_t region_bytes, const char *owner,
                           size_t slot_size, size_t n, size_t align);

// ── writer ──
void *nc_hub_write_begin(nc_hub_t *hub);          // 沒有空位 → NULL
esp_err_t nc_hub_write_commit(nc_hub_t *hub);     // begin 之後呼叫一次

// ── reader ──
void *nc_hub_read_begin(nc_hub_t *hub);           // 沒有 READY → NULL
void nc_hub_read_end(nc_hub_t *hub);              // 歸還目前持有的那格

// ── 查詢 ──
bool nc_hub_dirty(const nc_hub_t *hub);           // 有 READY（有新東西）
size_t nc_hub_fill(const nc_hub_t *hub);          // READY 的格數
size_t nc_hub_depth(const nc_hub_t *hub);         // N
size_t nc_hub_slot_size(const nc_hub_t *hub);
const char *nc_hub_owner(const nc_hub_t *hub);

// 所有已建立 hub 的報表（JSON 陣列）
size_t nc_hub_registry_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
