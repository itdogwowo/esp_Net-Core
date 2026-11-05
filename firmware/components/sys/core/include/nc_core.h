// 一次性 init「喺邊個核跑」。
//
// 用途：driver 的安裝（USB、網絡介面…）必須喺**服務所屬核**執行，
// 咁 ISR、driver task、producer、decode 就會全部同核，唔會跨核叫醒。
//
// 限制：只准開機 init 階段用。呢個內部用 esp_ipc（會打斷另一個核），
// 唔可以變成 runtime 常態。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nc_core_run_on(int core, esp_err_t (*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif
