// 純文字層的 JSON 值替換（不依賴 cJSON、可 host 測試）。
//
// 目的：改一個值，但不動使用者的排版、鍵順序、_comment 位置。
// 路徑語法：a.b.0.c（物件鍵用名字、陣列用索引），例如：
//     nc_cfg_patch_value(text, "cID", "A57C", ...)
//     nc_cfg_patch_value(text, "nc4.links.0.rx.decode", "decode1", ...)
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 把 path 指向的值換成 value（字串；內部會加引號與轉義）。
// 回傳 true 時 out 是新的全文（以 '\0' 結尾），out_len 不含結尾。
// 找不到路徑 / 緩衝不足 / 文字不是合法結構 → false（呼叫端可 fallback）。
bool nc_cfg_patch_value(const char *text, const char *path, const char *value,
                        char *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
