// 狀態管理器 v1：所有「會被改、而且可能要被永久化」的值都登記在這裡。
//
// 為什麼要有它（而不是散在各處寫 NVS／寫檔）：
//   1. **決定要不要落盤的地方只能有一個**。同一個事實有兩份旗標，遲早會互相矛盾
//      （例：`fs_init` 說已初始化、`fs_format` 說要重建，兩者同時為真）。
//   2. **寫入權限與時機是規則，不是 if**。誰可以寫（system / user / firmware 補值）
//      寫在哪裡，跟狀態本身一起宣告。
//   3. 之後要長成「三層狀態（hot／cold／sealed）＋字典」時，只是換 backend，
//      呼叫端的形狀不用改。
//
// 三種持久化：
//   NONE  = 只留在 RAM（reboot 就冇）
//   NVS   = 寫入 NVS（namespace / key 由 desc 指定）
//   FILE  = 交給註冊時提供的 save()（例：config 層 → JSON）
//
// 規則：**改了值不會自動落盤**。改完要明確叫 nc_state_commit()，
// 這樣「先做完一連串修改再一次性寫入」才不會寫到一半。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_STATE_MAX 12

typedef enum {
    NC_ST_PERSIST_NONE = 0,   // 只喺 RAM
    NC_ST_PERSIST_NVS,        // 落 NVS
    NC_ST_PERSIST_FILE,       // 交俾 save()
} nc_state_persist_t;

// 誰有權改（目前是**文件與記錄用**，不做權限拒絕；強制規則寫在各層的 load/save）。
typedef enum {
    NC_ST_BY_SYSTEM = 0,      // 開機流程／狀態機
    NC_ST_BY_USER,            // 用戶指令（NC4／HTTP）
    NC_ST_BY_FW_FILL,         // firmware 補值：只限用戶無法掌握的資訊（例如 cID 由 MAC 推導）
} nc_state_writer_t;

typedef struct nc_state nc_state_t;

typedef struct {
    const char         *name;      // "boot" / "cfg.system"
    nc_state_persist_t  persist;
    nc_state_writer_t   writer;
    void               *value;     // RAM 裡的真值（呼叫端擁有）
    size_t              size;
    const char         *nvs_ns;    // persist = NVS
    const char         *nvs_key;
    esp_err_t         (*save)(const void *v, size_t n);   // persist = FILE
} nc_state_desc_t;

// 登記（重名直接回舊的）。value 的初始值就是呼叫端先填好的預設。
nc_state_t *nc_state_add(const nc_state_desc_t *desc);

// 讀回所有 persist=NVS 的狀態（找不到就保留呼叫端的預設值）。
esp_err_t nc_state_load(void);

// 改值（整塊 copy）／只標 dirty（呼叫端已直接改 value）。
esp_err_t nc_state_set(nc_state_t *s, const void *v, nc_state_writer_t by);
void      nc_state_touch(nc_state_t *s);

// 把所有 dirty 的狀態落盤。成功才清 dirty；任何一個失敗都回該錯誤。
esp_err_t nc_state_commit(void);

// 給 STATUS 用（唯讀）
int         nc_state_count(void);
const char *nc_state_name(int idx);
const char *nc_state_persist_name(int idx);
bool        nc_state_is_dirty(int idx);

#ifdef __cplusplus
}
#endif
