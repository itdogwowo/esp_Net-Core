// 任務註冊表（對照 mp 版的 TaskManager）
//
// 舊專案的做法：每個 task 自己宣告「名字 / 啟動層 / affinity」，由 TaskManager 統一
// 註冊、依 layer 順序啟動、釘到指定核心，並且可以執行期查詢與改 affinity。
// 這裡保留同樣的四個性質：
//
//   1. 註冊（register）——不由 main 直接 xTaskCreate
//   2. 分層（layer）——L0 先起、L1 再起，避免「起來了但依賴的服務還沒好」
//   3. affinity——core = -1（不限）/ 0 / 1，實際跑在哪由 xTaskGetCoreID 回報
//   4. 硬體歸屬檢查——碰某些周邊的 task 不准排到不該去的核心（見 nc_task_hw_t）
//
// 與 mp 版的差異：舊版 affinity 是 (0,0)/(1,0)/(0,1) 三元組，這裡直接用 core = -1/0/1；
// (1,1)「兩核都要」在舊版是錯誤，這裡沒有對應值（因為本來就不該存在）。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_TASK_MAX        16
#define NC_TASK_LAYERS     8
#define NC_TASK_CORE_ANY   (-1)

typedef enum {
    NC_TASK_HW_NONE = 0,
    NC_TASK_HW_USB,      // TinyUSB（PHY 只有一個，不能兩核同時操作）
    NC_TASK_HW_FLASH,    // 直接寫 flash（OTA / 檔案）
} nc_task_hw_t;

typedef struct {
    const char *name;
    TaskFunction_t fn;
    void *arg;
    uint32_t stack;
    UBaseType_t prio;
    int core;            // NC_TASK_CORE_ANY / 0 / 1
    int layer;           // 0 = 最先啟動
    nc_task_hw_t hw;     // 硬體歸屬（決定是否強制綁核）
} nc_task_def_t;

// 註冊（不啟動）。重複名字回 ESP_ERR_INVALID_STATE。
esp_err_t nc_task_register(const nc_task_def_t *def);

// 依 layer 由小到大啟動全部已註冊任務。可重複呼叫（已啟動的跳過）。
esp_err_t nc_tasks_boot(void);

// 啟動指定 layer（給需要階段式啟動的場合）
esp_err_t nc_tasks_boot_layer(int layer);

// 查詢：回傳 JSON 陣列字串（含 name / layer / affinity / running_core / running）
size_t nc_tasks_status_json(char *buf, size_t cap);

// 單一任務目前跑在哪一核；找不到或未執行回 -1
int nc_task_running_core(const char *name);

#ifdef __cplusplus
}
#endif
