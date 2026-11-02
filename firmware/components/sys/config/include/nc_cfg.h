// 分層設定的讀取器：一個層一個檔（/data/config/<layer>.json）。
//
// 規則（與 docs/03_notes/config-layers.md 一致）：
//   · 缺檔 / 壞檔 → 回 NULL，呼叫端用預設（不是錯誤）
//   · firmware 只讀；唯一的寫入是「補值」（例如 cID 首次自動填入）
//   · 每個層只讀自己的檔，不跨檔
#pragma once

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfg_file cfg_file_t;

// 開檔並解析；"system" → /data/config/system.json。失敗回 NULL。
cfg_file_t *nc_cfg_open(const char *layer);
void nc_cfg_close(cfg_file_t *cfg);
const cJSON *nc_cfg_root(const cfg_file_t *cfg);

// 便利讀取（root 可為 NULL；缺鍵回傳預設）
const char *nc_cfg_str(const cJSON *root, const char *key, const char *def);
int nc_cfg_int(const cJSON *root, const char *key, int def);

// 補值：把 key 設成字串並寫回（先 .tmp 再 rename，舊檔留 .bak）。
// 只用在「檔案缺這個值」的情況；會重排一次格式。
bool nc_cfg_set_string(const char *layer, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
