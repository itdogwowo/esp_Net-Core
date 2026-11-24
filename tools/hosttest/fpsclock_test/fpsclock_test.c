#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "nc_fps_clock.h"

static int fails;
static void chk(bool ok, const char *what)
{
    if (!ok) {
        printf("  FAIL %s\n", what);
        fails++;
    } else {
        printf("  PASS %s\n", what);
    }
}

int main(void)
{
    const int64_t P = 20000;   // 20 ms
    const int64_t CAP = 50000; // 50 ms
    nc_fps_clock_t c;

    printf("[1] 準時：deadline 每輪 +period，slack 慢慢儲\n");
    nc_fps_clock_init(&c, 1000000, P, CAP);
    int64_t now = 1000000;
    for (int i = 0; i < 3; i++) {
        now += P;                    // 準時完成
        nc_fps_clock_done(&c, now);
    }
    chk(c.deadline_us == 1000000 + 4 * P, "deadline 在 grid 上");
    chk(c.slack_us == CAP, "一開始就滿，準時完成唔會變");

    printf("[2] 早完成：多餘時間入 slack，有上限\n");
    nc_fps_clock_init(&c, 0, P, CAP);
    c.slack_us = 0;                       // 由空開始，測試儲錢
    for (int i = 0; i < 5; i++) {
        nc_fps_clock_done(&c, c.deadline_us - 5000);   // 早 5 ms
    }
    chk(c.slack_us == 25000, "5 次 × 5 ms = 25 ms");
    nc_fps_clock_init(&c, 0, P, CAP);
    c.slack_us = 0;
    for (int i = 0; i < 20; i++) {
        nc_fps_clock_done(&c, c.deadline_us - 5000);
    }
    chk(c.slack_us == CAP, "上限 cap = 50 ms");

    printf("[3] 遲到但被 slack 吸收：deadline 留在 grid\n");
    nc_fps_clock_init(&c, 0, P, CAP);
    c.slack_us = 10000;
    const int64_t d0 = c.deadline_us;
    nc_fps_clock_done(&c, d0 + 6000);        // 遲 6 ms
    chk(c.slack_us == 4000, "slack 6 ms 被花掉");
    chk(c.deadline_us == d0 + P, "deadline 仍然 +period（可追返）");
    chk(c.catchup_n == 1, "catchup 計一次");
    chk(nc_fps_clock_wait(&c, d0 + 6000) == 14000, "下一輪等 14 ms（追返 6 ms）");

    printf("[4] 遲到超過 slack：重排，唔爆追\n");
    nc_fps_clock_init(&c, 0, P, CAP);
    c.slack_us = 5000;
    const int64_t d1 = c.deadline_us;
    nc_fps_clock_done(&c, d1 + 15000);       // 遲 15 ms
    chk(c.slack_us == 0, "slack 用盡");
    chk(c.overrun_n == 1, "overrun 計一次");
    chk(c.deadline_us == d1 + 15000 + P, "deadline 重設為 now + period");

    printf("[5] wait：正常回剩餘時間\n");
    nc_fps_clock_init(&c, 0, P, CAP);
    chk(nc_fps_clock_wait(&c, 5000) == 15000, "5 ms 時回 15 ms");
    chk(nc_fps_clock_wait(&c, 25000) == 0, "已過 deadline 回 0");

    printf(fails ? "FAIL（%d）\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
