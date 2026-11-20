// nc_px_json.c — 迷你 JSON 讀取器（見 nc_px_json.h）。
#include "nc_px_json.h"

#include <string.h>

void nc_px_js_open(nc_px_js_t *j, const char *text, size_t len)
{
    j->p = text;
    j->end = text + len;
    j->depth = 0;
}

bool nc_px_js_bool(nc_px_js_t *j, bool *out)
{
    nc_px_js_ws(j);
    if (out == NULL) {
        return false;
    }
    if ((size_t)(j->end - j->p) >= 4 && memcmp(j->p, "true", 4) == 0) {
        j->p += 4;
        *out = true;
        return true;
    }
    if ((size_t)(j->end - j->p) >= 5 && memcmp(j->p, "false", 5) == 0) {
        j->p += 5;
        *out = false;
        return true;
    }
    if ((size_t)(j->end - j->p) >= 4 && memcmp(j->p, "null", 4) == 0) {
        j->p += 4;
        *out = false;
        return true;
    }
    return false;
}

bool nc_px_js_at_end(nc_px_js_t *j)
{
    nc_px_js_ws(j);
    return j->p >= j->end;
}

// 只支援 schema 用到的子集：object／array／string／整數／true／false／null。
// 冇 DOM、冇 malloc、深度上限 8；全部係「讀咗就前進」，失敗就停。
void nc_px_js_ws(nc_px_js_t *j)
{
    while (j->p < j->end) {
        const char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->p++;
        } else {
            break;
        }
    }
}

bool nc_px_js_eat(nc_px_js_t *j, char c)
{
    nc_px_js_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

// 讀一個 JSON string；out 可以係 NULL（只食位）。太長就截斷。
bool nc_px_js_str(nc_px_js_t *j, char *out, size_t cap)
{
    nc_px_js_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char ch = *j->p++;
        if (ch == '\\') {
            if (j->p >= j->end) {
                return false;
            }
            const char e = *j->p++;
            switch (e) {
            case '"':  ch = '"';  break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/';  break;
            case 'n':  ch = '\n'; break;
            case 't':  ch = '\t'; break;
            case 'r':  ch = '\r'; break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            case 'u':                       // 只食位：路徑名唔會用到
                for (int k = 0; k < 4; k++) {
                    if (j->p >= j->end) {
                        return false;
                    }
                    j->p++;
                }
                ch = '?';
                break;
            default:
                return false;
            }
        }
        if (out != NULL && n + 1 < cap) {
            out[n++] = ch;
        }
    }
    if (j->p >= j->end) {
        return false;
    }
    j->p++;                                  // 收尾 "
    if (out != NULL) {
        out[n] = '\0';
    }
    return true;
}

// 讀整數（0..limit）。NC_PX_JSN_BAD 唔會前進；NC_PX_JSN_RANGE 已前進（呼叫者可以繼續讀落去）。
nc_px_jnum_t nc_px_js_num(nc_px_js_t *j, uint32_t limit, uint32_t *out)
{
    nc_px_js_ws(j);
    const char *const start = j->p;
    bool neg = false;
    if (j->p < j->end && *j->p == '-') {
        neg = true;
        j->p++;
    }
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
        j->p = start;
        return NC_PX_JSN_BAD;
    }
    uint64_t v = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        v = v * 10u + (uint64_t)(*j->p - '0');
        if (v > 0xFFFFFFFFull) {
            v = 0xFFFFFFFFull;
        }
        j->p++;
    }
    if (neg || v > limit) {
        return NC_PX_JSN_RANGE;
    }
    *out = (uint32_t)v;
    return NC_PX_JSN_OK;
}

bool nc_px_js_skip(nc_px_js_t *j);

static bool js_skip_nested(nc_px_js_t *j, char open, char close)
{
    if (++j->depth > 8) {
        return false;
    }
    if (!nc_px_js_eat(j, open)) {
        return false;
    }
    if (!nc_px_js_eat(j, close)) {
        for (;;) {
            if (open == '{' && (!nc_px_js_str(j, NULL, 0) || !nc_px_js_eat(j, ':'))) {
                return false;
            }
            if (!nc_px_js_skip(j)) {
                return false;
            }
            if (nc_px_js_eat(j, ',')) {
                continue;
            }
            if (nc_px_js_eat(j, close)) {
                break;
            }
            return false;
        }
    }
    j->depth--;
    return true;
}

static bool js_match(nc_px_js_t *j, const char *lit)
{
    const size_t n = strlen(lit);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n) != 0) {
        return false;
    }
    j->p += n;
    return true;
}

bool nc_px_js_skip(nc_px_js_t *j)
{
    nc_px_js_ws(j);
    if (j->p >= j->end) {
        return false;
    }
    const char c = *j->p;
    if (c == '"') {
        return nc_px_js_str(j, NULL, 0);
    }
    if (c == '{') {
        return js_skip_nested(j, '{', '}');
    }
    if (c == '[') {
        return js_skip_nested(j, '[', ']');
    }
    if (js_match(j, "true") || js_match(j, "false") || js_match(j, "null")) {
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        uint32_t v;
        if (nc_px_js_num(j, 0xFFFFFFFFu, &v) == NC_PX_JSN_BAD) {
            return false;
        }
        // 小數／指數：照跳到 token 尾（我們唔用，但唔想因為佢而整份壞）
        while (j->p < j->end && (strchr("0123456789.eE+-", *j->p) != NULL)) {
            j->p++;
        }
        return true;
    }
    return false;
}

bool nc_px_js_key(nc_px_js_t *j, char *key, size_t cap)
{
    return nc_px_js_str(j, key, cap) && nc_px_js_eat(j, ':');
}

