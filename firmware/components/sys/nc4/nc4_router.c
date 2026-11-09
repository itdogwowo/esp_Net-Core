// NC4 路由器：線 → 核（＋這條線收到幀之後要唔要轉出去）。
// 純邏輯、零依賴（唔用 ESP_LOG），所以 host 都跑得。
//
// 只做四件事：驗表、記表、查表、答轉寄目的地。唔建立 bus，唔建立任務，
// 唔配記憶體 —— 佢只答問題，由 callers 去做嘢。所以佢冇生命週期，亦都冇嘢要還。

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nc4_router.h"

static nc4_route_t s_routes[NC4_ROUTE_MAX];
static char s_names[NC4_ROUTE_MAX][NC4_ROUTE_NAME_MAX];   // 線名自己存，唔借 caller 嘅指標
static char s_out_names[NC4_ROUTE_MAX][NC4_ROUTE_OUT_MAX][NC4_ROUTE_NAME_MAX];
static int s_count;
static bool s_set;
static bool s_keep[2];

static bool core_ok(int core)
{
    return core == 0 || core == 1;
}

static bool name_ok(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return false;
    }
    // 太長就拒絕，唔截斷：截斷會令 "UART0" 同 "UART1" 靜靜變成同一條線。
    return strlen(line) < NC4_ROUTE_NAME_MAX;
}

// "decode0" / "decode1" → true。其他 decodeN（例如 decode2）當普通名字處理，
// 由建立通道那邊決定「未登記 → 警告＋忽略」——唔喺呢層假裝佢係一個核。
static bool decode_name(const char *s, int *core)
{
    if (s == NULL || strncmp(s, "decode", 6) != 0) {
        return false;
    }
    if (s[6] != '0' && s[6] != '1') {
        return false;
    }
    if (s[7] != '\0') {
        return false;
    }
    if (core != NULL) {
        *core = s[6] - '0';
    }
    return true;
}

void nc4_router_reset(void)
{
    memset(s_routes, 0, sizeof(s_routes));
    memset(s_names, 0, sizeof(s_names));
    memset(s_out_names, 0, sizeof(s_out_names));
    s_count = 0;
    s_set = false;
    s_keep[0] = s_keep[1] = false;
}

esp_err_t nc4_router_add(const char *line, int core)
{
    if (!name_ok(line) || !core_ok(core)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= NC4_ROUTE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_routes[i].line, line) == 0) {
            return ESP_ERR_INVALID_STATE;   // 一條線只可以有一個核
        }
    }
    memcpy(s_names[s_count], line, strlen(line) + 1);
    s_routes[s_count].line = s_names[s_count];
    s_routes[s_count].core = core;
    s_routes[s_count].n_out = 0;
    s_count++;
    s_set = true;
    return ESP_OK;
}

esp_err_t nc4_router_set(const nc4_route_t *routes, int n)
{
    if (routes == NULL || n <= 0 || n > NC4_ROUTE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // 先驗完先落地：一張半截嘅路由表比冇表更難查。
    for (int i = 0; i < n; i++) {
        if (!name_ok(routes[i].line) || !core_ok(routes[i].core)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (routes[i].n_out > NC4_ROUTE_OUT_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        // out 只接受兩種東西：decodeN（本地執行）或 link 名。
        // 名字是否「真的有這條 link」不在此驗 —— router 唔應該認識 transport，
        // 開機時由 main 的 link 稽核負責（未登記 → 警告）。
        for (uint8_t k = 0; k < routes[i].n_out; k++) {
            const char *s = routes[i].out[k];
            if (!name_ok(s)) {
                return ESP_ERR_INVALID_ARG;
            }
            int dc = NC4_ROUTE_NONE;
            if (decode_name(s, &dc) && dc != routes[i].core) {
                return ESP_ERR_INVALID_ARG;   // decode1 喺 out 但 core 寫 0 → 矛盾
            }
        }
        for (int k = 0; k < i; k++) {
            if (strcmp(routes[k].line, routes[i].line) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    // 只換路由，唔碰 keep：set 同 keep 嘅次序唔應該影響結果。
    memset(s_routes, 0, sizeof(s_routes));
    memset(s_names, 0, sizeof(s_names));
    memset(s_out_names, 0, sizeof(s_out_names));
    for (int i = 0; i < n; i++) {
        memcpy(s_names[i], routes[i].line, strlen(routes[i].line) + 1);
        s_routes[i].line = s_names[i];
        s_routes[i].core = routes[i].core;
        uint8_t n_out = 0;
        for (uint8_t k = 0; k < routes[i].n_out; k++) {
            const char *s = routes[i].out[k];
            // 去重（同一格寫兩次同一個目的地 = 同一件事，唔應該轉寄兩次）
            bool dup = false;
            for (uint8_t j = 0; j < n_out; j++) {
                if (strcmp(s_routes[i].out[j], s) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            memcpy(s_out_names[i][n_out], s, strlen(s) + 1);
            s_routes[i].out[n_out] = s_out_names[i][n_out];
            n_out++;
        }
        s_routes[i].n_out = n_out;
    }
    s_count = n;
    s_set = true;
    return ESP_OK;
}

int nc4_router_core(const char *line)
{
    if (line == NULL) {
        return NC4_ROUTE_NONE;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_routes[i].line, line) == 0) {
            return s_routes[i].core;
        }
    }
    return NC4_ROUTE_NONE;
}

const char *nc4_router_line(int i)
{
    if (i < 0 || i >= s_count) {
        return NULL;
    }
    return s_routes[i].line;
}

// 轉寄目的地：濾走 decodeN（那核自己會處理）同自己（自我反射），並去重。
int nc4_router_links(const char *line, const char **out, int max)
{
    if (line == NULL || out == NULL || max <= 0) {
        return 0;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_routes[i].line, line) != 0) {
            continue;
        }
        int n = 0;
        for (uint8_t k = 0; k < s_routes[i].n_out && n < max; k++) {
            const char *s = s_routes[i].out[k];
            if (decode_name(s, NULL)) {
                continue;                   // 本地執行，不是轉寄目標
            }
            if (strcmp(s, line) == 0) {
                continue;                   // 送返自己 = 無限反射，直接忽略
            }
            bool dup = false;
            for (int j = 0; j < n; j++) {
                if (strcmp(out[j], s) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            out[n++] = s;
        }
        return n;
    }
    return 0;
}

bool nc4_router_core_used(int core)
{
    if (!core_ok(core)) {
        return false;
    }
    if (s_keep[core]) {
        return true;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_routes[i].core == core) {
            return true;
        }
    }
    return false;
}

esp_err_t nc4_router_keep(int core)
{
    if (!core_ok(core)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_keep[core] = true;
    return ESP_OK;
}

int nc4_router_count(void)
{
    return s_count;
}

// 追加式格式化：永遠留一個 NUL；滿咗就唔再寫（唔會越界）。
static size_t jappend(char *buf, size_t cap, size_t len, const char *fmt, ...)
{
    if (len + 1 >= cap) {
        return len;
    }
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (w < 0) {
        return len;
    }
    const size_t n = (size_t)w;
    if (n >= cap - len) {
        return cap - 1;                    // 截斷
    }
    return len + n;
}

size_t nc4_router_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    int n0 = 0;
    int n1 = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_routes[i].core == 0) {
            n0++;
        } else {
            n1++;
        }
    }

    size_t len = 0;
    len = jappend(buf, cap, len, "{\"set\":%d,\"core0\":%d,\"core1\":%d,\"routes\":[",
                  s_set ? 1 : 0, n0, n1);
    for (int i = 0; i < s_count; i++) {
        len = jappend(buf, cap, len, "%s{\"line\":\"%s\",\"core\":%d,\"out\":[",
                      (i == 0) ? "" : ",", s_routes[i].line, s_routes[i].core);
        for (uint8_t k = 0; k < s_routes[i].n_out; k++) {
            len = jappend(buf, cap, len, "%s\"%s\"",
                          (k == 0) ? "" : ",", s_routes[i].out[k]);
        }
        len = jappend(buf, cap, len, "]}");
    }
    len = jappend(buf, cap, len, "]}");
    return len;
}
