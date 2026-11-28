// status_test — STATUS_GET 自註冊區塊（nc_status）：註冊／重名／取値／append／keys 過濾
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nc_status.h"

static int s_fail, s_case;

static void check_impl(int cond, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", cond ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    if (!cond) { printf("   (%s:%d)", file, line); s_fail++; }
    printf("\n");
    va_end(ap);
}
#define CHECK(cond, ...) check_impl((cond), __FILE__, __LINE__, __VA_ARGS__)
static void case_begin(const char *n) { s_case++; printf("[%d] %s\n", s_case, n); }

static size_t prov_a(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "{\"x\":1}");
}
static size_t prov_b(char *buf, size_t cap)
{
    return (size_t)snprintf(buf, cap, "{\"y\":2}");
}

int main(void)
{
    case_begin("註冊");
    CHECK(nc_status_register("a", prov_a) == ESP_OK, "註冊 a");
    CHECK(nc_status_register("b", prov_b) == ESP_OK, "註冊 b");
    CHECK(nc_status_count() == 2, "count=2（實際 %d）", nc_status_count());
    CHECK(nc_status_register("a", prov_b) == ESP_ERR_INVALID_STATE, "重名 → 拒絕");
    CHECK(nc_status_register(NULL, prov_a) == ESP_ERR_INVALID_ARG, "NULL key → 拒絕");
    CHECK(nc_status_register("c", NULL) == ESP_ERR_INVALID_ARG, "NULL fn → 拒絕");
    CHECK(nc_status_count() == 2, "失敗的註冊唔會加數");

    case_begin("取單一 key 的值");
    char v[64];
    CHECK(nc_status_value("a", v, sizeof(v)) == 7 && strcmp(v, "{\"x\":1}") == 0,
          "a → %s", v);
    CHECK(nc_status_value("zz", v, sizeof(v)) == 0, "冇這個 key → 0");

    case_begin("append：全部");
    char buf[256];
    size_t off = 0;
    bool first = true;
    nc_status_append("", buf, sizeof(buf), &off, &first);
    buf[off] = '\0';
    CHECK(off > 0 && !first, "有寫入");
    CHECK(strstr(buf, "\"a\":{\"x\":1}") != NULL, "含 a（%s）", buf);
    CHECK(strstr(buf, "\"b\":{\"y\":2}") != NULL, "含 b（%s）", buf);

    case_begin("append：keys 過濾");
    off = 0; first = true; buf[0] = '\0';
    nc_status_append("b", buf, sizeof(buf), &off, &first);
    buf[off] = '\0';
    CHECK(strstr(buf, "\"b\"") != NULL, "有 b");
    CHECK(strstr(buf, "\"a\"") == NULL, "冇 a");

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAIL" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
