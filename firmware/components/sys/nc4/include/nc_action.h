// Action 管理器：指令表的「統計 → 一次配置 → 填入 → 封存」。
//
// 為什麼要有這一層
// ----------------
// 以前 decoder 內部有一個寫死的 `nc_entry_t table[48]`，各模組用
// `nc4_decoder_on_owner()` 逐條往裡面塞。問題：
//   1. 48 是魔術數字 —— 沒人知道為什麼是 48，滿了只會靜靜失敗（真的發生過）。
//   2. 「要先支援多少」其實是靜態已知的：每條指令都寫在某個模組的宣告表裡。
//   3. 一邊註冊一邊長大會造成碎片；先數清楚再一次性配置就不會。
//
// 所以：模組**宣告**（const 表，放 flash），管理器在開機時統計總數，
// 一次過 malloc 剛好大小的陣列，填入、按 cmd 排序、封存。
// 加一條指令 = 在自己的表多寫一行 → 總數自動變大，不用改任何常數。
//
// 執行期註冊 API（nc4_decoder_on / on_owner / off_owner）已刪除 —— 表是靜態的，
// 本來就不該在執行期改變；留著只會讓下一個人以為可以註冊而靜靜失敗。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc4.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t      cmd;
    nc4_handler_t fn;
    const char   *owner;   // 誰的指令（模組名），給帳本／STATUS 用
} nc_action_t;

// 一個模組的指令表（靜態、const、放 flash）
typedef struct {
    const char        *svc;     // 模組名（"core" / "ota" / "file" / "test"）
    const nc_action_t *list;
    size_t             count;
} nc_action_table_t;

#define NC_ACTION_COUNT(_list) (sizeof(_list) / sizeof((_list)[0]))

// 用這個巨集宣告表：count 一定等於陣列長度，冇人手寫錯的機會。
//（手寫 .count 是唯一可以令「統計」同「填入」數唔一致的途徑，直接封死。）
//
// **component 自己現身**：巨集在 app_main 之前（constructor）把表交去管理器。
// 管理器一開始收在 8 格靜態表，滿了就配更大的 —— **冇固定上限**。
// 有 link 進來就有這條表、冇 link 就冇；main 唔需要「有哪些模組」的清單。
#define NC_ACTION_TABLE_DEF(_var, _svc, _list) \
    const nc_action_table_t _var = { \
        .svc = (_svc), .list = (_list), .count = NC_ACTION_COUNT(_list), \
    }; \
    static void __attribute__((constructor, used)) _nc_action_reg_##_var(void) \
    { \
        nc_action_add(&_var); \
    }

// 由 constructor 呼叫：把一條表交去管理器（main 之前跑完）。
void nc_action_add(const nc_action_table_t *t);

// 用「所有已登記的表」建立指令表（統計 → 一次 malloc → 填入 → 排序）。
// main 只需要叫這一個；唔需要知道有幾多張表、係邊個模組。
esp_err_t nc_action_init_all(void);

// 低階入口（保留給測試／特殊情況）：自己提供表清單。
esp_err_t nc_action_init(const nc_action_table_t *const *tables, size_t n_tables);

size_t nc_action_count(void);
size_t nc_action_bytes(void);          // 實際配置了幾多 bytes（給 STATUS）
const nc_action_t *nc_action_all(void); // 排序後的陣列（給遍歷用）

bool nc_action_at(size_t i, uint16_t *cmd, const char **owner, nc4_handler_t *fn);
nc4_handler_t nc_action_find(uint16_t cmd);   // 二分搜尋

#ifdef __cplusplus
}
#endif
