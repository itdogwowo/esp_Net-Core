// nc_status.c — STATUS_GET 的外掛區塊註冊表。
//
// 刻意冇鎖：註冊只喺開機（單執行緒）做；之後唯讀。兩個 decoder 同時讀唔會踩。
#include "nc_status.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "status";

typedef struct {
    const char     *key;
    nc_status_fn_t  fn;
} entry_t;

static entry_t s_e[NC_STATUS_MAX];
static int     s_n;

esp_err_t nc_status_register(const char *key, nc_status_fn_t fn)
{
    if (key == NULL || key[0] == '\0' || fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_e[i].key, key) == 0) {
            ESP_LOGW(TAG, "status 區塊 \"%s\" 重複註冊 → 拒絕", key);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_n >= NC_STATUS_MAX) {
        ESP_LOGW(TAG, "status 區塊滿了（%d）→ 拒絕 \"%s\"", NC_STATUS_MAX, key);
        return ESP_ERR_NO_MEM;
    }
    s_e[s_n].key = key;
    s_e[s_n].fn = fn;
    s_n++;
    ESP_LOGI(TAG, "status 區塊 \"%s\" 已註冊（%d）", key, s_n);
    return ESP_OK;
}

int nc_status_count(void)
{
    return s_n;
}

// 同 main 的 keys_want 同一套語義：空／"all" = 全部；其餘逗號分隔。
static bool key_wanted(const char *keys, const char *name)
{
    if (keys == NULL || keys[0] == '\0' || strcmp(keys, "all") == 0) {
        return true;
    }
    const size_t n = strlen(name);
    const char *p = keys;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (strncmp(p, name, n) == 0) {
            return true;
        }
        while (*p != '\0' && *p != ',') {
            p++;
        }
    }
    return false;
}

void nc_status_append(const char *keys, char *buf, size_t cap, size_t *off, bool *first)
{
    if (buf == NULL || off == NULL || first == NULL || cap == 0) {
        return;
    }
    for (int i = 0; i < s_n; i++) {
        if (!key_wanted(keys, s_e[i].key)) {
            continue;
        }
        if (*off + 4 >= cap) {
            return;                    // 唔夠位：留住已寫的（主機要分批問）
        }
        if (!*first) {
            buf[(*off)++] = ',';
        }
        const int n = snprintf(buf + *off, cap - *off, "\"%s\":", s_e[i].key);
        if (n < 0 || (size_t)n >= cap - *off) {
            *first = false;
            return;
        }
        *off += (size_t)n;
        const size_t w = s_e[i].fn(buf + *off, cap - *off);
        if (w >= cap - *off) {
            *off = cap - 1;            // provider 寫爆：截在這裡（仍然係合法 JSON 尾巴）
        } else {
            *off += w;
        }
        buf[*off] = '\0';
        *first = false;
    }
}
