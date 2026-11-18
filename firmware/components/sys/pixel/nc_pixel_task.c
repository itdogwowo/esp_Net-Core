// nc_pixel.h — 後端 / 環 / 編碼器的公開介面。
//
// 檔案分工（每個含義只有一處）：
//   nc_pixel_q.c           環（SPSC、假消費、gen 換手）
//   nc_pixel_encode.c      canonical R,G,B,W → 各驅動的物理格式（**唯一一份**轉換）
//   nc_pixel_backend_*.c   i80 / spi / direct / mock
//   nc_pixel_task.c        任務生命週期：declare 環、建後端、綁 lane
#include "nc_pixel.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nc_pixel_producer.h"
#include "nc_prio.h"
#include "nc_task.h"
#include "sdkconfig.h"

static const char *TAG = "pixel";

#if CONFIG_NETCORE_PIXEL

// ═══════════════════════ 註冊表：後端 / 環 ═══════════════════════
static nc_pixel_backend_t *s_be[NC_PX_MAX_LANES];
static int s_nbe;
static nc_pixel_q_t *s_ring[NC_PX_MAX_LANES];
static int s_nring;

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

esp_err_t nc_px_ring_register(nc_pixel_q_t *q)
{
    if (q == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *nm = nc_px_ring_name(q);
    if (nm == NULL || nc_px_ring_find(nm) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_nring >= NC_PX_MAX_LANES) {
        return ESP_ERR_NO_MEM;
    }
    s_ring[s_nring++] = q;
    return ESP_OK;
}

int nc_px_ring_count(void) { return s_nring; }

nc_pixel_q_t *nc_px_ring_at(int i)
{
    return (i >= 0 && i < s_nring) ? s_ring[i] : NULL;
}

nc_pixel_q_t *nc_px_ring_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_nring; i++) {
        if (strcmp(nc_px_ring_name(s_ring[i]), name) == 0) {
            return s_ring[i];
        }
    }
    return NULL;
}

nc_pixel_q_t *nc_px_active_ring(void) { return (s_nring > 0) ? s_ring[0] : NULL; }

bool nc_px_put(const uint8_t *rgbw, uint8_t src, uint16_t gen)
{
    return nc_px_ring_put(nc_px_active_ring(), rgbw, src, gen);
}

int nc_px_cfg_parse_backends(nc_pixel_cfg_t *cfg, const char *list)
{
    if (cfg == NULL) {
        return 0;
    }
    cfg->nlane = 0;
    if (list == NULL) {
        return 0;
    }
    const char *p = list;
    while (*p != '\0' && cfg->nlane < NC_PX_MAX_LANES) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        int len = (int)(p - start);
        while (len > 0 && start[len - 1] == ' ') {
            len--;
        }
        if (len > 0 && len < NC_PX_BACKEND_MAX) {
            nc_px_lane_cfg_t *ln = &cfg->lane[cfg->nlane++];
            memcpy(ln->be, start, (size_t)len);
            ln->be[len] = '\0';
            ln->ring[0] = '\0';
        }
    }
    return cfg->nlane;
}

// ═══════════════════════ 設定 ═══════════════════════
// 預設 = 「這塊板有 I80 就用 I80（由內建效果餵），否則退到 mock」；
// main.c／port 可以整份覆寫。環名帶 prd = 內建效果生產者餵這一格。
// 第二道 mock lane 是刻意留的：沒有燈板時整條路（環 → 編碼 → 後端 → 回收）
// 仍然會被驅動，0xF061 看得到 fps 與 hold/fresh 的比例。
static nc_pixel_cfg_t s_cfg = {
    .fps = 60,
    .nlane = 2,
    .lane = {
        { .be = "i80", .ring = "i80.prd" },
        { .be = "mock", .ring = "" },
    },
};

// 環名裡帶 "prd" 的環由內建生產者（效果）餵；其餘留給 `.nce`／主機推送。
static bool ring_is_builtin(const char *ring)
{
    return (ring != NULL) && (strstr(ring, ".prd") != NULL || strstr(ring, "prd.") != NULL);
}

void nc_pixel_set_cfg(const nc_pixel_cfg_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
        s_nplan = 0;                 // 設定改了，plan 重算
    }
}

const nc_pixel_cfg_t *nc_pixel_cfg(void)
{
    return &s_cfg;
}

// ═══════════════════════ 後端：誰存在、要幾大 ═══════════════════════
// 一個後端的「一格幾顆」由它自己回答，而且**不碰硬體**：
//   · i80／spi 只在 Kconfig 開了、而且板上有腳位時才算存在；
//   · direct／mock 永遠存在 —— 沒有燈板也能把整條路跑起來。
uint32_t nc_px_stage_px(const char *be)
{
    if (be == NULL) {
        return 0;
    }
    const uint32_t px = (CONFIG_NETCORE_PIXEL_LEDS > 0) ? (uint32_t)CONFIG_NETCORE_PIXEL_LEDS
                                                        : 0u;
    if (px == 0 || px > NC_PX_MAX_LEDS) {
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

const char *nc_px_stage_ring(const char *be, int lane)
{
    static char buf[NC_PX_NAME_MAX + 1];
    (void)snprintf(buf, sizeof(buf), "%s.%d", (be != NULL) ? be : "px", lane);
    return buf;
}

uint32_t nc_px_stage_slot_bytes(const char *be)
{
    const uint32_t px = nc_px_stage_px(be);
    return (px == 0) ? 0 : nc_px_slot_bytes(px);
}

typedef struct {
    char     ring[NC_PX_NAME_MAX + 1];
    char     be[NC_PX_BACKEND_MAX];
    uint32_t px;
    uint32_t slots;
} px_plan_t;

static px_plan_t s_plan[NC_PX_MAX_LANES];
static int s_nplan;
static nc_pixel_q_t s_rings[NC_PX_MAX_LANES];

static int plan_lanes(void)
{
    if (s_nplan > 0) {
        return s_nplan;
    }
    for (int i = 0; i < s_cfg.nlane && i < NC_PX_MAX_LANES; i++) {
        const uint32_t px = nc_px_stage_px(s_cfg.lane[i].be);
        if (px == 0) {
            continue;
        }
        px_plan_t *pl = &s_plan[s_nplan++];
        (void)snprintf(pl->be, sizeof(pl->be), "%s", s_cfg.lane[i].be);
        // 環名：lane cfg 寫了就用它（例如 "i80.prd" 表示「這條環由內建效果餵」），
        // 否則 "<後端>.<第幾道>"。
        if (s_cfg.lane[i].ring[0] != '\0') {
            (void)snprintf(pl->ring, sizeof(pl->ring), "%s", s_cfg.lane[i].ring);
        } else {
            (void)snprintf(pl->ring, sizeof(pl->ring), "%s.%d", s_cfg.lane[i].be, i);
        }
        pl->px = px;
        // 3 格：DMA 送第 N 格時，生產者可以填 N+1、N+2（P6 的三緩衝）。
        pl->slots = 3;
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

esp_err_t nc_px_setup_backends(void)
{
    const int n = plan_lanes();
    for (int i = 0; i < n; i++) {
        const px_plan_t *pl = &s_plan[i];
        nc_px_codec_cfg_t cc;
        codec_cfg_of(&cc, pl->px, pl->be);
#if CONFIG_NETCORE_PIXEL_I80
        if (strcmp(pl->be, "i80") == 0) {
            static const int s_pins[] = { CONFIG_NETCORE_PIXEL_I80_PINS, -1, -1 };
            nc_px_i80_cfg_t ic = {
                .pins = s_pins,
                .lanes = (uint8_t)CONFIG_NETCORE_PIXEL_I80_LANES,
                .pclk_hz = cc.wave.clk_hz,
                .px = pl->px,
                .chain = NULL,
                .fmt = cc.fmt,
                .wave = cc.wave,
                .dither_bits = cc.bits,
                .dither_mode = cc.dither_mode,
                .zone_px = (uint8_t)cc.zone_px,
                .slots = (uint8_t)pl->slots,
            };
            if (nc_px_backend_i80(&ic) == NULL) {
                ESP_LOGE(TAG, "i80 後端建立失敗（腳位被佔用或沒開）→ 這道 lane 停用");
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
            if (nc_px_backend_spi(&sc) == NULL) {
                ESP_LOGE(TAG, "spi 後端建立失敗 → 這道 lane 停用");
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
    bool                builtin;    // 這條環由內建效果生產者餵
} px_lane_t;

static struct {
    bool         started;
    uint16_t     fps;
    px_lane_t    lane[NC_PX_MAX_LANES];
    int          nlane;
    nc_px_stat_t st;
    uint32_t     mark_frames;
    int64_t      mark_us;
    int          prd_lane;         // 內建生產者餵第幾道 lane（-1 = 沒有）
    int64_t      prd_next_us;
} s_px;

void nc_px_get_stats(nc_px_stat_t *out)
{
    if (out != NULL) {
        *out = s_px.st;
    }
}

esp_err_t nc_px_bind_lanes(void)
{
    s_px.nlane = 0;
    for (int i = 0; i < s_nplan; i++) {
        nc_pixel_backend_t *be = nc_px_backend_find(s_plan[i].be);
        nc_pixel_q_t *q = nc_px_ring_find(s_plan[i].ring);
        if (be == NULL || q == NULL) {
            continue;                // 後端沒建起來（例如 GPIO 被佔用）→ 這道 lane 停用
        }
        s_px.lane[s_px.nlane].be = be;
        s_px.lane[s_px.nlane].q = q;
        s_px.lane[s_px.nlane].last_seq = 0;
        s_px.lane[s_px.nlane].builtin = ring_is_builtin(nc_px_ring_name(q));
        s_px.nlane++;
    }
    return (s_px.nlane > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ── 內建生產者的一格 ──
// 它**只**寫「最新那一格」：環是 SPSC，寫入端只有一個 writer（這個 loop）。
// 環滿了（渲染器還沒取走）就直接不寫、留給下一拍 —— 效果不需要補幀，
// 因為畫面是「當下時間的函數」，跳過一拍只是少算一次。
static void producer_tick(int64_t now_us)
{
    const int li = s_px.prd_lane;
    if (li < 0) {
        return;
    }
    px_lane_t *ln = &s_px.lane[li];
    if (!nc_px_producer_ready()) {
        return;
    }
    uint8_t *p = nc_px_ring_begin(ln->q);
    if (p == NULL) {
        s_px.st.prd_busy++;            // 環滿：下一個節拍再試（不覆蓋、不排隊）
        return;
    }
    if (!nc_px_producer_frame(now_us)) {
        nc_px_ring_abort(ln->q);
        return;
    }
    nc_px_ring_commit(ln->q);
    s_px.st.prd_frames++;
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
        s_px.st.starved++;             // 全部在飛（DMA 未回）→ 下一輪再試
        return;
    }
    if (seq == ln->last_seq) {
        s_px.st.holds++;               // 後端交回同一格 → 重播（hold）
    } else {
        s_px.st.fresh++;
        if (ln->last_seq != 0 && seq > ln->last_seq + 1u) {
            s_px.st.skipped += seq - ln->last_seq - 1u;   // 假消費：中間的格不補播
        }
        ln->last_seq = seq;
    }

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

static void px_loop(void *arg)
{
    (void)arg;
    int64_t prev = esp_timer_get_time();
    s_px.mark_us = prev;
    s_px.mark_frames = 0;
    s_px.prd_next_us = prev;
    uint32_t prd_mark = s_px.st.prd_frames;
    while (!nc_task_exit_requested()) {
        const int64_t now = esp_timer_get_time();

        // ① 生產者：效果只按自己的節奏更新（預設 60 fps），和送燈的解耦。
        //    同一條 loop 跑兩件事，是刻意的：效果與編碼搶同一份 cache，
        //    分兩個 loop 反而要兩份排程與兩條 stack，而 450 fps 的週期足夠塞得下。
        if (s_px.prd_lane >= 0 && now >= s_px.prd_next_us) {
            producer_tick(now);
            const uint16_t pfps = nc_px_producer_cfg().fps;
            s_px.prd_next_us += (pfps > 0) ? (1000000 / (int64_t)pfps) : 16666;
            if (s_px.prd_next_us < now) {
                s_px.prd_next_us = now;      // 落後太多就重新對齊，不追補
            }
        }

        for (int i = 0; i < s_px.nlane; i++) {
            lane_tick(&s_px.lane[i]);
        }
        const int64_t after = esp_timer_get_time();
        const uint32_t dt = (uint32_t)(after - prev);
        s_px.st.frame_us = dt;
        if (dt > s_px.st.frame_max_us) {
            s_px.st.frame_max_us = dt;
        }
        prev = after;

        const int64_t span = after - s_px.mark_us;
        if (span >= 1000000) {
            const uint32_t n = s_px.st.frames - s_px.mark_frames;
            s_px.st.fps_x10 = (uint32_t)(((int64_t)n * 10 * 1000000) / span);
            const uint32_t pn = s_px.st.prd_frames - prd_mark;
            s_px.st.prd_fps_x10 = (uint32_t)(((int64_t)pn * 10 * 1000000) / span);
            prd_mark = s_px.st.prd_frames;
            s_px.mark_us = after;
            s_px.mark_frames = s_px.st.frames;
            uint32_t drop = 0;
            for (int i = 0; i < s_nring; i++) {
                nc_px_q_stat_t qs;
                nc_px_ring_stats(s_ring[i], &qs);
                drop += qs.drops;
            }
            s_px.st.drop = drop;
        }
        nc_task_beat();

        if (s_px.fps > 0) {
            // 10 ms tick 粒度對 450 fps 太粗：睡到差 ~1 ms，剩下的交給下一輪的節拍
            // （µs 級精度不需要 —— 燈自己會保持上一幀）。
            const uint32_t period = 1000000u / s_px.fps;
            const uint32_t spent = (uint32_t)(esp_timer_get_time() - after);
            if (spent < period) {
                const uint32_t rest = period - spent;
                if (rest > 2000u) {
                    vTaskDelay(pdMS_TO_TICKS((rest - 1000u) / 1000u));
                }
            } else if (spent > period + 2000u) {
                s_px.st.late++;
            }
        }
    }
}

size_t nc_px_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap,
                     "{\"started\":%d,\"fps\":%u,\"fps_x10\":%lu,\"frames\":%lu"
                     ",\"fresh\":%lu,\"holds\":%lu,\"starved\":%lu,\"late\":%lu,\"fail\":%lu"
                     ",\"skipped\":%lu,\"drop\":%lu,\"frame_us\":%lu,\"frame_max_us\":%lu"
                     ",\"enc_us\":%lu,\"tx_us\":%lu,\"lanes\":[",
                     s_px.started ? 1 : 0, (unsigned)s_px.fps, (unsigned long)s_px.st.fps_x10,
                     (unsigned long)s_px.st.frames, (unsigned long)s_px.st.fresh,
                     (unsigned long)s_px.st.holds, (unsigned long)s_px.st.starved,
                     (unsigned long)s_px.st.late, (unsigned long)s_px.st.fail,
                     (unsigned long)s_px.st.skipped, (unsigned long)s_px.st.drop,
                     (unsigned long)s_px.st.frame_us, (unsigned long)s_px.st.frame_max_us,
                     (unsigned long)s_px.st.enc_us, (unsigned long)s_px.st.tx_us);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    // 生產者（環的寫入端）也一起回報：0xF061 的 "producer" 段就是這一份。
    {
        const int k = snprintf(out + n, cap - (size_t)n,
                               "\"prd\":{\"frames\":%lu,\"busy\":%lu,\"fps_x10\":%lu"
                               ",\"lane\":%d},",
                               (unsigned long)s_px.st.prd_frames,
                               (unsigned long)s_px.st.prd_busy,
                               (unsigned long)s_px.st.prd_fps_x10, s_px.prd_lane);
        if (k < 0 || (size_t)k >= cap - (size_t)n) {
            return (size_t)n;
        }
        n += k;
    }
    for (int i = 0; i < s_px.nlane; i++) {
        const int k = snprintf(out + n, cap - (size_t)n,
                               "%s{\"be\":\"%s\",\"lanes\":%u,\"ring\":\"%s\",\"px\":%lu"
                               ",\"seq\":%lu,\"fill\":%lu}",
                               (i > 0) ? "," : "", s_px.lane[i].be->name,
                               (unsigned)s_px.lane[i].be->lanes,
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
    if (k < 0) {
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
            // 環由 CPU 寫、CPU 讀；S3 的 I80 DMA 讀不到 PSRAM，但 DMA 讀的是後端
            // 自己的 internal 槽，不是環，所以環放 internal 只是為了速度。
            .cls = NC_PLAN_INTERNAL,
            .align = 64,
            .slot_bytes = nc_px_slot_bytes(s_plan[i].px),
            .n = (uint16_t)s_plan[i].slots,
        };
    }
    return k;
}

static esp_err_t px_start(const nc_task_res_view_t *res, int nres)
{
    // 生產者的預設值先立好（效果／亮度／位數／demo 都靠它）；
    // main.c 若在登記之前呼叫過 nc_px_producer_set_cfg，這裡不會覆蓋 ——
    // 判斷方式：cfg 已經被改過（effect 非零或 fps 非零）就尊重它。
    if (nc_px_producer_cfg().fps == 0) {
        nc_px_producer_defaults();
    }
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
        if (q == NULL || nc_px_ring_register(q) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (nc_px_setup_backends() != ESP_OK) {
        return ESP_FAIL;
    }
    const esp_err_t e = nc_px_bind_lanes();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "一個後端都綁不到環 → 渲染器不啟動");
        return e;
    }
    s_px.fps = s_cfg.fps;
    memset(&s_px.st, 0, sizeof(s_px.st));
    s_px.prd_lane = -1;
    for (int i = 0; i < s_px.nlane; i++) {
        // 開機時生產者也許已經先寫了一幀：從最新那一格開始（不補播舊幀）
        const nc_px_hdr_t *h = NULL;
        uint32_t seq = 0;
        if (nc_px_ring_peek(s_px.lane[i].q, &h, &seq) != NULL) {
            s_px.lane[i].last_seq = seq;
            nc_px_ring_release(s_px.lane[i].q, seq);
        }
    }
    // 內建效果生產者：綁到第一條「名字帶 prd」的環。
    // 沒有 prd 環（例如整條 lane 都是 mock）就整個不啟動 —— 生產者是這塊板的預設畫面來源。
    // 綁定的緩衝就是環的「第一格」那份 payload：環的每一格版面完全一樣（64 B 表頭 +
    // px × 4 B），而 begin() 交出來的指標一直有效到下一次 commit —— 但生產者會**在兩次
    // commit 之間**寫下一格，所以這裡自己算第 0 格的位址，而不是借用 begin() 的返回值。
    for (int i = 0; i < s_px.nlane; i++) {
        if (s_px.lane[i].builtin) {
            s_px.prd_lane = i;
            break;
        }
    }
    if (s_px.prd_lane >= 0) {
        nc_pixel_q_t *pq = s_px.lane[s_px.prd_lane].q;
        uint8_t *slot0 = (uint8_t *)nc_px_ring_region(pq);
        if (slot0 == NULL ||
            nc_px_producer_bind(slot0 + nc_px_payload_off(), nc_px_ring_px(pq), slot0,
                                nc_px_ring_slot_bytes(pq) - nc_px_payload_off()) != ESP_OK) {
            ESP_LOGE(TAG, "環 [%s] 綁不到生產者 → 生產者不啟動", nc_px_ring_name(pq));
            s_px.prd_lane = -1;
        } else {
            s_px.prd_next_us = esp_timer_get_time();
        }
    }
    s_px.started = true;
    ESP_LOGI(TAG, "渲染器就緒：%d 道 lane、%u fps；lane0 = %s（環 %s）；生產者=%s",
             s_px.nlane, (unsigned)s_px.fps, s_px.lane[0].be->name,
             nc_px_ring_name(s_px.lane[0].q),
             (s_px.prd_lane >= 0) ? "內建效果" : "外部（.nce／主機）");
    return ESP_OK;
}

static void px_stop(void)
{
    s_px.started = false;
}

static const nc_task_loop_t s_loops[] = {
    {
        .name = "pixel",
        .fn = px_loop,
        .stack = CONFIG_NETCORE_PIXEL_STACK,
        .prio = NC_PRIO_OTHERS_MAX,   // 租戶上限；寫更高會被管理器夾回來
        .core = CONFIG_NETCORE_PIXEL_CORE,
        .layer = 5,
        .period_ms = 0,               // loop 自己計節拍
    },
};

static const nc_task_t s_task = {
    .name = "pixel",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .declare = px_declare,
    .on_start = px_start,
    .on_stop = px_stop,
    .loops = s_loops,
    .nloops = 1,
};

const nc_task_t *nc_pixel_task(void)
{
    return &s_task;
}

#else   // !CONFIG_NETCORE_PIXEL

// 這塊板沒開 pixel 任務：main.c 不會登記，呼叫端只會拿到 NULL。
const nc_task_t *nc_pixel_task(void)
{
    return NULL;
}

#endif  // CONFIG_NETCORE_PIXEL

