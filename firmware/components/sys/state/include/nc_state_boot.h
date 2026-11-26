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
    uint16_t first_diff; // 第一筆對拍唔中的 key
    const char *why;     // miss 原因（靜態字串；hit 時 NULL）
} nc_state_boot_result_t;

// 在 B02（storage+cfg 之後）呼叫一次。out 可以 NULL。
esp_err_t nc_state_boot_sync(nc_state_boot_result_t *out);

// 上一次 sync 的結果（未跑過回 NULL）；給 status 用。
const nc_state_boot_result_t *nc_state_boot_last(void);

#ifdef __cplusplus
}
#endif
