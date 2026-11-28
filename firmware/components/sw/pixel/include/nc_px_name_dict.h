// nc_px_name_dict — pixel 名稱字典（UTF-8 bytes → 穩定 id）。
//
// 目的：外部 JSON 保留完整中文／任意長度名稱；掃描後只把 name→id 存入字典，
// 內部 struct 用 id 影射，唔再喺每個 struct 重複 char name[24]。
//
// 政策（用家定案）：
//   · 正常只喺字典 lookup；名唔存在才 append（dirty）。
//   · 用 id 找到舊 entry，但實際 name 同實時 JSON 唔同 → **直接覆蓋該 id 的 name**。
//   · 字典檔缺失／損壞 = 重建；正常開機唔重建。
//   · 手動刪字典檔 = 下次重建。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PX_NAME_DICT_BAD_ID UINT32_MAX

typedef struct nc_px_name_dict nc_px_name_dict_t;

nc_px_name_dict_t *nc_px_name_dict_create(void);
void nc_px_name_dict_destroy(nc_px_name_dict_t *d);

// 缺失 → ESP_ERR_NOT_FOUND；壞檔／CRC → ESP_ERR_INVALID_STATE。
esp_err_t nc_px_name_dict_load(nc_px_name_dict_t *d, const char *path);
// atomic tmp → rename；dirty=false。
esp_err_t nc_px_name_dict_save(nc_px_name_dict_t *d, const char *path);

// 精確 name lookup（bytes，含 UTF-8）；唔存在回 BAD_ID。
uint32_t nc_px_name_dict_find(nc_px_name_dict_t *d, const char *name, size_t len);

// 存在 → 用返 id；唔存在 → append（*added=true）。
uint32_t nc_px_name_dict_intern(nc_px_name_dict_t *d, const char *name, size_t len,
                                bool *added);

// 用「已知 id」更新：
//   id 存在且 name 相同 → 回 id，唔改；
//   id 存在但 name 不同 → **覆蓋**，*updated=true；
//   id 唔存在 → append 新 id（*added=true）。
uint32_t nc_px_name_dict_intern_id(nc_px_name_dict_t *d, uint32_t id,
                                   const char *name, size_t len,
                                   bool *added, bool *updated);

const char *nc_px_name_dict_name(const nc_px_name_dict_t *d, uint32_t id, uint16_t *len);
uint32_t    nc_px_name_dict_count(const nc_px_name_dict_t *d);
bool        nc_px_name_dict_dirty(const nc_px_name_dict_t *d);

// 全域字典（pixel layer 共用；生命週期跨 layer clear／reload）。
nc_px_name_dict_t *nc_px_name_dict_global(void);
void nc_px_name_dict_global_reset(void);   // 測試／reload 用
const char *nc_px_name_dict_global_str(uint32_t id);
uint32_t    nc_px_name_dict_global_intern(const char *name, size_t len);
const char *nc_px_name_dict_global_intern_cstr(const char *name);

#ifdef __cplusplus
}
#endif
