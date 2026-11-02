#pragma once
#include <stdint.h>

int64_t fx_host_now_us(void);

// host 測試假裝的晶片架構（NC_FX_TARGET_*）；測試可以改它來驗錯架構的路徑
extern int fx_host_arch_target;
