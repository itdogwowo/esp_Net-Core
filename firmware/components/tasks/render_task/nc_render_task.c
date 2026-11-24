// nc_render_task.c — 渲染任務（render）：環 → 編碼 → 後端／驅動。
//
// **只做渲染**：唔跑效果、唔處理 Map。效果生產者係另一個任務（tasks/pixel_task），
// 將來 LCD 同 LED 都由這一個 render 任務處理。
//
// 一個 render 任務可以有多道 lane；每道 lane = 一個後端 + 一條環。
// 環由 plan 宣告（declare），生產者（pixel 任務／.nce／主機）按名字找回同一條環。
//
// 檔案分工：
//   sw/pixel                   環、編碼器、效果引擎（純 C，host 測試同一份）
//   tasks/render_task          這個檔＋後端轉接（i80／spi／direct／mock）
//   driver/drv_i80、drv_spi_led 硬件（PCLK／DMA／完成）
#include "nc_render.h"
#include "nc_fps_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nc_cfg.h"
#include "nc_prio.h"
#include "nc_task.h"
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL

static const char *TAG = "render";

// ═══════════════════════ 後端註冊表 ═══════════════════════
static nc_pixel_backend_t *s_be[NC_PX_MAX_LANES];
static int s_nbe;

esp_err_t nc_px_backend_register(nc_pixel_backend_t *be)
{
    if (be == NULL || be->name[0] == '\0' || be->acquire == NULL || be->encode == NULL ||
        be->submit == NULL || be->reclaim == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_px_backend_find(be->name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_nbe >= NC_PX_MAX_LANES) {
        return ESP_ERR_NO_MEM;
    }
    s_be[s_nbe++] = be;
    return ESP_OK;
}

esp_err_t nc_px_backend_unregister(const char *name)
{
    for (int i = 0; i < s_nbe; i++) {
        if (strcmp(s_be[i]->name, name) == 0) {
            s_be[i] = s_be[--s_nbe];
            s_be[s_nbe] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

nc_pixel_backend_t *nc_px_backend_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_nbe; i++) {
        if (strcmp(s_be[i]->name, name) == 0) {
            return s_be[i];
        }
    }
    return NULL;
}

int nc_px_backend_count(void) { return s_nbe; }

// ═══════════════════════ 設定 ═══════════════════════
// 預設 = 「這塊板有 I80 就用 I80，否則退到 mock」。
// 環名帶 prd = 內建效果生產者（pixel 任務）餵這一格。
typedef struct {
    char     ring[NC_PX_NAME_MAX + 1];
    char     be[NC_PX_BACKEND_MAX];
    uint32_t px;
    uint32_t slots;
} px_plan_t;

static px_plan_t s_plan[NC_PX_MAX_LANES];
static int s_nplan;
static nc_pixel_q_t s_rings[NC_PX_MAX_LANES];

static nc_pixel_cfg_t s_cfg = {
    // 後備節拍：實際以 system.json 的 frame_interval_ms 為準（見 px_start）。
    .fps = CONFIG_NETCORE_PIXEL_FPS,
#if CONFIG_NETCORE_PIXEL_MOCK_ONLY
    .nlane = 1,
    .lane = {
        { .be = "mock", .ring = "mock.prd" },   // 量 CPU：唔受線路時間限制
    },
#else
    .nlane = 2,
    .lane = {
        { .be = "i80", .ring = "i80.prd" },
        { .be = "mock", .ring = "mock.prd" },   // 冇 I80 的組態：效果管線仍然有環可餵
    },
#endif
};

void nc_render_set_cfg(const nc_pixel_cfg_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
        s_nplan = 0;                 // 設定改了，plan 重算
    }
}

const nc_pixel_cfg_t *nc_render_cfg(void)
{
    return &s_cfg;
}

// ═══════════════════════ 後端：誰存在、要幾大 ═══════════════════════
// 一個後端的「一格幾顆」由它自己回答，而且**不碰硬體**：
//   · i80／spi 只在 Kconfig 開了、而且板上有腳位時才算存在；
//   · direct／mock 永遠存在 —— 沒有燈板也能把整條路跑起來。
static uint32_t stage_px(const char *be)
{
    if (be == NULL) {
        return 0;
    }
    const uint32_t px = (CONFIG_NETCORE_PIXEL_LEDS > 0) ? (uint32_t)CONFIG_NETCORE_PIXEL_LEDS
                                                        : 0u;
    if (px == 0) {
        return 0;
    }
#if CONFIG_NETCORE_PIXEL_I80
    if (strcmp(be, "i80") == 0) {
        return px;
    }
#endif
#if CONFIG_NETCORE_PIXEL_SPI
    if (strcmp(be, "spi") == 0) {
        return px;
    }
#endif
    if (strcmp(be, "direct") == 0 || strcmp(be, "mock") == 0) {
        return px;
    }
    return 0;
}

static int plan_lanes(void)
{
    if (s_nplan > 0) {
        return s_nplan;
    }
    for (int i = 0; i < s_cfg.nlane && i < NC_PX_MAX_LANES; i++) {
        const uint32_t px = stage_px(s_cfg.lane[i].be);
        if (px == 0) {
            continue;
        }
        px_plan_t *pl = &s_plan[s_nplan++];
        (void)snprintf(pl->be, sizeof(pl->be), "%s", s_cfg.lane[i].be);
        // 環名：lane cfg 寫了就用它（例如 "i80.prd" 表示「這條環由效果生產者餵」），
        // 否則 "<後端>.<第幾道>"。
        if (s_cfg.lane[i].ring[0] != '\0') {
            (void)snprintf(pl->ring, sizeof(pl->ring), "%s", s_cfg.lane[i].ring);
        } else {
            (void)snprintf(pl->ring, sizeof(pl->ring), "%s.%d", s_cfg.lane[i].be, i);
        }
        pl->px = px;
        // 3 格：DMA 送第 N 格時，生產者可以填 N+1、N+2（P6 的三緩衝）。
        pl->slots = (uint32_t)CONFIG_NETCORE_PIXEL_RING_SLOTS;
    }
    return s_nplan;
}

// codec 設定：格式與時序是「板子的物理事實」，全部由 Kconfig 給。
static void codec_cfg_of(nc_px_codec_cfg_t *cc, uint32_t px, const char *be)
{
    memset(cc, 0, sizeof(*cc));
    nc_px_format_t fmt = NC_PX_FMT_WS2812W;
    (void)nc_px_format_parse(CONFIG_NETCORE_PIXEL_FORMAT, &fmt);
    if (strcmp(be, "direct") == 0) {
        fmt = NC_PX_FMT_WS2812;      // direct 只是把波形放著，用小一點的格式
    }
    cc->fmt = fmt;
    nc_px_wave_default(fmt, &cc->wave);
    cc->wave.clk_hz = CONFIG_NETCORE_PIXEL_PCLK_HZ;
    cc->wave.bit_cells = CONFIG_NETCORE_PIXEL_BIT_CELLS;
    cc->wave.t0h = CONFIG_NETCORE_PIXEL_T0H;
    cc->wave.t1h = CONFIG_NETCORE_PIXEL_T1H;
    cc->bits = (uint8_t)CONFIG_NETCORE_PIXEL_DITHER_BITS;
    cc->dither_mode = (uint8_t)CONFIG_NETCORE_PIXEL_DITHER;
    cc->zone_px = (uint16_t)CONFIG_NETCORE_PIXEL_ZONE_PX;
    cc->px = px;
    cc->bright = 255;
}

#if CONFIG_NETCORE_PIXEL_I80
// 舊 sdkconfig 未有新符號時 fallback 8-bit（唔強制要 --fresh 才 build 得到）。
#ifndef CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH
#define CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH 8
#endif

// I80 的腳位字串：D0..D(width-1)、WR、DC（width=8 → 10 個；16 → 18 個）。
static bool parse_i80_pins(int out[18], int width)
{
    if (width != 8 && width != 16) {
        return false;
    }
    const char *s = CONFIG_NETCORE_PIXEL_I80_PINS;
    for (int i = 0; i < width + 2; i++) {
        char *end = NULL;
        const long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 48) {
            ESP_LOGE(TAG, "I80 腳位字串第 %d 個唔合法（剩返 \"%s\"）", i + 1, s);
            return false;
        }
        out[i] = (int)v;
        s = end;
        while (*s == ' ' || *s == ',') {
            s++;
        }
    }
    return true;
}
#endif

static esp_err_t setup_backends(void)
{
    const int n = plan_lanes();
    for (int i = 0; i < n; i++) {
        const px_plan_t *pl = &s_plan[i];
        nc_px_codec_cfg_t cc;
        codec_cfg_of(&cc, pl->px, pl->be);
#if CONFIG_NETCORE_PIXEL_I80
        if (strcmp(pl->be, "i80") == 0) {
            static int s_i80_pins[18];
            const uint8_t width = (CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH == 16) ? 16u : 8u;
            if (!parse_i80_pins(s_i80_pins, width)) {
                ESP_LOGE(TAG, "I80 腳位設定壞了 → 這道 lane 停用");
                continue;
            }
            nc_px_i80_cfg_t ic = {
                .pins = s_i80_pins,
                .lanes = (uint8_t)CONFIG_NETCORE_PIXEL_I80_LANES,
                .bus_width = width,
                .pclk_hz = cc.wave.clk_hz,
                .px = pl->px,
                .fmt = cc.fmt,
                .wave = cc.wave,
                .dither_bits = cc.bits,
                .dither_mode = cc.dither_mode,
                .zone_px = (uint8_t)cc.zone_px,
                .slots = (uint8_t)pl->slots,
            };
            nc_pixel_backend_t *be = nc_px_backend_i80(&ic);
            if (be == NULL) {
                ESP_LOGE(TAG, "i80 後端建立失敗（腳位被佔用或沒開）→ 這道 lane 停用");
            } else if (nc_px_backend_register(be) != ESP_OK) {
                ESP_LOGE(TAG, "i80 後端登記失敗 → 這道 lane 停用");
            }
            continue;
        }
#endif
#if CONFIG_NETCORE_PIXEL_SPI
        if (strcmp(pl->be, "spi") == 0) {
            nc_px_spi_cfg_t sc = {
                .pin_mosi = CONFIG_NETCORE_PIXEL_SPI_MOSI,
                .pin_sclk = CONFIG_NETCORE_PIXEL_SPI_SCLK,
                .host = -1,
                .clock_hz = CONFIG_NETCORE_PIXEL_SPI_HZ,
                .px = pl->px,
                .fmt = cc.fmt,
                .dither_bits = cc.bits,
                .dither_mode = cc.dither_mode,
                .zone_px = (uint8_t)cc.zone_px,
                .bright = 255,
                .slots = (uint8_t)pl->slots,
            };
            nc_pixel_backend_t *be = nc_px_backend_spi(&sc);
            if (be == NULL) {
                ESP_LOGE(TAG, "spi 後端建立失敗 → 這道 lane 停用");
            } else if (nc_px_backend_register(be) != ESP_OK) {
                ESP_LOGE(TAG, "spi 後端登記失敗 → 這道 lane 停用");
            }
            continue;
        }
#endif
        if (strcmp(pl->be, "direct") == 0) {
            nc_px_direct_bind(&cc);
            (void)nc_px_backend_register(nc_px_backend_direct());
            continue;
        }
        (void)nc_px_backend_register(nc_px_backend_mock());
    }
    return (s_nbe > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ═══════════════════════ 執行期狀態 ═══════════════════════
typedef struct {
    nc_pixel_backend_t *be;
    nc_pixel_q_t       *q;
    uint32_t            last_seq;
} px_lane_t;

static struct {
    bool         started;
    uint32_t     period_us;      // 由 system.json frame_interval_ms 換算
    uint16_t     fps;            // 1000 / frame_interval_ms（顯示用）
    px_lane_t    lane[NC_PX_MAX_LANES];
    int          nlane;
    nc_px_stat_t st;
    uint32_t     mark_frames;
    int64_t      mark_us;
    SemaphoreHandle_t tick;      // 節拍：esp_timer 週期性給（唔 busy-wait）
    esp_timer_handle_t timer;
} s_px;
static nc_fps_clock_t s_clock;

// esp_timer 回呼（esp_timer 任務 context，不是 ISR）→ 給一次節拍。
static void render_tick(void *arg)
{
    (void)arg;
    if (s_px.tick != NULL) {
        xSemaphoreGive(s_px.tick);
    }
}

void nc_render_get_stats(nc_px_stat_t *out)
{
    if (out != NULL) {
        *out = s_px.st;
    }
}

static esp_err_t bind_lanes(void)
{
    s_px.nlane = 0;
    for (int i = 0; i < s_nplan; i++) {
        nc_pixel_backend_t *be = nc_px_backend_find(s_plan[i].be);
        nc_pixel_q_t *q = nc_px_ring_find(s_plan[i].ring);
        if (be == NULL || q == NULL) {
            ESP_LOGW(TAG, "lane %d 停用：後端 %s %s、環 %s %s", i, s_plan[i].be,
                     (be != NULL) ? "有" : "**起唔到**", s_plan[i].ring,
                     (q != NULL) ? "有登記" : "**未登記**");
            continue;
        }
        be->ring = q;                // 後端要知道讀哪條環（i80／spi 的 acquire 用）
        // mock 後端有自己的 q 指標（它不經 be->ring）；唔 bind 就永遠 acquire 失敗。
        if (strcmp(be->name, "mock") == 0) {
            nc_px_backend_mock_bind(be, q);
        }
        s_px.lane[s_px.nlane].be = be;
        s_px.lane[s_px.nlane].q = q;
        s_px.lane[s_px.nlane].last_seq = 0;
        s_px.nlane++;
    }
    return (s_px.nlane > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// 一道 lane 的一輪。
static void lane_tick(px_lane_t *ln)
{
    // ① 先還掉「已送完」的格（DMA 完成才算；後端自己知道）
    ln->be->reclaim(ln->be);

    // ② 問後端拿一格比上一次新的
    uint32_t slot = 0;
    const uint8_t *payload = NULL;
    uint32_t px = 0;
    uint32_t seq = 0;
    if (!ln->be->acquire(ln->be, ln->last_seq, &slot, &payload, &px, &seq)) {
        s_px.st.starved++;             // 沒有新格／全部在飛 → 下一輪再試
        return;
    }
    s_px.st.fresh++;
    if (ln->last_seq != 0 && seq > ln->last_seq + 1u) {
        s_px.st.skipped += seq - ln->last_seq - 1u;   // 假消費：中間的格不補播
    }
    ln->last_seq = seq;

    const int64_t t0 = esp_timer_get_time();
    if (!ln->be->encode(ln->be, slot)) {
        s_px.st.fail++;
        if (payload != NULL) {
            nc_px_ring_release(ln->q, seq);   // 編不了就立刻還格（不可以洩漏）
        }
        return;
    }
    const int64_t t1 = esp_timer_get_time();
    s_px.st.enc_us = (uint32_t)(t1 - t0);
    if (!ln->be->submit(ln->be, slot, seq)) {
        nc_px_ring_release(ln->q, seq);       // 沒有提交 → 立刻還格
        s_px.st.fail++;
        return;
    }
    s_px.st.tx_us = (uint32_t)(esp_timer_get_time() - t1);
    s_px.st.frames++;
}

const char *nc_render_first_ring(void)
{
    if (s_px.nlane <= 0 || s_px.lane[0].q == NULL) {
        return NULL;
    }
    return nc_px_ring_name(s_px.lane[0].q);
}

// ── FPS 穩定度：逐輪 dt 的統計（0xF081）──
typedef struct {
    uint32_t n;
    uint64_t sum;          // Σ dt
    uint64_t sumsq;        // Σ dt²
    uint32_t min_us;
    uint32_t max_us;
    uint32_t bad;          // |dt - target| > target/5
    uint32_t hist[64];     // 0..62 ms，63 = ≥63 ms
} fps_jit_t;

static fps_jit_t s_jit;

static void jit_record(uint32_t dt, uint32_t target_us)
{
    s_jit.n++;
    s_jit.sum += dt;
    s_jit.sumsq += (uint64_t)dt * dt;
    if (s_jit.min_us == 0 || dt < s_jit.min_us) {
        s_jit.min_us = dt;
    }
    if (dt > s_jit.max_us) {
        s_jit.max_us = dt;
    }
    if (target_us > 0) {
        const uint32_t diff = (dt > target_us) ? (dt - target_us) : (target_us - dt);
        if (diff > target_us / 5u) {
            s_jit.bad++;
        }
    }
    uint32_t b = dt / 1000u;
    if (b > 63u) {
        b = 63u;
    }
    s_jit.hist[b]++;
}

void nc_render_fps_reset(void)
{
    memset(&s_jit, 0, sizeof(s_jit));
}

size_t nc_render_fps_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const uint32_t n = s_jit.n;
    const uint32_t avg = (n > 0) ? (uint32_t)(s_jit.sum / n) : 0;
    uint32_t sd = 0;
    if (n > 0) {
        const uint64_t mean = s_jit.sum / n;
        const uint64_t var = (s_jit.sumsq / n) - mean * mean;
        sd = (uint32_t)sqrt((double)((var > 0) ? var : 0));
    }
    int k = snprintf(out, cap,
                     "{\"target_us\":%u,\"n\":%u,\"avg_us\":%u,\"sd_us\":%u,"
                     "\"min_us\":%u,\"max_us\":%u,\"bad\":%u,"
                     "\"starved\":%lu,\"late\":%lu,\"frames\":%lu,"
                     "\"slack_us\":%lld,\"catchup\":%lu,\"overrun\":%lu,"
                     "\"sum_us\":%llu,\"hist\":[",
                     (unsigned)s_px.period_us, (unsigned)n, (unsigned)avg, (unsigned)sd,
                     (unsigned)s_jit.min_us, (unsigned)s_jit.max_us, (unsigned)s_jit.bad,
                     (unsigned long)s_px.st.starved, (unsigned long)s_px.st.late,
                     (unsigned long)s_px.st.frames,
                     (long long)s_clock.slack_us, (unsigned long)s_clock.catchup_n,
                     (unsigned long)s_clock.overrun_n,
                     (unsigned long long)s_jit.sum);
    if (k < 0 || (size_t)k >= cap) {
        return 0;
    }
    for (int i = 0; i < 64; i++) {
        const int w = snprintf(out + k, cap - (size_t)k, "%s%u", (i > 0) ? "," : "",
                               (unsigned)s_jit.hist[i]);
        if (w < 0 || (size_t)(k + w) >= cap) {
            return 0;
        }
        k += w;
    }
    const int w = snprintf(out + k, cap - (size_t)k, "]}");
    return (w > 0 && (size_t)(k + w) < cap) ? (size_t)(k + w) : 0;
}

// ── 渲染 loop：只做「取最新一格 → 編碼 → 交給後端」，不跑效果。
static void px_render_loop(void *arg)
{
    (void)arg;
    int64_t prev = esp_timer_get_time();
    s_px.mark_us = prev;
    s_px.mark_frames = 0;
    while (!nc_task_exit_requested()) {
        // deadline 時鐘：未到就 arm 一次性 timer 等；已過（catch-up）就即刻做。
        const int64_t wait_us = nc_fps_clock_wait(&s_clock, esp_timer_get_time());
        if (wait_us > 0 && s_px.timer != NULL && s_px.tick != NULL) {
            (void)esp_timer_start_once(s_px.timer, (uint64_t)wait_us);
            (void)xSemaphoreTake(s_px.tick, pdMS_TO_TICKS(1000));
        }
        for (int i = 0; i < s_px.nlane; i++) {
            lane_tick(&s_px.lane[i]);
        }
        const int64_t after = esp_timer_get_time();
        nc_fps_clock_done(&s_clock, after);
        const uint32_t dt = (uint32_t)(after - prev);
        s_px.st.frame_us = dt;
        jit_record(dt, s_px.period_us);
        if (dt > s_px.st.frame_max_us) {
            s_px.st.frame_max_us = dt;
        }
        prev = after;

        const int64_t span = after - s_px.mark_us;
        if (span >= 1000000) {
            const uint32_t n = s_px.st.frames - s_px.mark_frames;
            s_px.st.fps_x10 = (uint32_t)(((int64_t)n * 10 * 1000000) / span);
            s_px.mark_us = after;
            s_px.mark_frames = s_px.st.frames;
            uint32_t drop = 0;
            for (int i = 0; i < nc_px_ring_count(); i++) {
                nc_px_q_stat_t qs;
                nc_px_ring_stats(nc_px_ring_at(i), &qs);
                drop += qs.drops;
            }
            s_px.st.drop = drop;
        }
        nc_task_beat();

        // 節拍由 timer 控制；如果這一輪做超過一個 period，記一次 late。
        if (s_px.period_us > 0) {
            const uint32_t spent = (uint32_t)(esp_timer_get_time() - after);
            if (spent > s_px.period_us) {
                s_px.st.late++;
            }
        }
    }
}

size_t nc_render_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap,
                     "{\"started\":%d,\"fps\":%u,\"fps_x10\":%lu,\"frames\":%lu"
                     ",\"fresh\":%lu,\"starved\":%lu,\"late\":%lu"
                     ",\"fail\":%lu,\"skipped\":%lu,\"drop\":%lu,\"frame_us\":%lu"
                     ",\"frame_max_us\":%lu,\"enc_us\":%lu,\"tx_us\":%lu"
                     ",\"lanes\":[",
                     s_px.started ? 1 : 0, (unsigned)s_px.fps, (unsigned long)s_px.st.fps_x10,
                     (unsigned long)s_px.st.frames, (unsigned long)s_px.st.fresh,
                     (unsigned long)s_px.st.starved, (unsigned long)s_px.st.late,
                     (unsigned long)s_px.st.fail, (unsigned long)s_px.st.skipped,
                     (unsigned long)s_px.st.drop, (unsigned long)s_px.st.frame_us,
                     (unsigned long)s_px.st.frame_max_us, (unsigned long)s_px.st.enc_us,
                     (unsigned long)s_px.st.tx_us);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    for (int i = 0; i < s_px.nlane; i++) {
        const int k = snprintf(out + n, cap - (size_t)n,
                               "%s{\"be\":\"%s\",\"lanes\":%u,\"width\":%u,\"ring\":\"%s\""
                               ",\"px\":%lu,\"seq\":%lu,\"fill\":%lu}",
                               (i > 0) ? "," : "", s_px.lane[i].be->name,
                               (unsigned)s_px.lane[i].be->lanes,
                               (unsigned)s_px.lane[i].be->width,
                               nc_px_ring_name(s_px.lane[i].q),
                               (unsigned long)nc_px_ring_px(s_px.lane[i].q),
                               (unsigned long)s_px.lane[i].last_seq,
                               (unsigned long)nc_px_ring_fill(s_px.lane[i].q));
        if (k < 0 || (size_t)k >= cap - (size_t)n) {
            return (size_t)n;
        }
        n += k;
    }
    const int k = snprintf(out + n, cap - (size_t)n, "]}");
    if (k < 0 || (size_t)k >= cap - (size_t)n) {
        return (size_t)n;
    }
    n += k;
    return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

// ═══════════════════════ 任務 ═══════════════════════
static int px_declare(nc_task_res_t *out, int max)
{
    const int n = plan_lanes();
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        out[k++] = (nc_task_res_t){
            .name = s_plan[i].ring,
            .bytes = nc_px_ring_bytes(s_plan[i].px, s_plan[i].slots),
            // 環由 CPU 寫、CPU 讀；DMA 讀的是後端自己的 internal 槽，不是環。
            // 大畫面（9,600 px × 4 × 3 = 115 KB）internal 放唔落 → build-time 政策選 PSRAM。
#if CONFIG_NETCORE_PIXEL_RING_PSRAM
            .cls = NC_PLAN_PSRAM,
#else
            .cls = NC_PLAN_INTERNAL,
#endif
            .align = 64,
            .slot_bytes = nc_px_slot_bytes(s_plan[i].px),
            .n = (uint16_t)s_plan[i].slots,
        };
    }
    return k;
}

static esp_err_t px_start(const nc_task_res_view_t *res, int nres)
{
    const int n = plan_lanes();
    if (n == 0) {
        ESP_LOGE(TAG, "沒有可用後端 → 渲染器不啟動");
        return ESP_ERR_NOT_FOUND;
    }
    if (nres < n) {
        ESP_LOGE(TAG, "只拿到 %d 項資源（要 %d）→ 渲染器不啟動", nres, n);
        return ESP_FAIL;
    }
    for (int i = 0; i < n; i++) {
        if (res[i].ptr == NULL) {
            ESP_LOGE(TAG, "[%s] 沒有拿到環", s_plan[i].ring);
            return ESP_FAIL;
        }
        nc_pixel_q_t *q = nc_px_ring_init(&s_rings[i], &(nc_px_ring_cfg_t){
            .name = s_plan[i].ring,
            .region = res[i].ptr,
            .region_bytes = (uint32_t)res[i].len,
            .px = s_plan[i].px,
            .slots = s_plan[i].slots, });
        if (q == NULL || nc_px_ring_register(q) != 0) {
            return ESP_FAIL;
        }
    }
    if (setup_backends() != ESP_OK) {
        return ESP_FAIL;
    }
    const esp_err_t e = bind_lanes();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "一個後端都綁不到環 → 渲染器不啟動");
        return e;
    }
    // 渲染節拍由 **設定檔** 控制：system.json 的 frame_interval_ms（預設 20 ms = 50 fps）。
    // 呢個係用戶可見、可改的節奏；Kconfig 的 FPS 只在設定拿不到時做後備。
    uint32_t interval_ms = 20;
    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys != NULL && sys->frame_interval_ms > 0) {
        interval_ms = sys->frame_interval_ms;
    }
    s_px.period_us = interval_ms * 1000u;
    s_px.fps = (uint16_t)(1000u / interval_ms);
    memset(&s_px.st, 0, sizeof(s_px.st));
    for (int i = 0; i < s_px.nlane; i++) {
        // 開機時生產者也許已經先寫了一幀：從最新那一格開始（不補播舊幀）
        const nc_px_hdr_t *h = NULL;
        uint32_t seq = 0;
        if (nc_px_ring_peek(s_px.lane[i].q, &h, &seq) != NULL) {
            s_px.lane[i].last_seq = seq;
            nc_px_ring_release(s_px.lane[i].q, seq);
        }
    }
    // 節拍：esp_timer **一次性**，每輪按 deadline 時鐘重新 arm（可追趕）。
    s_px.tick = xSemaphoreCreateBinary();
    const esp_timer_create_args_t targs = {
        .callback = render_tick,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "render_tick",
    };
    if (s_px.tick == NULL || esp_timer_create(&targs, &s_px.timer) != ESP_OK) {
        ESP_LOGE(TAG, "節拍 timer 建立失敗（%u µs）", (unsigned)s_px.period_us);
        return ESP_FAIL;
    }
    nc_fps_clock_init(&s_clock, esp_timer_get_time(), (int64_t)s_px.period_us,
                      (int64_t)CONFIG_NETCORE_PIXEL_SLACK_MS * 1000);
    s_px.started = true;
    ESP_LOGI(TAG, "渲染器就緒：%d 道 lane、%u fps（frame_interval_ms=%u）；lane0 = %s（環 %s）",
             s_px.nlane, (unsigned)s_px.fps, (unsigned)interval_ms, s_px.lane[0].be->name,
             nc_px_ring_name(s_px.lane[0].q));
    return ESP_OK;
}

static void px_stop(void)
{
    s_px.started = false;
    if (s_px.timer != NULL) {
        (void)esp_timer_stop(s_px.timer);
        (void)esp_timer_delete(s_px.timer);
        s_px.timer = NULL;
    }
    if (s_px.tick != NULL) {
        vSemaphoreDelete(s_px.tick);
        s_px.tick = NULL;
    }
}

static const nc_task_loop_t s_loops[] = {
    {
        // 渲染器：環 → 編碼 → 後端。效果在另一個任務（pixel）。
        .name = "render",
        .fn = px_render_loop,
        .stack = CONFIG_NETCORE_PIXEL_STACK,
        .prio = NC_PRIO_OTHERS_MAX,   // 租戶上限；寫更高會被管理器夾回來
        .core = CONFIG_NETCORE_PIXEL_CORE,
        .layer = 5,
        .period_ms = 0,               // loop 自己計節拍
    },
};

static const nc_task_t s_task = {
    .name = "render",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 10,
    .declare = px_declare,
    .on_start = px_start,
    .on_stop = px_stop,
    .loops = s_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(render, &s_task);

const nc_task_t *nc_render_task(void)
{
    return &s_task;
}

#else   // !CONFIG_NETCORE_PIXEL

// 這塊板沒開 pixel：main.c 不會登記，呼叫端只會拿到 NULL。
const nc_task_t *nc_render_task(void)
{
    return NULL;
}

void nc_render_get_stats(nc_px_stat_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

size_t nc_render_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(out, cap, "{\"started\":0,\"err\":\"disabled\"}");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

#endif  // CONFIG_NETCORE_PIXEL
