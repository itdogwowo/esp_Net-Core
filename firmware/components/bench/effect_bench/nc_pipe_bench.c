// nc_pipe_bench.c — 9,600 個效果值 → 配對 → 12→8 的整條 effect 生產線（0xF069）。
//
// 為什麼要這條：0xF065 的效果值固定 `pixel_hint=64`（8×8 的真實用法），
// 但 16×600（9,600 px）如果要「每顆 LED 一個波」，效果生成器要跑 9,600 次
// program_value（RGB 更要 28,800 次）；這一段在之前的 bench 裡是看不到的。
//
// 量四個階段（同一輪、同一塊板、同一個 task）：
//   gen          : nc_px_job_vals() 產生 nv 個 12-bit 值（nv = px 或 3*px）
//   pair_range   : nc_px_pair_apply12()（現行 range 版）
//   pair_table   : 用 uint16 offs[px] 查表寫入（可選 contiguous／shuffled）
//   conv         : nc_px_canvas12_to_8()（四捨五入）
//   full_range   : gen + pair_range + conv
//   full_table   : gen + pair_table + conv
//   full_ps      : gen + pair_range + conv，12-bit canvas 在 PSRAM
//
// RGBW→I80 的編碼／送燈**不在這裡**：它已經有 render backend；這支只量
// 「效果生成器＋配對」這段新工作，避免把已知的 encode 成本混進來。
//
// payload [px:u16 LE][frames:u16 LE][mode:u8]
//   mode 0 = 1 值/px（write wwww，四個通道同值）
//   mode 1 = 3 值/px（write rgb）
//   mode 2 = 1 值/px（write w，只寫 W）
// 回覆 0xF06A JSON。

#include "sdkconfig.h"

#if CONFIG_NETCORE_EFFECT_BENCH

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nc4.h"
#include "nc_action.h"
#include "nc_px_asset.h"
#include "nc_px_program.h"
#include "nc_px_show.h"

static const char *TAG = "pipe_bench";

#define NC4_CMD_PIPEBENCH     0xF069
#define NC4_CMD_PIPEBENCH_RSP 0xF06A

static volatile uint32_t s_sink;
static bool s_busy;

static void *alloc_buf(size_t bytes, uint32_t caps)
{
    return heap_caps_aligned_alloc(64, bytes, caps | MALLOC_CAP_8BIT);
}

static uint32_t us_per_frame(int64_t t0, uint32_t frames)
{
    int64_t dt = esp_timer_get_time() - t0;
    if (dt < 1) {
        dt = 1;
    }
    return (uint32_t)(dt / (int64_t)frames);
}

static void fill_asset(nc_px_asset_t *a, uint16_t nvals)
{
    memset(a, 0, sizeof(*a));
    strcpy(a->name, "pipe");
    a->kind = NC_PX_ASSET_PROGRAM;
    a->pixel_hint = nvals;
    a->step = 3;
    a->spacing = 7;
    a->offset = 11;
    a->speed = 1;
    a->nseg = 4;
    a->seg[0] = (nc_px_seg_t){ .type = NC_PX_SEG_MATH_NOW, .f = 10, .l_max = 4095,
                               .l_lim = 100, .phi = 0, .end_f = 64, .pulse = 2047 };
    a->seg[1] = (nc_px_seg_t){ .type = NC_PX_SEG_SQUARE_WAVE_NOW, .f = 17, .l_max = 3000,
                               .l_lim = 0, .phi = 700, .end_f = 128, .pulse = 2047 };
    a->seg[2] = (nc_px_seg_t){ .type = NC_PX_SEG_PULSE_WAVE, .f = 23, .l_max = 4095,
                               .l_lim = 200, .phi = 1400, .end_f = 192, .pulse = 1200 };
    a->seg[3] = (nc_px_seg_t){ .type = NC_PX_SEG_KEEP, .f = 1, .l_max = 4095,
                               .l_lim = 0, .phi = 0, .end_f = 256, .pulse = 2047 };
}

static void fill_job(nc_px_job_t *j, const nc_px_asset_t *a, uint16_t px, uint8_t write)
{
    memset(j, 0, sizeof(*j));
    j->asset = a;
    j->write = write;
    j->nranges = 1;
    j->rng[0].base = 0;
    j->rng[0].count = px;
    j->nled = px;
    (void)nc_px_engine_program_compile(a, &j->prog);
}

// 查表配對：offs[k] = 這顆值要寫去哪個 pixel。table 可以係連續或打亂。
static void table_apply12(const uint16_t *offs, const uint16_t *vals, uint16_t px,
                          uint16_t *dst, uint8_t write)
{
    for (uint16_t k = 0; k < px; k++) {
        uint16_t *o = dst + ((uint32_t)offs[k] << 2);
        if (write == NC_PX_WRITE_W) {
            o[3] = vals[k];
        } else if (write == NC_PX_WRITE_RGB) {
            o[0] = vals[3u * k];
            o[1] = vals[3u * k + 1u];
            o[2] = vals[3u * k + 2u];
            o[3] = 0;
        } else { // WWWW
            const uint16_t v = vals[k];
            o[0] = v;
            o[1] = v;
            o[2] = v;
            o[3] = v;
        }
    }
}

// 8-bit 版查表（production pair 的寫法）：RGB 一次 32-bit store，W 只寫 1 B。
static void table_apply8(const uint16_t *offs, const uint16_t *vals, uint16_t px,
                         uint8_t *dst, uint8_t write)
{
    for (uint16_t k = 0; k < px; k++) {
        uint8_t *o = dst + ((uint32_t)offs[k] << 2);
        if (write == NC_PX_WRITE_W) {
            o[3] = (uint8_t)(vals[k] >> 4);
        } else if (write == NC_PX_WRITE_RGB) {
            const uint32_t w = (uint32_t)(vals[3u * k] >> 4) |
                               ((uint32_t)(vals[3u * k + 1u] >> 4) << 8) |
                               ((uint32_t)(vals[3u * k + 2u] >> 4) << 16);
            *(uint32_t *)o = w;
        } else {
            const uint8_t v = (uint8_t)(vals[k] >> 4);
            *(uint32_t *)o = (uint32_t)v * 0x01010101u;
        }
    }
}

// inverse：destination 順序寫，source 隨機讀（改善 cache／write combining）。
static void inv_apply8(const uint16_t *inv, const uint16_t *vals, uint16_t px,
                       uint8_t *dst, uint8_t write)
{
    for (uint16_t d = 0; d < px; d++) {
        const uint16_t k = inv[d];
        uint8_t *o = dst + ((uint32_t)d << 2);
        if (write == NC_PX_WRITE_W) {
            o[3] = (uint8_t)(vals[k] >> 4);
        } else if (write == NC_PX_WRITE_RGB) {
            const uint32_t w = (uint32_t)(vals[3u * k] >> 4) |
                               ((uint32_t)(vals[3u * k + 1u] >> 4) << 8) |
                               ((uint32_t)(vals[3u * k + 2u] >> 4) << 16);
            *(uint32_t *)o = w;
        } else {
            const uint8_t v = (uint8_t)(vals[k] >> 4);
            *(uint32_t *)o = (uint32_t)v * 0x01010101u;
        }
    }
}

// 12-bit packed：R/G 一個 32-bit、B/W 一個 32-bit（仍然 8 B/px，但 2 次 store）。
static void table_apply12_packed(const uint16_t *offs, const uint16_t *vals,
                                 uint16_t px, uint16_t *dst, uint8_t write)
{
    for (uint16_t k = 0; k < px; k++) {
        uint16_t *o = dst + ((uint32_t)offs[k] << 2);
        uint32_t *w = (uint32_t *)o;
        if (write == NC_PX_WRITE_W) {
            w[0] = 0;
            w[1] = (uint32_t)vals[k] << 16;
        } else { // RGB probe
            w[0] = (uint32_t)vals[3u * k] | ((uint32_t)vals[3u * k + 1u] << 16);
            w[1] = (uint32_t)vals[3u * k + 2u];
        }
    }
}

typedef struct {
    nc_px_job_t *job;
    uint16_t *vals;
    uint16_t nv;
    uint16_t px;
    uint16_t *c12;
    uint16_t *c12p;
    uint16_t *table;
    uint16_t *table_rnd;
    uint8_t *c8;
    uint8_t write;
    uint8_t mode;
} ctx_t;

static uint32_t bench_gen(ctx_t *c, uint32_t frames)
{
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        s_sink += nc_px_job_vals(c->job, f * 3u, c->vals, c->nv);
    }
    return us_per_frame(t0, frames);
}

static uint32_t bench_gen_cached(ctx_t *c, uint32_t frames,
                              const uint16_t *wave, uint16_t total)
{
    const int64_t t0 = esp_timer_get_time();
    const nc_px_asset_t *a = c->job->asset;
    const uint16_t speed = (a->speed == 0) ? 1 : a->speed;
    for (uint32_t f = 0; f < frames; f++) {
        uint32_t ph = ((f * 3u) / speed) * (uint32_t)a->step + (uint32_t)a->offset;
        ph = ph % total;
        for (uint16_t i = 0; i < c->nv; i++) {
            uint32_t p = ph + (uint32_t)i * (uint32_t)a->spacing;
            s_sink += wave[p % total];
        }
    }
    return us_per_frame(t0, frames);
}

static uint32_t bench_pair_range(ctx_t *c, uint32_t frames)
{
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        nc_px_pair_apply12(c->job, c->vals, c->nv, c->c12);
    }
    s_sink += c->c12[0];
    return us_per_frame(t0, frames);
}

static uint32_t bench_pair_table(ctx_t *c, uint32_t frames, const uint16_t *table)
{
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        table_apply12(table, c->vals, c->px, c->c12, c->write);
    }
    s_sink += c->c12[0];
    return us_per_frame(t0, frames);
}

static uint32_t bench_pair_packed(ctx_t *c, uint32_t frames)
{
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        table_apply12_packed(c->table_rnd, c->vals, c->px, c->c12, c->write);
    }
    s_sink += c->c12[0];
    return us_per_frame(t0, frames);
}

static uint32_t bench_conv(ctx_t *c, uint32_t frames, uint16_t *c12)
{
    int32_t err[4] = { 0, 0, 0, 0 };
    const uint32_t nch = (uint32_t)c->px * 4u;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        nc_px_canvas12_to_8(c12, c->c8, nch, false, err);
    }
    s_sink += c->c8[0];
    return us_per_frame(t0, frames);
}

static uint32_t bench_full(ctx_t *c, uint32_t frames, const uint16_t *table, uint16_t *c12)
{
    int32_t err[4] = { 0, 0, 0, 0 };
    const uint32_t nch = (uint32_t)c->px * 4u;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        s_sink += nc_px_job_vals(c->job, f * 3u, c->vals, c->nv);
        if (table != NULL) {
            table_apply12(table, c->vals, c->px, c12, c->write);
        } else {
            nc_px_pair_apply12(c->job, c->vals, c->nv, c12);
        }
        nc_px_canvas12_to_8(c12, c->c8, nch, false, err);
    }
    s_sink += c->c8[0];
    return us_per_frame(t0, frames);
}

static uint32_t bench_full8(ctx_t *c, uint32_t frames, const uint16_t *table)
{
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t f = 0; f < frames; f++) {
        s_sink += nc_px_job_vals(c->job, f * 3u, c->vals, c->nv);
        table_apply8(table, c->vals, c->px, c->c8, c->write);
    }
    s_sink += c->c8[0];
    return us_per_frame(t0, frames);
}

static uint32_t bench_full8_cached(ctx_t *c, uint32_t frames,
                                 const uint16_t *wave, uint16_t total,
                                 const uint16_t *table)
{
    const int64_t t0 = esp_timer_get_time();
    const nc_px_asset_t *a = c->job->asset;
    const uint16_t speed = (a->speed == 0) ? 1 : a->speed;
    for (uint32_t f = 0; f < frames; f++) {
        uint32_t ph = ((f * 3u) / speed) * (uint32_t)a->step + (uint32_t)a->offset;
        ph = ph % total;
        for (uint16_t i = 0; i < c->nv; i++) {
            const uint16_t v = wave[(ph + (uint32_t)i * (uint32_t)a->spacing) % total];
            c->vals[i] = v;
            s_sink += v;
        }
        table_apply8(table, c->vals, c->px, c->c8, c->write);
    }
    s_sink += c->c8[0];
    return us_per_frame(t0, frames);
}

static void reply_json(nc4_ctx_t *ctx, const char *json)
{
    if (ctx == NULL || ctx->send == NULL || ctx->decoder == NULL) {
        return;
    }
    uint8_t *frame = nc4_decoder_reply_buf(ctx->decoder);
    if (frame == NULL) {
        return;
    }
    size_t n = strlen(json);
    uint8_t *data = frame + NC4_HDR_LEN;
    if (n > NC4_REPLY_JSON_MAX) {
        n = (size_t)snprintf((char *)data + 2, NC4_REPLY_JSON_MAX + 1,
                             "{\"err\":\"reply_too_big\",\"need\":%u}",
                             (unsigned)strlen(json));
    } else {
        memcpy(data + 2, json, n);
    }
    data[0] = (uint8_t)n;
    data[1] = (uint8_t)(n >> 8);
    const int total = nc4_pack_into(frame, NC4_REPLY_FRAME_MAX, NC4_CMD_PIPEBENCH_RSP,
                                    nc4_reply_addr(ctx), data, 2 + n);
    if (total > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)total);
    }
}

static void on_pipebench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t px = 9600;
    uint16_t frames = 5;
    uint8_t mode = 0;
    if (f->len >= 2) {
        px = (uint16_t)(f->data[0] | ((uint16_t)f->data[1] << 8));
    }
    if (f->len >= 4) {
        frames = (uint16_t)(f->data[2] | ((uint16_t)f->data[3] << 8));
    }
    if (f->len >= 5) {
        mode = f->data[4];
    }
    if (px < 8) {
        px = 8;
    }
    if (px > 16384) {
        px = 16384;
    }
    if (frames < 1) {
        frames = 1;
    }
    if (frames > 8) {
        frames = 8;
    }
    if (mode > 2) {
        mode = 0;
    }
    if (s_busy) {
        reply_json(ctx, "{\"err\":\"busy\"}");
        return;
    }
    s_busy = true;

    const uint8_t write = (mode == 1) ? NC_PX_WRITE_RGB
                                      : (mode == 2) ? NC_PX_WRITE_W : NC_PX_WRITE_WWWW;
    const uint16_t nv = (mode == 1) ? (uint16_t)(px * 3u) : px;

    static ctx_t c;
    memset(&c, 0, sizeof(c));
    c.px = px;
    c.nv = nv;
    c.write = write;
    c.mode = mode;
    // 先配 c8（38.4 KB）再做 c12／table，避免 76.8 KB 的 c12 先把 internal 切碎。
    c.vals = (uint16_t *)alloc_buf((size_t)nv * 2u, MALLOC_CAP_INTERNAL);
    c.c8 = (uint8_t *)alloc_buf((size_t)px * 4u, MALLOC_CAP_INTERNAL);
    c.table = (uint16_t *)alloc_buf((size_t)px * 2u, MALLOC_CAP_INTERNAL);
    c.table_rnd = (uint16_t *)alloc_buf((size_t)px * 2u, MALLOC_CAP_INTERNAL);
    c.c12 = (uint16_t *)alloc_buf((size_t)px * 8u, MALLOC_CAP_INTERNAL);
    c.c12p = (uint16_t *)alloc_buf((size_t)px * 8u, MALLOC_CAP_SPIRAM);

    // 9,600 px 的 12-bit canvas（76.8 KB）在 internal 可能因為碎片拿不到；
    // 退到 PSRAM 仍然可以量「生成＋配對＋轉換」，只是 full_ps 與 full_range 同一條。
    if (c.c12 == NULL && c.c12p != NULL) {
        c.c12 = c.c12p;
        c.c12p = NULL;
    }

    if (c.vals == NULL || c.c12 == NULL || c.table == NULL || c.table_rnd == NULL ||
        c.c8 == NULL) {
        const int pv = c.vals != NULL, pc12i = c.c12 != NULL, pc12p = c.c12p != NULL;
        const int ptab = c.table != NULL, prnd = c.table_rnd != NULL, pc8 = c.c8 != NULL;
        if (c.vals) heap_caps_free(c.vals);
        if (c.c12) heap_caps_free(c.c12);
        if (c.c12p) heap_caps_free(c.c12p);
        if (c.table) heap_caps_free(c.table);
        if (c.table_rnd) heap_caps_free(c.table_rnd);
        if (c.c8) heap_caps_free(c.c8);
        s_busy = false;
        char oj[256];
        (void)snprintf(oj, sizeof(oj),
                       "{\"err\":\"oom\",\"p\":{\"vals\":%d,\"c12i\":%d,\"c12p\":%d,"
                       "\"tab\":%d,\"rnd\":%d,\"c8\":%d},"
                       "\"free\":{\"i\":%u,\"p\":%u}}",
                       pv, pc12i, pc12p, ptab, prnd, pc8,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        reply_json(ctx, oj);
        return;
    }

    static nc_px_asset_t asset;
    static nc_px_job_t job;
    fill_asset(&asset, nv);
    fill_job(&job, &asset, px, write);
    c.job = &job;
    static uint16_t wave[8192];
    const uint16_t wave_total = (job.prog.total <= 8192) ? (uint16_t)job.prog.total : 0;
    for (uint16_t p = 0; p < wave_total; p++) {
        wave[p] = nc_px_engine_program_value(&job.prog, p);
    }

    // 連續 offs（k）與打亂 offs（permutation）：用來分開「表查詢成本」與「隨機寫入成本」。
    for (uint16_t k = 0; k < px; k++) {
        c.table[k] = k;
        c.table_rnd[k] = (uint16_t)((uint32_t)k * 7919u % px);
    }

    // 先填一次，避免 pair 時 vals 全 0 被特別對待。
    (void)nc_px_job_vals(&job, 3, c.vals, nv);
    (void)bench_gen(&c, 1);
    vTaskDelay(2);

    uint32_t gen_us = 0, pair_range = 0, pair_table = 0, pair_rnd = 0, conv_i = 0;
    uint32_t pair8_rnd = 0, pair8_inv = 0, pair8_blk = 0, full8_rnd = 0;
    uint32_t pair_packed = 0;
    uint32_t gen_cached = 0, full8_cached = 0;
    uint32_t full_range = 0, full_table = 0, full_rnd = 0, full_ps = 0;
    const uint8_t reps = (nv > 20000) ? 2 : 3;
    for (uint8_t r = 0; r < reps; r++) {
        uint32_t v;
        v = bench_gen(&c, frames);
        if (r == 0 || v < gen_us) gen_us = v;
        v = bench_pair_range(&c, frames);
        if (r == 0 || v < pair_range) pair_range = v;
        v = bench_pair_table(&c, frames, c.table);
        if (r == 0 || v < pair_table) pair_table = v;
        v = bench_pair_table(&c, frames, c.table_rnd);
        if (r == 0 || v < pair_rnd) pair_rnd = v;
        v = bench_pair_packed(&c, frames);
        if (r == 0 || v < pair_packed) pair_packed = v;
        v = bench_conv(&c, frames, c.c12);
        if (r == 0 || v < conv_i) conv_i = v;
        v = bench_full(&c, frames, NULL, c.c12);
        if (r == 0 || v < full_range) full_range = v;
        v = bench_full(&c, frames, c.table, c.c12);
        if (r == 0 || v < full_table) full_table = v;
        v = bench_full(&c, frames, c.table_rnd, c.c12);
        if (r == 0 || v < full_rnd) full_rnd = v;
        if (c.c12p != NULL) {
            v = bench_full(&c, frames, NULL, c.c12p);
            if (r == 0 || v < full_ps) full_ps = v;
        }
        vTaskDelay(2);
    }

    // 8-bit random / inverse / 16-px block；table 共用，避免多 38 KB 配置。
    for (uint16_t k = 0; k < px; k++) {
        c.table[c.table_rnd[k]] = k;
    }
    {
        const int64_t t0 = esp_timer_get_time();
        for (uint8_t r = 0; r < reps; r++) {
            table_apply8(c.table_rnd, c.vals, px, c.c8, c.write);
        }
        pair8_rnd = us_per_frame(t0, reps);
        s_sink += c.c8[0];
    }
    {
        const int64_t t0 = esp_timer_get_time();
        for (uint8_t r = 0; r < reps; r++) {
            inv_apply8(c.table, c.vals, px, c.c8, c.write);
        }
        pair8_inv = us_per_frame(t0, reps);
        s_sink += c.c8[0];
    }
    {
        const uint16_t chunk = 16;
        const uint16_t nblocks = (uint16_t)(px / chunk);
        for (uint16_t k = 0; k < px; k++) {
            const uint16_t b = (uint16_t)(k / chunk);
            const uint16_t within = (uint16_t)(k % chunk);
            c.table_rnd[k] = (uint16_t)(((uint32_t)((b * 7u) % nblocks) * chunk) + within);
        }
        const int64_t t0 = esp_timer_get_time();
        for (uint8_t r = 0; r < reps; r++) {
            table_apply8(c.table_rnd, c.vals, px, c.c8, c.write);
        }
        pair8_blk = us_per_frame(t0, reps);
        s_sink += c.c8[0];
    }

    for (uint16_t k = 0; k < px; k++) {
        c.table_rnd[k] = (uint16_t)((uint32_t)k * 7919u % px);
    }
    full8_rnd = bench_full8(&c, frames, c.table_rnd);
    if (wave_total > 0) {
        gen_cached = bench_gen_cached(&c, frames, wave, wave_total);
        full8_cached = bench_full8_cached(&c, frames, wave, wave_total, c.table_rnd);
    }

    static char j[1200];
    (void)snprintf(
        j, sizeof(j),
        "{\"px\":%u,\"nv\":%u,\"mode\":%u,\"fr\":%u,\"rep\":%u,"
        "\"gen_us\":%u,\"gen_cached_us\":%u,"
        "\"pair_us\":{\"range\":%u,\"table\":%u,\"rnd\":%u,\"packed12\":%u,"
        "\"rnd8\":%u,\"inv8\":%u,\"blk8\":%u},"
        "\"conv_us\":%u,\"full_us\":{\"range\":%u,\"table\":%u,\"rnd\":%u,\"ps\":%u,"
        "\"rnd8\":%u,\"cached8\":%u},"
        "\"mem\":{\"c12_int\":%d,\"c12_ps\":%d,\"c8_int\":%d,\"c8_ps\":%d}}",
        (unsigned)px, (unsigned)nv, (unsigned)mode, (unsigned)frames, (unsigned)reps,
        (unsigned)gen_us, (unsigned)gen_cached,
        (unsigned)pair_range, (unsigned)pair_table, (unsigned)pair_rnd,
        (unsigned)pair_packed, (unsigned)pair8_rnd, (unsigned)pair8_inv, (unsigned)pair8_blk,
        (unsigned)conv_i, (unsigned)full_range, (unsigned)full_table, (unsigned)full_rnd,
        (unsigned)full_ps, (unsigned)full8_rnd, (unsigned)full8_cached,
        c.c12 ? (esp_ptr_internal(c.c12) ? 1 : 0) : -1,
        c.c12 ? (esp_ptr_external_ram(c.c12) ? 1 : 0) : -1,
        c.c8 ? (esp_ptr_internal(c.c8) ? 1 : 0) : -1,
        c.c8 ? (esp_ptr_external_ram(c.c8) ? 1 : 0) : -1);

    ESP_LOGI(TAG, "px=%u nv=%u mode=%u gen=%u pair=%u/%u conv=%u full=%u/%u/%u/%u us",
             (unsigned)px, (unsigned)nv, (unsigned)mode, (unsigned)gen_us,
             (unsigned)pair_range, (unsigned)pair_table, (unsigned)conv_i,
             (unsigned)full_range, (unsigned)full_table, (unsigned)full_rnd,
             (unsigned)full_ps);

    heap_caps_free(c.vals);
    heap_caps_free(c.c12);
    if (c.c12p) heap_caps_free(c.c12p);
    heap_caps_free(c.table);
    heap_caps_free(c.table_rnd);
    heap_caps_free(c.c8);
    s_busy = false;
    reply_json(ctx, j);
}

static const nc_action_t s_actions[] = {
    { NC4_CMD_PIPEBENCH, on_pipebench, "pipe_bench" },
};

NC_ACTION_TABLE_DEF(nc_pipe_bench_table, "pipe", s_actions);

#endif  // CONFIG_NETCORE_EFFECT_BENCH
