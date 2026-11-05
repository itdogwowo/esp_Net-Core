// 開機自測任務（測試用）：宣告一塊 hub，寫一格再讀返，驗證 hub 交換。
#pragma once
#include "nc_task.h"
#ifdef __cplusplus
extern "C" {
#endif
const nc_task_t *nc_test_probe_task(void);
#ifdef __cplusplus
}
#endif
