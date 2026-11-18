// 測試用 freertos/task.h shim：nc_task.h 只在 firmware 用；host 只借它的型別。
#pragma once
#include <stdint.h>
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
