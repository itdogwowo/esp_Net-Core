// 開機時 sync state.NCSTATE：由 config 派生（P2 第一步）。
//
// 流程（定案：開機對哈希；一樣＝直接載入；唔一樣＝重新生成）：
//   1. 算 config JSON 的 src_hash（hardware／driver／system／tasks／resources）
//   2. peek state.NCSTATE：ver／magic／CRC／src_hash 全中 → hit（log）
//   3. 否則由已經 parse 好的 cfg 造 record → atomic 寫檔（舊檔 → .bak）
//
// 保護層：L0 出廠預設（firmware const）／L1 JSON（權威）／L2 NCSTATE（快取）／
//         L2b .bak（last-known-good）。任何一層壞都唔會令設定「靜靜變零」。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_state_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_STATE_PATH      "/data/config/state.ncst"
#define NC_STATE_PATH_BAK  NC_STATE_PATH ".bak"

typedef struct {
    uint32_t src_hash;
    uint32_t n_records;
    uint32_t n_skipped;
    size_t   bytes;      // 磁碟上檔案大小（hit 時）
    bool     hit;        // 檔案存在且 src_hash／ver／CRC 全中
    bool     cmp_ok;     // hit 時同 cfg 對拍完全一致（=真 hit）
    bool     rebuilt;    // 今次有重新生成並寫檔
    uint32_t n_mismatch; // 對拍唔中筆數（hit 但 cache 舊）
    uint32_t first_diff; // 第一筆對拍唔中的 key
    uint32_t n_schema_bad;  // 由 cfg 造出的 record 同 schema 對唔上的筆數（防 drift）
    uint32_t n_inactive;    // 因為 unit 未啟動而跳過的 value key 數（S4）
    const char *why;     // miss 原因（靜態字串；hit 時 NULL）
} nc_state_boot_result_t;

// 在 B02（storage+cfg 之後）呼叫一次。out 可以 NULL。
esp_err_t nc_state_boot_sync(nc_state_boot_result_t *out);

// 開機（B02）之後、**B05 之前**呼叫一次：把 store 的 value 蓋返 cfg struct。
// 回傳套用筆數；0 = 冇 override（cfg 保持 JSON 原值）。
// 為什麼要在 B05 之前：driver 裝機時讀的是 cfg，太遲套等於冇套。
uint32_t nc_state_apply_to_cfg(void);

// S3：把 RAM store 回寫成 state.ncst（原子寫；src_hash 沿用 boot 的值集 hash）。
// 只喺「用戶明確要求 persist」或 reboot 前呼叫；平時 set 只改 RAM。
esp_err_t nc_state_persist(void);
esp_err_t nc_state_persist_path(const char *path);   // host test／自訂路徑

// 由 decode 任務（STATUS_SET handler）呼叫：**只排隊，唔喺度寫 FAT**。
// 為什麼：decode 任務 stack 只有 3,072 B，FAT 寫入路徑連 VFS／FatFS 會爆
// （2026-11-27 實測 rst=4 panic）。真正落盤喺 reboot 前由 main 任務做。
void nc_state_persist_request(void);
bool nc_state_persist_pending(void);

// boot 由 cfg 算出的值集 hash（commit 要沿用同一個，否則下次開機 hash 唔對就重建）。
uint32_t nc_state_boot_src_hash(void);

// 上一次 sync 的結果（未跑過回 NULL）；給 status 用。
const nc_state_boot_result_t *nc_state_boot_last(void);

// RAM store（19 筆左右；未 sync 回 NULL）；給 nc_state_api／driver 用（唯讀）。
const nc_state_store_t *nc_state_boot_store(void);

// 同一個 store 的可寫版本（nc_state_set_* 用）。未 sync 回 NULL。
// 寫入只改 RAM；下次開機仍以 /data/config/*.json 為權威（見 nc_state_api.h）。
nc_state_store_t *nc_state_boot_store_mut(void);

#ifdef __cplusplus
}
#endif
