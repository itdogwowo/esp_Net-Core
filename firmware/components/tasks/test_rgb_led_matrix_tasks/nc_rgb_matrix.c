// 8×8 WS2812 矩陣：I80 波形 + 分區時間抖動 + 測試效果。
//
// 波形（一個 bit = NC_RGB_BIT_CYCLES 個 PCLK 週期，D0 就是資料線）：
//   2.5 MHz ⇒ bit 1.200 µs；0 碼高 1 cycle = 400 ns、1 碼高 2 = 800 ns。
//   64 × 24 × 3 = 4,608 B ＋ 256 B ≈ 1.95 ms／幀（+交易開銷 ~0.45 ms ≈ 2.4 ms → ~420 fps）。
//
// 抖動：效果算出的是 8.8 定點亮度（想要的值），每幀用誤差回饋決定實際送
//   floor／ceil 哪一個 8-bit 值；長期平均 = 想要的值 ⇒ 等效多 8 bit 灰階。
//   8 個邏輯區各自有 rate（幾幀更新一次）與 phase（錯開），所以不會全板同頻閃。
#include "nc_rgb_matrix.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nc_hw.h"
#include "nc_prio.h"
#include "nc_task.h"
#include "sdkconfig.h"

#if CONFIG_NETCORE_RGB_MATRIX

static const char *TAG = "rgbmtx";

static volatile uint32_t s_dma_us;      // 由 ISR 寫：兩次 trans-done 之間
static volatile uint32_t s_dma_max_us;
static volatile int64_t  s_done_us;

#define RGB_TX_BYTES  NC_RGB_TX_BYTES
#define RGB_PI        3.14159265358979323846f

// 一個 bit 的高／低電平：byte 的 bit0 上 D0，其餘 7 條是將來的並行 lane。
static const uint8_t s_code0[NC_RGB_BIT_CYCLES] = { 0x01, 0x00, 0x00 };   // 高 1 cycle = 400 ns
static const uint8_t s_code1[NC_RGB_BIT_CYCLES] = { 0x01, 0x01, 0x00 };   // 高 2 = 800 ns

typedef struct {
    uint32_t frames;
    uint32_t late;       // 趕唔上下一幀（週期拉長／抖動）
    uint32_t tx_err;     // I80 送出失敗／超時
    uint32_t dt_us;      // 最近一幀的實際週期
    uint32_t dt_max_us;
    uint32_t tx_us;      // 最近一次提交（tx_color）耗時
    uint32_t render_us;  // 效果 + 抖動
    uint32_t enc_us;     // 編碼成 I80 byte 流
    uint32_t dma_us;     // ISR 量到的「兩次完成之間」（真 DMA 週期）
    uint32_t dma_max_us;
    uint32_t fps_x10;    // 實測 fps × 10（每 ~1 s 更新）
    int64_t  mark_us;
    uint32_t mark_frames;
} rgb_stat_t;

static nc_rgb_cfg_t s_cfg;
static uint8_t     *s_tx;                       // plan 的 DMA 緩衝
static uint8_t      s_led[NC_RGB_N][3];         // 實際送出的 8-bit 值
static int32_t      s_acc[NC_RGB_N][3];         // 抖動誤差（單位 1/256）
static uint16_t     s_tgt[NC_RGB_N][3];         // 8.8 定點目標
static uint16_t     s_gam[1024];                // gamma：10-bit linear → 8.8
static int16_t      s_sin[1024];                // Q15 sine（init 時建，熱路徑冇浮點）
static esp_lcd_i80_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static SemaphoreHandle_t s_tick;       // 節拍：esp_timer 每幀給一次
static esp_timer_handle_t s_pace;
static bool         s_ready;
static uint32_t     s_frame;
static int          s_slot;                     // 下一個要寫的 buffer 格
static int          s_inflight;                 // 已提交、未完成的 DMA 幀數
static int64_t      s_phase_us;                 // 效果時間（實際 dt 累加）
static bool         s_demo;                     // 自動 demo 開／關
static int          s_demo_stage;               // 第幾段
static int64_t      s_demo_t0;                  // 本段開始時間
static rgb_stat_t   s_stat;

// ── 表：gamma（10-bit linear → 8.8 定點）＋ sine（Q15）──
// 只在開機建一次；之後所有效果都用整數查表，冇浮點。
static void tables_init(void)
{
    for (int i = 0; i < 1024; i++) {
        const float x = (float)i / 1023.0f;
        float y = powf(x, 2.2f) * 255.0f * 256.0f;   // 8.8
        if (y > 65535.0f) {
            y = 65535.0f;
        }
        s_gam[i] = (uint16_t)(y + 0.5f);
        s_sin[i] = (int16_t)lroundf(sinf(2.0f * RGB_PI * (float)i / 1024.0f) * 32767.0f);
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

// Q15 乘法：a × q / 32768
static inline uint32_t mul_q15(uint32_t a, int32_t q)
{
    return (a * (uint32_t)q) >> 15;
}

static inline uint16_t gamma_fx(uint16_t lin)
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

// ── 效果（逐區）──
// 8 區純邏輯：第 k 區 = 鏈上第 k*8 .. k*8+7 顆。每區可以獨立揀效果（0xFF = 跟全域），
// 所以同一個 GPIO14 上面可以「第 0 行呼吸、第 1 行波浪、第 2 行單色…」。
static void zone_clear(int z)
{
    memset(&s_tgt[z * NC_RGB_ZONE_N][0], 0, sizeof(uint16_t) * 3 * NC_RGB_ZONE_N);
}

// 第一區（第 0 橫行）由暗到亮的 8 級；其餘區用這個效果就全黑。
// 用途：對照絲印／接線次序，確認 64 顆都食到新資料（有漏顆／沾色即刻見到）。
static void zone_zone1(int z)
{
    zone_clear(z);
    if (z != 0) {
        return;
    }
    for (int x = 0; x < NC_RGB_W; x++) {
        const uint16_t lin = lin_of((uint32_t)(80 + x * 21));   // 顯示值約 20..200
        s_tgt[x][0] = gamma_fx(lin);
        s_tgt[x][1] = gamma_fx(lin / 3);
        s_tgt[x][2] = gamma_fx(lin / 6);
    }
}

// RGB 輪流呼吸（全版同步）：每 breath_ms 一段，R → G → B。
// 八區時間一致 —— 要「整塊板一齊呼吸」就係呢個。
static void zone_breath(int z, uint32_t seg_us)
{
    (void)z;
    const uint32_t t = (uint32_t)((uint64_t)s_phase_us % ((uint64_t)seg_us * 3u));
    const int seg = (int)(t / seg_us);
    const uint32_t ph = (uint32_t)(((uint64_t)(t % seg_us) * 512u) / seg_us);   // 64-bit！12 s 一段 ×512 會爆 32-bit
    const int32_t s = s_sin[ph & 1023u];
    const uint16_t lin = (uint16_t)(((uint32_t)s * 2u * s_cfg.gain) / 255u);
    const uint16_t v = gamma_fx(lin);
    const int base = z * NC_RGB_ZONE_N;
    for (int i = 0; i < NC_RGB_ZONE_N; i++) {
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
    const int base = z * NC_RGB_ZONE_N;
    for (int x = 0; x < NC_RGB_W; x++) {
        const uint32_t wph = (uint32_t)(x * 128 + z * 96 - spat) & 1023u;
        int32_t w = 16384 + (s_sin[wph] >> 1);              // 0..32767
        w = (w * w) >> 15;                                  // 暗部佔多數
        w = mul_q15((uint32_t)w, env);                      // × 呼吸包絡
        const uint16_t lin = (uint16_t)(((uint32_t)w * 2u * s_cfg.gain) / 255u);
        for (int c = 0; c < 3; c++) {
            s_tgt[base + x][c] = (c == seg) ? gamma_fx(lin) : 0;   // 純色：只亮一個通道
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
    const int base = z * NC_RGB_ZONE_N;
    for (int i = 0; i < NC_RGB_ZONE_N; i++) {
        s_tgt[base + i][0] = r;
        s_tgt[base + i][1] = g;
        s_tgt[base + i][2] = b;
    }
}

// 64 顆由 0 到 252（linear）一條線：抖動開／關一眼看得出分別。
static void zone_ramp(int z)
{
    const int base = z * NC_RGB_ZONE_N;
    for (int i = 0; i < NC_RGB_ZONE_N; i++) {
        const uint16_t v = gamma_fx(lin_of((uint32_t)((base + i) * 4)));
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
    case NC_RGB_EFF_ZONE1:  zone_zone1(z); break;
    case NC_RGB_EFF_BREATH: zone_breath(z, seg_us ? seg_us : 1u); break;
    case NC_RGB_EFF_WAVE:   zone_wave(z, t_ms); break;
    case NC_RGB_EFF_SOLID:  zone_solid(z); break;
    case NC_RGB_EFF_RAMP:   zone_ramp(z); break;
    case NC_RGB_EFF_OFF:
    default:                zone_clear(z); break;
    }
}

// ── 自動 demo ──
static void pace_set(uint16_t fps);      // 定義在下面（節拍 timer）
// 目的：唔使打字都可以一路觀察「暗光階梯、抖動開關、8 區不同頻率／位數」。
typedef struct {
    const char *name;
    uint32_t    ms;        // 這段幾長
    uint8_t     effect;
    uint8_t     gain;
    uint8_t     bits;
    uint8_t     dither;    // 0 off / 1 temporal / 2 spatial（唔可以用 bool，2 會變 1）
    uint16_t    speed;     // 呼吸每段 ms
    uint8_t     rate_mode; // 0 = 全部 rate 1；1 = 1..8（每區抖動頻率都唔同）
    uint8_t     bit_mode;  // 0 = 全部 bits；1 = 8..15
    uint8_t     eff_mode;  // 0 = 跟全域；1 = 呼吸/波浪/單色/色階 輪住
    uint16_t    wave_ms;   // 波浪速度（只對 wave 段有意義）
    uint8_t     ladder;    // 1 = 8 區抖動階梯
    uint8_t     ab;        // 1 = 上下 A/B：區 0–3 用 12 位抖動、區 4–7 用 8 位原生
    uint8_t     freq;      // 1 = 抖動頻率階梯（26/52/104/208 Hz 及更低）
    uint8_t     sim6;      // 1 = 左右×3 帶模擬 fps（419/210/140/105/70/52）
} demo_stage_t;

static const demo_stage_t s_stages[] = {
    // ① 6 區模擬 fps（左/右 × 3 橫帶），10 位、時間抖動：
    //    419 / 210 / 140 / 105 / 70 / 52 fps（= 419 / rate）
    { "呼吸 6 區模擬 fps（419→52）", 90000, NC_RGB_EFF_BREATH, 40, 12, 1, 20000, 0, 0, 0, 3000, 0, 0, 0, 1 },
    { "波浪 6 區模擬 fps（419→52）", 90000, NC_RGB_EFF_WAVE,   45, 12, 1, 20000, 0, 0, 0, 14000, 0, 0, 0, 1 },
    // ② 三段位數對照（8 / 10 / 12）
    { "呼吸：上 8 位 / 中 10 位 / 下 12 位", 90000, NC_RGB_EFF_BREATH, 40, 12, 1, 20000, 0, 0, 0, 3000, 0, 1, 0, 0 },
    { "波浪：上 8 位 / 中 10 位 / 下 12 位", 90000, NC_RGB_EFF_WAVE,   45, 12, 1, 20000, 0, 0, 0, 14000, 0, 1, 0, 0 },
};
#define DEMO_N (int)(sizeof(s_stages) / sizeof(s_stages[0]))

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
    memset(s_cfg.zone_dither, 0xFF, sizeof(s_cfg.zone_dither));
    s_cfg.sim6 = 0;
    s_cfg.sim_rate[0] = 1; s_cfg.sim_rate[1] = 2; s_cfg.sim_rate[2] = 3;
    s_cfg.sim_rate[3] = 4; s_cfg.sim_rate[4] = 6; s_cfg.sim_rate[5] = 8;
    memset(s_cfg.zone_bits, 0, sizeof(s_cfg.zone_bits));
    memset(s_cfg.zone_eff, 0xFF, sizeof(s_cfg.zone_eff));
    for (int z = 0; z < NC_RGB_ZONES; z++) {
        s_cfg.zone_rate[z] = st->rate_mode ? (uint8_t)(z + 1) : 1;
        s_cfg.zone_phase[z] = (uint8_t)z;
        if (st->bit_mode) {
            s_cfg.zone_bits[z] = (uint8_t)(8 + z);
        }
        if (st->eff_mode) {
            static const uint8_t seq[4] = { NC_RGB_EFF_BREATH, NC_RGB_EFF_WAVE,
                                            NC_RGB_EFF_SOLID, NC_RGB_EFF_RAMP };
            s_cfg.zone_eff[z] = seq[z & 3];
        }
    }

    // 純色段：純 R/G/B，顯示 3.5/255
    if (st->effect == NC_RGB_EFF_SOLID && st->gain == 255) {
        static const uint8_t pal[3][3] = { { 3, 0, 0 }, { 0, 3, 0 }, { 0, 0, 3 } };
        const int p = (i >= 2 && i <= 4) ? (i - 2) : 0;
        s_cfg.r = pal[p][0];
        s_cfg.g = pal[p][1];
        s_cfg.b = pal[p][2];
        s_cfg.frac = 128;                        // +0.5 級 → 顯示 3.5/255
    }

    // demo 全部對齊 400 fps：rate 1 = 400 fps、rate 2 = 200 fps（整數，方便對照）
    s_cfg.fps = 400;
    pace_set(400);

    s_cfg.sim6 = st->sim6 ? 1 : 0;
    if (st->sim6) {
        for (int z = 0; z < NC_RGB_ZONES; z++) {
            s_cfg.zone_bits[z] = 10;
            s_cfg.zone_dither[z] = 1;
            s_cfg.zone_rate[z] = 1;
            s_cfg.zone_phase[z] = 0;
        }
    }

    // 抖動頻率階梯（417 fps；fps / (rate × 2^(bits-8))）
    //   區 0：12 位 rate 1 → 26 Hz      區 1：11 位 → 52 Hz
    //   區 2：10 位 → 104 Hz            區 3：9 位  → 208 Hz
    //   區 4：8 位原生（唔抖）          區 5：12 位 rate 2 → 13 Hz
    //   區 6：12 位 rate 4 → 6.5 Hz     區 7：12 位 rate 8 → 3.3 Hz
    if (st->freq) {
        static const uint8_t fb[8] = { 12, 11, 10, 9, 8, 12, 12, 12 };
        static const uint8_t fr[8] = { 1, 1, 1, 1, 1, 2, 4, 8 };
        static const uint8_t fm[8] = { 1, 1, 1, 1, 0, 1, 1, 1 };
        for (int z = 0; z < NC_RGB_ZONES; z++) {
            s_cfg.zone_bits[z] = fb[z];
            s_cfg.zone_rate[z] = fr[z];
            s_cfg.zone_dither[z] = fm[z];
            s_cfg.zone_phase[z] = 0;
        }
    }

    // 三段分區（現在 418 fps 極限）：
    //   區 0–2（LED 0–23）  ＝ 原生 8 位、完全唔抖（基準）
    //   區 3–4（LED 24–39） ＝ 10 位、時間抖動（105 Hz）
    //   區 5–7（LED 40–63） ＝ 12 位、時間抖動（26 Hz）
    if (st->ab) {
        for (int z = 0; z < NC_RGB_ZONES; z++) {
            if (z < 3) {
                s_cfg.zone_bits[z] = 8;
                s_cfg.zone_dither[z] = 0;
            } else if (z < 5) {
                s_cfg.zone_bits[z] = 10;
                s_cfg.zone_dither[z] = 1;
            } else {
                s_cfg.zone_bits[z] = 12;
                s_cfg.zone_dither[z] = 1;
            }
            s_cfg.zone_rate[z] = 1;
            s_cfg.zone_phase[z] = 0;
        }
    }

    // 8 區抖動階梯：區 0 完全唔抖（原生 8-bit 參考）；
    // 區 1..6 = temporal rate 1/2/3/4/6/8（約 417/208/139/104/69/52 Hz）；區 7 = spatial。
    if (st->ladder) {
        // 區 0 = 唔抖（參考）；其餘全部時間抖動，只改 rate
        static const uint8_t modes[8] = { 0, 1, 1, 1, 1, 1, 1, 1 };
        static const uint8_t rates[8] = { 1, 1, 2, 3, 4, 6, 8, 1 };
        for (int z = 0; z < NC_RGB_ZONES; z++) {
            s_cfg.zone_dither[z] = modes[z];
            s_cfg.zone_rate[z] = rates[z];
            s_cfg.zone_phase[z] = 0;
        }
    }
}

static int64_t s_demo_blank_until;   // 轉場黑屏到幾時

static bool demo_blanking(void)
{
    return s_demo && esp_timer_get_time() < s_demo_blank_until;
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

static void effect_render(void)
{
    if (demo_blanking()) {
        for (int z = 0; z < NC_RGB_ZONES; z++) {
            zone_clear(z);
        }
        return;
    }
    for (int z = 0; z < NC_RGB_ZONES; z++) {
        if (!(s_cfg.zone_on & (1u << z))) {
            zone_clear(z);
            continue;
        }
        const uint8_t eff = (s_cfg.zone_eff[z] != 0xFF) ? s_cfg.zone_eff[z] : s_cfg.effect;
        render_zone(z, eff);
    }
}

// ── 時間抖動 ──
// rate 幀一個決策週期：want = 目標 × rate + 上一週期的餘數；
// v = floor(want / (256 × rate))、餘數留給下一週期 ⇒ 每 rate 幀的總量剛好等於目標。
// 4×4 Bayer（ordered dither）：空間上分散「不足一級」的差，
// 逐幀把 pattern 轉一格（rotate），令每顆燈的開關頻率維持喺 ~200 Hz（睇唔到），
// 而長期／4×4 區域平均值仍然等於目標。
static const uint8_t s_bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static void dither(void)
{
    const uint32_t rot = s_frame >> 1;      // 每兩幀轉一格
    for (int z = 0; z < NC_RGB_ZONES; z++) {
        for (int i = z * NC_RGB_ZONE_N; i < (z + 1) * NC_RGB_ZONE_N; i++) {
            // 6 區（左/右 × 3 橫帶）模擬 fps：每區唔同 rate
            uint8_t mode = (s_cfg.zone_dither[z] != 0xFF) ? s_cfg.zone_dither[z] : s_cfg.dither;
            uint8_t rate = (s_cfg.zone_rate[z] != 0) ? s_cfg.zone_rate[z] : 1;
            uint8_t bits = s_cfg.zone_bits[z] ? s_cfg.zone_bits[z] : s_cfg.bits;
            uint8_t phase = s_cfg.zone_phase[z];
            if (s_cfg.sim6) {
                // 3 層 × 左右：
                //   上帶（第 0–2 行）＝ 8 位原生（唔抖，唔做頻率比較）
                //   中帶（第 3–4 行）＝ 10 位；左 = 原本 417 fps、右 = 模擬 200 fps（rate 2）
                //   下帶（第 5–7 行）＝ 12 位；左 = 原本 417 fps、右 = 模擬 200 fps（rate 2）
                const int px = i % NC_RGB_W;
                const int py = i / NC_RGB_W;
                const int band = (py < 3) ? 0 : (py < 5) ? 1 : 2;
                const bool right = (px >= (NC_RGB_W / 2));
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
                    const int px = (int)(i % NC_RGB_W);
                    const int py = (int)(i / NC_RGB_W);
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

// ── 編碼：8.8 目標 → 每 bit NC_RGB_BIT_CYCLES 個 byte ──
static void encode(uint8_t *o)
{
    for (int i = 0; i < NC_RGB_N; i++) {
        // NEO_RGB：R 先、G、B（同 Waveshare demo 一致）
        for (int c = 0; c < 3; c++) {
            const uint8_t v = s_led[i][c];
            for (int bit = 7; bit >= 0; bit--) {
                const uint8_t *code = (v & (1u << bit)) ? s_code1 : s_code0;
                memcpy(o, code, NC_RGB_BIT_CYCLES);
                o += NC_RGB_BIT_CYCLES;
            }
        }
    }
    memset(o, 0, NC_RGB_RESET_BYTES);
}

// ── 節拍 ──
// 唔可以 busy-wait：本任務 prio 6，spin 嘅時候 core 嘅 idle task 永遠冇機會跑
// → CONFIG_ESP_TASK_WDT_PANIC=y 之下 5 秒後 Task WDT reset（2026-11-11 實測 rst=6）。
static void IRAM_ATTR pace_cb(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_tick, &hp);
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void pace_set(uint16_t fps)
{
    if (s_pace == NULL) {
        return;
    }
    (void)esp_timer_stop(s_pace);
    if (fps > 0) {
        (void)esp_timer_start_periodic(s_pace, 1000000u / fps);
    }
}

// ── I80 ──
static bool IRAM_ATTR on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                              void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    const int64_t now = esp_timer_get_time();
    if (s_done_us != 0) {
        const uint32_t d = (uint32_t)(now - s_done_us);
        s_dma_us = d;
        if (d > s_dma_max_us) {
            s_dma_max_us = d;
        }
    }
    s_done_us = now;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

// 提交一幀；唔等完成（ping-pong）。等 slot 的邏輯在 loop 裡面。
static esp_err_t tx_submit(uint8_t *buf)
{
    return esp_lcd_panel_io_tx_color(s_io, -1, buf, RGB_TX_BYTES);
}

static bool parse_pins(gpio_num_t data[8], int *wr, int *dc)
{
    const char *s = CONFIG_NETCORE_RGB_MATRIX_I80_PINS;
    int v[10];
    for (int i = 0; i < 10; i++) {
        char *end = NULL;
        const long x = strtol(s, &end, 10);
        if (end == s || x < 0 || x > 48) {
            ESP_LOGE(TAG, "I80 腳位字串第 %d 個唔合法（剩返 \"%s\"）", i + 1, s);
            return false;
        }
        v[i] = (int)x;
        s = end;
        while (*s == ' ' || *s == ',') {
            s++;
        }
    }
    for (int i = 0; i < 8; i++) {
        data[i] = (gpio_num_t)v[i];
    }
    *wr = v[8];
    *dc = v[9];
    return true;
}

static esp_err_t hw_init(void)
{
    gpio_num_t data[8];
    int wr = -1, dc = -1;
    if (!parse_pins(data, &wr, &dc)) {
        return ESP_ERR_INVALID_ARG;
    }
    int all[10];
    for (int i = 0; i < 8; i++) {
        all[i] = (int)data[i];
    }
    all[8] = wr;
    all[9] = dc;
    for (int i = 0; i < 10; i++) {
        if (all[i] < 0) {
            continue;
        }
        if (nc_hw_reserved(all[i])) {
            ESP_LOGE(TAG, "GPIO%d 係保留腳（PSRAM／flash／USB／console／strapping）→ 換腳",
                     all[i]);
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (all[j] == all[i]) {
                ESP_LOGE(TAG, "GPIO%d 重複", all[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = (gpio_num_t)dc,
        .wr_gpio_num = (gpio_num_t)wr,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .bus_width = 8,
        .max_transfer_bytes = RGB_TX_BYTES,
        .dma_burst_size = 0,      // 0 = 用驅動預設（32）
    };
    for (int i = 0; i < 8; i++) {
        bus_cfg.data_gpio_nums[i] = data[i];
    }
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &s_bus), TAG, "I80 bus 建立失敗");

    s_done = xSemaphoreCreateBinary();
    s_tick = xSemaphoreCreateBinary();
    if (s_done == NULL || s_tick == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t pace_args = {
        .callback = pace_cb,
        .name = "rgbpace",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&pace_args, &s_pace), TAG, "節拍 timer");

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1,        // 沒有 CS：整條 bus 由我們獨佔
        .pclk_hz = CONFIG_NETCORE_RGB_MATRIX_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = { 0, 0, 0, 0 },
        .flags = {
            .pclk_active_neg = 0,
            .pclk_idle_low = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(s_bus, &io_cfg, &s_io), TAG,
                        "I80 panel IO 建立失敗");

    for (int i = 0; i < 10; i++) {
        const char *label = (i < 8) ? "d" : (i == 8 ? "wr" : "dc");
        if (nc_hw_claim(all[i], "rgbmatrix", label, NC_HW_CLAIM_WRITE) != ESP_OK) {
            ESP_LOGW(TAG, "GPIO%d 已被其他 owner 佔用（%s）", all[i], label);
        }
    }
    ESP_LOGI(TAG, "I80 起來：D0=GPIO%d（其餘 D1..D7／WR／DC 係 dummy）pclk=%d Hz，一幀 %.1f ms",
             (int)data[0], (int)CONFIG_NETCORE_RGB_MATRIX_PCLK_HZ,
             (double)RGB_TX_BYTES * 1000000.0 / (double)CONFIG_NETCORE_RGB_MATRIX_PCLK_HZ / 1000.0);
    return ESP_OK;
}

// ── 設定 ──
static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.effect = NC_RGB_EFF_BREATH;
    s_cfg.dither = 1;
    s_cfg.r = s_cfg.g = s_cfg.b = 24;
    s_cfg.gain = 60;               // 暗（linear 0..255 → 顯示值約 10/255）；產品頁警告過熱
    s_cfg.bits = 12;               // 最終設定：12 位
    s_cfg.breath_ms = 4000;        // 呼吸慢一點，暗部抖動睇唔到
    s_cfg.wave_ms = 3000;          // 波浪掃一個週期 3 s（原本 ~0.26 s，太快）
    s_demo = true;                 // 開機自動跑 demo（任何手動指令會熄）
    s_cfg.fps = (CONFIG_NETCORE_RGB_MATRIX_FPS > NC_RGB_FPS_MAX)
                    ? NC_RGB_FPS_MAX : (uint16_t)CONFIG_NETCORE_RGB_MATRIX_FPS;
    s_cfg.zone_on = 0xFF;
    memset(s_cfg.zone_eff, 0xFF, sizeof(s_cfg.zone_eff));   // 全部跟全域
    memset(s_cfg.zone_dither, 0xFF, sizeof(s_cfg.zone_dither));
    s_cfg.sim6 = 0;
    s_cfg.sim_rate[0] = 1; s_cfg.sim_rate[1] = 2; s_cfg.sim_rate[2] = 3;
    s_cfg.sim_rate[3] = 4; s_cfg.sim_rate[4] = 6; s_cfg.sim_rate[5] = 8;
    s_cfg.frac = 0;
    // 每區相位錯開；偶數區每幀更新（450 Hz）、奇數區兩幀（225 Hz）。
    // 兩個頻率都遠高於閃爍融合，只係唔想全板同一個節拍。
    for (int z = 0; z < NC_RGB_ZONES; z++) {
        s_cfg.zone_rate[z] = (uint8_t)(1 + (z & 1));
        s_cfg.zone_phase[z] = (uint8_t)z;
    }
}

// ── 任務 ──
static int rgb_declare(nc_task_res_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_task_res_t){
        .name = "rgb.tx",
        .bytes = RGB_TX_BYTES * NC_RGB_SLOTS,   // ping-pong 兩格
        .cls = NC_PLAN_DMA,        // I80 的 GDMA 只讀 internal SRAM
        .align = 64,
    };
    return 1;
}

static bool rgb_enabled(void)
{
    return s_ready;
}

static void rgb_loop(void *arg)
{
    (void)arg;
    s_stat.mark_us = esp_timer_get_time();
    int64_t prev = esp_timer_get_time();
    while (!nc_task_exit_requested()) {
        const int64_t now = esp_timer_get_time();
        int64_t dt = now - prev;
        if (dt < 0) {
            dt = 0;
        } else if (dt > 200000) {     // 停過（debug／載入）就唔好一次跳好多
            dt = 200000;
        }
        s_phase_us += dt;
        prev = now;

        // fps = 0 = 全速（唔做節拍）：量 pipeline 容量用，dt_us 就係一幀真正幾耐。
        // 等本幀的節拍（blocking；idle task 有機會跑）
        if (s_cfg.fps != 0) {
            if (xSemaphoreTake(s_tick, pdMS_TO_TICKS(20)) != pdTRUE) {
                s_stat.late++;      // 節拍冇到（系統卡住）
            }
        }

        // 兩格都在飛 → 等一格回來（正常情況這就是節拍：等 DMA 送完上一幀）
        if (s_inflight >= NC_RGB_SLOTS) {
            if (xSemaphoreTake(s_done, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_inflight--;
            } else {
                s_stat.tx_err++;      // DMA 卡住；唔好無限等
            }
        }

        demo_step();

        const int64_t t_render = esp_timer_get_time();
        effect_render();
        dither();
        const int64_t t_enc = esp_timer_get_time();
        s_stat.render_us = (uint32_t)(t_enc - t_render);

        uint8_t *buf = s_tx + (size_t)(s_slot & (NC_RGB_SLOTS - 1)) * RGB_TX_BYTES;
        encode(buf);
        s_stat.enc_us = (uint32_t)(esp_timer_get_time() - t_enc);

        const int64_t t0 = esp_timer_get_time();
        const esp_err_t e = tx_submit(buf);
        s_stat.tx_us = (uint32_t)(esp_timer_get_time() - t0);
        if (e == ESP_OK) {
            s_slot++;
            s_inflight++;
        } else {
            s_stat.tx_err++;
        }
        s_stat.dma_us = s_dma_us;
        s_stat.dma_max_us = s_dma_max_us;
        s_stat.frames++;
        s_frame++;

        const int64_t after = esp_timer_get_time();
        s_stat.dt_us = (uint32_t)(after - now);
        if (s_stat.dt_us > s_stat.dt_max_us) {
            s_stat.dt_max_us = s_stat.dt_us;
        }

        const int64_t mnow = esp_timer_get_time();
        const int64_t span = mnow - s_stat.mark_us;
        if (span >= 1000000) {
            const uint32_t n = s_stat.frames - s_stat.mark_frames;
            s_stat.fps_x10 = (uint32_t)(((int64_t)n * 10 * 1000000) / span);
            s_stat.mark_us = mnow;
            s_stat.mark_frames = s_stat.frames;
        }
        nc_task_beat();
    }
}

static esp_err_t rgb_start(const nc_task_res_view_t *res, int nres)
{
    if (nres < 1 || res[0].ptr == NULL || res[0].len < RGB_TX_BYTES * NC_RGB_SLOTS) {
        ESP_LOGE(TAG, "拿不到 rgb.tx（要 %d B，實得 %u）", RGB_TX_BYTES * NC_RGB_SLOTS,
                 nres > 0 ? (unsigned)res[0].len : 0u);
        return ESP_FAIL;
    }
    s_tx = res[0].ptr;
    memset(s_tx, 0, (size_t)RGB_TX_BYTES * NC_RGB_SLOTS);
    memset(s_led, 0, sizeof(s_led));
    memset(s_acc, 0, sizeof(s_acc));
    // 抖動初始相位：每一顆、每個通道錯開（位置決定，唔用隨機數；長期平均不變）。
    // 唔錯開的話，同一區 8 顆會在**同一幀**一齊跳，暗部看起來像整條在閃。
    for (int i = 0; i < NC_RGB_N; i++) {
        for (int c = 0; c < 3; c++) {
            s_acc[i][c] = (int32_t)(((uint32_t)i * 37u + (uint32_t)c * 91u) & 0xFFu);
        }
    }
    tables_init();
    cfg_defaults();
    if (hw_init() != ESP_OK) {
        s_tx = NULL;
        return ESP_FAIL;
    }
    s_phase_us = 0;
    if (s_demo) {
        s_demo_stage = 0;
        s_demo_t0 = esp_timer_get_time();
        demo_apply(0);
        ESP_LOGI(TAG, "demo 開始（%d 段 × 20–24 s，循環）", DEMO_N);
    }
    pace_set(s_cfg.fps);
    s_ready = true;
    ESP_LOGI(TAG, "rgbmatrix 準備好：%d×%d（%d 顆，%d 區）上限 %d fps，預設效果 %u",
             NC_RGB_W, NC_RGB_H, NC_RGB_N, NC_RGB_ZONES, NC_RGB_FPS_MAX, s_cfg.effect);
    return ESP_OK;
}

static const nc_task_loop_t s_loops[] = {
    {
        .name = "rgbmtx",
        .fn = rgb_loop,
        .stack = 4096,
        .prio = NC_PRIO_OTHERS_MAX,
        .core = 1,               // 同 tasks.json 的 affinity [0,1] 一致

        .layer = 5,
        .period_ms = 0,          // loop 自己計時，唔要管理器延遲
        .enabled = rgb_enabled,
    },
};

static const nc_task_t s_task = {
    .name = "rgbmatrix",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .declare = rgb_declare,
    .on_start = rgb_start,
    .loops = s_loops,
    .nloops = 1,
};

const nc_task_t *nc_rgb_matrix_task(void)
{
    return &s_task;
}

// ── 對外 API（decode task 可以 call；單一欄位寫入，唔使鎖）──
bool nc_rgb_matrix_ready(void)
{
    return s_ready;
}

nc_rgb_cfg_t nc_rgb_matrix_cfg(void)
{
    return s_cfg;
}

void nc_rgb_matrix_set_effect(uint8_t effect)
{
    s_demo = false;      // 手動接管
    if (effect < NC_RGB_EFF_COUNT) {
        s_cfg.effect = effect;
    }
}

void nc_rgb_matrix_set_dither(bool on)
{
    s_demo = false;      // 手動接管
    s_cfg.dither = on ? 1 : 0;
}

void nc_rgb_matrix_set_dither_mode(uint8_t mode)
{
    s_demo = false;
    s_cfg.dither = (mode <= 3) ? mode : 1;
}

void nc_rgb_matrix_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_demo = false;      // 手動接管
    s_cfg.r = r;
    s_cfg.g = g;
    s_cfg.b = b;
}

void nc_rgb_matrix_set_gain(uint8_t gain)
{
    s_demo = false;      // 手動接管
    s_cfg.gain = gain;
}

void nc_rgb_matrix_set_fps(uint16_t fps)
{
    s_demo = false;      // 手動接管
    s_cfg.fps = (fps > NC_RGB_FPS_MAX) ? NC_RGB_FPS_MAX : fps;
    pace_set(s_cfg.fps);
}

void nc_rgb_matrix_set_zone(int idx, uint8_t rate, uint8_t phase)
{
    s_demo = false;      // 手動接管
    if (idx < 0 || idx >= NC_RGB_ZONES) {
        return;
    }
    if (rate == 0) {
        rate = 1;
    }
    if (rate > 8) {
        rate = 8;
    }
    s_cfg.zone_rate[idx] = rate;
    s_cfg.zone_phase[idx] = phase;
}

void nc_rgb_matrix_set_zone_mask(uint8_t mask)
{
    s_demo = false;      // 手動接管
    s_cfg.zone_on = mask;
}

void nc_rgb_matrix_set_bits(uint8_t bits)
{
    s_demo = false;      // 手動接管
    if (bits < 8) {
        bits = 8;
    } else if (bits > 16) {
        bits = 16;
    }
    s_cfg.bits = bits;
    memset(s_cfg.zone_bits, 0, sizeof(s_cfg.zone_bits));
}

void nc_rgb_matrix_set_zone_bits(int idx, uint8_t bits)
{
    s_demo = false;      // 手動接管
    if (idx < 0 || idx >= NC_RGB_ZONES) {
        return;
    }
    s_cfg.zone_bits[idx] = (bits >= 8 && bits <= 16) ? bits : 0;
}

void nc_rgb_matrix_set_zone_bits_all(const uint8_t bits[NC_RGB_ZONES])
{
    s_demo = false;      // 手動接管
    for (int i = 0; i < NC_RGB_ZONES; i++) {
        s_cfg.zone_bits[i] = (bits[i] >= 8 && bits[i] <= 16) ? bits[i] : 0;
    }
}

void nc_rgb_matrix_set_zone_effect(int idx, uint8_t effect)
{
    s_demo = false;      // 手動接管
    if (idx < 0 || idx >= NC_RGB_ZONES) {
        return;
    }
    s_cfg.zone_eff[idx] = (effect == 0xFF || effect < NC_RGB_EFF_COUNT) ? effect : 0xFF;
}

void nc_rgb_matrix_set_fraction(uint8_t frac8)
{
    s_demo = false;
    s_cfg.frac = frac8;
}

void nc_rgb_matrix_set_zone_dither_all(const uint8_t mode[NC_RGB_ZONES])
{
    s_demo = false;
    for (int i = 0; i < NC_RGB_ZONES; i++) {
        s_cfg.zone_dither[i] = (mode[i] <= 3 || mode[i] == 0xFF) ? mode[i] : 0xFF;
    }
}

void nc_rgb_matrix_set_zone_effects_all(const uint8_t eff[NC_RGB_ZONES])
{
    s_demo = false;      // 手動接管
    for (int i = 0; i < NC_RGB_ZONES; i++) {
        s_cfg.zone_eff[i] = (eff[i] == 0xFF || eff[i] < NC_RGB_EFF_COUNT) ? eff[i] : 0xFF;
    }
}

void nc_rgb_matrix_set_wave_speed(uint16_t wave_ms)
{
    s_demo = false;
    s_cfg.wave_ms = (wave_ms < 200) ? 200 : wave_ms;
}

void nc_rgb_matrix_set_speed(uint16_t breath_ms)
{
    s_demo = false;      // 手動接管
    s_cfg.breath_ms = (breath_ms < 200) ? 200 : breath_ms;
}

void nc_rgb_matrix_demo(bool on)
{
    s_demo = on;
    if (on) {
        s_demo_stage = 0;
        s_demo_t0 = esp_timer_get_time();
        demo_apply(0);
        ESP_LOGI(TAG, "demo 重開");
    }
}

uint8_t nc_rgb_matrix_stage(void)
{
    return (uint8_t)(s_demo ? s_demo_stage : 0xFF);
}

size_t nc_rgb_matrix_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(
        out, cap,
        "{\"ready\":%d,\"eff\":%u,\"dither\":%u,\"gain\":%u,\"fps\":%u,\"fps_x10\":%"
        PRIu32 ",\"frames\":%" PRIu32 ",\"late\":%" PRIu32 ",\"tx_err\":%" PRIu32
        ",\"dt_us\":%" PRIu32 ",\"dt_max_us\":%" PRIu32 ",\"tx_us\":%" PRIu32
        ",\"render_us\":%" PRIu32 ",\"enc_us\":%" PRIu32
        ",\"dma_us\":%" PRIu32 ",\"dma_max_us\":%" PRIu32
        ",\"tx_bytes\":%u,\"bits\":%u,\"bits_z\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"frac\":%u,\"sim6\":%u,\"zdither\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"speed\":%u,\"wspeed\":%u,\"demo\":%u,\"stage\":%u,\"zeff\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"rate\":[%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"phase\":[%u,%u,%u,%u,%u,%u,%u,%u],\"zmask\":%u}",
        s_ready ? 1 : 0, s_cfg.effect, (unsigned)s_cfg.dither, s_cfg.gain, s_cfg.fps,
        s_stat.fps_x10, s_stat.frames, s_stat.late, s_stat.tx_err, s_stat.dt_us,
        s_stat.dt_max_us, s_stat.tx_us, s_stat.render_us, s_stat.enc_us,
        s_stat.dma_us, s_stat.dma_max_us,
        (unsigned)RGB_TX_BYTES,
        s_cfg.bits, s_cfg.zone_bits[0], s_cfg.zone_bits[1], s_cfg.zone_bits[2], s_cfg.zone_bits[3],
        s_cfg.zone_bits[4], s_cfg.zone_bits[5], s_cfg.zone_bits[6], s_cfg.zone_bits[7],
        s_cfg.frac, (unsigned)s_cfg.sim6,
        s_cfg.zone_dither[0], s_cfg.zone_dither[1], s_cfg.zone_dither[2], s_cfg.zone_dither[3],
        s_cfg.zone_dither[4], s_cfg.zone_dither[5], s_cfg.zone_dither[6], s_cfg.zone_dither[7],
        s_cfg.breath_ms, s_cfg.wave_ms, s_demo ? 1u : 0u, nc_rgb_matrix_stage(),
        s_cfg.zone_eff[0], s_cfg.zone_eff[1], s_cfg.zone_eff[2], s_cfg.zone_eff[3],
        s_cfg.zone_eff[4], s_cfg.zone_eff[5], s_cfg.zone_eff[6], s_cfg.zone_eff[7],
        s_cfg.zone_rate[0], s_cfg.zone_rate[1], s_cfg.zone_rate[2], s_cfg.zone_rate[3],
        s_cfg.zone_rate[4], s_cfg.zone_rate[5], s_cfg.zone_rate[6], s_cfg.zone_rate[7],
        s_cfg.zone_phase[0], s_cfg.zone_phase[1], s_cfg.zone_phase[2], s_cfg.zone_phase[3],
        s_cfg.zone_phase[4], s_cfg.zone_phase[5], s_cfg.zone_phase[6], s_cfg.zone_phase[7],
        s_cfg.zone_on);
    if (n < 0) {
        return 0;
    }
    return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

#else   // !CONFIG_NETCORE_RGB_MATRIX ── 測試映像裡有這個任務，但呢塊板冇開 → 全部 no-op

// 唔宣告資源、唔開 loop：管理器照樣登記，但一個 buffer 都唔會配。
const nc_task_t *nc_rgb_matrix_task(void)
{
    static const nc_task_t s_off = {
        .name = "rgbmatrix",
        .kind = NC_TASK_NORMAL,
        .level = 4,
    };
    return &s_off;
}

bool nc_rgb_matrix_ready(void)
{
    return false;
}

nc_rgb_cfg_t nc_rgb_matrix_cfg(void)
{
    nc_rgb_cfg_t c;
    memset(&c, 0, sizeof(c));
    return c;
}

void nc_rgb_matrix_set_effect(uint8_t effect) { (void)effect; }
void nc_rgb_matrix_set_dither(bool on) { (void)on; }
void nc_rgb_matrix_set_dither_mode(uint8_t mode) { (void)mode; }
void nc_rgb_matrix_set_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void nc_rgb_matrix_set_gain(uint8_t gain) { (void)gain; }
void nc_rgb_matrix_set_fps(uint16_t fps) { (void)fps; }
void nc_rgb_matrix_set_zone(int idx, uint8_t rate, uint8_t phase) { (void)idx; (void)rate; (void)phase; }
void nc_rgb_matrix_set_zone_mask(uint8_t mask) { (void)mask; }
void nc_rgb_matrix_set_bits(uint8_t bits) { (void)bits; }
void nc_rgb_matrix_set_zone_bits(int idx, uint8_t bits) { (void)idx; (void)bits; }
void nc_rgb_matrix_set_zone_bits_all(const uint8_t bits[NC_RGB_ZONES]) { (void)bits; }
void nc_rgb_matrix_set_zone_effect(int idx, uint8_t eff) { (void)idx; (void)eff; }
void nc_rgb_matrix_set_fraction(uint8_t frac8)
{
    s_demo = false;
    s_cfg.frac = frac8;
}

void nc_rgb_matrix_set_zone_dither_all(const uint8_t mode[NC_RGB_ZONES])
{
    s_demo = false;
    for (int i = 0; i < NC_RGB_ZONES; i++) {
        s_cfg.zone_dither[i] = (mode[i] <= 3 || mode[i] == 0xFF) ? mode[i] : 0xFF;
    }
}

void nc_rgb_matrix_set_zone_effects_all(const uint8_t eff[NC_RGB_ZONES]) { (void)eff; }
void nc_rgb_matrix_set_fraction(uint8_t f) { (void)f; }
void nc_rgb_matrix_set_zone_dither_all(const uint8_t mode[NC_RGB_ZONES]) { (void)mode; }
void nc_rgb_matrix_set_speed(uint16_t ms) { (void)ms; }
void nc_rgb_matrix_set_wave_speed(uint16_t ms) { (void)ms; }
void nc_rgb_matrix_demo(bool on) { (void)on; }
uint8_t nc_rgb_matrix_stage(void) { return 0xFF; }

void nc_rgb_matrix_demo(bool on)
{
    s_demo = on;
    if (on) {
        s_demo_stage = 0;
        s_demo_t0 = esp_timer_get_time();
        demo_apply(0);
        ESP_LOGI(TAG, "demo 重開");
    }
}

uint8_t nc_rgb_matrix_stage(void)
{
    return (uint8_t)(s_demo ? s_demo_stage : 0xFF);
}

size_t nc_rgb_matrix_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(out, cap, "{\"ready\":0,\"err\":\"disabled\"}");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

#endif  // CONFIG_NETCORE_RGB_MATRIX
