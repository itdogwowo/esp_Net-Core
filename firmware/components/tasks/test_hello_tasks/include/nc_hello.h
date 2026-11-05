// hello 任務：最小框架自測，每秒印一次。只回傳宣告，登記與啟動交給管理器。
#pragma once
#include "nc_task.h"

#ifdef __cplusplus
extern "C" {
#endif

const nc_task_t *nc_hello_task(void);

#ifdef __cplusplus
}
#endif
