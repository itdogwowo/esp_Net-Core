// nc_px_json.h — 迷你 JSON 讀取器（共用）：object／array／string／整數／true／false／null。
//
// 為什麼共用：effects／map／modes／registry 四層都係 JSON，而且都要「有界、冇 malloc、
// 壞資料即刻停」。與其抄四份，不如一份；語義（深度上限 8、超範圍回 RANGE、壞 token 停）
// 由 asset 層先用開，host test 釘住。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *p;
    const char *end;
    int         depth;
} nc_px_js_t;

typedef enum {
    NC_PX_JSN_OK = 0,
    NC_PX_JSN_BAD,      // 唔係數字（cursor 冇動 → 呼叫者可以 skip）
    NC_PX_JSN_RANGE,    // 係數字但超範圍（cursor 已過）
} nc_px_jnum_t;

void          nc_px_js_open(nc_px_js_t *j, const char *text, size_t len);
void          nc_px_js_ws(nc_px_js_t *j);
bool          nc_px_js_eat(nc_px_js_t *j, char c);
bool          nc_px_js_str(nc_px_js_t *j, char *out, size_t cap);
char         *nc_px_js_str_dup(nc_px_js_t *j);   // malloc；呼叫者 free；失敗 NULL
nc_px_jnum_t  nc_px_js_num(nc_px_js_t *j, uint32_t limit, uint32_t *out);
bool          nc_px_js_skip(nc_px_js_t *j);
bool          nc_px_js_key(nc_px_js_t *j, char *key, size_t cap);
bool          nc_px_js_at_end(nc_px_js_t *j);
bool          nc_px_js_bool(nc_px_js_t *j, bool *out);

#ifdef __cplusplus
}
#endif
