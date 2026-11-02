#include "nc_cfg_patch.h"

#include <ctype.h>
#include <string.h>

static void skip_ws(const char *t, size_t n, size_t *p)
{
    while (*p < n && isspace((unsigned char)t[*p])) {
        (*p)++;
    }
}

// 跳過一個值（字串／數字／true/false/null／物件／陣列）
static bool skip_value(const char *t, size_t n, size_t *p)
{
    skip_ws(t, n, p);
    if (*p >= n) {
        return false;
    }
    const char c = t[*p];
    if (c == '"') {
        (*p)++;
        while (*p < n) {
            if (t[*p] == '\\') {
                (*p) += 2;
                continue;
            }
            if (t[*p] == '"') {
                (*p)++;
                return true;
            }
            (*p)++;
        }
        return false;
    }
    if (c == '{' || c == '[') {
        const char open = c;
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (*p < n) {
            const char d = t[*p];
            if (d == '"') {
                (*p)++;
                while (*p < n) {
                    if (t[*p] == '\\') {
                        (*p) += 2;
                        continue;
                    }
                    if (t[*p] == '"') {
                        break;
                    }
                    (*p)++;
                }
                if (*p >= n) {
                    return false;
                }
                (*p)++;
                continue;
            }
            if (d == open) {
                depth++;
            } else if (d == close) {
                depth--;
                if (depth == 0) {
                    (*p)++;
                    return true;
                }
            }
            (*p)++;
        }
        return false;
    }
    while (*p < n) {
        const char d = t[*p];
        if (d == ',' || d == '}' || d == ']' || isspace((unsigned char)d)) {
            return true;
        }
        (*p)++;
    }
    return true;
}

// 在物件內找 key；成功時 *p 在值的開頭。
static bool enter_object_key(const char *t, size_t n, size_t *p,
                             const char *key, size_t klen)
{
    skip_ws(t, n, p);
    if (*p >= n || t[*p] != '{') {
        return false;
    }
    (*p)++;
    for (;;) {
        skip_ws(t, n, p);
        if (*p >= n || t[*p] == '}') {
            return false;
        }
        if (t[*p] != '"') {
            return false;
        }
        (*p)++;
        const size_t ks = *p;
        while (*p < n) {
            if (t[*p] == '\\') {
                (*p) += 2;
                continue;
            }
            if (t[*p] == '"') {
                break;
            }
            (*p)++;
        }
        if (*p >= n) {
            return false;
        }
        const size_t ke = *p;
        (*p)++;
        skip_ws(t, n, p);
        if (*p >= n || t[*p] != ':') {
            return false;
        }
        (*p)++;
        skip_ws(t, n, p);

        if ((ke - ks) == klen && memcmp(t + ks, key, klen) == 0) {
            return true;    // *p 在值開頭
        }
        if (!skip_value(t, n, p)) {
            return false;
        }
        skip_ws(t, n, p);
        if (*p < n && t[*p] == ',') {
            (*p)++;
            continue;
        }
        return false;
    }
}

// 在陣列內找 index；成功時 *p 在元素的開頭。
static bool enter_array_index(const char *t, size_t n, size_t *p, int index)
{
    skip_ws(t, n, p);
    if (*p >= n || t[*p] != '[') {
        return false;
    }
    (*p)++;
    for (int i = 0;; i++) {
        skip_ws(t, n, p);
        if (*p >= n || t[*p] == ']') {
            return false;
        }
        if (i == index) {
            return true;
        }
        if (!skip_value(t, n, p)) {
            return false;
        }
        skip_ws(t, n, p);
        if (*p < n && t[*p] == ',') {
            (*p)++;
            continue;
        }
        return false;
    }
}

bool nc_cfg_patch_value(const char *text, const char *path, const char *value,
                        char *out, size_t out_cap, size_t *out_len)
{
    if (text == NULL || path == NULL || value == NULL || out == NULL || out_cap == 0) {
        return false;
    }
    const size_t n = strlen(text);
    const size_t vlen = strlen(value);

    size_t i = 0;
    skip_ws(text, n, &i);
    if (i >= n || text[i] != '{') {
        return false;
    }

    const char *p = path;
    bool first = true;
    while (p != NULL && *p != '\0') {
        const char *dot = strchr(p, '.');
        const size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0) {
            return false;
        }
        bool digits = true;
        for (size_t k = 0; k < len; k++) {
            if (!isdigit((unsigned char)p[k])) {
                digits = false;
                break;
            }
        }

        if (digits) {
            int idx = 0;
            for (size_t k = 0; k < len; k++) {
                idx = idx * 10 + (p[k] - '0');
            }
            if (!enter_array_index(text, n, &i, idx)) {
                return false;
            }
        } else {
            if (!enter_object_key(text, n, &i, p, len)) {
                return false;
            }
        }
        first = false;
        (void)first;

        if (dot == NULL) {
            const size_t vstart = i;
            size_t vend = i;
            if (!skip_value(text, n, &vend)) {
                return false;
            }
            const size_t need = vstart + (vlen + 2) + (n - vend) + 1;
            if (need > out_cap) {
                return false;
            }
            memcpy(out, text, vstart);
            size_t o = vstart;
            out[o++] = '"';
            memcpy(out + o, value, vlen);
            o += vlen;
            out[o++] = '"';
            memcpy(out + o, text + vend, n - vend);
            o += n - vend;
            out[o] = '\0';
            if (out_len != NULL) {
                *out_len = o;
            }
            return true;
        }
        p = dot + 1;
    }
    return false;
}
