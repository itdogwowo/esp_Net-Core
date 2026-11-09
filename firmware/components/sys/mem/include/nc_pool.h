// NC4 共用緩衝池：固定 size class ＋ descriptor ＋ refcount。
//
// 為什麼要有這一層（2026-11-09 設計）：
//   · 每條線自己一份 parser ring／slots，加起來重複又難調；
//   · 轉寄要跨核時，來源核不能揸住 parser 環去等目的核（會 block 整個 decode）；
//   · 大指令（OTA／FILE 4 KB）只在少數時候出現，不值得每條線常駐 4 KB。
//
// 這個 pool 只做「資料格」：三個 class（small／bulk／forward），每個 slot 有
// state ＋ src/dst link ＋ refcount ＋ generation。parser 狀態仍然 per-link。
//
// 執行緒安全：alloc／commit／retain／release 都可以由兩個核呼叫；
// 內部只有一個 portMUX 保護 free list 與 refcount（臨界區很短，只做指標操作）。
// **不可以在 ISR 裡 block**；本層不會 block。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_POOL_MAX_CLASSES  4
#define NC_POOL_MAX_SLOTS    32
#define NC_POOL_NAME_MAX     16

typedef enum {
    NC_POOL_SMALL = 0,     // 一般命令（1 KB）
    NC_POOL_BULK,          // 大指令／OTA（4 KB / 8 KB）
    NC_POOL_FORWARD,       // 跨核轉寄 staging
    NC_POOL_CLASS_MAX,
} nc_pool_class_t;

// class 設定（由 config 來；name 只用於 log／JSON）
typedef struct {
    char name[NC_POOL_NAME_MAX];
    uint16_t size;             // 每格 bytes
    uint8_t  count;            // 幾格
    nc_mem_class_t where;      // INTERNAL / PSRAM / DMA
    bool enable;
} nc_pool_class_cfg_t;

// slot 狀態（descriptor 用）
typedef enum {
    NC_POOL_ST_FREE = 0,
    NC_POOL_ST_WRITING,        // producer／DMA 正在寫
    NC_POOL_ST_READY,          // 已 commit，等 consumer
    NC_POOL_ST_READING,        // consumer 正在讀
    NC_POOL_ST_FORWARD,        // 已入 forward queue，等目的核送
    NC_POOL_ST_SENDING,        // 正在送（TX）
} nc_pool_state_t;

typedef struct nc_pool_slot {
    uint8_t *data;             // 指向 class arena
    uint16_t cap;              // 這格的容量
    uint16_t len;              // 已寫入 bytes
    uint8_t  cls;              // nc_pool_class_t
    uint8_t  state;            // nc_pool_state_t
    uint8_t  src_link;         // 邊條線嚟（index，給統計）
    uint8_t  dst_link;         // 轉寄目的地（index；0xFF = 無）
    uint16_t refcnt;           // 0 = 可回收
    uint16_t gen;              // 每次 alloc ++（防 ABA）
    uint32_t ts_ms;            // alloc 時間（除錯／TTL 用）
    struct nc_pool_slot *next; // free list
} nc_pool_slot_t;

// 清空（測試用）
void nc_pool_reset(void);

// 由 config 建立 pool（一次配置，之後不再向系統要記憶體）。
// 回傳 ESP_OK；個別 class 失敗會印 log 並繼續（fail-visible）。
esp_err_t nc_pool_init(const nc_pool_class_cfg_t *cfg, int n);

// 取一格（state = WRITING）。冇格回 NULL（caller 自己丟／計數）。
nc_pool_slot_t *nc_pool_alloc(nc_pool_class_t cls, uint8_t src_link);

// 寫完：state → READY。
void nc_pool_commit(nc_pool_slot_t *slot, uint16_t len);

// 多一個人要用（tap／多目的地）：refcnt++。
void nc_pool_retain(nc_pool_slot_t *slot);

// 用完：refcnt--；0 → FREE。
void nc_pool_release(nc_pool_slot_t *slot);

// 查詢（給 STATUS／測試）
void nc_pool_stats(int cls, uint32_t *count, uint32_t *used, uint32_t *peak, uint32_t *fails);
size_t nc_pool_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
