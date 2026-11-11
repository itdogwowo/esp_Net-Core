// 分層設定：一個層一個檔（/data/config/<layer>.json），一個層一個 struct。
//
// 三個階段（不要混在一起）：
//   seed  —— 分區空白時，用「最基礎的形狀」產生第一份 JSON（一次性）
//   load  —— 開機讀一次，JSON → struct；之後全系統只讀 struct
//   save  —— 用戶明確要求時，struct → JSON（目前只有「補值」，全量寫入下一步）
//
// 規則（2026-11-06 定案）：
//   · **預設不是常駐覆蓋層**：寫完底座之後 JSON 就是唯一真相。
//   · 缺檔 → 重建底座；壞檔 → 整層用預設（RAM），**唔改用戶的檔**。
//   · 缺欄位／型別錯／超範圍 → 用預設（RAM），只計數。
//   · firmware 只會寫「用戶無法掌握」的值（cID 由 MAC 推導、tasks 補條目）。
//   · 所有設定都是**重啟後才生效**。
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "nc_cfg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cfg_file cfg_file_t;

// ───────────────────────── 檔案（低階） ─────────────────────────

// 開檔並解析；"system" → /data/config/system.json。
// exists 非 NULL 時回報「檔案本身在不在」（用來分辨「缺檔」同「壞檔」）。
cfg_file_t *nc_cfg_open_ex(const char *layer, bool *exists);
cfg_file_t *nc_cfg_open(const char *layer);
void nc_cfg_close(cfg_file_t *cfg);
const cJSON *nc_cfg_root(const cfg_file_t *cfg);

const char *nc_cfg_str(const cJSON *root, const char *key, const char *def);
int nc_cfg_int(const cJSON *root, const char *key, int def);

// 補值：把 key 設成字串並寫回（先 .tmp 再 rename，舊檔留 .bak）。
// 只用在「用戶無法掌握」的資訊（目前是 cID）；不重排其他內容。
bool nc_cfg_set_string(const char *layer, const char *key, const char *value);

// 補條目：把一個 JSON 物件插進 array 尾端（保留原排版，只加不改）。
// 用於 tasks.json：固件有、config 沒有的任務。
bool nc_cfg_append_obj(const char *layer, const char *array_key, const char *obj_json);

// ───────────────────────── 型別化（struct = schema） ─────────────────────────

// 預設（最基礎的形狀）。指標有效期限 = 程式執行期。
const nc_system_cfg_t *nc_cfg_defaults_system(void);
const nc_tasks_cfg_t  *nc_cfg_defaults_tasks(void);
const nc_hardware_cfg_t *nc_cfg_defaults_hardware(void);

// 依 kind 給一條 link 的 rx 預設（config 只寫 name/kind/enable 時用）。
void nc_cfg_rx_default(nc_link_kind_t kind, nc_link_rx_t *rx);

// 純解析：cJSON root → struct（起點是預設）。root = NULL → 全部預設、missing=1。
// 抽出來是為了 host test 可以不用檔案系統直接驗。
esp_err_t nc_cfg_apply_system(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st);
esp_err_t nc_cfg_apply_tasks(const cJSON *root, nc_tasks_cfg_t *out, nc_cfg_stat_t *st);
esp_err_t nc_cfg_apply_hardware(const cJSON *root, nc_hardware_cfg_t *out, nc_cfg_stat_t *st);

// 開機載入一次（永遠回 ESP_OK：最差 = 全部預設）。
// hw_base_json = build-time 的 wiring.json；用戶 /data/config/hardware.json 逐欄位覆蓋。
esp_err_t nc_cfg_boot_ex(const char *hw_base_json, size_t hw_base_len);
esp_err_t nc_cfg_boot(void);

// 之後全系統只讀這些。
const nc_system_cfg_t *nc_cfg_system(void);
const nc_tasks_cfg_t  *nc_cfg_tasks(void);
const nc_hardware_cfg_t *nc_cfg_hardware(void);
const nc_cfg_stat_t   *nc_cfg_stat_of(const char *layer);   // "system" / "tasks"

// 查一條 link / 一列 route（找不到回 NULL）
const nc_link_cfg_t  *nc_cfg_link(const char *name);
const nc_route_cfg_t *nc_cfg_route(const char *name);

// 一層的載入結果是否「有問題」（缺檔／壞檔）—— 給 degraded 用。
bool nc_cfg_layer_bad(const char *layer);

// STATUS 用：{"sys":{"miss":0,"bad":0,"def":0,"inv":0,"unk":0},"tasks":{...}}
void nc_cfg_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
