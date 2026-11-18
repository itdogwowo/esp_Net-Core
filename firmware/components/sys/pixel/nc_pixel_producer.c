// nc_pixel_producer.c — 內建效果生產者（"rgbmatrix"）。
//
// 職責**只有**：算出 R,G,B,W 畫面 → 寫進環的一格 → 交給渲染器。
// 唔碰 I80／SPI／DMA／時序，唔知有幾多條 lane，唔知幀率上限。
//
// 為什麼效果 60 fps 就夠（舊 rgbmatrix 是 450 fps）：
//   呼吸／波浪最快的一個週期是幾秒，60 fps 已經有 60 個取樣；
//   而「抖動」的閃爍頻率 = 更新率 / (rate × 2^(bits-8))，12 位 rate 1 時是
//   450/16 = 28 Hz（舊版）對 60/16 = 3.75 Hz（這裡）。3.75 Hz 在整塊板同步
//   呼吸時是「慢慢亮一級」，唔會像 28 Hz 那樣格格跳。
//   想要更快就把 cfg.fps 開到 450 —— 那是設定，不是重新設計。
#include "nc_pixel_producer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nc_pixel_core.h"
#include "sdkconfig.h"

static const char *TAG = "px_prd";

#if CONFIG_NETCORE_PIXEL

#define RGB_PI_F 3.14159265358979f

// ═══════════════════════ 表頭（plan 交付的那 24 B）═══════════════════════
// 為什麼不直接寫 nc_px_hdr_t *：那 24 B 是渲染器環的一部分，seq 由 commit 寫，
// 這裡只碰「內容」相關的欄位。用一個鏡像結構，避免生產者拿到 seq 亂寫。
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
static uint8_t *s_buf;              // plan 交付的 payload（px × 4 B）
static prd_hdr_t *s_hdr;
static uint32_t s_px;               // 這條 lane 幾顆
static bool     s_bound;

static nc_px_producer_cfg_t s_cfg;

static uint8_t  s_led[NC_PX_PRD_N][3];    // 這一格實際送出的 8-bit（已過抖動）
static uint16_t s_tgt[NC_PX_PRD_N][3];    // 8.8 定點目標
static int32_t  s_acc[NC_PX_PRD_N][3];    // 抖動誤差（單位 1/256）
static uint16_t s_gam[1024];              // gamma：(4..255) linear → 8.8
static int16_t  s_sin[1024];              // Q15 sine（init 建一次，熱路徑冇浮點）

static uint32_t s_frame;                  // 算過幾格
static int64_t  s_phase_us;               // 效果時間（累加實際 dt）
static int64_t  s_prev_us;
static uint16_t s_gen = 1;                // 換場代數（換效果／全黑 → +1）

static bool     s_demo;
static int      s_demo_stage;
static int64_t  s_demo_t0;
static int64_t  s_demo_blank_until;

static nc_px_producer_stat_t s_stat;
static int64_t s_mark_us;

// ═══════════════════════ 表 ═══════════════════════
static void tables_init(void)
{
    if (s_gam[1023] != 0) {
        return;                         // 已經建過
    }
    for (int i = 0; i < 1024; i++) {
        const float x = (float)i / 1023.0f;
        float y = powf(x, 2.2f) * 255.0f * 256.0f;   // 8.8
        if (y > 65535.0f) {
            y = 65535.0f;
        }
        s_gam[i] = (uint16_t)(y + 0.5f);
        s_sin[i] = (int16_t)lroundf(sinf(2.0f * RGB_PI_F * (float)i / 1024.0f) * 32767.0f);
    }
}

// 把 8.8 的目標量化到 bits 位（8..16）。例：bits=12 ⇒ 每級 16/256 = 1/16 個 8-bit 級。
static inline uint16_t quant_bits(uint16_t tgt, uint8_t bits)
{
    if (bits >= 16) {
        return tgt;
    }
    if (bits < 8) {
        bits = 8;
    }
    const uint32_t step = 1u << (16u - bits);
    return (uint16_t)((((uint32_t)tgt + step / 2u) / step) * step);
}

static inline uint32_t mul_q15(uint32_t a, int32_t q)
{
    return (a * (uint32_t)q) >> 15;
}

static inline uint16_t gamma_mod(uint16_t lin)
{
    uint32_t idx = (uint32_t)lin >> 6;          // 0..1023
    const uint32_t frac = (uint32_t)lin & 63u;
    if (idx >= 1023u) {
        return s_gam[1023];
    }
    const uint32_t a = s_gam[idx];
    const uint32_t b = s_gam[idx + 1u];
    return (uint16_t)(a + (((b - a) * frac) >> 6));
}

// 0..255 線性亮度（已含 gain）→ 16-bit linear
static inline uint16_t lin_of(uint32_t level255)
{
    return (uint16_t)((level255 * (uint32_t)s_cfg.gain * 257u) / 255u);
}

// ═══════════════════════ 效果（逐區）═══════════════════════
// 8 區純邏輯：第 k 區 = 鏈上第 k*8 .. k*8+7 顆。每區可以獨立揀效果（0xFF = 跟全域），
// 所以同一條 GPIO14 上面可以「第 0 行呼吸、第 1 行波浪、第 2 行單色……」。
static void zone_clear(int z)
{
    memset(&s_tgt[z * NC_PX_PRD_ZONE_N][0], 0, sizeof(uint16_t) * 3 * NC_PX_PRD_ZONE_N);
}

// 第一區（第 0 橫行）由暗到亮的 8 級；其餘區用這個效果就全黑。
// 用途：對照絲印／接線次序，確認全部顆數都食到新資料（有漏顆／沾色即刻見到）。
static void zone_zone1(int z)
{
    zone_clear(z);
    if (z != 0) {
        return;
    }
    for (int x = 0; x < NC_PX_PRD_W; x++) {
        const uint16_t lin = lin_of((uint32_t)(80 + x * 21));   // 顯示值約 20..200
        s_tgt[x][0] = gamma_mod(lin);
        s_tgt[x][1] = gamma_mod(lin / 3);
        s_tgt[x][2] = gamma_mod(lin / 6);
    }
}

// RGB 輪流呼吸（全板同步）：每 breath_ms 一段，R → G → B。
static void zone_breath(int z, uint32_t seg_us)
{
    const uint32_t t = (uint32_t)((uint64_t)s_phase_us % ((uint64_t)seg_us * 3u));
    const int seg = (int)(t / seg_us);
    // 64-bit：12 s 一段 × 512 會爆 32-bit
    const uint32_t ph = (uint32_t)(((uint64_t)(t % seg_us) * 512u) / seg_us);
    const int32_t s = s_sin[ph & 1023u];
    const uint16_t lin = (uint16_t)(((uint32_t)s * 2u * s_cfg.gain) / 255u);
    const uint16_t v = gamma_mod(lin);
    const int base = z * NC_PX_PRD_ZONE_N;
    for (int i = 0; i < NC_PX_PRD_ZONE_N; i++) {
        s_tgt[base + i][0] = s_tgt[base + i][1] = s_tgt[base + i][2] = 0;
        s_tgt[base + i][seg] = v;
    }
}

// 純色波浪呼吸（整數）：
//   · 顏色：R → G → B 純色輪流（每 breath_ms 一段），唔用彩虹色相；
//   · 亮度：全局呼吸包絡（0.25..1.0）＋ 斜向空間波浪。
static void zone_wave(int z, int32_t t_ms)
{
    const int32_t wms = (s_cfg.wave_ms >= 200) ? (int32_t)s_cfg.wave_ms : 3000;
    const int32_t spat = (int32_t)(((int64_t)t_ms * 1024) / wms);
    const uint32_t seg_us = (uint32_t)s_cfg.breath_ms * 1000u;
    const int seg = seg_us ? (int)(((uint64_t)s_phase_us / seg_us) % 3u) : 0;
    const uint32_t ph = seg_us ? (uint32_t)(((uint64_t)(s_phase_us % seg_us) * 512u) / seg_us) : 0;
    const int32_t env = 8192 + (s_sin[ph & 1023u] >> 2);   // 0.25 .. 1.0
    const int base = z * NC_PX_PRD_ZONE_N;
    for (int x = 0; x < NC_PX_PRD_W; x++) {
        const uint32_t wph = (uint32_t)(x * 128 + z * 96 - spat) & 1023u;
        int32_t w = 16384 + (s_sin[wph] >> 1);              // 0..32767
        w = (w * w) >> 15;                                  // 暗部佔多數
        w = (int32_t)mul_q15((uint32_t)w, env);             // × 呼吸包絡
        const uint16_t lin = (uint16_t)(((uint32_t)w * 2u * s_cfg.gain) / 255u);
        for (int c = 0; c < 3; c++) {
            s_tgt[base + x][c] = (c == seg) ? gamma_mod(lin) : 0;   // 純色：只亮一個通道
        }
    }
}

// SOLID：r/g/b 係「顯示值」0..255，frac 再補 1/256 級（唔經 gamma）。
static uint16_t solid_tgt(uint8_t c)
{
    if (c == 0) {
        return 0;
    }
    const uint32_t v = ((uint32_t)c * 256u * s_cfg.gain) / 255u;
    return (uint16_t)(v + s_cfg.frac);
}

static void zone_solid(int z)
{
    const uint16_t r = solid_tgt(s_cfg.r);
    const uint16_t g = solid_tgt(s_cfg.g);
    const uint16_t b = solid_tgt(s_cfg.b);
    const int base = z * NC_PX_PRD_ZONE_N;
    for (int i = 0; i < NC_PX_PRD_ZONE_N; i++) {
        s_tgt[base + i][0] = r;
        s_tgt[base + i][1] = g;
        s_tgt[base + i][2] = b;
    }
}

// 全部顆數由 0 到 252（linear）一條線：抖動開／關一眼看得出分別。
static void zone_ramp(int z)
{
    const int base = z * NC_PX_PRD_ZONE_N;
    for (int i = 0; i < NC_PX_PRD_ZONE_N; i++) {
        const uint16_t v = gamma_mod(lin_of((uint32_t)((base + i) * 4)));
        s_tgt[base + i][0] = v;
        s_tgt[base + i][1] = v;
        s_tgt[base + i][2] = v;
    }
}

static void render_zone(int z, uint8_t eff)
{
    const uint32_t seg_us = (uint32_t)s_cfg.breath_ms * 1000u;
    const int32_t t_ms = (int32_t)(s_phase_us / 1000);
    switch (eff) {
    case NC_PX_EFF_ZONE1:  zone_zone1(z); break;
    case NC_PX_EFF_BREATH: zone_breath(z, seg_us ? seg_us : 1u); break;
    case NC_PX_EFF_WAVE:   zone_wave(z, t_ms); break;
    case NC_PX_EFF_SOLID:  zone_solid(z); break;
    case NC_PX_EFF_RAMP:   zone_ramp(z); break;
    case NC_PX_EFF_OFF:
    default:               zone_clear(z); break;
    }
}

static void effect_render(void)
{
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
        if (!(s_cfg.zone_on & (1u << z))) {
            zone_clear(z);
            continue;
        }
        const uint8_t eff = (s_cfg.zone_eff[z] != 0xFF) ? s_cfg.zone_eff[z] : s_cfg.effect;
        render_zone(z, eff);
    }
}

// ═══════════════════════ 抖動 ═══════════════════════
// rate 幀一個決策週期：want = 目標 × rate + 上一週期的餘數；
// v = floor(want / (256 × rate))、餘數留給下一週期 ⇒ 每 rate 幀的總量剛好等於目標。
// 4×4 Bayer（ordered dither）：空間上分散「不足一級」的差，逐幀轉一格，
// 令每顆燈的開關頻率維持在遠高於閃爍融合的範圍，而長期平均值仍等於目標。
static const uint8_t s_bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static void dither(void)
{
    const uint32_t rot = s_frame >> 1;      // 每兩幀轉一格
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
        for (int i = z * NC_PX_PRD_ZONE_N; i < (z + 1) * NC_PX_PRD_ZONE_N; i++) {
            uint8_t mode = (s_cfg.zone_dither[z] != 0xFF) ? s_cfg.zone_dither[z] : s_cfg.dither;
            uint8_t rate = (s_cfg.zone_rate[z] != 0) ? s_cfg.zone_rate[z] : 1;
            uint8_t bits = s_cfg.zone_bits[z] ? s_cfg.zone_bits[z] : s_cfg.bits;
            uint8_t phase = s_cfg.zone_phase[z];
            if (s_cfg.sim6) {
                // 3 層 × 左右：
                //   上帶（第 0–2 行）＝ 8 位原生（唔抖，做頻率比較的基準）
                //   中帶（第 3–4 行）＝ 10 位；左 = rate 1、右 = rate 2
                //   下帶（第 5–7 行）＝ 12 位；左 = rate 1、右 = rate 2
                const int px = i % NC_PX_PRD_W;
                const int py = i / NC_PX_PRD_W;
                const int band = (py < 3) ? 0 : (py < 5) ? 1 : 2;
                const bool right = (px >= (NC_PX_PRD_W / 2));
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
                const uint16_t q = quant_bits(s_tgt[i][c], bits);
                if (spatial) {
                    const int px = (int)(i % NC_PX_PRD_W);
                    const int py = (int)(i / NC_PX_PRD_W);
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
// WS2812 之類沒有白通道的燈，白只能在產生者這一側處理：
//   白 → R=G=B（唔用「min(R,G,B) 抽到 W」那種寫法，因為那是 RGBW 燈珠的省電技巧，
//   在 RGB 燈上會把白色抽成 0，畫面直接變黑）。
static void emit(void)
{
    const uint32_t n = (s_px < NC_PX_PRD_N) ? s_px : (uint32_t)NC_PX_PRD_N;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t r = s_led[i][0];
        const uint8_t g = s_led[i][1];
        const uint8_t b = s_led[i][2];
        uint8_t *d = s_buf + (size_t)i * NC_PX_BPP;
        d[0] = r;
        d[1] = g;
        d[2] = b;
        // 唔用 white→RGB 的省電技巧；W 欄位只在 RGBW 燈上有意義。
        // RGB 全亮（r=g=b=255）就係白；渲染器只會把 W 丟掉或送出去。
        d[3] = (uint8_t)((r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b));
    }
    // 這條 lane 比畫面大（多串了一段）：剩下的熄掉，免得留下上一幀的殘影。
    for (uint32_t i = n; i < s_px; i++) {
        uint8_t *d = s_buf + (size_t)i * NC_PX_BPP;
        d[0] = d[1] = d[2] = d[3] = 0;
    }
}

// ═══════════════════════ 自動 demo ═══════════════════════
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
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
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
esp_err_t nc_px_producer_bind(uint8_t *buf, uint32_t px, void *hdr, uint32_t region_bytes)
{
    if (buf == NULL || hdr == NULL || px == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // region_bytes = payload 可用空間（不含表頭）。0 = 不檢查（呼叫端自己負責）。
    if (region_bytes != 0 && region_bytes < px * NC_PX_BPP) {
        ESP_LOGE(TAG, "環的 payload 只有 %u B，裝不下 %u 顆 × %u B",
                 (unsigned)region_bytes, (unsigned)px, (unsigned)NC_PX_BPP);
        return ESP_ERR_INVALID_SIZE;
    }
    s_buf = buf;
    s_px = px;
    s_hdr = (prd_hdr_t *)hdr;
    memset(s_hdr, 0, sizeof(*s_hdr));
    s_hdr->src = NC_PX_SRC_BUILTIN;
    s_hdr->gen = s_gen;
    s_bound = true;
    tables_init();
    nc_px_producer_reset();
    ESP_LOGI(TAG, "生產者綁定：%u 顆（畫面 %d 顆），效果 %u，%u fps",
             (unsigned)px, (int)NC_PX_PRD_N, s_cfg.effect, (unsigned)s_cfg.fps);
    return ESP_OK;
}

bool nc_px_producer_ready(void)
{
    return s_bound;
}

void nc_px_producer_reset(void)
{
    memset(s_led, 0, sizeof(s_led));
    memset(s_tgt, 0, sizeof(s_tgt));
    // 抖動初始相位：每一顆、每個通道錯開（由位置決定，唔用隨機數；長期平均不變）。
    // 唔錯開的話，同一區 8 顆會在**同一格**一齊跳，暗部看起來像整條在閃。
    for (int i = 0; i < NC_PX_PRD_N; i++) {
        for (int c = 0; c < 3; c++) {
            s_acc[i][c] = (int32_t)(((uint32_t)i * 37u + (uint32_t)c * 91u) & 0xFFu);
        }
    }
}

bool nc_px_producer_frame(int64_t now_us)
{
    if (!s_bound || s_buf == NULL || s_hdr == NULL) {
        return false;
    }
    int64_t dt = now_us - s_prev_us;
    if (dt < 0 || dt > 200000) {     // 停過（debug／載入）就唔好一次跳好多
        dt = 200000;
    }
    s_prev_us = now_us;
    s_phase_us += dt;

    const int64_t t0 = esp_timer_get_time();
    demo_step();
    effect_render();
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

void nc_px_producer_stats(nc_px_producer_stat_t *out)
{
    if (out != NULL) {
        *out = s_stat;
    }
}

// ═══════════════════════ 設定 ═══════════════════════
void nc_px_producer_set_cfg(const nc_px_producer_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    nc_px_producer_cfg_t c = *cfg;
    if (c.bits < 8) { c.bits = 8; }
    if (c.bits > 16) { c.bits = 16; }
    if (c.dither > 2) { c.dither = 1; }
    if (c.breath_ms < 200) { c.breath_ms = 200; }
    if (c.wave_ms < 200) { c.wave_ms = 200; }
    if (c.fps == 0) { c.fps = 60; }
    s_cfg = c;
    s_gen++;
}

void nc_px_producer_set_effect(uint8_t effect)
{
    manual();
    if (effect < NC_PX_EFF_COUNT) {
        if (s_cfg.effect != effect) {
            s_gen++;
        }
        s_cfg.effect = effect;
    }
}

void nc_px_producer_set_dither_mode(uint8_t mode)
{
    manual();
    // 舊 API 是「bool on」：1 就是 temporal（=1），0 就是 off（=0），剛好一致。
    s_cfg.dither = (mode > 2) ? 1 : mode;
}

void nc_px_producer_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    manual();
    s_cfg.r = r;
    s_cfg.g = g;
    s_cfg.b = b;
}

void nc_px_producer_set_gain(uint8_t gain)
{
    manual();
    s_cfg.gain = gain;
}

void nc_px_producer_set_fps(uint16_t fps)
{
    manual();
    s_cfg.fps = (fps == 0) ? 60 : fps;
}

void nc_px_producer_set_zone(int idx, uint8_t rate, uint8_t phase)
{
    manual();
    if (idx >= 0 && idx < NC_PX_PRD_ZONES) {
        s_cfg.zone_rate[idx] = rate ? rate : 1;
        s_cfg.zone_phase[idx] = phase;
    }
}

void nc_px_producer_set_zone_mask(uint8_t mask)
{
    manual();
    s_cfg.zone_on = mask;
}

void nc_px_producer_set_bits(uint8_t bits)
{
    manual();
    s_cfg.bits = (bits < 8) ? 8 : (bits > 16) ? 16 : bits;
}

void nc_px_producer_set_zone_bits(int idx, uint8_t bits)
{
    manual();
    if (idx >= 0 && idx < NC_PX_PRD_ZONES) {
        s_cfg.zone_bits[idx] = (bits == 0) ? 0 : ((bits < 8) ? 8 : (bits > 16) ? 16 : bits);
    }
}

void nc_px_producer_set_zone_bits_all(const uint8_t bits[NC_PX_PRD_ZONES])
{
    manual();
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
        nc_px_producer_set_zone_bits(z, bits[z]);
    }
}

void nc_px_producer_set_zone_effect(int idx, uint8_t effect)
{
    manual();
    if (idx >= 0 && idx < NC_PX_PRD_ZONES) {
        s_cfg.zone_eff[idx] = (effect >= NC_PX_EFF_COUNT) ? 0xFF : effect;
    }
}

void nc_px_producer_set_zone_effects_all(const uint8_t eff[NC_PX_PRD_ZONES])
{
    manual();
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
        s_cfg.zone_eff[z] = (eff[z] >= NC_PX_EFF_COUNT) ? 0xFF : eff[z];
    }
}

void nc_px_producer_set_fraction(uint8_t frac8)
{
    manual();
    s_cfg.frac = frac8;
}

void nc_px_producer_set_zone_dither_all(const uint8_t mode[NC_PX_PRD_ZONES])
{
    manual();
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
        s_cfg.zone_dither[z] = (mode[z] > 2) ? 0xFF : mode[z];
    }
}

void nc_px_producer_set_speed(uint16_t breath_ms)
{
    manual();
    s_cfg.breath_ms = (breath_ms < 200) ? 200 : breath_ms;
}

void nc_px_producer_set_wave_speed(uint16_t wave_ms)
{
    manual();
    s_cfg.wave_ms = (wave_ms < 200) ? 200 : wave_ms;
}

void nc_px_producer_demo(bool on)
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

uint8_t nc_px_producer_stage(void)
{
    return (uint8_t)(s_demo ? s_demo_stage : 0xFF);
}

size_t nc_px_producer_json(char *out, size_t cap)
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
        s_cfg.breath_ms, s_cfg.wave_ms, s_demo ? 1u : 0u, nc_px_producer_stage(),
        s_cfg.zone_eff[0], s_cfg.zone_eff[1], s_cfg.zone_eff[2], s_cfg.zone_eff[3],
        s_cfg.zone_eff[4], s_cfg.zone_eff[5], s_cfg.zone_eff[6], s_cfg.zone_eff[7],
        s_cfg.zone_rate[0], s_cfg.zone_rate[1], s_cfg.zone_rate[2], s_cfg.zone_rate[3],
        s_cfg.zone_rate[4], s_cfg.zone_rate[5], s_cfg.zone_rate[6], s_cfg.zone_rate[7],
        s_cfg.zone_phase[0], s_cfg.zone_phase[1], s_cfg.zone_phase[2], s_cfg.zone_phase[3],
        s_cfg.zone_phase[4], s_cfg.zone_phase[5], s_cfg.zone_phase[6], s_cfg.zone_phase[7],
        s_cfg.zone_on, (unsigned)s_gen, (unsigned long)s_px);
    if (n < 0) {
        return 0;
    }
    return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

// ═══════════════════════ 預設值／初始化 ═══════════════════════
// 開機預設（同舊 rgbmatrix 一致的那幾項）：
//   gain 60 = 暗（產品頁警告過熱）；12 位；呼吸 4 s、波浪 3 s；8 區相位錯開。
// 唔同的一項：更新率 60 fps（舊版 450）—— 理由見檔頭。
void nc_px_producer_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.effect = NC_PX_EFF_BREATH;
    s_cfg.dither = 1;
    s_cfg.r = s_cfg.g = s_cfg.b = 24;
    s_cfg.gain = 60;
    s_cfg.bits = 12;
    s_cfg.breath_ms = 4000;
    s_cfg.wave_ms = 3000;
    s_cfg.fps = 60;
    s_cfg.zone_on = 0xFF;
    memset(s_cfg.zone_eff, 0xFF, sizeof(s_cfg.zone_eff));
    memset(s_cfg.zone_dither, 0xFF, sizeof(s_cfg.zone_dither));
    s_cfg.sim_rate[0] = 1; s_cfg.sim_rate[1] = 2; s_cfg.sim_rate[2] = 3;
    s_cfg.sim_rate[3] = 4; s_cfg.sim_rate[4] = 6; s_cfg.sim_rate[5] = 8;
    // 每區相位錯開；偶數區每格更新、奇數區兩格（60 fps / 2 = 30 Hz 仍然遠高於閃爍融合）。
    for (int z = 0; z < NC_PX_PRD_ZONES; z++) {
        s_cfg.zone_rate[z] = (uint8_t)(1 + (z & 1));
        s_cfg.zone_phase[z] = (uint8_t)z;
    }
    s_demo = true;                      // 開機自動跑 demo（任何手動指令會熄）
    s_prev_us = esp_timer_get_time();
    s_mark_us = s_prev_us;
}

#else   // !CONFIG_NETCORE_PIXEL ── 這塊板冇開 pixel → 全部 no-op（main 仍然可以登記）

esp_err_t nc_px_producer_bind(uint8_t *buf, uint32_t px, void *hdr, uint32_t region_bytes)
{
    (void)buf; (void)px; (void)hdr; (void)region_bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

bool nc_px_producer_ready(void) { return false; }
bool nc_px_producer_frame(int64_t now_us) { (void)now_us; return false; }
void nc_px_producer_reset(void) { }

nc_px_producer_cfg_t nc_px_producer_cfg(void)
{
    nc_px_producer_cfg_t c;
    memset(&c, 0, sizeof(c));
    return c;
}

void nc_px_producer_set_cfg(const nc_px_producer_cfg_t *cfg) { (void)cfg; }
void nc_px_producer_set_effect(uint8_t effect) { (void)effect; }
void nc_px_producer_set_dither_mode(uint8_t mode) { (void)mode; }
void nc_px_producer_set_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void nc_px_producer_set_gain(uint8_t gain) { (void)gain; }
void nc_px_producer_set_fps(uint16_t fps) { (void)fps; }
void nc_px_producer_set_zone(int idx, uint8_t rate, uint8_t phase) { (void)idx; (void)rate; (void)phase; }
void nc_px_producer_set_zone_mask(uint8_t mask) { (void)mask; }
void nc_px_producer_set_bits(uint8_t bits) { (void)bits; }
void nc_px_producer_set_zone_bits(int idx, uint8_t bits) { (void)idx; (void)bits; }
void nc_px_producer_set_zone_bits_all(const uint8_t bits[NC_PX_PRD_ZONES]) { (void)bits; }
void nc_px_producer_set_zone_effect(int idx, uint8_t effect) { (void)idx; (void)effect; }
void nc_px_producer_set_zone_effects_all(const uint8_t eff[NC_PX_PRD_ZONES]) { (void)eff; }
void nc_px_producer_set_fraction(uint8_t frac8) { (void)frac8; }
void nc_px_producer_set_zone_dither_all(const uint8_t mode[NC_PX_PRD_ZONES]) { (void)mode; }
void nc_px_producer_set_speed(uint16_t breath_ms) { (void)breath_ms; }
void nc_px_producer_set_wave_speed(uint16_t wave_ms) { (void)wave_ms; }
void nc_px_producer_demo(bool on) { (void)on; }
uint8_t nc_px_producer_stage(void) { return 0xFF; }
void nc_px_producer_stats(nc_px_producer_stat_t *out) { if (out) { memset(out, 0, sizeof(*out)); } }
void nc_px_producer_defaults(void) { }

size_t nc_px_producer_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(out, cap, "{\"ready\":0,\"err\":\"disabled\"}");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

#endif  // CONFIG_NETCORE_PIXEL
