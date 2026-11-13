// nc_core 的 host 替身：任務管理器只用到 nc_core_clamp()，
// 但 include 了 nc_core.h，所以四個 API 都給一個最小實作。
// 預設 2 核 —— host 測試模擬的是雙核板（與合併前的行為一致）。
#include <stdbool.h>

#include "nc_core.h"

int nc_core_probe(void)
{
    return 2;
}

int nc_core_count(void)
{
    return 2;
}

bool nc_core_has(int core)
{
    return core >= 0 && core < 2;
}

int nc_core_clamp(int core)
{
    if (core == NC_CORE_ANY) {
        return core;
    }
    return (core >= 0 && core < 2) ? core : 0;
}
