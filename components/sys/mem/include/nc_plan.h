// 記憶體 Plan：服務自己宣告 → 加總 → 每 class 一次 carve → 逐項發指標
//
// 規則（與設計文件一致）：
//   ① 服務自己宣告，管理器不認得任何應用
//   ② 宣告窗口在 commit 時關閉
//   ③ 依 level 由小到大配置；塞不下就「標記該項失敗」，其他人照跑
//   ④ 每個 class 只向系統要一次大塊，再切給各項（不逐項 alloc）
//   ⑤ 沒有撇除、沒有整批失敗 —— 失敗是逐項的

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_PLAN_DMA = 0,
    NC_PLAN_INTERNAL,
    NC_PLAN_PSRAM,
    NC_PLAN_CLASSES
} nc_plan_class_t;

typedef struct {
    const char *service;      // 「誰要的」：nc4 / probe / pixel …
    const char *name;         // 邏輯名："nc4.rx" / "probe.hub"
    int level;                // 小的先配（0 = 最基礎）
    nc_plan_class_t cls;
    size_t bytes;
    size_t align;             // 0 → 32
} nc_plan_decl_t;

#define NC_PLAN_MAX_DECLS 64

typedef struct {
    size_t declared[NC_PLAN_CLASSES];    // 宣告總量
    size_t planned[NC_PLAN_CLASSES];     // 實際要 carve 的量（成功項的總和）
    size_t allocated[NC_PLAN_CLASSES];   // 真的要到的量
    size_t avail[NC_PLAN_CLASSES];       // 呼叫端給的可用量
    int decl_count;
    int ok_count;
    int fail_count;
    bool committed;
} nc_plan_report_t;

void nc_plan_reset(void);

// 宣告（服務自己呼叫）。commit 之後回 ESP_ERR_INVALID_STATE。
esp_err_t nc_plan_declare(const nc_plan_decl_t *decl);

// 一次配置。逐項成功/失敗，不會整批失敗。
esp_err_t nc_plan_commit(const size_t avail[NC_PLAN_CLASSES], bool allow_shed);

// 取指標；該項失敗或不存在 → NULL
void *nc_plan_get(const char *name);

bool nc_plan_was_shed(const char *name);   // 名字保留：回報「這一項是否失敗」
const nc_plan_report_t *nc_plan_report(void);
size_t nc_plan_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
