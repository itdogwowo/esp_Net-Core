// NC4 路由器 host 測試（唔需要板子、唔需要 IDF）
//
// 驗嘅係「線 → 核」呢張表嘅全部規則：
//   空表 / 正常表 / 重名 / 壞核 / 超上限 / 原子性（驗唔過就唔落地）/
//   keep（冇線都要保留一核）/ set 之後 keep 仲喺唔喺 / JSON 截斷安全。
//
// 呢個測試同時係「路由器唔靠 IDF」嘅證明：nc4_router.c 只 include
// esp_err.h 同 libc，所以可以直接用 MSVC / cc 編。
#include <stdio.h>
#include <string.h>

#include "nc4_router.h"

static int s_fail;
static int s_case;

static void check_impl(int cond, int line, const char *what)
{
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        printf("        (%s:%d)\n", __FILE__, line);
        s_fail++;
    }
}

#define CHECK(cond, what) check_impl((cond), __LINE__, what)

static void case_begin(const char *name)
{
    s_case++;
    printf("[%d] %s\n", s_case, name);
}

int main(void)
{
    printf("nc4_router host test\n");

    // ── 1. 空表 ──
    case_begin("空表：冇路由、冇核在用");
    nc4_router_reset();
    CHECK(nc4_router_count() == 0, "count == 0");
    CHECK(nc4_router_core("CDC") == NC4_ROUTE_NONE, "查唔到 → NONE");
    CHECK(nc4_router_core(NULL) == NC4_ROUTE_NONE, "NULL 線名 → NONE");
    CHECK(!nc4_router_core_used(0), "core0 冇線");
    CHECK(!nc4_router_core_used(1), "core1 冇線");
    CHECK(!nc4_router_core_used(2), "core2 唔存在 → false");
    CHECK(nc4_router_core("") == NC4_ROUTE_NONE, "空字串 → NONE（冇 fallback）");

    // ── 2. 正常表 ──
    case_begin("兩條線：CDC→0、UART→1");
    {
        const nc4_route_t r[] = {
            { .line = "CDC",  .core = 0 },
            { .line = "UART", .core = 1 },
        };
        CHECK(nc4_router_set(r, 2) == ESP_OK, "set 成功");
        CHECK(nc4_router_count() == 2, "count == 2");
        CHECK(nc4_router_core("CDC") == 0, "CDC → 0");
        CHECK(nc4_router_core("UART") == 1, "UART → 1");
        CHECK(nc4_router_core("TCP") == NC4_ROUTE_NONE, "未登記的線 → NONE");
        CHECK(nc4_router_core("TCP2") == NC4_ROUTE_NONE, "只做全名比對 → NONE");
        CHECK(nc4_router_core("CD") == NC4_ROUTE_NONE, "前綴唔算命中 → NONE");
        CHECK(nc4_router_core_used(0), "core0 有線");
        CHECK(nc4_router_core_used(1), "core1 有線");
    }

    // ── 3. 重名 ──
    case_begin("重名：整張表拒絕，舊表原封不動");
    {
        const nc4_route_t bad[] = {
            { .line = "CDC", .core = 0 },
            { .line = "CDC", .core = 1 },
        };
        CHECK(nc4_router_set(bad, 2) == ESP_ERR_INVALID_ARG, "重名 → INVALID_ARG");
        CHECK(nc4_router_count() == 2, "舊表仍然係 2 條");
        CHECK(nc4_router_core("CDC") == 0, "CDC 仍然 → 0（冇被改）");
    }

    // ── 4. 壞輸入 ──
    case_begin("壞輸入：壞核 / 空名 / 空表 / 超上限");
    {
        const nc4_route_t bad_core[] = { { .line = "X", .core = 2 } };
        const nc4_route_t bad_name[] = { { .line = "",  .core = 0 } };
        const nc4_route_t null_name[] = { { .line = NULL, .core = 0 } };
        CHECK(nc4_router_set(bad_core, 1) == ESP_ERR_INVALID_ARG, "core 2 → INVALID_ARG");
        CHECK(nc4_router_set(bad_name, 1) == ESP_ERR_INVALID_ARG, "空名 → INVALID_ARG");
        CHECK(nc4_router_set(null_name, 1) == ESP_ERR_INVALID_ARG, "NULL 名 → INVALID_ARG");
        CHECK(nc4_router_set(NULL, 1) == ESP_ERR_INVALID_ARG, "NULL 表 → INVALID_ARG");
        CHECK(nc4_router_set(bad_core, 0) == ESP_ERR_INVALID_ARG, "n=0 → INVALID_ARG");
        CHECK(nc4_router_set(bad_core, NC4_ROUTE_MAX + 1) == ESP_ERR_INVALID_ARG,
              "n > MAX → INVALID_ARG");
        CHECK(nc4_router_count() == 2, "全部失敗之後舊表仍然係 2 條");
    }

    // ── 5. add ──
    case_begin("add：逐條加、重複加、加到滿");
    {
        nc4_router_reset();
        CHECK(nc4_router_add("CDC", 0) == ESP_OK, "add CDC");
        CHECK(nc4_router_add("CDC", 1) == ESP_ERR_INVALID_STATE, "重複 add → INVALID_STATE");
        CHECK(nc4_router_core("CDC") == 0, "重複 add 冇改到原本");
        CHECK(nc4_router_add("UART", 2) == ESP_ERR_INVALID_ARG, "壞核 → INVALID_ARG");
        CHECK(nc4_router_add(NULL, 0) == ESP_ERR_INVALID_ARG, "NULL → INVALID_ARG");
        for (int i = 1; i < NC4_ROUTE_MAX; i++) {
            char name[8];
            snprintf(name, sizeof(name), "L%d", i);
            CHECK(nc4_router_add(name, i & 1) == ESP_OK, "加到滿為止");
        }
        CHECK(nc4_router_count() == NC4_ROUTE_MAX, "count == MAX");
        CHECK(nc4_router_add("OVER", 0) == ESP_ERR_NO_MEM, "再加 → NO_MEM");
    }

    // ── 6. keep ──
    case_begin("keep：冇線都要保留一核");
    {
        const nc4_route_t r[] = { { .line = "UART", .core = 1 } };
        nc4_router_reset();
        CHECK(nc4_router_set(r, 1) == ESP_OK, "set UART→1");
        CHECK(!nc4_router_core_used(0), "core0 冇線");
        CHECK(nc4_router_keep(0) == ESP_OK, "keep(0)");
        CHECK(nc4_router_core_used(0), "keep 之後 core0 在用");
        CHECK(nc4_router_keep(2) == ESP_ERR_INVALID_ARG, "keep(2) → INVALID_ARG");
        // set 唔應該清走 keep
        CHECK(nc4_router_set(r, 1) == ESP_OK, "再 set 一次");
        CHECK(nc4_router_core_used(0), "keep 仍然生效（set 唔清 keep）");
        nc4_router_reset();
        CHECK(!nc4_router_core_used(0), "reset 清走 keep");
    }

    // ── 7. JSON ──
    case_begin("JSON：內容正確、截斷安全");
    {
        const nc4_route_t r[] = {
            { .line = "CDC",  .core = 0 },
            { .line = "UART", .core = 1 },
            { .line = "TCP",  .core = 0 },
        };
        char buf[256];
        nc4_router_set(r, 3);
        const size_t n = nc4_router_json(buf, sizeof(buf));
        CHECK(n > 0 && n < sizeof(buf), "有寫入而且冇滿");
        CHECK(strstr(buf, "\"set\":1") != NULL, "set:1");
        CHECK(strstr(buf, "\"core0\":2") != NULL, "core0:2");
        CHECK(strstr(buf, "\"core1\":1") != NULL, "core1:1");
        CHECK(strstr(buf, "{\"line\":\"UART\",\"core\":1,\"out\":[]") != NULL,
              "UART 條目（out 為空時仍然有 out 欄，主機唔需要猜）");
        CHECK(buf[n] == '\0', "有 NUL 結尾");

        // 截斷：任何 cap 都唔可以越界、一定要有 NUL
        for (size_t cap = 1; cap <= 40; cap++) {
            char small[64];
            memset(small, 0x7F, sizeof(small));
            const size_t w = nc4_router_json(small, cap);
            const int nul_ok = (cap > 0) && (small[cap - 1] == '\0' || w == 0);
            if (!nul_ok) {
                printf("        cap=%u 冇 NUL（w=%u）\n", (unsigned)cap, (unsigned)w);
            }
            CHECK(w < cap, "回傳值 < cap");
            CHECK(nul_ok, "cap 之內有 NUL");
        }
        CHECK(nc4_router_json(NULL, 10) == 0, "NULL 緩衝 → 0");
        CHECK(nc4_router_json(buf, 0) == 0, "cap=0 → 0");
    }

    // ── 8. out：轉寄目的地 ──
    case_begin("out：decodeN 是本地、link 名是轉寄；自我反射與重複要濾走");
    {
        const nc4_route_t r[] = {
            { .line = "CDC", .core = 0, .n_out = 2, .out = { "decode0", "UART" } },
            { .line = "UART", .core = 1, .n_out = 1, .out = { "decode1" } },
            { .line = "TCP", .core = 0, .n_out = 4,
              .out = { "decode0", "UART", "UART", "DECODE9" } },
            { .line = "WS", .core = 0, .n_out = 2, .out = { "WS", "UDP" } },
        };
        CHECK(nc4_router_set(r, 4) == ESP_OK, "set 成功");

        const char *outs[NC4_ROUTE_OUT_MAX];
        int n = nc4_router_links("CDC", outs, NC4_ROUTE_OUT_MAX);
        CHECK(n == 1, "CDC：一個轉寄目標（decode0 濾走）");
        CHECK(n == 1 && strcmp(outs[0], "UART") == 0, "CDC → UART");

        n = nc4_router_links("UART", outs, NC4_ROUTE_OUT_MAX);
        CHECK(n == 0, "UART 只有 decode1 → 冇轉寄");

        n = nc4_router_links("TCP", outs, NC4_ROUTE_OUT_MAX);
        CHECK(n == 2, "TCP：重複的 UART 去掉、DECODE9 不是 decodeN 所以當 link 名留着");
        CHECK(n >= 1 && strcmp(outs[0], "UART") == 0, "TCP 第一個是 UART");
        CHECK(n >= 2 && strcmp(outs[1], "DECODE9") == 0, "TCP 第二個是 DECODE9");

        n = nc4_router_links("WS", outs, NC4_ROUTE_OUT_MAX);
        CHECK(n == 1, "WS：自己唔轉寄自己");
        CHECK(n == 1 && strcmp(outs[0], "UDP") == 0, "WS → UDP");

        CHECK(nc4_router_links("NOPE", outs, NC4_ROUTE_OUT_MAX) == 0, "查唔到的線 → 0");
        CHECK(nc4_router_links(NULL, outs, NC4_ROUTE_OUT_MAX) == 0, "NULL 線名 → 0");
        CHECK(nc4_router_links("CDC", NULL, 4) == 0, "NULL 輸出 → 0");
        CHECK(nc4_router_links("CDC", outs, 0) == 0, "max=0 → 0");

        // 只回 max 個
        n = nc4_router_links("TCP", outs, 1);
        CHECK(n == 1, "max=1 只回一個");

        CHECK(strcmp(nc4_router_line(0), "CDC") == 0, "line(0) == CDC");
        CHECK(nc4_router_line(-1) == NULL, "line(-1) == NULL");
        CHECK(nc4_router_line(4) == NULL, "line(4) 越界 == NULL");

        // JSON 要帶 out（ROUTE_GET 才看得出「會不會轉寄」）
        char buf[512];
        const size_t jn = nc4_router_json(buf, sizeof(buf));
        CHECK(jn > 0 && jn < sizeof(buf), "JSON 有寫入");
        CHECK(strstr(buf, "\"out\":[\"decode0\",\"UART\"]") != NULL, "CDC 的 out 在 JSON 裡");
    }

    // ── 9. out 的矛盾要整張表拒絕 ──
    case_begin("out 驗證：decode 核矛盾／壞名字／超上限 → 整張表拒絕，舊表不動");
    {
        const nc4_route_t good[] = {
            { .line = "CDC", .core = 0, .n_out = 1, .out = { "decode0" } },
        };
        CHECK(nc4_router_set(good, 1) == ESP_OK, "先放一張好表");

        const nc4_route_t mismatch[] = {
            { .line = "X", .core = 0, .n_out = 2, .out = { "decode0", "decode1" } },
        };
        CHECK(nc4_router_set(mismatch, 1) == ESP_ERR_INVALID_ARG,
              "out 有 decode1 但 core=0 → INVALID_ARG");
        CHECK(nc4_router_count() == 1, "舊表仍然係 1 列");
        CHECK(nc4_router_core("X") == NC4_ROUTE_NONE, "矛盾那列冇落地");

        const nc4_route_t badname[] = {
            { .line = "X", .core = 0, .n_out = 1, .out = { "" } },
        };
        CHECK(nc4_router_set(badname, 1) == ESP_ERR_INVALID_ARG, "out 有空名 → INVALID_ARG");

        const nc4_route_t too_many[] = {
            { .line = "X", .core = 0, .n_out = NC4_ROUTE_OUT_MAX + 1,
              .out = { "A", "B", "C", "D" } },
        };
        CHECK(nc4_router_set(too_many, 1) == ESP_ERR_INVALID_ARG, "out 超過上限 → INVALID_ARG");
        CHECK(nc4_router_core("CDC") == 0, "舊表原封不動");
    }

    printf("\n%s：%d 個 case，%d 個失敗\n", s_fail ? "FAIL" : "ALL PASS", s_case, s_fail);
    return s_fail ? 1 : 0;
}
