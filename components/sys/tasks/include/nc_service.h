// 服務管理器（service manager）
//
// 生命週期只有三個動詞（＋一個「跑」交給 task）：
//
//   ① declare()  開機、commit 之前：宣告「我要多少記憶體」＋登記我要的 task
//   ② start()    資源管理器 carve 之後：把指標交給你（由管理器去取，不是你自己 get）
//   ③ stop()     停機：先停 task，再呼叫你；資源由管理器歸還
//
// 「跑」不在這裡：你的 loop 是一個 task，用 nc_task_register() 登記，
// 由 nc_tasks_boot() 依 layer 啟動、釘在你指定的核心。
//
// 規則（與「平常寫 FreeRTOS」不同的地方）：
//   · 不要 malloc：要的記憶體在 declare() 寫清楚
//   · 不要 xTaskCreate：用 nc_task_register()，否則沒人知道你跑在哪一核、哪一層
//   · 不要自己去 nc_plan_get()：start() 會把指標交進來
//   · 不要直接呼叫別的服務：透過共用的 handle / bus / 事件
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_SERVICE_MAX       16
#define NC_SERVICE_MAX_RES   8

// 一項資源需求
typedef struct {
    const char *name;        // "probe.hub"（全系統唯一）
    size_t      bytes;
    nc_plan_class_t cls;
    size_t      align;       // 0 → 32
} nc_res_req_t;

// 交給服務的資源視圖
typedef struct {
    void  *ptr;
    size_t len;
} nc_res_view_t;

typedef struct {
    const char *name;        // "probe"（唯一）
    int level;               // 小的先啟動；0 = 系統必需

    // ① 宣告：把需求寫進 out[]，回傳項數（<= max_res）。commit 之前呼叫。
    //    也可以在這裡 nc_task_register() 你的 loop（同樣是「開機宣告」）。
    int (*declare)(nc_res_req_t *out, int max_res);

    // ② 啟動：res[i] 對應你在 declare() 寫的第 i 項；拿到才呼叫。
    esp_err_t (*start)(const nc_res_view_t *res, int nres);

    // ③ 停止（可選）：此時 task 已停。
    void (*stop)(void);
} nc_service_t;

// 登記一個服務（靜態；開機時呼叫）
esp_err_t nc_service_add(const nc_service_t *svc);

// ① 讓每個服務宣告資源（會呼叫 declare()）→ 之後呼叫 nc_plan_commit()
int  nc_service_declare_all(void);

// ② carve 之後：逐服務取得資源 → start()；任何一項拿不到 → 這個服務不啟動，
//    其他服務照跑（逐項失敗）
int  nc_service_start_all(void);

// ③ 反向停止
void nc_service_stop_all(void);

// 查詢：回傳 JSON 陣列
size_t nc_service_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
