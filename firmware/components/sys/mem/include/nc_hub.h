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
//   · write_begin 只寫 w；w 不是 IDLE → 回 NULL（環滿，呼叫端自己跳過這一輪）
//     writer 不可跳格 → commit 順序＝slot 順序（這是 FIFO 的基礎）
//   · read_begin 只讀 r；r 不是 READY → NULL
//   · 兩者都不可跳格 → commit 順序＝讀取順序（嚴格 FIFO）
//     （2026-11-09 壓力測試：任一方跳格都會在 N=2 造成 gap/dup）
//
// 跨核同步契約（2026-11-09 補）：
//   · 資料寫完 → commit 以 release 發佈 READY；reader 以 acquire 取得 READY。
//     因此 reader 看到 READY 時，slot 內容一定已寫完。
//   · read_end 以 release 歸還 IDLE；writer 以 acq_rel CAS 取得 IDLE。
//   · 每格有 slot generation；新增 API 可在取的同時拿到它。
//   · 首次 write_begin／read_begin 會記錄核心；之後若從另一核呼叫，
//     core_violations 會增加（只計數，不改變行為）。
//   · 不收集 lease 時間戳：排序只靠 generation／狀態機，熱路徑不讀計時器。
//
// 誰負責什麼：
//   管理器：格子數量、狀態、指標、owner／核心記錄、generation
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
// n 上限 256（state 4 B＋generation 4 B，約 8 B/slot）；registry 上限 32 個 hub。
// align 0 → 32（cache line / DMA）。
nc_hub_t *nc_hub_create(const char *owner, size_t slot_size, size_t n,
                        nc_hub_class_t cls, size_t align);

// 在「外部已配好的區域」上建立（plan 給的記憶體走這條）
nc_hub_t *nc_hub_create_in(void *region, size_t region_bytes, const char *owner,
                           size_t slot_size, size_t n, size_t align);

// 釋放 hub 本身與 state；只會 free nc_hub_create() 自己配置的 slot 區，
// nc_hub_create_in() 的外部區域不動（由 plan／owner 管理）。
// 呼叫前必須確保沒有任何 writer／reader 還在引用。
void nc_hub_destroy(nc_hub_t *hub);

// ── writer ──
void *nc_hub_write_begin(nc_hub_t *hub);          // 沒有空位 → NULL
esp_err_t nc_hub_write_commit(nc_hub_t *hub);     // begin 之後呼叫一次

// ── reader ──
void *nc_hub_read_begin(nc_hub_t *hub);           // 沒有 READY → NULL
// 同上，並回傳該格最後一次 publish 的 generation（可選）
void *nc_hub_read_begin_gen(nc_hub_t *hub, uint32_t *gen);
void nc_hub_read_end(nc_hub_t *hub);              // 歸還目前持有的那格

// ── 查詢 ──
bool nc_hub_dirty(const nc_hub_t *hub);           // 有 READY（有新東西）
size_t nc_hub_fill(const nc_hub_t *hub);          // READY 的格數
size_t nc_hub_depth(const nc_hub_t *hub);         // N
size_t nc_hub_slot_size(const nc_hub_t *hub);
const char *nc_hub_owner(const nc_hub_t *hub);

// 最後一次成功 publish 的 generation（0 = 還沒 publish）
uint32_t nc_hub_generation(const nc_hub_t *hub);

typedef struct {
    uint32_t generation;
    size_t fill;
    int writer_core;          // -1 = 尚未使用
    int reader_core;
    uint32_t core_violations; // 從非首次核心呼叫的次數
    uint32_t publishes;
} nc_hub_stats_t;

void nc_hub_stats(const nc_hub_t *hub, nc_hub_stats_t *out);

// 所有已建立 hub 的報表（JSON 陣列）
size_t nc_hub_registry_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
