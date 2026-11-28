// NCSTATE RAM store：把 blob decode 出嚟的值放喺 **開機掃描後精確配置** 的 buffer。
//
// 用途（P3 第一步）：
//   - 開機 hit 時，由 state.ncst 載入 → RAM store；
//   - 同「由 JSON parse 出嚟的 cfg record」**對拍**：全中才當 hit（防止舊 cache 靜默生效）；
//   - 對拍唔中 → 當 miss 重寫（self-heal），並喺 status 顯示。
//
// 2026-11-28（用家要求：開機先掃描有幾多值，再動態申請，減少靜態 standby）：
//   `nc_state_store_t` 只保留 pointer＋len＋cap；真正 entries 由 `nc_state_boot`
//   在 B02 掃完 records（＋舊 blob 筆數）之後 `heap_caps_malloc(MALLOC_CAP_INTERNAL)` 一次。
//   仍然守鐵律：開機一次配置、永不釋放、runtime 唔租。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_state_blob.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_STATE_STORE_MAX        96   // 動態 capacity 上限（亦係 dirty 表上限）；2026-11-27 96
#define NC_STATE_DYN_RESERVE      8    // 熱插拔任務／效果孤兒值 headroom（未接前預留）

// 12 B／筆（2026-11-27）：state 只存**數值**。
//   - key 4 + type 1 + pad 3 + union 4
//   - 冇 bytes[16]：字串唔應該入 state（名字在 schema 字典＝flash，0 RAM）；
//     LED 的線材格式已由 str 改成 enum（存 index 0..4）。
//   - u／i 用 union：同一筆只會用其中一個（之前兩個各 4 B）。
typedef struct {
    uint32_t key;
    uint8_t  type;
    uint8_t  _pad[3];
    union {
        uint32_t u;
        int32_t  i;
    };
} nc_state_ent_t;

typedef struct {
    nc_state_ent_t *e;     // 指向開機配置的內部 buffer（永遠有效；NULL = 未配置）
    uint16_t n;            // 已用筆數
    uint16_t cap;          // 配置容量（≤ NC_STATE_STORE_MAX）
} nc_state_store_t;

// 綁定外部 buffer（由 nc_state_boot 開機掃描後配置；之後唔會再換）。
bool nc_state_store_bind(nc_state_store_t *s, nc_state_ent_t *buf, uint16_t cap);
uint16_t nc_state_store_cap(const nc_state_store_t *s);

void nc_state_store_clear(nc_state_store_t *s);

// 放入一筆（重複 key 覆蓋）；store 滿／bytes 太長 → false（明示，唔截斷）。
bool nc_state_store_put(nc_state_store_t *s, const nc_state_rec_t *r);

// 給 nc_state_blob_decode 用的 callback（ud = nc_state_store_t*）。
esp_err_t nc_state_store_cb(void *ud, const nc_state_rec_t *r);

// 由 records 一次過填；回成功放入筆數。
uint16_t nc_state_store_from_recs(nc_state_store_t *s, const nc_state_rec_t *recs, size_t n);

const nc_state_ent_t *nc_state_store_find(const nc_state_store_t *s, uint32_t key);
bool nc_state_store_get(const nc_state_store_t *s, uint32_t key, nc_state_rec_t *out);

// 只保留「期望的 records」有的 key（清走停用 unit 的舊記錄）；回移除筆數。
uint16_t nc_state_store_prune(nc_state_store_t *s, const nc_state_rec_t *recs, size_t n);

// 同「期望的 records」對拍：回唔一致筆數（0 = 全中）；first_diff 回第一筆唔同的 key。
// store 有多出嚟的 key（舊 blob 遺留）唔算唔一致（向前兼容）。
uint32_t nc_state_store_cmp(const nc_state_store_t *s, const nc_state_rec_t *recs, size_t n,
                            uint32_t *first_diff);

#ifdef __cplusplus
}
#endif
