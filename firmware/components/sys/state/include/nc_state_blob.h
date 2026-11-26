// NCSTATE：state manager 的唯一持久格式（統一＋內部自描述）。
//
// 定案（2026-11-26，詳見 docs/03_notes/state-storage-format.md）：
//   - 全專案只有兩個格式：NCSTATE（狀態值）＋ NCLAYOUT（pixel layout 快取）；
//   - magic 用 ASCII 全名、唔加版本數字（版本喺 header 的 ver）；
//   - 每筆 record 自描述（type＋len），reader 可以跳過唔識嘅欄位（向前兼容）；
//   - sparse：只存同出廠預設唔同嘅值。
//
// Header（24 B，全 LE）：
//   magic[8] = "NCSTATE\0" | ver u16 | flags u16 | count u16 |
//   src_hash u32 | payload_len u32 | blob_crc u32
// Record：
//   type u8 | key varint | len varint | value
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_STATE_BLOB_MAGIC_BYTES 8
#define NC_STATE_BLOB_VER         1
#define NC_STATE_BLOB_HDR         26

typedef enum {
    NC_STATE_T_UINT = 1,    // 無號整數（LEB128）
    NC_STATE_T_SINT = 2,    // 有號整數（zigzag LEB128）
    NC_STATE_T_BYTES = 3,   // 原始 bytes／UTF-8 字串
    NC_STATE_T_BOOL = 4,    // 0／1
} nc_state_type_t;

typedef struct {
    uint16_t ver;
    uint16_t flags;
    uint32_t src_hash;      // 來源 JSON 的 CRC32（開機核對版本）
} nc_state_meta_t;

typedef struct {
    uint32_t    key;        // schema key id（u32：domain|unit|index|field）
    uint8_t     type;       // NC_STATE_T_*
    uint32_t    u;          // UINT／BOOL
    int32_t     i;          // SINT
    const void *bytes;      // BYTES
    uint16_t    bytes_len;  // BYTES 長度
} nc_state_rec_t;

typedef esp_err_t (*nc_state_rec_cb_t)(void *ud, const nc_state_rec_t *rec);

typedef struct {
    uint16_t ver;
    uint16_t flags;
    uint32_t src_hash;
    uint32_t n_records;     // header 寫的 record 數
    uint32_t n_skipped;     // 唔識 type、跳過嘅 record 數（向前兼容）
} nc_state_info_t;

// 值的 CRC32（record 序列的 canonical 編碼，唔含 header）。
// 用途：cache 失效判斷（src_hash）。改 JSON 的註解／排版唔會令它變。
// invalid input 回 0 —— 呼叫端要當「唔同」，唔可以當「相同」。
uint32_t nc_state_blob_rec_crc(const nc_state_rec_t *recs, size_t nrec);

// 計 encode 出嚟會幾大（唔含 cap 檢查）。有 invalid input 回 0。
size_t nc_state_blob_size(const nc_state_rec_t *recs, size_t nrec);

// encode；成功回寫入 byte 數，cap 唔夠／參數非法回 0。
size_t nc_state_blob_encode(uint8_t *out, size_t cap, const nc_state_meta_t *meta,
                            const nc_state_rec_t *recs, size_t nrec);

// decode；每筆已知 record 叫 cb（cb 可以 NULL，只驗證＋統計）。
// 錯誤：ESP_ERR_INVALID_ARG（NULL）／ESP_ERR_INVALID_SIZE（截斷、長度不符）／
//       ESP_ERR_INVALID_STATE（magic／ver 唔夾）／ESP_FAIL（CRC 錯）。
esp_err_t nc_state_blob_decode(const uint8_t *buf, size_t len,
                               nc_state_rec_cb_t cb, void *ud,
                               nc_state_info_t *info);

#ifdef __cplusplus
}
#endif
