// 任務與任務管理器 —— 這是系統裡「跑東西」的唯一入口。
//
// 對照 mp_Net-Core 的 task.py / task_manager.py：
//   一個 Task  = 身份 + 資源 + 生命週期 + 一到數個 loop
//   TaskManager = 登記 → 宣告資源 → 依 level 啟動 → 依 layer 開 loop → 量測 → 狀態
//
// **task 與 service 是同一件事**（舊版分了兩層，是命名造成的誤會）：
// 兩者都只是「會被管理器啟動、可能跑迴圈、可能吃資源的東西」。
// 所以這裡只有一種單位：`nc_task_t`。差別只剩 kind：
//
//   SYSTEM  韌體的一部分（硬體/通道/指令表），固定順序、不可卸載
//   NORMAL  一般任務（含以前的「服務」）
//   EFFECT  由檔案載入的任務（.fxe），runtime 可 attach/detach
//
// 你只要寫一個 `nc_task_t` 並呼叫 `nc_task_register()`，其餘（plan 宣告、
// loop 建立、層級排序、核心綁定、停止、量測、狀態）都由管理器做。
//
// **指令不在這裡**：NC4 指令是核心執行能力（action），由 decoder 直接持有，
// 開機時註冊一次；任務只負責「把它們恆常化、排程化」（見 docs/03_notes/writing-a-task.md §3.2）。
//
// ⚠️ 執行緒：管理器沒有鎖。開機路徑由 app_main 單執行緒走完；runtime 的
//    attach/detach 目前只從 fx task 呼叫；status 讀取可能看到「正在搬家」的瞬間
//    （最多少一個/重複一個），不會越界。要從 decode task 直接載入 .fxe 之前先補鎖。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nc_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_TASK_MAX        20      // 管理器最多幾個任務
#define NC_TASK_LAYERS     8       // 開機層（0 = 最先）
#define NC_TASK_LOOPS_MAX  2       // 一個任務最多幾個 loop（例：兩核各一個解碼）
#define NC_TASK_MEM_MAX    8
#define NC_TASK_CLAIM_MAX  8
#define NC_TASK_PROV_MAX   8
#define NC_PROVIDE_MAX     24
#define NC_TASK_CORE_ANY   (-1)

typedef enum {
    NC_TASK_SYSTEM = 0,   // 硬體 / driver / 通道 / 指令表：固定順序、不可卸載
    NC_TASK_NORMAL,       // 一般任務（以前的 service 也是這個）
    NC_TASK_EFFECT,       // 檔案載入的任務（.fxe），runtime 可換
    NC_TASK_KIND_COUNT
} nc_task_kind_t;

const char *nc_task_kind_name(nc_task_kind_t kind);
bool nc_task_kind_parse(const char *s, nc_task_kind_t *out);
// 可以由 attach 進來的 kind（system 不行：它由韌體決定、不可卸載）
bool nc_task_kind_is_runtime(nc_task_kind_t kind);

// ── 資源（declare → commit → 由 on_start 接收）──
typedef struct {
    const char *name;        // 全系統唯一："fx.arena"
    size_t      bytes;
    nc_plan_class_t cls;
    size_t      align;       // 0 → 32
} nc_task_res_t;

typedef struct {
    void  *ptr;
    size_t len;
} nc_task_res_view_t;

// ── 接線宣告（誰要什麼）──
typedef struct {
    const char *id;          // "pin:16" / "bus:spi1" / "display:tft"
    bool exclusive;
} nc_task_claim_t;

// ── 對外提供（別人可以讀什麼）──
typedef enum {
    NC_PROVIDE_BUF = 0,      // 一塊記憶體
    NC_PROVIDE_JSON,         // 即時查詢
    NC_PROVIDE_U32,          // 單一數值
} nc_provide_kind_t;

const char *nc_provide_kind_name(nc_provide_kind_t kind);

typedef struct {
    const char *name;        // 全系統唯一："nc4.rx" / "fx.info"
    nc_provide_kind_t kind;
    const char *desc;
    bool   (*buf)(void **ptr, size_t *bytes);   // BUF：回 false = 目前沒有
    size_t (*json)(char *out, size_t cap);      // JSON
    bool   (*u32)(uint32_t *out);               // U32
} nc_task_provide_t;

// ── 一個 loop（任務的執行單位）──
// 通常一個任務只有一個 loop；nc4 這種「兩核各一個解碼迴圈」才會有兩個。
//
// core 完全由你選（-1 = 不指定）。框架不做「硬體歸屬 → 強制某核」這種事：
//   · USB：TinyUSB 的 ISR 會留在 install 的那一核（通常是 core0），
//     但 tud_task/CDC 讀寫可以跑在另一核 —— 只要「同一時間只有一個 task 呼叫 TinyUSB」。
//   · Flash：寫入時 IDF 會用 IPC 把另一核停住（不是限制你的核，是全局停頓），
//     所以兩個核都能寫，只是別把它放進高頻迴圈。
//   · 共用週邊的互斥屬於 claims[]（宣告「我要獨占 pin:16」），不是靠釘核解決。
typedef struct {
    const char *name;        // FreeRTOS task 名（全系統唯一）
    void (*fn)(void *arg);   // 迴圈本體；用 nc_task_exit_requested() 判斷要不要結束
    void *arg;
    uint32_t stack;
    UBaseType_t prio;
    int core;                // NC_TASK_CORE_ANY / 0 / 1
    int layer;               // 0..NC_TASK_LAYERS-1，小的先啟動
    uint32_t period_ms;      // >0 = 管理器每輪幫你延遲；0 = loop 自己阻塞
    uint32_t budget_us;      // 每輪預算（0 = 不統計超支）
    bool run_once;           // 只跑一次（開機自測那種）
    // 房東自己話事嘅開關：回 false = 呢個 loop 唔建立（連棧都唔配置）。
    // NULL = 永遠開。config 管唔到 —— 呢個係固件宣告嘅一部分。
    bool (*enabled)(void);
} nc_task_loop_t;

// ── 一個任務的完整宣告 ──
// 全部欄位都可以留空（NULL / 0），只有 name、kind 必填。
typedef struct nc_task {
    const char *name;        // 全系統唯一
    nc_task_kind_t kind;
    int level;               // 小的先宣告、先啟動

    // 資源
    const nc_task_res_t *mem; // 靜態需求（可留空）
    int nmem;
    int (*declare)(nc_task_res_t *out, int max);  // 動態需求（照實際硬體算）

    // 接線 / 對外提供（指令不在這裡：動作是 decoder 的事，見 docs）
    const nc_task_claim_t *claims;
    int nclaims;
    const nc_task_provide_t *provides;
    int nprovides;

    // 執行
    const nc_task_loop_t *loops;
    int nloops;

    // 生命週期：資源到手 → on_start；loop 結束 → on_stop
    esp_err_t (*on_start)(const nc_task_res_view_t *res, int nres);
    void (*on_stop)(void);
} nc_task_t;

// ═══════════════════════ 任務管理器 ═══════════════════════
// 「註冊 → 宣告 → 啟動 → 開 loop」四步。開機由 main 依序呼叫；
// runtime 只有 effect 可以 attach / detach。

void nc_task_reset(void);                                   // 清空（重開機用）

// ① 登記一個任務（只登記，不啟動）。重名回 ESP_ERR_INVALID_STATE。
esp_err_t nc_task_register(const nc_task_t *task);

// 補寫：把「固件有／runtime attach 了、但 tasks.json 沒有」的租戶任務補進去。
int nc_task_sync_cfg(void);

// ② 宣告資源 → nc_plan（同時跑帳本檢查：重名 / 接線）
int nc_task_declare_all(void);

// ③ 資源到手 → on_start → 準備 loop（還不開 loop）
int nc_task_start_all(void);

// ④ 依 layer 開 loop（0 先）。可重複呼叫，已開的跳過。
int nc_task_boot(void);

// 反向：停 loop → on_stop → 收回 provide
void nc_task_stop_all(void);

// ── runtime（只有 kind = EFFECT）──
// attach：檢查 → 登記 → on_start → 直接開 loop。
// 記憶體：plan 已 commit，runtime 任務請留空 mem[]；要用記憶體就從父層 arena 切。
esp_err_t nc_task_attach(const nc_task_t *task);
esp_err_t nc_task_detach(const char *name);

// ── 單一任務控制（管理器代管）──
esp_err_t nc_task_start(const char *name);                 // 開這個任務的所有 loop
esp_err_t nc_task_stop(const char *name, uint32_t wait_ms);// 合作式停止（等它自己返回）
esp_err_t nc_task_enable(const char *name, bool on);       // 暫停/恢復（loop 還活著，只是不呼叫）
// 冇 nc_task_set_core()：**跑在哪個核是開機時決定的**（宣告的 affinity ＋ router），
// 執行期換核會同 router 的決定打架，而且「停 → 改 → 起」中間的狀態極難收拾。
// 要換核就改宣告／改 config，然後重開機。
bool      nc_task_exit_requested(void);                    // loop 裡查：有人請你結束

// ── 查詢 / 狀態 ──
const nc_task_t *nc_task_find(const char *name);
bool nc_task_started(const char *name);
nc_task_kind_t nc_task_kind_of(const char *name);
int  nc_task_count(void);

// 一張表（開機印一次）：name / kind / level / mem / loops(核心·層)
size_t nc_task_ledger(char *buf, size_t cap);
// 狀態：任務 + 資源 + 每個 loop 的實際核心與量測（count / last / avg / max / over / wrap）
size_t nc_task_json(char *buf, size_t cap);

// ── 對外提供的查詢（消費端）──
// 只查「已啟動任務」的 provide；找不到（或提供者已卸載）一律回 false / 0 / NULL。
// BUF 的生命週期綁在提供者身上：拿到之後不要跨它的 stop/detach 使用。
const nc_task_provide_t *nc_provide_find(const char *name);
const char *nc_provide_owner(const char *name);
bool   nc_provide_buf(const char *name, void **ptr, size_t *bytes);
size_t nc_provide_json(const char *name, char *out, size_t cap);
bool   nc_provide_u32(const char *name, uint32_t *out);
int    nc_provide_count(void);
size_t nc_provide_list_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
