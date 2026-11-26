// NCSTATE schema 查詢／驗證：**全專案唯一一個地方**查 key 的型別、單位、範圍。
//
// 資料表本身由 tools/gen_state_keys.py 由 schema/state-keys.json 生成
// （nc_state_schema_data.[ch]），呢個模組只做查表＋驗證，冇第二份名單。
//
// 加／減能力（key／unit）：
//   1. 改 schema/state-keys.json（加 key 要跟 id 的 unit nibble）
//   2. python3 tools/gen_state_keys.py
//   3. rebuild —— driver／工具經呢個 API 查到新 key，唔使逐處改名單。
//
// 「宣告」同「實際」唔一致時（例如 boot 由 cfg 造 record 的型別同 schema 唔同），
// nc_state_schema_check() 會回明示錯誤碼；boot 會 log 出嚟，唔會靜靜接受。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nc_state_blob.h"
#include "nc_state_schema_data.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── 查表 ──────────────────────────────────────────────────────
const nc_state_keydef_t *nc_state_key(uint32_t id);            // 未知 → NULL
const nc_state_keydef_t *nc_state_key_name(const char *name);  // "NC_KEY_LED_PX"
const nc_state_keydef_t *nc_state_key_at(uint16_t i);          // 逐筆列舉（工具／status）
uint16_t nc_state_key_count(void);

const nc_state_unitdef_t *nc_state_unit(uint8_t id);           // NC_UNIT_*
const nc_state_unitdef_t *nc_state_unit_at(uint16_t i);
const nc_state_unitdef_t *nc_state_unit_of(const nc_state_keydef_t *def);
uint16_t nc_state_unit_count(void);

// 型別名（"uint"／"sint"／"bool"／"bytes"）；未知回 "?"
const char *nc_state_type_name(uint8_t type);

// enum 值 → 名（例：LED_BACKEND 的 1 → "i80"）；唔係 enum／超出範圍回 NULL。
const char *nc_state_enum_name(const nc_state_keydef_t *def, uint32_t v);

// ── 驗證（set 同 boot 共用同一條規則） ─────────────────────────
// enum 名 → index（例："ws2812" → 0）；唔係 enum／冇呢個名 → -1。
// 用途：JSON 寫的是人讀名（driver.json 的 "format": "ws2812"），state 只存 index。
int nc_state_enum_id(const nc_state_keydef_t *def, const char *name);

// 數值（uint／sint／bool／enum）：min <= v <= max。
bool nc_state_key_in_range(const nc_state_keydef_t *def, int64_t v);

// 字串：len（唔含 NUL）要 <= max_len。
bool nc_state_key_str_ok(const nc_state_keydef_t *def, size_t len);

// record 同 schema 對唔對得上（boot 由 cfg 造 record 後驗一次，防 code／schema drift）。
// 回 0 = OK；NC_STATE_CHK_* = 唔同種類的錯。
enum {
    NC_STATE_CHK_OK = 0,
    NC_STATE_CHK_UNKNOWN_KEY = 1,   // schema 冇呢個 key（加咗 code 未加 schema）
    NC_STATE_CHK_TYPE = 2,          // blob 型別同 schema 唔同（例如 uint 當 sint）
    NC_STATE_CHK_RANGE = 3,         // 值超出 schema 的範圍／字串太長
};
uint8_t nc_state_schema_check(const nc_state_rec_t *rec);
const char *nc_state_schema_check_name(uint8_t code);

// schema 本身的版本／CRC（CRC = canonical schema json 的 crc32；build 同 FAT 對數用）。
uint16_t nc_state_schema_version(void);
uint32_t nc_state_schema_crc(void);

#ifdef __cplusplus
}
#endif
