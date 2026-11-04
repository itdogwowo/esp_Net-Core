// nc_epoch host 測試：唔需要板子、唔需要 IDF。
// 驗的是「一輪怎樣才算完成」與統計值，時間/cycles 全部由測試提供。
#include <stdio.h>
#include <string.h>

#include "nc_epoch.h"

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
    printf("nc_epoch host test\n");

    // ── 1. 空表 ──
    case_begin("reset：兩個核心都係空");
    nc_epoch_reset();
    for (int c = 0; c < NC_EPOCH_CORES; c++) {
        nc_epoch_stats_t s;
        nc_epoch_get(c, &s);
        CHECK(s.rounds == 0 && s.loops == 0 && !s.collecting, "無 loop、無輪次");
    }

    // ── 2. 兩條 loop：一輪要兩條都跑過 ──
    case_begin("一輪 = 同一核所有 loop 都跑過一次");
    const int a = nc_epoch_add(0);
    const int b = nc_epoch_add(0);
    CHECK(a == 0 && b == 1, "token 0,1");
    CHECK(nc_epoch_mark(0, a, 1000, 100, 10) == false, "只跑一條 → 未完成");
    CHECK(nc_epoch_mark(0, b, 1200, 200, 20) == true, "兩條都跑 → 完成");

    // 第一輪只做 base，唔計 period
    nc_epoch_stats_t s0;
    nc_epoch_get(0, &s0);
    CHECK(s0.rounds == 0, "第一輪唔計 period");
    CHECK(s0.loops == 2 && s0.collecting, "兩條 loop 已登記");

    // ── 3. 第二輪：period / exec / cycles ──
    case_begin("第二輪：period（wall）、exec（fn 總和）、cycles");
    CHECK(nc_epoch_mark(0, a, 2000, 1000, 30) == false, "第一個參與者");
    CHECK(nc_epoch_mark(0, b, 2200, 1100, 40) == true, "第二個參與者 → 完成");
    nc_epoch_get(0, &s0);
    CHECK(s0.rounds == 1, "rounds == 1");
    CHECK(s0.period_last_us == 1000, "period = 2200 − 1200");
    CHECK(s0.exec_last_us == 70, "exec = 30 + 40");
    CHECK(s0.cycles_last == 100, "cycles = 1100 − 1000");

    // ── 4. 第三輪：jitter / avg / duty ──
    case_begin("第三輪：min/max/jitter/avg/duty");
    CHECK(nc_epoch_mark(0, a, 3200, 2000, 50) == false, "第一個參與者");
    CHECK(nc_epoch_mark(0, b, 3400, 2100, 60) == true, "完成");
    nc_epoch_get(0, &s0);
    CHECK(s0.rounds == 2, "rounds == 2");
    CHECK(s0.period_last_us == 1200, "period = 3400 − 2200");
    CHECK(s0.period_min_us == 1000 && s0.period_max_us == 1200, "min/max");
    CHECK(s0.jitter_us == 200, "jitter = max − min");
    CHECK(s0.period_avg_us == 1100, "avg period");
    CHECK(s0.exec_last_us == 110, "exec = 50 + 60");
    CHECK(s0.exec_avg_us == 90, "avg exec");
    CHECK(s0.exec_max_us == 110, "max exec");
    CHECK(s0.duty_permille == 81, "duty = 90/1100 × 1000");

    // ── 5. 同一條 loop 跑兩次：唔會誤判完成，但 exec 照計 ──
    case_begin("同一條 loop 這一輪跑兩次：只算一次參與者");
    nc_epoch_reset();
    const int c0 = nc_epoch_add(1);
    const int c1 = nc_epoch_add(1);
    CHECK(nc_epoch_mark(1, c0, 100, 10, 5) == false, "第一次");
    CHECK(nc_epoch_mark(1, c0, 150, 20, 7) == false, "第二次（同一輪）");
    CHECK(nc_epoch_mark(1, c1, 200, 30, 3) == true, "另一條跑完 → 完成（第一輪）");
    // 第二輪：c0 跑兩次，exec 要包含兩次
    CHECK(nc_epoch_mark(1, c0, 300, 40, 5) == false, "第二輪 c0 #1");
    CHECK(nc_epoch_mark(1, c0, 350, 50, 7) == false, "第二輪 c0 #2");
    CHECK(nc_epoch_mark(1, c1, 400, 60, 3) == true, "第二輪完成");
    nc_epoch_stats_t s1;
    nc_epoch_get(1, &s1);
    CHECK(s1.rounds == 1 && s1.period_last_us == 200, "period = 400 − 200");
    CHECK(s1.exec_last_us == 15, "exec = 5 + 7 + 3（重跑都算）");
    CHECK(s1.cycles_last == 20, "cycles = 60 − 40");

    // ── 6. 中途加一條 loop：要等它跑才算完成 ──
    case_begin("中途加 loop：這一輪要等新 loop");
    nc_epoch_reset();
    const int d0 = nc_epoch_add(0);
    CHECK(nc_epoch_mark(0, d0, 1000, 0, 1) == true, "單條 loop → 每一跑都完成（第一輪）");
    CHECK(nc_epoch_mark(0, d0, 1100, 0, 1) == true, "第二輪（period = 100）");
    const int d1 = nc_epoch_add(0);            // 中途加
    CHECK(d1 == 1, "新 token");
    CHECK(nc_epoch_mark(0, d0, 1200, 0, 1) == false, "d0 已跑，但 d1 未跑");
    CHECK(nc_epoch_mark(0, d1, 1300, 0, 1) == true, "d1 補跑 → 完成");
    nc_epoch_get(0, &s0);
    CHECK(s0.rounds == 2 && s0.loops == 2, "rounds=2，loops=2");
    CHECK(s0.period_last_us == 200, "period = 1300 − 1100");

    // ── 7. 中途移除未跑的 loop：唔會卡住 ──
    case_begin("中途移除未跑的 loop：這一輪唔會卡死");
    nc_epoch_reset();
    const int e0 = nc_epoch_add(0);
    const int e1 = nc_epoch_add(0);
    CHECK(nc_epoch_mark(0, e0, 1000, 0, 1) == false, "e0 跑");
    CHECK(nc_epoch_mark(0, e1, 1100, 0, 1) == true, "e1 跑 → 第一輪完成");
    CHECK(nc_epoch_mark(0, e0, 1200, 0, 1) == false, "第二輪：e0 跑，等 e1");
    nc_epoch_remove(0, e1);                    // e1 未跑就移除
    nc_epoch_get(0, &s0);
    CHECK(s0.loops == 1, "loops 變 1");
    CHECK(nc_epoch_mark(0, e0, 1300, 0, 1) == true, "e0 再跑 → 一輪完成");
    nc_epoch_get(0, &s0);
    CHECK(s0.rounds == 1, "移除造成嘅假 period 唔計");
    CHECK(s0.period_last_us == 200, "period = 1300 − 1100");

    // ── 8. token 上限 ──
    case_begin("token 上限：第 9 條 loop 失敗");
    nc_epoch_reset();
    int last = -1;
    for (int i = 0; i < NC_EPOCH_LOOPS; i++) {
        last = nc_epoch_add(0);
        CHECK(last >= 0, "加到滿");
    }
    CHECK(nc_epoch_add(0) < 0, "再加 → -1");
    CHECK(nc_epoch_add(5) < 0, "壞核 → -1");

    // ── 9. cycles wrap ──
    case_begin("cycles wrap：u32 差仍然正確");
    nc_epoch_reset();
    const int w0 = nc_epoch_add(0);
    const int w1 = nc_epoch_add(0);
    CHECK(nc_epoch_mark(0, w0, 1000, 0xFFFFFF00u, 1) == false, "起點接近頂");
    CHECK(nc_epoch_mark(0, w1, 1200, 0x00000100u, 1) == true, "完成（第一輪）");
    CHECK(nc_epoch_mark(0, w0, 2000, 0xFFFFFF00u, 1) == false, "第二輪起點");
    CHECK(nc_epoch_mark(0, w1, 2200, 0x00000100u, 1) == true, "第二輪完成");
    nc_epoch_get(0, &s0);
    CHECK(s0.cycles_last == 0x200, "delta = 0x100 − 0xFFFFFF00 = 0x200");

    // ── 10. JSON ──
    case_begin("JSON：兩個核心、合法結尾");
    nc_epoch_reset();
    const int j0 = nc_epoch_add(0);
    nc_epoch_mark(0, j0, 1000, 0, 1);
    nc_epoch_mark(0, j0, 2000, 0, 1);          // 單條 loop，每跑一次就一輪
    char buf[512];
    const size_t n = nc_epoch_json(buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf), "有寫入");
    CHECK(strstr(buf, "\"core0\"") != NULL && strstr(buf, "\"core1\"") != NULL, "兩個核心都有");
    CHECK(strstr(buf, "\"period\"") != NULL && strstr(buf, "\"duty\"") != NULL, "欄位齊");
    CHECK(buf[n] == '\0' && buf[n - 1] == '}', "合法結尾");
    CHECK(nc_epoch_json(NULL, 10) == 0, "NULL 緩衝 → 0");
    CHECK(nc_epoch_json(buf, 0) == 0, "cap=0 → 0");

    printf("\n%s：%d 個 case，%d 個失敗\n", s_fail ? "FAIL" : "ALL PASS", s_case, s_fail);
    return s_fail ? 1 : 0;
}
