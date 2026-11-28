#pragma once
#include <stdint.h>

int64_t mod_host_now_us(void);

// host 測試假裝的晶片架構（NC_MOD_TARGET_*）；測試可以改它來驗錯架構的路徑
extern int mod_host_arch_target;
