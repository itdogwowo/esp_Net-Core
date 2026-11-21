// nc_effect_bench.c — 純「效果管線」效能量測（0xF065 EFBENCH）。
//
// 為什麼另開一支、而不是加在 test_action：
//   · test_action 的 58 條指令是「整個測試映像」：network／ncm／量測全部一齊入，
//     量效果的時候旁邊一定有其他東西（cache／IRAM／任務）在動；
//   · 這一支只做效果：program interpreter → 12-bit 配對 → 抖動／12→8 → 畫布；
//     沒有 0xF060–0xF064、沒有 network、沒有 ncm；
//   · 由 CONFIG_NETCORE_EFFECT_BENCH 開關，**不需要 NC_ENABLE_TESTS**。
//
// 量什麼（全部同一塊板、同一輪、同一個 task 內）：
//   eval       : nc_px_engine_program_value() 逐次成本（program 的「畫波」核心）
//   vals       : nc_px_job_vals() 每幀算 64 個 12-bit 值（8×8 的真實用法）
//   pair8/12   : nc_px_pair_apply()／pair_apply12() 把值散入畫布
//   conv       : nc_px_canvas12_to_8() 四捨五入
//   dither     : 同一支 1D 誤差擴散
//   pipe       : 整條效果（vals＋pair＋轉換／抖動），分：
//                  i8  = 8-bit 畫布（internal）
//                  p8  = 8-bit 畫布（PSRAM）
//                  i12 = 12-bit 畫布 → 8-bit（兩塊都 internal）
//                  p12 = 12-bit 畫布 → 8-bit（兩塊都 PSRAM）
//                  mix = 12-bit 畫布 PSRAM → 8-bit 畫布 internal（最現實的混合）
//   chunk      : 12-bit 畫布切 64／256／1024 px 一塊（internal scratch）→ 12→8
//   jobs       : 2／4 個 job（mode 委派）合成到同一塊 12-bit 畫布
//
// 為什麼要 PSRAM 版本：ESP32-S3 的 PSRAM 經 cache，順序寫入不一定慢；
// 但 9,600 px 的 12-bit 畫布 76.8 KB 放不進 internal，遲早要落 PSRAM。
// 這支量的是「同一個 px 之下，放 internal 與放 PSRAM 的差」，以及「切塊」能否
// 把 PSRAM 的隨機寫入成本壓下來（status §7.147 的待辦）。
//
// 注意：64 px 的畫布只有 512 B／1 KB，放 PSRAM 仍然會命中 cache；
// 要看到 PSRAM 的真正代價，請用 --px 960／9600（工具預設會跑一組）。

#include "sdkconfig.h"

#if CONFIG_NETCORE_EFFECT_BENCH

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nc4.h"
#include "nc_action.h"
#include "nc_prio.h"
#include "nc_px_engine.h"
#include "nc_px_program.h"
#include "nc_px_show.h"
#include "nc_task.h"

static const char *TAG = "ebench";

#define NC4_CMD_EFBENCH      0xF065
#define NC4_CMD_EFBENCH_RSP  0xF066

#define EFBENCH_VALS_MAX     256     // nc_px_show.c 的 VAL_MAX
#define EFBENCH_CHUNK_MAX    1024    // 分塊量測的最大一塊（px）
#define EFBENCH_JOB_MAX      4

// ═══════════════════════ 量測結果 ═══════════════════════

typedef struct {
    uint16_t px;
    uint16_t frames;
    uint8_t  reps;
    uint16_t nv;                    // 每幀算幾個值（= min(px, 64)）

    bool c8_int_ok;                 // 8-bit 畫布在 internal
    bool c8_ps_ok;                  // 8-bit 畫布在 PSRAM
    bool c12_int_ok;                // 12-bit 畫布在 internal
    bool c12_ps_ok;                 // 12-bit 畫布在 PSRAM

    uint32_t eval_ns;               // 每個 program_value
    uint32_t vals_ns;               // 每幀 64 個值
    uint32_t pair8_i, pair8_p;
    uint32_t pair12_i, pair12_p;
    uint32_t conv_i, conv_p;
    uint32_t dither_i, dither_p;
    uint32_t pipe8_i, pipe8_p;
    uint32_t pipe12_i, pipe12_p, pipe12_mix;
    uint32_t chunk[3];              // 64／256／1024 px 一塊
    uint32_t jobs2, jobs4;
} ebench_t;

// ═══════════════════════ 共用狀態 ═══════════════════════
// 量測係單線程的（boot task 或 NC4 decode task 其中一個）；
// s_busy 防止兩邊同時改 s_job／s_ctx。
static volatile uint32_t s_busy;

static nc_px_asset_t s_asset;
static nc_px_job_t   s_job;
static nc_px_job_t   s_jobs[EFBENCH_JOB_MAX];
static uint16_t      s_vals[EFBENCH_VALS_MAX];
static volatile uint32_t s_sink;

typedef struct {
    uint16_t  px;
    uint16_t  nv;
    uint8_t  *c8_i;
    uint8_t  *c8_p;
    uint16_t *c12_i;
    uint16_t *c12_p;
    uint16_t *chunk12;              // internal scratch（EFBENCH_CHUNK_MAX × 4 × 2 B）
    uint16_t  chunk_px;
    uint8_t   jobs;
    bool      dither;
} ebench_ctx_t;

static ebench_ctx_t s_ctx;

// ═══════════════════════ 工具 ═══════════════════════

static void *alloc_caps(size_t bytes, uint32_t caps)
{
    // 64 B 對齊：PSRAM 是 cache line 32 B，對齊避免一顆像素跨兩條 line。
    return heap_caps_aligned_alloc(64, bytes, caps | MALLOC_CAP_8BIT);
}

static uint32_t ns_per_frame(int64_t t0, uint32_t frames)
{
    int64_t dt = esp_timer_get_time() - t0;
    if (dt < 1) {
        dt = 1;
    }
    // 先除再乘，避免 dt×1000 在幾秒的 rep 上溢位 u32。
    return (uint32_t)((dt / (int64_t)frames) * 1000 +
                      ((dt % (int64_t)frames) * 1000) / (int64_t)frames);
}

static void fill_asset(nc_px_asset_t *a, uint16_t px)
{
    memset(a, 0, sizeof(*a));
    strcpy(a->name, "ebench");
    a->kind = NC_PX_ASSET_PROGRAM;
    a->pixel_hint = (px < 64) ? px : 64;   // 8×8 的實際用法：效果只算 64 個值
    a->step = 3;
    a->spacing = 7;
    a->offset = 11;
    a->speed = 1;

    // 四段都有：正弦、方波、脈衝串、保持。最後一段 end_f = 波長。
    a->nseg = 4;
    a->seg[0].type = NC_PX_SEG_MATH_NOW;
    a->seg[0].f = 10;
    a->seg[0].l_max = 4095;
    a->seg[0].l_lim = 100;
    a->seg[0].phi = 0;
    a->seg[0].end_f = 64;
    a->seg[0].pulse = 2047;

    a->seg[1].type = NC_PX_SEG_SQUARE_WAVE_NOW;
    a->seg[1].f = 17;
    a->seg[1].l_max = 3000;
    a->seg[1].l_lim = 0;
    a->seg[1].phi = 700;
    a->seg[1].end_f = 128;
    a->seg[1].pulse = 2047;

    a->seg[2].type = NC_PX_SEG_PULSE_WAVE;
    a->seg[2].f = 23;
    a->seg[2].l_max = 4095;
    a->seg[2].l_lim = 200;
    a->seg[2].phi = 1400;
    a->seg[2].end_f = 192;
    a->seg[2].pulse = 1200;

    a->seg[3].type = NC_PX_SEG_KEEP;
    a->seg[3].f = 1;
    a->seg[3].l_max = 4095;
    a->seg[3].l_lim = 0;
    a->seg[3].phi = 0;
    a->seg[3].end_f = 256;
    a->seg[3].pulse = 2047;
}

static void fill_job(nc_px_job_t *j, const nc_px_asset_t *a, uint16_t base, uint16_t count)
{
    memset(j, 0, sizeof(*j));
    j->asset = a;
    j->write = NC_PX_WRITE_RGB;
    j->nranges = 1;
    j->rng[0].base = base;
    j->rng[0].count = count;
    j->nled = count;
    (void)nc_px_engine_program_compile(a, &j->prog);
}

// ═══════════════════════ 各段量測 ═══════════════════════
// 每個 fn(arg, frames) 內部跑 frames 幀；外面量時間再除 frames。

static void b_eval(void *arg, uint32_t frames)
{
    (void)arg;
    uint32_t acc = 0;
    const uint16_t nv = s_ctx.nv;
    for (uint32_t f = 0; f < frames; f++) {
        uint32_t g = (f * 2654435761u) >> 8;
        for (uint16_t i = 0; i < nv; i++) {
            acc += nc_px_engine_program_value(&s_job.prog, g + i * 7u);
        }
    }
    s_sink += acc;
}

static void b_vals(void *arg, uint32_t frames)
{
    (void)arg;
    uint32_t acc = 0;
    for (uint32_t f = 0; f < frames; f++) {
        acc += nc_px_job_vals(&s_job, f * 3u, s_vals, EFBENCH_VALS_MAX);
    }
    s_sink += acc;
}

static void b_pair8(void *arg, uint32_t frames)
{
    uint8_t *dst = (uint8_t *)arg;
    for (uint32_t f = 0; f < frames; f++) {
        nc_px_pair_apply(&s_job, s_vals, s_ctx.nv, dst);
    }
    s_sink += dst[0];
}

static void b_pair12(void *arg, uint32_t frames)
{
    uint16_t *dst = (uint16_t *)arg;
    for (uint32_t f = 0; f < frames; f++) {
        nc_px_pair_apply12(&s_job, s_vals, s_ctx.nv, dst);
    }
    s_sink += dst[0];
}

static void b_conv(void *arg, uint32_t frames)
{
    (void)arg;
    int32_t err[4] = { 0, 0, 0, 0 };
    for (uint32_t f = 0; f < frames; f++) {
        nc_px_canvas12_to_8(s_ctx.c12_i ? s_ctx.c12_i : s_ctx.c12_p,
                            s_ctx.c8_i, (uint32_t)s_ctx.px * 4u, s_ctx.dither, err);
    }
    s_sink += s_ctx.c8_i[0];
}

static void b_conv_ps(void *arg, uint32_t frames)
{
    (void)arg;
    int32_t err[4] = { 0, 0, 0, 0 };
    for (uint32_t f = 0; f < frames; f++) {
        nc_px_canvas12_to_8(s_ctx.c12_p, s_ctx.c8_i, (uint32_t)s_ctx.px * 4u,
                            s_ctx.dither, err);
    }
    s_sink += s_ctx.c8_i[0];
}

static void b_pipe8(void *arg, uint32_t frames)
{
    uint8_t *dst = (uint8_t *)arg;
    for (uint32_t f = 0; f < frames; f++) {
        (void)nc_px_job_vals(&s_job, f * 3u, s_vals, EFBENCH_VALS_MAX);
        nc_px_pair_apply(&s_job, s_vals, s_ctx.nv, dst);
    }
    s_sink += dst[0];
}

static void b_pipe12(void *arg, uint32_t frames)
{
    bool psram = (bool)(uintptr_t)arg;
    uint16_t *c12 = psram ? s_ctx.c12_p : s_ctx.c12_i;
    uint8_t *c8 = psram ? s_ctx.c8_p : s_ctx.c8_i;
    int32_t err[4] = { 0, 0, 0, 0 };
    for (uint32_t f = 0; f < frames; f++) {
        (void)nc_px_job_vals(&s_job, f * 3u, s_vals, EFBENCH_VALS_MAX);
        nc_px_pair_apply12(&s_job, s_vals, s_ctx.nv, c12);
        nc_px_canvas12_to_8(c12, c8, (uint32_t)s_ctx.px * 4u, s_ctx.dither, err);
    }
    s_sink += c8[0];
}

static void b_pipe12_mix(void *arg, uint32_t frames)
{
    (void)arg;
    int32_t err[4] = { 0, 0, 0, 0 };
    for (uint32_t f = 0; f < frames; f++) {
        (void)nc_px_job_vals(&s_job, f * 3u, s_vals, EFBENCH_VALS_MAX);
        nc_px_pair_apply12(&s_job, s_vals, s_ctx.nv, s_ctx.c12_p);
        nc_px_canvas12_to_8(s_ctx.c12_p, s_ctx.c8_i, (uint32_t)s_ctx.px * 4u,
                            s_ctx.dither, err);
    }
    s_sink += s_ctx.c8_i[0];
}

// 分塊：把 12-bit 畫布切成 chunk_px 一塊，只在 internal scratch 上做配對＋轉換。
// 這正是 §7.147「12-bit 畫布要分塊」的候選做法。
static void b_chunk(void *arg, uint32_t frames)
{
    (void)arg;
    const uint16_t chunk = s_ctx.chunk_px;
    nc_px_job_t *j = &s_job;
    int32_t err[4] = { 0, 0, 0, 0 };
    for (uint32_t f = 0; f < frames; f++) {
        for (uint16_t off = 0; off < s_ctx.px; off = (uint16_t)(off + chunk)) {
            uint16_t n = (uint16_t)(s_ctx.px - off);
            if (n > chunk) {
                n = chunk;
            }
            // scratch 的座標由 0 開始（唔係畫布絕對位），pair12 的 base 必須清零；
            // 否則 off>0 時會寫到 chunk12 之外（960 px 剛好還在 8 KB 內，9,600 px 會爆）。
            j->rng[0].base = 0;
            j->rng[0].count = n;
            j->nled = n;
            const uint16_t nv = nc_px_job_vals(j, f * 3u, s_vals, EFBENCH_VALS_MAX);
            nc_px_pair_apply12(j, s_vals, nv, s_ctx.chunk12);
            nc_px_canvas12_to_8(s_ctx.chunk12, s_ctx.c8_i + (uint32_t)off * 4u,
                                (uint32_t)n * 4u, s_ctx.dither, err);
        }
    }
    s_sink += s_ctx.c8_i[0];
}

static void b_jobs(void *arg, uint32_t frames)
{
    const uint8_t njobs = s_ctx.jobs;
    uint16_t *c12 = s_ctx.c12_i ? s_ctx.c12_i : s_ctx.c12_p;
    for (uint32_t f = 0; f < frames; f++) {
        for (uint8_t j = 0; j < njobs; j++) {
            (void)nc_px_job_vals(&s_jobs[j], f * 3u, s_vals, EFBENCH_VALS_MAX);
            nc_px_pair_apply12(&s_jobs[j], s_vals, s_ctx.nv, c12);
        }
    }
    s_sink += c12[0];
}

// 3 次（大 px 2 次）取最小；每組先熱身 1/8，避免第一幀的 cache miss 被當成常態。
// **每輪之間要 vTaskDelay(2)**：這一支可能連續跑幾秒（9,600 px），而 task WDT
// 看的是 idle 有沒有機會跑。為什麼不是 1：decode task 的優先級高過 idle，
// vTaskDelay(1) 每次都在 tick 邊界醒來，idle 永遠被搶先 → 5 秒後仍然 task_wdt。
// 2 ticks 會留下一個完整 tick 給 idle；只在量測區間之外讓，不影響數字。
static uint32_t bench_best(void (*fn)(void *, uint32_t), void *arg,
                           uint32_t frames, uint8_t reps)
{
    const uint32_t warm = (frames / 8u) ? (frames / 8u) : 1u;
    fn(arg, warm);
    vTaskDelay(2);                       // 餵 task WDT（量測之外；要 2 ticks 才輪到 idle）
    uint32_t best = 0xFFFFFFFFu;
    for (uint8_t r = 0; r < reps; r++) {
        const int64_t t0 = esp_timer_get_time();
        fn(arg, frames);
        const uint32_t dt = ns_per_frame(t0, frames);
        if (dt < best) {
            best = dt;
        }
        vTaskDelay(2);                   // 同上；下一次量測前的排程點
    }
    return best;
}

// ═══════════════════════ 主量測 ═══════════════════════

static void run_bench(uint16_t px, uint16_t frames_in, ebench_t *out)
{
    memset(out, 0, sizeof(*out));
    if (px < 8) {
        px = 8;
    }
    if (px > 16384) {
        px = 16384;
    }

    uint16_t frames = frames_in;
    if (frames == 0) {
        if (px <= 128) {
            frames = 10000;
        } else if (px <= 1024) {
            frames = 500;
        } else if (px <= 4096) {
            frames = 100;
        } else {
            frames = 20;
        }
    }
    // 單次 fn() 要 < 5 s（task WDT 是 5 s，而 vTaskDelay 只在每輪之間）。
    // 保守用 1,000,000 / px 幀：9,600 px → 104 幀（≈1.2 s），64 px → 15,625 幀。
    const uint32_t cap_frames = (px > 0u) ? (1000000u / px) : 1u;
    if (frames > cap_frames) {
        frames = (uint16_t)cap_frames;
    }
    if (frames == 0) {
        frames = 1;
    }
    const uint8_t reps = (px <= 1024) ? 3 : 2;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.px = px;
    s_ctx.nv = (px < 64) ? px : 64;

    fill_asset(&s_asset, px);
    fill_job(&s_job, &s_asset, 0, px);
    for (uint8_t i = 0; i < EFBENCH_JOB_MAX; i++) {
        const uint16_t part = (uint16_t)(px / EFBENCH_JOB_MAX);
        fill_job(&s_jobs[i], &s_asset, (uint16_t)(i * part), part);
    }

    nc_px_engine_tables_init();

    const size_t b8 = (size_t)px * 4u;
    const size_t b12 = (size_t)px * 4u * sizeof(uint16_t);
    s_ctx.c8_i = (uint8_t *)alloc_caps(b8, MALLOC_CAP_INTERNAL);
    s_ctx.c8_p = (uint8_t *)alloc_caps(b8, MALLOC_CAP_SPIRAM);
    s_ctx.c12_i = (uint16_t *)alloc_caps(b12, MALLOC_CAP_INTERNAL);
    s_ctx.c12_p = (uint16_t *)alloc_caps(b12, MALLOC_CAP_SPIRAM);
    s_ctx.chunk12 = (uint16_t *)alloc_caps((size_t)EFBENCH_CHUNK_MAX * 4u * sizeof(uint16_t),
                                           MALLOC_CAP_INTERNAL);
    if (s_ctx.c8_i != NULL) {
        memset(s_ctx.c8_i, 0, b8);
    }
    if (s_ctx.c8_p != NULL) {
        memset(s_ctx.c8_p, 0, b8);
    }
    if (s_ctx.c12_i != NULL) {
        memset(s_ctx.c12_i, 0, b12);
    }
    if (s_ctx.c12_p != NULL) {
        memset(s_ctx.c12_p, 0, b12);
    }

    out->px = px;
    out->frames = frames;
    out->reps = reps;
    out->nv = s_ctx.nv;
    out->c8_int_ok = (s_ctx.c8_i != NULL);
    out->c8_ps_ok = (s_ctx.c8_p != NULL);
    out->c12_int_ok = (s_ctx.c12_i != NULL);
    out->c12_ps_ok = (s_ctx.c12_p != NULL);

    // 先算一組值，pair／conv 才有內容（避免全 0 被 branch predictor 特別對待）。
    (void)nc_px_job_vals(&s_job, 123, s_vals, EFBENCH_VALS_MAX);

    out->eval_ns = bench_best(b_eval, NULL, frames, reps);
    if (s_ctx.nv != 0) {
        out->eval_ns /= s_ctx.nv;          // b_eval 每幀算 nv 個值
    }
    out->vals_ns = bench_best(b_vals, NULL, frames, reps);

    if (s_ctx.c8_i != NULL) {
        out->pair8_i = bench_best(b_pair8, s_ctx.c8_i, frames, reps);
    }
    if (s_ctx.c8_p != NULL) {
        out->pair8_p = bench_best(b_pair8, s_ctx.c8_p, frames, reps);
    }
    if (s_ctx.c12_i != NULL) {
        out->pair12_i = bench_best(b_pair12, s_ctx.c12_i, frames, reps);
    }
    if (s_ctx.c12_p != NULL) {
        out->pair12_p = bench_best(b_pair12, s_ctx.c12_p, frames, reps);
    }

    s_ctx.dither = false;
    if (s_ctx.c12_i != NULL && s_ctx.c8_i != NULL) {
        out->conv_i = bench_best(b_conv, NULL, frames, reps);
    }
    if (s_ctx.c12_p != NULL && s_ctx.c8_i != NULL) {
        out->conv_p = bench_best(b_conv_ps, NULL, frames, reps);
    }

    s_ctx.dither = true;
    if (s_ctx.c12_i != NULL && s_ctx.c8_i != NULL) {
        out->dither_i = bench_best(b_conv, NULL, frames, reps);
    }
    if (s_ctx.c12_p != NULL && s_ctx.c8_i != NULL) {
        out->dither_p = bench_best(b_conv_ps, NULL, frames, reps);
    }
    s_ctx.dither = false;

    if (s_ctx.c8_i != NULL) {
        out->pipe8_i = bench_best(b_pipe8, s_ctx.c8_i, frames, reps);
    }
    if (s_ctx.c8_p != NULL) {
        out->pipe8_p = bench_best(b_pipe8, s_ctx.c8_p, frames, reps);
    }
    if (s_ctx.c12_i != NULL && s_ctx.c8_i != NULL) {
        out->pipe12_i = bench_best(b_pipe12, (void *)(uintptr_t)0, frames, reps);
    }
    if (s_ctx.c12_p != NULL && s_ctx.c8_p != NULL) {
        out->pipe12_p = bench_best(b_pipe12, (void *)(uintptr_t)1, frames, reps);
    }
    if (s_ctx.c12_p != NULL && s_ctx.c8_i != NULL) {
        out->pipe12_mix = bench_best(b_pipe12_mix, NULL, frames, reps);
    }

    if (s_ctx.c8_i != NULL && s_ctx.chunk12 != NULL) {
        static const uint16_t sizes[3] = { 64, 256, 1024 };
        for (int i = 0; i < 3; i++) {
            s_ctx.chunk_px = (sizes[i] < px) ? sizes[i] : px;
            out->chunk[i] = bench_best(b_chunk, NULL, frames, reps);
        }
    }

    if (s_ctx.c12_i != NULL || s_ctx.c12_p != NULL) {
        s_ctx.jobs = 2;
        out->jobs2 = bench_best(b_jobs, NULL, frames, reps);
        s_ctx.jobs = 4;
        out->jobs4 = bench_best(b_jobs, NULL, frames, reps);
        s_ctx.jobs = 0;
    }

    // 收工：把量測結果的 live 值讀一次（防優化），再釋放。
    s_sink += (uint32_t)(uintptr_t)s_ctx.c8_i;
    if (s_ctx.c8_i != NULL) {
        heap_caps_free(s_ctx.c8_i);
    }
    if (s_ctx.c8_p != NULL) {
        heap_caps_free(s_ctx.c8_p);
    }
    if (s_ctx.c12_i != NULL) {
        heap_caps_free(s_ctx.c12_i);
    }
    if (s_ctx.c12_p != NULL) {
        heap_caps_free(s_ctx.c12_p);
    }
    if (s_ctx.chunk12 != NULL) {
        heap_caps_free(s_ctx.chunk12);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));

    // 還原 job 的 range（下一次量測會重新 fill_job，這裡只是保持整潔）。
    (void)nc_px_job_vals(&s_job, 0, s_vals, EFBENCH_VALS_MAX);
}

static size_t result_json(const ebench_t *r, char *out, size_t cap)
{
    const size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const int n = snprintf(
        out, cap,
        "{\"px\":%u,\"fr\":%u,\"rep\":%u,\"nv\":%u,"
        "\"mem\":{\"c8i\":%d,\"c8p\":%d,\"c12i\":%d,\"c12p\":%d,\"ifree\":%u,\"pfree\":%u},"
        "\"eval_ns\":%u,\"vals_ns\":%u,"
        "\"pair8\":{\"i\":%u,\"p\":%u},\"pair12\":{\"i\":%u,\"p\":%u},"
        "\"conv\":{\"i\":%u,\"p\":%u,\"di\":%u,\"dp\":%u},"
        "\"pipe\":{\"i8\":%u,\"p8\":%u,\"i12\":%u,\"p12\":%u,\"mix\":%u},"
        "\"chunk\":{\"64\":%u,\"256\":%u,\"1024\":%u},"
        "\"jobs\":{\"j2\":%u,\"j4\":%u}}",
        (unsigned)r->px, (unsigned)r->frames, (unsigned)r->reps, (unsigned)r->nv,
        r->c8_int_ok ? 1 : 0, r->c8_ps_ok ? 1 : 0,
        r->c12_int_ok ? 1 : 0, r->c12_ps_ok ? 1 : 0,
        (unsigned)i_free, (unsigned)p_free,
        (unsigned)r->eval_ns, (unsigned)r->vals_ns,
        (unsigned)r->pair8_i, (unsigned)r->pair8_p,
        (unsigned)r->pair12_i, (unsigned)r->pair12_p,
        (unsigned)r->conv_i, (unsigned)r->conv_p,
        (unsigned)r->dither_i, (unsigned)r->dither_p,
        (unsigned)r->pipe8_i, (unsigned)r->pipe8_p,
        (unsigned)r->pipe12_i, (unsigned)r->pipe12_p, (unsigned)r->pipe12_mix,
        (unsigned)r->chunk[0], (unsigned)r->chunk[1], (unsigned)r->chunk[2],
        (unsigned)r->jobs2, (unsigned)r->jobs4);
    if (n < 0) {
        return 0;
    }
    return ((size_t)n < cap) ? (size_t)n : (cap - 1);
}

static void log_result(const ebench_t *r)
{
    ESP_LOGI(TAG, "px=%u fr=%u rep=%u nv=%u", (unsigned)r->px, (unsigned)r->frames,
             (unsigned)r->reps, (unsigned)r->nv);
    ESP_LOGI(TAG, "eval=%u ns/val  vals=%u ns/frame", (unsigned)r->eval_ns,
             (unsigned)r->vals_ns);
    ESP_LOGI(TAG, "pair8 i/p=%u/%u  pair12 i/p=%u/%u ns",
             (unsigned)r->pair8_i, (unsigned)r->pair8_p,
             (unsigned)r->pair12_i, (unsigned)r->pair12_p);
    ESP_LOGI(TAG, "conv i/p=%u/%u  dither i/p=%u/%u ns",
             (unsigned)r->conv_i, (unsigned)r->conv_p,
             (unsigned)r->dither_i, (unsigned)r->dither_p);
    ESP_LOGI(TAG, "pipe8 i/p=%u/%u  pipe12 i/p/mix=%u/%u/%u ns",
             (unsigned)r->pipe8_i, (unsigned)r->pipe8_p,
             (unsigned)r->pipe12_i, (unsigned)r->pipe12_p, (unsigned)r->pipe12_mix);
    ESP_LOGI(TAG, "chunk 64/256/1024=%u/%u/%u ns  jobs 2/4=%u/%u ns  canvas=%s%s%s%s",
             (unsigned)r->chunk[0], (unsigned)r->chunk[1], (unsigned)r->chunk[2],
             (unsigned)r->jobs2, (unsigned)r->jobs4,
             r->c8_int_ok ? "c8i " : "", r->c8_ps_ok ? "c8p " : "",
             r->c12_int_ok ? "c12i " : "", r->c12_ps_ok ? "c12p" : "");
}

// ═══════════════════════ NC4 回覆 ═══════════════════════

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
    const int total = nc4_pack_into(frame, NC4_REPLY_FRAME_MAX, NC4_CMD_EFBENCH_RSP,
                                    nc4_reply_addr(ctx), data, 2 + n);
    if (total > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)total);
    }
}

static void on_efbench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t px = 64;
    uint16_t frames = 0;
    if (f->len >= 2) {
        px = (uint16_t)(f->data[0] | ((uint16_t)f->data[1] << 8));
    }
    if (f->len >= 4) {
        frames = (uint16_t)(f->data[2] | ((uint16_t)f->data[3] << 8));
    }
    if (frames > 20000) {
        frames = 20000;
    }

    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQ_REL) != 0) {
        reply_json(ctx, "{\"err\":\"busy\"}");
        return;
    }
    ebench_t r;
    run_bench(px, frames, &r);
    char j[1400];
    result_json(&r, j, sizeof(j));
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
    log_result(&r);
    reply_json(ctx, j);
}

// ═══════════════════════ 開機自測任務 ═══════════════════════
// OTA 之後約 8 秒跑一次 64 px（8×8 的真實大小），log 直接顯示在 WebMaster 終端。
// 其他 px 由主機用 0xF065 指定（或 tools/effect_bench.py --px 960,9600）。
static void ebench_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));      // 等 USB CDC／WebMaster 接返上
    if (__atomic_exchange_n(&s_busy, 1u, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    ebench_t r;
    // 開機只跑 2,000 幀：完整 suite（10000 幀）要 ~29 秒，會拖慢 OTA 後第一次監看。
    // 要更穩的數字由 0xF065 指定 frames（例如 10000）。
    run_bench(64, 2000, &r);
    log_result(&r);
    __atomic_store_n(&s_busy, 0u, __ATOMIC_RELEASE);
}

static const nc_task_loop_t s_loops[] = {
    {
        .name = "ebench",
        .fn = ebench_task,
        .stack = 4096,
        .prio = NC_PRIO_OTHERS_MAX,
        .core = 1,
        .layer = 5,
        .period_ms = 0,
        .run_once = true,
    },
};

static const nc_task_t s_task = {
    .name = "effect_bench",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 21,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(effect_bench, &s_task);

// ═══════════════════════ 指令表 ═══════════════════════

static const nc_action_t s_actions[] = {
    { NC4_CMD_EFBENCH, on_efbench, "effect_bench" },
};

NC_ACTION_TABLE_DEF(nc_effect_bench_table, "ebench", s_actions);

#endif  // CONFIG_NETCORE_EFFECT_BENCH
