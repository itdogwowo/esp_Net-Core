#include "nc_fps_clock.h"

void nc_fps_clock_init(nc_fps_clock_t *c, int64_t now_us, int64_t period_us, int64_t cap_us)
{
    if (c == 0) {
        return;
    }
    c->period_us = (period_us > 0) ? period_us : 1;
    c->cap_us = (cap_us > 0) ? cap_us : 0;
    c->deadline_us = now_us + c->period_us;
    c->slack_us = c->cap_us;      // 一開始就滿：避免開機追趕影響同步
    c->catchup_n = 0;
    c->overrun_n = 0;
    c->late_n = 0;
}

int64_t nc_fps_clock_wait(const nc_fps_clock_t *c, int64_t now_us)
{
    if (c == 0) {
        return 0;
    }
    const int64_t d = c->deadline_us - now_us;
    return (d > 0) ? d : 0;
}

void nc_fps_clock_done(nc_fps_clock_t *c, int64_t now_us)
{
    if (c == 0) {
        return;
    }
    const int64_t late = now_us - c->deadline_us;
    if (late <= 0) {
        int64_t add = -late;
        if (c->slack_us + add > c->cap_us) {
            add = c->cap_us - c->slack_us;
        }
        if (add > 0) {
            c->slack_us += add;
        }
        c->deadline_us += c->period_us;
        return;
    }
    c->late_n++;
    int64_t spend = late;
    if (spend > c->slack_us) {
        spend = c->slack_us;
    }
    if (spend > 0) {
        c->slack_us -= spend;
        c->catchup_n++;
    }
    const int64_t remain = late - spend;
    if (remain > 0) {
        c->overrun_n++;
        c->deadline_us = now_us + c->period_us;   // 追唔到：重排，避免爆追
    } else {
        c->deadline_us += c->period_us;           // 留在原本 grid：下一輪會即刻做
        if (c->deadline_us < now_us) {
            c->deadline_us = now_us + c->period_us;
        }
    }
}
