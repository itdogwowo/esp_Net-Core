// NCSTATE schema 的 runtime 表（宣告）。
//
// **本檔由 tools/gen_state_keys.py 生成，唔好手改**：改 schema/state-keys.json 再跑工具。
#pragma once

#include <stdint.h>

#include "nc_state_blob.h"
#include "nc_state_keys.h"   // NC_UNIT_*／NC_KEY_*

#define NC_STATE_SCHEMA_VERSION 2u
#define NC_STATE_SCHEMA_CRC     0xf22a74f7u
#define NC_STATE_KEY_COUNT      25u
#define NC_STATE_UNIT_COUNT     4u

// 一筆 key 的完整描述（id／能力群組／型別／可接受範圍／人讀資料）。
// type 用 nc_state_blob.h 的 NC_STATE_T_*；str 的長度上限在 max_len。
typedef struct {
    uint16_t id;
    uint8_t  unit;        // NC_UNIT_*
    uint8_t  type;        // NC_STATE_T_UINT／SINT／BOOL／BYTES
    int32_t  min;         // 數值範圍（str 用 0..max_len）
    int32_t  max;
    uint16_t max_len;     // str 專用（唔含 NUL；其他 type 為 0）
    uint8_t  n_enum;      // enum 值個數（其他 type 為 0）
    const char *const *enum_names;   // enum：0..n_enum-1 的名字
    const char *name;     // NC_KEY_*
    const char *path;     // 人讀：config JSON 的路徑
    const char *desc;
} nc_state_keydef_t;

typedef struct {
    uint8_t     id;
    const char *name;
    const char *desc;
} nc_state_unitdef_t;

extern const nc_state_keydef_t  nc_state_schema_keys[NC_STATE_KEY_COUNT];
extern const nc_state_unitdef_t nc_state_schema_units[NC_STATE_UNIT_COUNT];
