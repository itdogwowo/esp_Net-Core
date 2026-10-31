// 緩衝區的「可設定」部分
//
// C 版的緩衝**不是**燒死在 firmware 裡的靜態陣列 —— 那樣配置就改不動了。
// 做法是：開機時才向系統要記憶體，大小/數量來自 config.json，預設值寫在程式裡。
//
//   燒進 firmware 的是「預設值 + 上限」；config.json 決定「這次用多少」。
//
// 為什麼這樣可行（DMA 也一樣）：
//   DMA 要的是「初始化之後位址固定」，不是「編譯期就要有」。開機配置、之後不動，
//   就滿足 RMT/SPI 的要求。真正做不到的只有「執行中改大小」—— 那個必須重開機。
//
// 上限（clamp）存在的原因：config.json 是人寫的，也可能是使用者拖進去的。
// 沒有上限，一個手滑就會把 heap 吃光，而且症狀是「開不起來」。

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "nc_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

// 上限：超過就夾住並警告，不讓 config 把系統弄死
#define NC_MEM_FRAME_BLOCK_MAX   (32 * 1024)
#define NC_MEM_FRAME_COUNT_MAX   8
#define NC_MEM_ASSET_BYTES_MAX   (512 * 1024)

typedef struct {
    size_t frame_block;   // 一幀多少 bytes（px × 3，RGBW 要 ×4）
    size_t frame_count;   // 幾塊（compute / output / 上一幀 / 餘裕）
    size_t asset_bytes;   // arena 總大小
} nc_mem_limits_t;

// 程式內建預設（config 讀不到時用這個）
void nc_mem_limits_default(nc_mem_limits_t *out);

// 從整份 config.json 覆寫；缺欄位保留原值，超上限夾住。
// 回傳 ESP_ERR_NOT_FOUND = 沒有 "Buffer" 段（視為「全部用預設」，不算錯）。
esp_err_t nc_mem_limits_from_json(nc_mem_limits_t *lim, const char *json);

void nc_mem_limits_log(const nc_mem_limits_t *lim);

#ifdef __cplusplus
}
#endif
