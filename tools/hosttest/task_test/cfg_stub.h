// cfg_stub：host test 用的假 tasks.json
//
// 用法：
//     cfg_stub_set(entries, n);   // 安裝一份 config（n = 0 等於「沒有 tasks.json」）
//     ... nc_task_declare_all(); ...
//     cfg_stub_appended(i);       // 看補寫進去的條目長什麼樣
//
// 只認得 "tasks" 這一層；其他層一律當缺檔（回 NULL）。
#pragma once

#include <stdbool.h>

typedef struct {
    const char *name;
    int level;
    bool has_level;      // false = 這個條目沒寫 level
    int aff[2];          // -1 = JSON null（測「只有一邊有值」用）
    bool has_aff;        // false = 這個條目沒寫 affinity
} cfg_stub_task_t;

void cfg_stub_set(const cfg_stub_task_t *tasks, int n);
void cfg_stub_clear(void);

// 補寫觀察點（nc_cfg_append_obj 收到的字串）
int cfg_stub_appended_count(void);
const char *cfg_stub_appended(int i);
