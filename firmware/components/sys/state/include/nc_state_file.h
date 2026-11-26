// NCSTATE 檔案層：讀／寫／原子替換／來源哈希。
//
// 檔名規矩：**檔名 = 用途 + 格式名** → `/data/config/state.NCSTATE`
// （用家 2026-11-26 定案：檔名要見到格式名，唔好叫 state.bin）。
//
// 保護層（失敗退還，唔會比之前少）：
//   L0 出廠預設（nc_port.c／schema default）——永遠喺 firmware，唔會失
//   L1 FAT JSON（完整、非 sparse）——人手改嘅源頭
//   L2 state.NCSTATE（sparse 派生快取）——壞／版本唔夾 → 由 L1 重生
//   L2b state.NCSTATE.bak——每次寫新檔前保留上一份（last-known-good）
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_state_blob.h"

#ifdef __cplusplus
extern "C" {
#endif

// 由一組來源檔算 src_hash：crc32 逐個檔累加 crc32("<basename>\0" + raw bytes)。
// 唔存在嘅檔跳過（唔算錯）；out 一定寫。回 ESP_OK／ESP_ERR_INVALID_ARG。
esp_err_t nc_state_file_hash_files(const char *const *paths, size_t npaths, uint32_t *out);

// 讀整個檔案 → decode（cb 可以 NULL）。info 可以 NULL。
esp_err_t nc_state_file_load(const char *path, nc_state_rec_cb_t cb, void *ud,
                             nc_state_info_t *info);

// 只讀 header（驗證 magic／ver／CRC；唔交 record）。info 可以 NULL。
esp_err_t nc_state_file_peek(const char *path, nc_state_info_t *info);

// 寫檔（原子）：encode → 寫 <path>.tmp → 舊檔搬 <path>.bak → rename。
esp_err_t nc_state_file_save(const char *path, const nc_state_meta_t *meta,
                             const nc_state_rec_t *recs, size_t nrec);

// 檔案大小（bytes）；唔存在回 0。
size_t nc_state_file_size(const char *path);

#ifdef __cplusplus
}
#endif
