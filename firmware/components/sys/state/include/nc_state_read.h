// NCSTATE 讀取 API：driver／config 由 state store 拿值（P3）。
//
// 語意：**只回傳 store 內真正有嘅值**（回 true）或「冇」（回 false）。
//   - false 唔代表錯誤：可能係 sparse 冇存／未 sync；呼叫端用自己嘅預設／cfg。
//   - 唔會回「零」代表「冇」；型別唔啱都當冇（唔靜默轉型）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool nc_state_read_u32(uint16_t key, uint32_t *out);
bool nc_state_read_i32(uint16_t key, int32_t *out);
bool nc_state_read_bool(uint16_t key, bool *out);

// 字串：store 內 bytes 複製到 buf（保證 NUL）；cap 唔夠／太長 → false（唔截斷）。
bool nc_state_read_str(uint16_t key, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
