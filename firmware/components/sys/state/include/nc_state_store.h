// NCSTATE RAM store：把 blob decode 出嚟的值放喺固定陣列（唔用 PSRAM、唔用 heap）。
//
// 用途（P3 第一步）：
//   - 開機 hit 時，由 state.ncst 載入 → RAM store；
//   - 同「由 JSON parse 出嚟的 cfg record」**對拍**：全中才當 hit（防止舊 cache 靜默生效）；
//   - 對拍唔中 → 當 miss 重寫（self-heal），並喺 status 顯示。
//
// 全部固定大小：24 筆 × ~24 B ≈ 576 B（實際 boot mirror 18 筆），internal RAM。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_state_blob.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_STATE_STORE_MAX        64
#define NC_STATE_STORE_BYTES_MAX  16   // 短字串（led fmt 最多 12）

typedef struct {
    uint16_t key;
    uint8_t  type;
    uint8_t  bytes_len;
    uint32_t u;
    int32_t  i;
    char     bytes[NC_STATE_STORE_BYTES_MAX];
} nc_state_ent_t;

typedef struct {
    nc_state_ent_t e[NC_STATE_STORE_MAX];
    uint16_t n;
} nc_state_store_t;

void nc_state_store_clear(nc_state_store_t *s);

// 放入一筆（重複 key 覆蓋）；store 滿／bytes 太長 → false（明示，唔截斷）。
bool nc_state_store_put(nc_state_store_t *s, const nc_state_rec_t *r);

// 給 nc_state_blob_decode 用的 callback（ud = nc_state_store_t*）。
esp_err_t nc_state_store_cb(void *ud, const nc_state_rec_t *r);

// 由 records 一次過填；回成功放入筆數。
uint16_t nc_state_store_from_recs(nc_state_store_t *s, const nc_state_rec_t *recs, size_t n);

const nc_state_ent_t *nc_state_store_find(const nc_state_store_t *s, uint16_t key);
bool nc_state_store_get(const nc_state_store_t *s, uint16_t key, nc_state_rec_t *out);

// 同「期望的 records」對拍：回唔一致筆數（0 = 全中）；first_diff 回第一筆唔同的 key。
// store 有多出嚟的 key（舊 blob 遺留）唔算唔一致（向前兼容）。
uint32_t nc_state_store_cmp(const nc_state_store_t *s, const nc_state_rec_t *recs, size_t n,
                            uint16_t *first_diff);

#ifdef __cplusplus
}
#endif
