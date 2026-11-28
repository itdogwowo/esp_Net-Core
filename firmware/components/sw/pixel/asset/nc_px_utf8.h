// UTF-8 小工具：固定 char array 的截斷要停在 codepoint 邊界，唔可以切一半。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 回傳 <= n 的最大長度，保證 s[0..ret) 係完整 UTF-8（遇到非法序列就截到之前）。
static inline size_t nc_px_utf8_trim(const char *s, size_t n)
{
    size_t cut = n;
    while (cut > 0) {
        size_t start = cut - 1;
        while (start > 0 && ((unsigned char)s[start] & 0xC0u) == 0x80u) {
            start--;
        }
        const unsigned char lead = (unsigned char)s[start];
        size_t need = 0;
        if ((lead & 0x80u) == 0) {
            need = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            need = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            need = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            need = 4;
        } else {
            return start;                 // 非法 lead：截到之前
        }
        if (start + need <= cut) {
            return cut;
        }
        cut = start;
    }
    return 0;
}

// 寫一個 codepoint 到 out（cap 含 NUL）；空間不足回 false 且唔寫。
static inline bool nc_px_utf8_put(char *out, size_t cap, size_t *n, uint32_t cp)
{
    uint8_t b[4];
    size_t len;
    if (cp <= 0x7Fu) {
        b[0] = (uint8_t)cp;
        len = 1;
    } else if (cp <= 0x7FFu) {
        b[0] = (uint8_t)(0xC0u | (cp >> 6));
        b[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        len = 2;
    } else if (cp <= 0xFFFFu) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            return false;                 // surrogate：唔支援
        }
        b[0] = (uint8_t)(0xE0u | (cp >> 12));
        b[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        len = 3;
    } else if (cp <= 0x10FFFFu) {
        b[0] = (uint8_t)(0xF0u | (cp >> 18));
        b[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        len = 4;
    } else {
        return false;
    }
    if (out != NULL && *n + len + 1u > cap) {
        return false;
    }
    if (out != NULL) {
        for (size_t i = 0; i < len; i++) {
            out[*n + i] = (char)b[i];
        }
    }
    *n += len;
    return true;
}
