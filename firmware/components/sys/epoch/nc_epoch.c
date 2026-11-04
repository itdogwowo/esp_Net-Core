// 每核「大循環」量測的實作。純邏輯：唔 include IDF、唔 include FreeRTOS。
#include "nc_epoch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool used;
    uint32_t seen_seq;        // 最後一次參與的輪次
} ep_loop_t;

typedef struct {
    ep_loop_t loops[NC_EPOCH_LOOPS];
    int total;
    int remaining;            // 這一輪仲有幾條 loop 未跑
    uint32_t seq;             // 目前輪次（0 = 未用）

    bool have_end;
    int64_t last_end_us;      // 上一輪完成的時間
    int64_t first_us;         // 這一輪第一個參與者
    uint32_t cyc_start;
    uint64_t exec_acc_us;     // 這一輪所有 fn 時間總和

    uint32_t rounds;
    uint64_t p_sum, e_sum, c_sum;
    uint32_t p_last, p_min, p_max;
    uint32_t e_last, e_min, e_max;
    uint32_t c_last;
} ep_core_t;

static ep_core_t s_core[NC_EPOCH_CORES];

static bool core_ok(int core)
{
    return core >= 0 && core < NC_EPOCH_CORES;
}

static void core_normalize(ep_core_t *e)
{
    if (e->seq == 0) {
        e->seq = 1;           // 0 保留畀「未參與」
    }
}

void nc_epoch_reset(void)
{
    memset(s_core, 0, sizeof(s_core));
    for (int c = 0; c < NC_EPOCH_CORES; c++) {
        s_core[c].seq = 1;
    }
}

int nc_epoch_add(int core)
{
    if (!core_ok(core)) {
        return -1;
    }
    ep_core_t *e = &s_core[core];
    core_normalize(e);
    for (int i = 0; i < NC_EPOCH_LOOPS; i++) {
        if (!e->loops[i].used) {
            e->loops[i].used = true;
            e->loops[i].seen_seq = 0;
            e->total++;
            e->remaining++;            // 新 loop 未跑過呢一輪
            return i;
        }
    }
    return -1;
}

void nc_epoch_remove(int core, int token)
{
    if (!core_ok(core) || token < 0 || token >= NC_EPOCH_LOOPS) {
        return;
    }
    ep_core_t *e = &s_core[core];
    ep_loop_t *l = &e->loops[token];
    if (!l->used) {
        return;
    }
    core_normalize(e);
    if (l->seen_seq != e->seq && e->remaining > 0) {
        e->remaining--;                // 佢未跑，唔使再等
    }
    l->used = false;
    l->seen_seq = 0;
    if (e->total > 0) {
        e->total--;
    }
    if (e->remaining > e->total) {
        e->remaining = e->total;
    }
    if (e->remaining == 0 && e->total > 0) {
        // 移除之後剛好齊：唔用呢個時間點做統計（會變成一個假 period）
        e->seq++;
        e->remaining = e->total;
    }
}

bool nc_epoch_mark(int core, int token, int64_t now_us, uint32_t cycles, uint32_t exec_us)
{
    if (!core_ok(core) || token < 0 || token >= NC_EPOCH_LOOPS) {
        return false;
    }
    ep_core_t *e = &s_core[core];
    ep_loop_t *l = &e->loops[token];
    if (!l->used || e->total <= 0) {
        return false;
    }
    core_normalize(e);

    // 不管這一輪有沒有計過，fn 時間都算進這一輪的用量
    e->exec_acc_us += exec_us;

    if (l->seen_seq == e->seq) {
        return false;                  // 同一條 loop 在這一輪跑了第二次
    }
    l->seen_seq = e->seq;

    if (e->remaining == e->total) {    // 第一個參與者
        e->first_us = now_us;
        e->cyc_start = cycles;
    }
    if (e->remaining > 0) {
        e->remaining--;
    }
    if (e->remaining > 0) {
        return false;
    }

    // ── 一輪完成 ──
    const uint32_t exec = (e->exec_acc_us > UINT32_MAX)
                              ? UINT32_MAX
                              : (uint32_t)e->exec_acc_us;
    const uint32_t cyc = (uint32_t)(cycles - e->cyc_start);   // u32 差，> 10.7 s 會 wrap
    e->exec_acc_us = 0;

    if (e->have_end) {
        const int64_t d = now_us - e->last_end_us;
        if (d > 0 && (uint64_t)d <= UINT32_MAX) {
            const uint32_t p = (uint32_t)d;
            if (e->rounds == 0) {
                e->p_min = e->p_max = p;
                e->e_min = e->e_max = exec;
            } else {
                if (p < e->p_min) e->p_min = p;
                if (p > e->p_max) e->p_max = p;
                if (exec < e->e_min) e->e_min = exec;
                if (exec > e->e_max) e->e_max = exec;
            }
            e->p_last = p;
            e->e_last = exec;
            e->c_last = cyc;
            e->p_sum += p;
            e->e_sum += exec;
            e->c_sum += cyc;
            e->rounds++;
        }
    }
    e->last_end_us = now_us;
    e->have_end = true;
    e->seq++;
    e->remaining = e->total;
    return true;
}

void nc_epoch_get(int core, nc_epoch_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!core_ok(core)) {
        return;
    }
    const ep_core_t *e = &s_core[core];
    out->rounds = e->rounds;
    out->period_last_us = e->p_last;
    out->period_min_us = e->p_min;
    out->period_max_us = e->p_max;
    out->jitter_us = (e->p_max >= e->p_min) ? (e->p_max - e->p_min) : 0;
    out->exec_last_us = e->e_last;
    out->exec_max_us = e->e_max;
    out->cycles_last = e->c_last;
    out->loops = (uint8_t)e->total;
    out->collecting = (e->total > 0);
    if (e->rounds > 0) {
        out->period_avg_us = (uint32_t)(e->p_sum / e->rounds);
        out->exec_avg_us = (uint32_t)(e->e_sum / e->rounds);
        out->cycles_avg = (uint32_t)(e->c_sum / e->rounds);
        if (out->period_avg_us > 0) {
            out->duty_permille = (uint16_t)((uint64_t)out->exec_avg_us * 1000u /
                                            out->period_avg_us);
        }
    }
}

static size_t put(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (off >= cap) {
        return off;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    off += (size_t)n;
    return (off < cap) ? off : cap - 1;
}

size_t nc_epoch_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = put(buf, cap, 0, "{");
    for (int c = 0; c < NC_EPOCH_CORES; c++) {
        nc_epoch_stats_t s;
        nc_epoch_get(c, &s);
        off = put(buf, cap, off,
                  "%s\"core%d\":{\"rounds\":%u,\"loops\":%u,"
                  "\"period\":{\"last\":%u,\"avg\":%u,\"min\":%u,\"max\":%u,\"jitter\":%u},"
                  "\"exec\":{\"last\":%u,\"avg\":%u,\"max\":%u},"
                  "\"cycles\":{\"last\":%u,\"avg\":%u},"
                  "\"duty\":%u}",
                  (c == 0) ? "" : ",", c, (unsigned)s.rounds, (unsigned)s.loops,
                  (unsigned)s.period_last_us, (unsigned)s.period_avg_us,
                  (unsigned)s.period_min_us, (unsigned)s.period_max_us,
                  (unsigned)s.jitter_us,
                  (unsigned)s.exec_last_us, (unsigned)s.exec_avg_us,
                  (unsigned)s.exec_max_us,
                  (unsigned)s.cycles_last, (unsigned)s.cycles_avg,
                  (unsigned)s.duty_permille);
    }
    off = put(buf, cap, off, "}");
    return off;
}
