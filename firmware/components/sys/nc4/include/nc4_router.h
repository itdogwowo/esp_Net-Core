// NC4 路由器：決定「邊條線由邊個核服務」
//
// 呢個係全系統**唯一**決定核歸屬嘅地方。bus 只係執行者，唔自己諗：
//
//   nc4_router_core("UART") → 1  →  nc4_bus_add(..., .core = 1)
//
// 開機次序（app_main）：
//   ① nc4_router_set(表)       ← 一定要早過 nc_task_start_all
//   ② nc_task_start_all()      ← decode0 / decode1 問 router 決定開唔開
//   ③ nc4_bus_add(...)         ← .core 由 router 查返嚟
//
// 點解要早過 ②：loop 一建立就會食 6 KB 棧同 parser 緩衝。
// 「冇線去 core1」→ nc4_router_core_used(1) == false → decode1 索性唔建立，
// 嗰份記憶體由頭到尾都唔存在。呢個係省資源嘅開關，config 改唔到
// （SYSTEM 任務唔讀 tasks.json，見 nc_task.c:411）。
//
// 執行緒安全：set / add 只可以喺建立任務之前做；之後全部係唯讀。
// 線名會**複製**入 router 自己嘅表（最長 NC4_ROUTE_NAME_MAX-1 個字元），
// 所以 callers 嘅字串可以係臨時嘅 —— 唔使擔心壽命。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC4_ROUTE_MAX       8
#define NC4_ROUTE_NAME_MAX  16     // 線名上限（含 NUL）
#define NC4_ROUTE_NONE      (-1)   // 冇人路由去呢條線

// 一行 = 一條通道（"CDC" / "UART" / "TCP" / "WS" / "UDP" …）
typedef struct {
    const char *line;
    int core;              // 0 / 1
} nc4_route_t;

// 一次過設定整張表。先驗完先落地：唔會留低半張表。
esp_err_t nc4_router_set(const nc4_route_t *routes, int n);

// 加一條（重名 / 超出上限 → 錯誤）
esp_err_t nc4_router_add(const char *line, int core);

// 清空（測試用；正常開機唔需要叫）
void nc4_router_reset(void);

// 查核：冇路由返 NC4_ROUTE_NONE
int nc4_router_core(const char *line);

// 冇 `_or(line, fallback)` 變體，是故意的：核歸屬只由 config 決定。
// 呼叫端取唔到就自己決定「唔建立」（fail-visible），
// 唔准在 code 裡寫預設核 —— 打錯 config 名會靜靜跑喺錯嘅核，最難查。

// 呢一核有冇任何線？decode 任務嘅開關就係問呢個
bool nc4_router_core_used(int core);

// 強制保留某一核（即使一條線都冇）。用途：開機保證至少有一個 decode 任務，
// 否則「點解冇線」都冇人答得到。只影響 core_used()，唔會憑空生一條線。
esp_err_t nc4_router_keep(int core);

int  nc4_router_count(void);

// {"set":1,"core0":3,"core1":1,"routes":[{"line":"CDC","core":0},…]}
size_t nc4_router_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
