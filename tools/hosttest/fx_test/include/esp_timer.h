// 測試用 esp_timer.h shim：時間由測試自己餵（demo／統計用得到）。
#pragma once
#include <stdint.h>

extern int64_t g_fake_us;                 // 測試設定的假時鐘
static inline int64_t esp_timer_get_time(void) { return g_fake_us; }
