// nc_px_engine_core.c — 效果引擎（工具）：畫布 → 量化 → 抖動 → canonical R,G,B,W。
//
// 引擎**唔知道**有邊幾個效果：它只認 id，向 registry 查 desc。
// 具體效果住 `components/effects/<域>_effects/`（實作），只 include `nc_px_engine.h`。
// 規則見 docs/03_notes/behavior-taxonomy.md §2.0／§3。
//
// 為什麼效果 60 fps 就夠（舊 rgbmatrix 是 450 fps）：
//   呼吸／波浪最快的一個週期是幾秒，60 fps 已經有 60 個取樣；
//   而「抖動」的閃爍頻率 = 更新率 / (rate × 2^(bits-8))，12 位 rate 1 時是
//   60/16 = 3.75 Hz。3.75 Hz 在整塊板同步呼吸時是「慢慢亮一級」，唔會格格跳。
#include "nc_px_engine.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nc_pixel_core.h"
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL

static const char *TAG = "px_eng";

// ═══════════════════════ 表頭（plan 交付的那 24 B）═══════════════════════
// 為什麼不直接寫 nc_px_hdr_t *：那 24 B 是渲染器環的一部分，seq 由 commit 寫，
// 這裡只碰「內容」相關的欄位。用一個鏡像結構，避免引擎拿到 seq 亂寫。
typedef struct {
    uint32_t seq;
    uint32_t ts_us;
    uint16_t gen;
    uint8_t  src;
    uint8_t  flags;
    uint32_t drops;
    uint8_t  _rsv[8];
} prd_hdr_t;

_Static_assert(sizeof(prd_hdr_t) == NC_PX_HDR_BYTES, "prd_hdr_t 必須同 nc_px_hdr_t 一樣大");

// ═══════════════════════ 狀態 ═══════════════════════
static uint8_t *s_buf;              // 這一次寫入的 payload（px × 4 B）
static prd_hdr_t *s_hdr;
static uint32_t s_px;               // 這條 lane 幾顆
static bool     s_bound;
static uint8_t *s_last_payload;     // 上一格算好的內容（多 lane 複製用）
static prd_hdr_t s_last_hdr;
static bool     s_last_valid;

static nc_px_engine_cfg_t s_cfg;

static uint16_t s_canvas[NC_PX_CANVAS_N][3];   // 效果寫的 8.8 顯示值畫布
static uint8_t  s_led[NC_PX_CANVAS_N][3];      // 這一格實際送出的 8-bit（已過抖動）
static int32_t  s_acc[NC_PX_CANVAS_N][3];      // 抖動誤差（單位 1/256）

static uint32_t s_frame;                  // 算過幾格
static int64_t  s_phase_us;               // 效果時間（累加實際 dt）
static int64_t  s_prev_us;
static uint16_t s_gen = 1;                // 換場代數（換效果／全黑 → +1）

static bool     s_demo;
static int      s_demo_stage;
static int64_t  s_demo_t0;
static int64_t  s_demo_blank_until;

static nc_px_engine_stat_t s_stat;
static int64_t s_mark_us;

// ═══════════════════════ 效果註冊表 ═══════════════════════
#define PX_FX_MAX 16
static const nc_px_engine_desc_t *s_fx[PX_FX_MAX];
static int s_nfx;

void nc_px_engine_register(const nc_px_engine_desc_t *d)
{
    if (d == NULL || d->render == NULL) {
        return;
    }
    if (nc_px_engine_find(d->id) != NULL) {
        ESP_LOGW(TAG, "效果 id %u 重複註冊（%s），忽略", (unsigned)d->id, d->name ? d->name : "?");
        return;
    }
    if (s_nfx >= PX_FX_MAX) {
        ESP_LOGE(TAG, "效果註冊表滿了（%d），%s 唔會生效", PX_FX_MAX, d->name ? d->name : "?");
        return;
    }
    s_fx[s_nfx++] = d;
}

const nc_px_engine_desc_t *nc_px_engine_find(uint8_t id)
{
    for (int i = 0; i < s_nfx; i++) {
        if (s_fx[i]->id == id) {
            return s_fx[i];
        }
    }
    return NULL;
}

int nc_px_engine_fx_count(void) { return s_nfx; }

// ═══════════════════════ 逐區 render ═══════════════════════
// 8 區純邏輯：第 k 區 = 鏈上第 k*8 .. k*8+7 顆。每區可以獨立揀效果（0xFF = 跟全域），
// 所以同一條 GPIO14 上面可以「第 0 行呼吸、第 1 行波浪、第 2 行單色……」。
static void render_zones(const nc_px_engine_params_t *p)
{
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        uint16_t *dst = &s_canvas[z * NC_PX_CANVAS_ZONE_N][0];
        memset(dst, 0, sizeof(uint16_t) * 3 * NC_PX_CANVAS_ZONE_N);   // 全黑＝清楚嘅起點
        if (!(s_cfg.zone_on & (1u << z))) {
            continue;
        }
        const uint8_t eff = (s_cfg.zone_eff[z] != 0xFF) ? s_cfg.zone_eff[z] : s_cfg.effect;
        const nc_px_engine_desc_t *d = nc_px_engine_find(eff);
        if (d == NULL) {
            continue;                       // OFF／未註冊 → 保持黑
        }
        const nc_px_engine_in_t in = {
            .px = NC_PX_CANVAS_ZONE_N,
            .w = NC_PX_CANVAS_W,
            .h = 1,
            .zone = (uint16_t)z,
            .now_us = s_phase_us,
            .params = p,
        };
        d->render(NULL, &in, dst);
    }
}

// ═══════════════════════ 抖動 ═══════════════════════
// rate 幀一個決策週期：want = 目標 × rate + 上一週期的餘數；
// v = floor(want / (256 × rate))、餘數留給下一週期 ⇒ 每 rate 幀的總量剛好等於目標。
// 4×4 Bayer（ordered dither）：空間上分散「不足一級」的差，逐幀轉一格。
static const uint8_t s_bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static void dither(void)
{
    const uint32_t rot = s_frame >> 1;      // 每兩幀轉一格
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        for (int i = z * NC_PX_CANVAS_ZONE_N; i < (z + 1) * NC_PX_CANVAS_ZONE_N; i++) {
            uint8_t mode = (s_cfg.zone_dither[z] != 0xFF) ? s_cfg.zone_dither[z] : s_cfg.dither;
            uint8_t rate = (s_cfg.zone_rate[z] != 0) ? s_cfg.zone_rate[z] : 1;
            uint8_t bits = s_cfg.zone_bits[z] ? s_cfg.zone_bits[z] : s_cfg.bits;
            uint8_t phase = s_cfg.zone_phase[z];
            if (s_cfg.sim6) {
                // 3 層 × 左右：
                //   上帶（第 0–2 行）＝ 8 位原生（唔抖，做頻率比較的基準）
                //   中帶（第 3–4 行）＝ 10 位；左 = rate 1、右 = rate 2
                //   下帶（第 5–7 行）＝ 12 位；左 = rate 1、右 = rate 2
                const int px = i % NC_PX_CANVAS_W;
                const int py = i / NC_PX_CANVAS_W;
                const int band = (py < 3) ? 0 : (py < 5) ? 1 : 2;
                const bool right = (px >= (NC_PX_CANVAS_W / 2));
                if (band == 0) {
                    bits = 8;
                    mode = 0;
                } else {
                    bits = (band == 1) ? 10 : 12;
                    mode = 1;
                    rate = right ? 2 : 1;
                }
                phase = 0;
            }
            const bool spatial = (mode == 2 || mode == 3);
            const uint32_t rot_z = (mode == 3) ? 0u : rot;
            const bool tick = (((s_frame + phase) % rate) == 0);
            for (int c = 0; c < 3; c++) {
                const uint16_t q = nc_px_engine_quant(s_canvas[i][c], bits);
                if (spatial) {
                    const int px = (int)(i % NC_PX_CANVAS_W);
                    const int py = (int)(i / NC_PX_CANVAS_W);
                    const uint8_t b = s_bayer[(px + (int)rot_z) & 3][(py + (int)(rot_z >> 2)) & 3];
                    int32_t v = (int32_t)q >> 8;
                    const int32_t frac = (int32_t)q & 0xFF;
                    if (frac * 16 > (int32_t)b * 256 + 128) {
                        v++;
                    }
                    if (v < 0) {
                        v = 0;
                    } else if (v > 255) {
                        v = 255;
                    }
                    s_led[i][c] = (uint8_t)v;
                    s_acc[i][c] = 0;
                    continue;
                }
                if (mode == 0) {
                    s_led[i][c] = (uint8_t)((q + 128u) >> 8);
                    s_acc[i][c] = 0;
                    continue;
                }
                if (!tick) {
                    continue;                       // 保留上一個決策的值
                }
                const int32_t tgt = (int32_t)q;
                const int32_t want = tgt * (int32_t)rate + s_acc[i][c];
                int32_t v = want / (256 * (int32_t)rate);
                if (v < 0) {
                    v = 0;
                } else if (v > 255) {
                    v = 255;
                }
                s_led[i][c] = (uint8_t)v;
                s_acc[i][c] = want - v * 256 * (int32_t)rate;
                if (s_acc[i][c] < 0 || s_acc[i][c] > 256 * (int32_t)rate) {
                    s_acc[i][c] = 0;
                }
            }
        }
    }
}

// ═══════════════════════ 寫進環的一格 ═══════════════════════
// 這裡是**唯一**知道「canonical = R,G,B,W 四位」的地方（除了編碼器的格式表）。
// WS2812 之類沒有白通道的燈，白只能在引擎這一側處理：
//   白 → R=G=B（唔用「min(R,G,B) 抽到 W」那種省電技巧，那在 RGB 燈上會把白色抽成 0）。
static void emit(void)
{
    const uint32_t n = (s_px < NC_PX_CANVAS_N) ? s_px : (uint32_t)NC_PX_CANVAS_N;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t r = s_led[i][0];
        const uint8_t g = s_led[i][1];
        const uint8_t b = s_led[i][2];
        uint8_t *d = s_buf + (size_t)i * NC_PX_BPP;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        d[3] = (uint8_t)((r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b));
    }
    // 這條 lane 比畫面大（多串了一段）：剩下的熄掉，免得留下上一幀的殘影。
    for (uint32_t i = n; i < s_px; i++) {
        uint8_t *d = s_buf + (size_t)i * NC_PX_BPP;
        d[0] = d[1] = d[2] = d[3] = 0;
    }
}

// ═══════════════════════ 自動 demo（原廠導覽）═══════════════════════
typedef struct {
    const char *name;
    uint32_t    ms;
    uint8_t     effect;
    uint8_t     gain;
    uint8_t     bits;
    uint8_t     dither;
    uint16_t    speed;
    uint16_t    wave_ms;
    uint8_t     ab;        // 1 = 每兩區一層位數（8/10/11/12）
} demo_stage_t;

static const demo_stage_t s_stages[] = {
    // 同一塊板上並排 4 種位數：第 0–1 行 8 位、2–3 行 10 位、4–5 行 11 位、6–7 行 12 位。
    { "呼吸：8 / 10 / 11 / 12 位（每兩行一層）", 90000, NC_PX_EFF_BREATH, 40, 12, 1, 20000, 3000, 1 },
    { "波浪：8 / 10 / 11 / 12 位（每兩行一層）", 90000, NC_PX_EFF_WAVE,   45, 12, 1, 20000, 14000, 1 },
};
#define DEMO_N ((int)(sizeof(s_stages) / sizeof(s_stages[0])))

static void demo_apply(int i)
{
    const demo_stage_t *st = &s_stages[i];
    s_cfg.effect = st->effect;
    s_cfg.gain = st->gain;
    s_cfg.bits = st->bits;
    s_cfg.dither = st->dither;
    s_cfg.breath_ms = st->speed;
    s_cfg.wave_ms = st->wave_ms ? st->wave_ms : 3000;
    s_cfg.r = s_cfg.g = s_cfg.b = 255;
    s_cfg.frac = 0;
    s_cfg.sim6 = 0;
    s_cfg.zone_on = 0xFF;
    memset(s_cfg.zone_dither, 0xFF, sizeof(s_cfg.zone_dither));
    memset(s_cfg.zone_eff, 0xFF, sizeof(s_cfg.zone_eff));
    memset(s_cfg.zone_bits, 0, sizeof(s_cfg.zone_bits));
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        s_cfg.zone_rate[z] = 1;
        s_cfg.zone_phase[z] = (uint8_t)z;
        if (st->ab) {
            uint8_t b = 8;
            if (z >= 2) { b = 10; }
            if (z >= 4) { b = 11; }
            if (z >= 6) { b = 12; }
            s_cfg.zone_bits[z] = b;
            s_cfg.zone_dither[z] = (b == 8) ? 0 : 1;
            s_cfg.zone_phase[z] = 0;
        }
    }
    s_gen++;                    // 換場 → 渲染器整格重載
}

static void demo_step(void)
{
    if (!s_demo) {
        return;
    }
    const int64_t el_ms = (esp_timer_get_time() - s_demo_t0) / 1000;
    if (s_demo_stage >= 0 && el_ms < (int64_t)s_stages[s_demo_stage].ms) {
        return;
    }
    s_demo_stage = (s_demo_stage + 1) % DEMO_N;
    s_demo_t0 = esp_timer_get_time();
    s_demo_blank_until = s_demo_t0 + 300000;      // 轉場：全黑 300 ms
    demo_apply(s_demo_stage);
    ESP_LOGI(TAG, "demo %d/%d：%s（%u ms）", s_demo_stage + 1, DEMO_N,
             s_stages[s_demo_stage].name, (unsigned)s_stages[s_demo_stage].ms);
}

// demo 期間手動 setter 都會呼叫這個：手動就等於接管。
static void manual(void)
{
    s_demo = false;
}

// ═══════════════════════ 生命週期 ═══════════════════════
esp_err_t nc_px_engine_bind(uint32_t px)
{
    if (px == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_px = px;
    s_bound = true;
    s_last_valid = false;
    s_last_payload = NULL;
    nc_px_engine_tables_init();
    nc_px_engine_reset();
    ESP_LOGI(TAG, "引擎綁定：%u 顆（畫布 %d 顆），效果 %u，%d 個效果已註冊",
             (unsigned)px, (int)NC_PX_CANVAS_N, s_cfg.effect, nc_px_engine_fx_count());
    return ESP_OK;
}

bool nc_px_engine_ready(void)
{
    return s_bound;
}

void nc_px_engine_reset(void)
{
    memset(s_led, 0, sizeof(s_led));
    memset(s_canvas, 0, sizeof(s_canvas));
    // 抖動初始相位：每一顆、每個通道錯開（由位置決定，唔用隨機數；長期平均不變）。
    for (int i = 0; i < NC_PX_CANVAS_N; i++) {
        for (int c = 0; c < 3; c++) {
            s_acc[i][c] = (int32_t)(((uint32_t)i * 37u + (uint32_t)c * 91u) & 0xFFu);
        }
    }
}

bool nc_px_engine_frame(uint8_t *dst, void *hdr, int64_t now_us)
{
    if (!s_bound || dst == NULL || hdr == NULL) {
        return false;
    }
    // 每次寫的是「呼叫端剛 begin 的那一格」。環的 writer 游標每次 commit 都前進。
    s_buf = dst;
    s_hdr = (prd_hdr_t *)hdr;
    int64_t dt = now_us - s_prev_us;
    if (dt < 0 || dt > 200000) {     // 停過（debug／載入）就唔好一次跳好多
        dt = 200000;
    }
    s_prev_us = now_us;
    s_phase_us += dt;

    const nc_px_engine_params_t p = {
        .gain = s_cfg.gain,
        .r = s_cfg.r,
        .g = s_cfg.g,
        .b = s_cfg.b,
        .frac = s_cfg.frac,
        .breath_ms = s_cfg.breath_ms,
        .wave_ms = s_cfg.wave_ms,
    };

    const int64_t t0 = esp_timer_get_time();
    demo_step();
    render_zones(&p);
    dither();
    emit();

    // 表頭：**唔碰 seq**（seq 是渲染器的發布旗標，由 commit 寫）。
    s_hdr->ts_us = (uint32_t)(uint64_t)now_us;
    s_hdr->gen = s_gen;
    s_hdr->src = NC_PX_SRC_BUILTIN;
    s_hdr->flags = (s_cfg.effect == NC_PX_EFF_OFF) ? NC_PX_F_CLEAR : 0;
    if (s_demo && now_us < s_demo_blank_until) {
        s_hdr->flags |= NC_PX_F_CLEAR;
    }
    s_hdr->drops = s_stat.late;

    s_last_payload = s_buf;
    s_last_hdr = *s_hdr;
    s_last_valid = true;
    s_stat.render_us = (uint32_t)(esp_timer_get_time() - t0);
    s_stat.frames++;
    s_frame++;

    const int64_t span = now_us - s_mark_us;
    if (span >= 1000000) {
        s_stat.fps_x10 = (uint32_t)(((int64_t)s_stat.frames * 10 * 1000000) / span);
        s_stat.frames = 0;
        s_mark_us = now_us;
    }
    return true;
}

bool nc_px_engine_copy(uint8_t *dst, void *hdr, uint32_t capacity_bytes)
{
    if (!s_bound || !s_last_valid || dst == NULL || hdr == NULL || s_last_payload == NULL) {
        return false;
    }
    if (capacity_bytes < s_px * NC_PX_BPP) {
        return false;
    }
    memcpy(dst, s_last_payload, (size_t)s_px * NC_PX_BPP);
    prd_hdr_t *h = (prd_hdr_t *)hdr;
    *h = s_last_hdr;         // ts/gen/src/flags/drops 跟同一格
    h->seq = 0;              // seq 由 ring commit 寫
    return true;
}

void nc_px_engine_stats(nc_px_engine_stat_t *out)
{
    if (out != NULL) {
        *out = s_stat;
    }
}

// ═══════════════════════ 設定 ═══════════════════════
void nc_px_engine_set_cfg(const nc_px_engine_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    nc_px_engine_cfg_t c = *cfg;
    if (c.bits < 8) { c.bits = 8; }
    if (c.bits > 16) { c.bits = 16; }
    if (c.dither > 2) { c.dither = 1; }
    if (c.breath_ms < 200) { c.breath_ms = 200; }
    if (c.wave_ms < 200) { c.wave_ms = 200; }
    if (c.fps == 0) { c.fps = 60; }
    s_cfg = c;
    s_gen++;
}

nc_px_engine_cfg_t nc_px_engine_cfg(void)
{
    return s_cfg;
}

void nc_px_engine_set_effect(uint8_t effect)
{
    manual();
    if (effect < NC_PX_EFF_COUNT) {
        if (s_cfg.effect != effect) {
            s_gen++;
        }
        s_cfg.effect = effect;
    }
}

void nc_px_engine_set_dither_mode(uint8_t mode)
{
    manual();
    // 舊 API 是「bool on」：1 就是 temporal（=1），0 就是 off（=0），剛好一致。
    s_cfg.dither = (mode > 2) ? 1 : mode;
}

void nc_px_engine_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    manual();
    s_cfg.r = r;
    s_cfg.g = g;
    s_cfg.b = b;
}

void nc_px_engine_set_gain(uint8_t gain)
{
    manual();
    s_cfg.gain = gain;
}

void nc_px_engine_set_fps(uint16_t fps)
{
    manual();
    s_cfg.fps = (fps == 0) ? 60 : fps;
}

void nc_px_engine_set_zone(int idx, uint8_t rate, uint8_t phase)
{
    manual();
    if (idx >= 0 && idx < NC_PX_CANVAS_ZONES) {
        s_cfg.zone_rate[idx] = rate ? rate : 1;
        s_cfg.zone_phase[idx] = phase;
    }
}

void nc_px_engine_set_zone_mask(uint8_t mask)
{
    manual();
    s_cfg.zone_on = mask;
}

void nc_px_engine_set_bits(uint8_t bits)
{
    manual();
    s_cfg.bits = (bits < 8) ? 8 : (bits > 16) ? 16 : bits;
}

void nc_px_engine_set_zone_bits(int idx, uint8_t bits)
{
    manual();
    if (idx >= 0 && idx < NC_PX_CANVAS_ZONES) {
        s_cfg.zone_bits[idx] = (bits == 0) ? 0 : ((bits < 8) ? 8 : (bits > 16) ? 16 : bits);
    }
}

void nc_px_engine_set_zone_bits_all(const uint8_t bits[NC_PX_CANVAS_ZONES])
{
    manual();
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        nc_px_engine_set_zone_bits(z, bits[z]);
    }
}

void nc_px_engine_set_zone_effect(int idx, uint8_t effect)
{
    manual();
    if (idx >= 0 && idx < NC_PX_CANVAS_ZONES) {
        s_cfg.zone_eff[idx] = (effect >= NC_PX_EFF_COUNT) ? 0xFF : effect;
    }
}

void nc_px_engine_set_zone_effects_all(const uint8_t eff[NC_PX_CANVAS_ZONES])
{
    manual();
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        s_cfg.zone_eff[z] = (eff[z] >= NC_PX_EFF_COUNT) ? 0xFF : eff[z];
    }
}

void nc_px_engine_set_fraction(uint8_t frac8)
{
    manual();
    s_cfg.frac = frac8;
}

void nc_px_engine_set_zone_dither_all(const uint8_t mode[NC_PX_CANVAS_ZONES])
{
    manual();
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        s_cfg.zone_dither[z] = (mode[z] > 2) ? 0xFF : mode[z];
    }
}

void nc_px_engine_set_speed(uint16_t breath_ms)
{
    manual();
    s_cfg.breath_ms = (breath_ms < 200) ? 200 : breath_ms;
}

void nc_px_engine_set_wave_speed(uint16_t wave_ms)
{
    manual();
    s_cfg.wave_ms = (wave_ms < 200) ? 200 : wave_ms;
}

void nc_px_engine_demo(bool on)
{
    s_demo = on;
    if (on) {
        s_demo_stage = 0;
        s_demo_t0 = esp_timer_get_time();
        s_demo_blank_until = s_demo_t0;
        demo_apply(0);
        ESP_LOGI(TAG, "demo 重開");
    }
}

uint8_t nc_px_engine_stage(void)
{
    return (uint8_t)(s_demo ? s_demo_stage : 0xFF);
}

size_t nc_px_engine_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(
        out, cap,
        "{\"ready\":%d,\"eff\":%u,\"dither\":%u,\"gain\":%u,\"fps\":%u,\"fps_x10\":%lu"
        ",\"frames\":%lu,\"render_us\":%lu,\"late\":%lu"
        ",\"bits\":%u,\"bits_z\":[%u,%u,%u,%u,%u,%u,%u,%u]"
        ",\"frac\":%u,\"sim6\":%u,\"zdither\":[%u,%u,%u,%u,%u,%u,%u,%u]"
        ",\"speed\":%u,\"wspeed\":%u,\"demo\":%u,\"stage\":%u"
        ",\"zeff\":[%u,%u,%u,%u,%u,%u,%u,%u]"
        ",\"rate\":[%u,%u,%u,%u,%u,%u,%u,%u]"
        ",\"phase\":[%u,%u,%u,%u,%u,%u,%u,%u],\"zmask\":%u,\"gen\":%u,\"px\":%lu}",
        s_bound ? 1 : 0, s_cfg.effect, (unsigned)s_cfg.dither, s_cfg.gain,
        (unsigned)s_cfg.fps, (unsigned long)s_stat.fps_x10, (unsigned long)s_stat.frames,
        (unsigned long)s_stat.render_us, (unsigned long)s_stat.late,
        s_cfg.bits, s_cfg.zone_bits[0], s_cfg.zone_bits[1], s_cfg.zone_bits[2],
        s_cfg.zone_bits[3], s_cfg.zone_bits[4], s_cfg.zone_bits[5], s_cfg.zone_bits[6],
        s_cfg.zone_bits[7],
        s_cfg.frac, (unsigned)s_cfg.sim6,
        s_cfg.zone_dither[0], s_cfg.zone_dither[1], s_cfg.zone_dither[2], s_cfg.zone_dither[3],
        s_cfg.zone_dither[4], s_cfg.zone_dither[5], s_cfg.zone_dither[6], s_cfg.zone_dither[7],
        s_cfg.breath_ms, s_cfg.wave_ms, s_demo ? 1u : 0u, nc_px_engine_stage(),
        s_cfg.zone_eff[0], s_cfg.zone_eff[1], s_cfg.zone_eff[2], s_cfg.zone_eff[3],
        s_cfg.zone_eff[4], s_cfg.zone_eff[5], s_cfg.zone_eff[6], s_cfg.zone_eff[7],
        s_cfg.zone_rate[0], s_cfg.zone_rate[1], s_cfg.zone_rate[2], s_cfg.zone_rate[3],
        s_cfg.zone_rate[4], s_cfg.zone_rate[5], s_cfg.zone_rate[6], s_cfg.zone_rate[7],
        s_cfg.zone_phase[0], s_cfg.zone_phase[1], s_cfg.zone_phase[2], s_cfg.zone_phase[3],
        s_cfg.zone_phase[4], s_cfg.zone_phase[5], s_cfg.zone_phase[6], s_cfg.zone_phase[7],
        s_cfg.zone_on, (unsigned)s_gen, (unsigned long)s_px);
    // 截斷 = 回 0（呼叫端會退到 {}），不要交半截 JSON 出去。
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}

// ═══════════════════════ 預設值 ═══════════════════════
// 開機預設（同舊 rgbmatrix 一致的那幾項）：
//   gain 60 = 暗（產品頁警告過熱）；呼吸 4 s、波浪 3 s；8 區相位錯開。
// 抖動係實驗功能：預設 8-bit、唔抖動。
void nc_px_engine_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.effect = NC_PX_EFF_BREATH;
    s_cfg.dither = 0;
    s_cfg.r = s_cfg.g = s_cfg.b = 24;
    s_cfg.gain = 60;
    s_cfg.bits = 8;
    s_cfg.breath_ms = 4000;
    s_cfg.wave_ms = 3000;
    s_cfg.fps = 0;            // 0 = 自由填環（節奏由 render 任務控制）
    s_cfg.zone_on = 0xFF;
    memset(s_cfg.zone_eff, 0xFF, sizeof(s_cfg.zone_eff));
    memset(s_cfg.zone_dither, 0xFF, sizeof(s_cfg.zone_dither));
    s_cfg.sim_rate[0] = 1; s_cfg.sim_rate[1] = 2; s_cfg.sim_rate[2] = 3;
    s_cfg.sim_rate[3] = 4; s_cfg.sim_rate[4] = 6; s_cfg.sim_rate[5] = 8;
    for (int z = 0; z < NC_PX_CANVAS_ZONES; z++) {
        s_cfg.zone_rate[z] = (uint8_t)(1 + (z & 1));
        s_cfg.zone_phase[z] = (uint8_t)z;
    }
    s_demo = false;
    s_prev_us = esp_timer_get_time();
    s_mark_us = s_prev_us;
}

#else   // !CONFIG_NETCORE_PIXEL ── 這塊板冇開 pixel → 全部 no-op（main 仍然可以登記）

void nc_px_engine_register(const nc_px_engine_desc_t *d) { (void)d; }
const nc_px_engine_desc_t *nc_px_engine_find(uint8_t id) { (void)id; return NULL; }
int nc_px_engine_fx_count(void) { return 0; }
void nc_px_engine_tables_init(void) { }
uint16_t nc_px_engine_gamma(uint16_t lin16) { (void)lin16; return 0; }
uint16_t nc_px_engine_lin(const nc_px_engine_params_t *p, uint32_t level255) { (void)p; (void)level255; return 0; }
int32_t nc_px_engine_sin(uint32_t phase) { (void)phase; return 0; }
uint16_t nc_px_engine_quant(uint16_t tgt88, uint8_t bits) { (void)tgt88; (void)bits; return 0; }

esp_err_t nc_px_engine_bind(uint32_t px)
{
    (void)px;
    return ESP_ERR_NOT_SUPPORTED;
}

bool nc_px_engine_ready(void) { return false; }
bool nc_px_engine_frame(uint8_t *dst, void *hdr, int64_t now_us)
{
    (void)dst; (void)hdr; (void)now_us;
    return false;
}
bool nc_px_engine_copy(uint8_t *dst, void *hdr, uint32_t capacity_bytes)
{
    (void)dst; (void)hdr; (void)capacity_bytes;
    return false;
}
void nc_px_engine_reset(void) { }

nc_px_engine_cfg_t nc_px_engine_cfg(void)
{
    nc_px_engine_cfg_t c;
    memset(&c, 0, sizeof(c));
    return c;
}

void nc_px_engine_set_cfg(const nc_px_engine_cfg_t *cfg) { (void)cfg; }
void nc_px_engine_set_effect(uint8_t effect) { (void)effect; }
void nc_px_engine_set_dither_mode(uint8_t mode) { (void)mode; }
void nc_px_engine_set_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void nc_px_engine_set_gain(uint8_t gain) { (void)gain; }
void nc_px_engine_set_fps(uint16_t fps) { (void)fps; }
void nc_px_engine_set_zone(int idx, uint8_t rate, uint8_t phase) { (void)idx; (void)rate; (void)phase; }
void nc_px_engine_set_zone_mask(uint8_t mask) { (void)mask; }
void nc_px_engine_set_bits(uint8_t bits) { (void)bits; }
void nc_px_engine_set_zone_bits(int idx, uint8_t bits) { (void)idx; (void)bits; }
void nc_px_engine_set_zone_bits_all(const uint8_t bits[NC_PX_CANVAS_ZONES]) { (void)bits; }
void nc_px_engine_set_zone_effect(int idx, uint8_t effect) { (void)idx; (void)effect; }
void nc_px_engine_set_zone_effects_all(const uint8_t eff[NC_PX_CANVAS_ZONES]) { (void)eff; }
void nc_px_engine_set_fraction(uint8_t frac8) { (void)frac8; }
void nc_px_engine_set_zone_dither_all(const uint8_t mode[NC_PX_CANVAS_ZONES]) { (void)mode; }
void nc_px_engine_set_speed(uint16_t breath_ms) { (void)breath_ms; }
void nc_px_engine_set_wave_speed(uint16_t wave_ms) { (void)wave_ms; }
void nc_px_engine_demo(bool on) { (void)on; }
uint8_t nc_px_engine_stage(void) { return 0xFF; }
void nc_px_engine_stats(nc_px_engine_stat_t *out) { if (out) { memset(out, 0, sizeof(*out)); } }
void nc_px_engine_defaults(void) { }

size_t nc_px_engine_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(out, cap, "{\"ready\":0,\"err\":\"disabled\"}");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

#endif  // CONFIG_NETCORE_PIXEL
