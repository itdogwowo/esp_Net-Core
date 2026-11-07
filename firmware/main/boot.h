// 開機階段的**順序**與**政策**。
//
// 為什麼要拆出來
// --------------
// app_main 以前是一支 400 行的直落函數：NVS、storage、config、身份、硬體、
// 路由、宣告、commit、啟動、USB、通道、帳 —— 全部靠註解分段。問題是：
//   · 「邊一步失敗會點」只寫在每個 if 裡面，要讀完全部才知道
//   · 出事之後（例如 USB 未起來）冇一個地方可以一眼看到「跑到邊、邊步失敗」
//   · 加一步就要記住它應該放喺邊（次序係隱性的）
//
// 現在：階段寫成表（順序即資料），runner 逐個跑、計時、記錄結果。
// 每個階段是 `esp_err_t (*)(void)`，回 ESP_OK 就繼續 —— 失敗要唔要停，
// 由呼叫者（app_main）決定，唔會藏喺分支裡面。
//
// 呢一層刻意保持「零政策」：只負責跑同記。所有硬體知識仍然喺階段實作裡面。
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;          // 顯示用（開機 log 會印）
    esp_err_t (*fn)(void);
} nc_boot_phase_t;

#define NC_BOOT_COUNT(_arr) (sizeof(_arr) / sizeof((_arr)[0]))

// 依序執行全部階段（**不會因為失敗而停** —— 失敗只會被記錄）。
// 回傳第一個失敗的階段索引；全部成功回 -1。
int nc_boot_run(const nc_boot_phase_t *phases, size_t n);
