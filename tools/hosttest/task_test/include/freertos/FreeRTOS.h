// host 測試用的最小 FreeRTOS 型別（不模擬排程，只讓 nc_task.h 編得過）
#pragma once

#include <stdint.h>

typedef void (*TaskFunction_t)(void *);
typedef uint32_t TickType_t;
typedef unsigned int UBaseType_t;
typedef int BaseType_t;
