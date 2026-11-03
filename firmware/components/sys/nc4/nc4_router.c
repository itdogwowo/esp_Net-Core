// NC4 路由器：線 → 核。純邏輯、零依賴（唔用 ESP_LOG），所以 host 都跑得。
//
// 只做三件事：驗表、記表、查表。唔建立 bus，唔建立任務，唔配記憶體 ——
// 佢只答問題，由 callers 去做嘢。所以佢冇生命週期，亦都冇嘢要還。

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nc4_router.h"

static nc4_route_t s_routes[NC4_ROUTE_MAX];
static char s_names[NC4_ROUTE_MAX][NC4_ROUTE_NAME_MAX];   // 線名自己存，唔借 caller 嘅指標
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

void nc4_router_reset(void)
{
    memset(s_routes, 0, sizeof(s_routes));
    memset(s_names, 0, sizeof(s_names));
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
        for (int k = 0; k < i; k++) {
            if (strcmp(routes[k].line, routes[i].line) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    // 只換路由，唔碰 keep：set 同 keep 嘅次序唔應該影響結果。
    memset(s_routes, 0, sizeof(s_routes));
    memset(s_names, 0, sizeof(s_names));
    for (int i = 0; i < n; i++) {
        memcpy(s_names[i], routes[i].line, strlen(routes[i].line) + 1);
        s_routes[i].line = s_names[i];
        s_routes[i].core = routes[i].core;
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

int nc4_router_core_or(const char *line, int fallback)
{
    const int core = nc4_router_core(line);
    return (core == NC4_ROUTE_NONE) ? fallback : core;
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
        len = jappend(buf, cap, len, "%s{\"line\":\"%s\",\"core\":%d}",
                      (i == 0) ? "" : ",", s_routes[i].line, s_routes[i].core);
    }
    len = jappend(buf, cap, len, "]}");
    return len;
}
