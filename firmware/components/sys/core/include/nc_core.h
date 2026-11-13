// 一次性 init「喺邊個核跑」。
//
// 用途：driver 的安裝（USB、網絡介面…）必須喺**服務所屬核**執行，
// 咁 ISR、driver task、producer、decode 就會全部同核，唔會跨核叫醒。
//
// 限制：只准開機 init 階段用。呢個內部用 esp_ipc（會打斷另一個核），
// 唔可以變成 runtime 常態。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nc_core_run_on(int core, esp_err_t (*fn)(void *), void *arg);

// ── 核數：開機第一個要回答的問題 ───────────────────────────────
//
// 全 firmware **只有這裡**知道「這塊 MCU 有幾多核」。
// 其他地方（system.json 的 decodeN、tasks.json 的 affinity、任務藍圖的 core）
// 都只是「要求」，一律要經 nc_core_clamp() 收斂成合法值 ——
// 單核板上寫 core1 會變成 core0 ＋ 一行警告，而不是在 xTaskCreatePinnedToCore 裡 assert。
//
// 為什麼要集中在這裡：散落的 #if CONFIG_FREERTOS_UNICORE 每個地方都要記得寫；
// 集中在一個函式之後，加一種新晶片只改 nc_core_probe()。
#define NC_CORES_MAX   2       // 框架只認 core0／core1（decode0／decode1）
#define NC_CORE_ANY  (-1)      // 唔指定：交返 FreeRTOS（= NC_TASK_CORE_ANY）

int  nc_core_probe(void);      // 探測一次並記住（幂等）；回傳 1 或 2
int  nc_core_count(void);      // 未 probe 過會先 probe
bool nc_core_has(int core);    // core 存在？
int  nc_core_clamp(int core);  // 非法核 → 0；NC_CORE_ANY 原樣放行

#ifdef __cplusplus
}
#endif
