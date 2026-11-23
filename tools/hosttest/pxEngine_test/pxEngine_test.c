// pxEngine_test — 效果引擎的 host 測試（PC 上跑，不需要 ESP-IDF 或燈板）。
//
// 驗的是**重構前後行為一樣**這件事，不是「看起來差不多」：
//   同一組 (params, 時間序列) → 同一串 frame 的 payload → 同一個 hash。
//   golden.txt 係重構前用同一支測試影低的；之後任何改動只要畫面變了就會 diff 出嚟。
//
// 為什麼 hash 而不是逐格比對檔案：一幀 64 px × 4 B，15 幀 × 14 個案例就 53 KB，
// hash 令 golden 一眼看完，而 FNV-1a 對「一格像素差 1」一樣敏感。
//
// 另外有 sin_q12 的**斷言檢查**（純整數、可手算）：成功唔出聲，所以 golden 不變。
//
// 用法：
//   bash tools/hosttest/pxEngine_test/run.sh           跑並對 golden
//   bash tools/hosttest/pxEngine_test/run.sh --update  重影 golden（只可以在「已知行為變更」時用）
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nc_px_engine_test.h"

int64_t g_fake_us;

#define PXMAX       2048
#define FRAMES      15
#define STEP_US     16666            // 60 fps
#define T0          1000000

static uint8_t g_payload[PXMAX * 4];
static uint8_t g_hdr[64];

static int g_fail;

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// 一個案例：設定 → 綁定 → 跑 FRAMES 格 → 印 hash。
static void run(const char *name, const nc_px_test_cfg_t *cfg, uint32_t px)
{
    g_fake_us = T0;
    nc_px_test_set_cfg(cfg);

    if (nc_px_test_bind(px) != 0) {
        printf("%-30s BIND-FAIL\n", name);
        g_fail++;
        return;
    }
    nc_px_test_reset();

    uint64_t h = 1469598103934665603ull;
    for (int f = 0; f < FRAMES; f++) {
        const int64_t t = T0 + (int64_t)(f + 1) * STEP_US;
        g_fake_us = t;
        if (!nc_px_test_frame(g_payload, g_hdr, t)) {
            printf("%-30s FRAME-FAIL\n", name);
            g_fail++;
            return;
        }
        h = fnv1a(h, g_payload, (size_t)px * 4u);
    }
    printf("%-30s %016llx\n", name, (unsigned long long)h);
}

// 由預設值出發，改幾項就成為一個案例。
static void base(nc_px_test_cfg_t *c)
{
    nc_px_test_defaults();
    *c = nc_px_test_cfg();
}

// ── 正弦（引擎只有一個實作：sin_lin）────────────────────────────────────
// 純整數，期望值由 Python 精算；成功唔出聲（golden 不變）。
static void check_sin_lin(void)
{
    static const struct { uint16_t ph; uint16_t want; } k2[] = {
        {     0, 2048 }, {  2048, 2448 }, {  4096, 2832 }, {  8192, 3496 },
        { 16384, 4095 }, { 24576, 3496 }, { 32768, 2048 }, { 40960,  600 },
        { 49152,    0 }, { 57344,  600 },
    };
    for (size_t i = 0; i < sizeof(k2) / sizeof(k2[0]); i++) {
        const uint16_t got = nc_px_engine_sin_lin(k2[i].ph);
        if (got != k2[i].want) {
            printf("sin_lin(%u) = %u，期望 %u\n", (unsigned)k2[i].ph, (unsigned)got, (unsigned)k2[i].want);
            g_fail++;
        }
    }
    {
        uint16_t prev = 0;
        for (uint32_t p = 0; p <= 16384u; p++) {
            const uint16_t v = nc_px_engine_sin_lin(p);
            if (v < prev) {
                printf("sin_lin 在 %u 唔單調：%u < %u\n", (unsigned)p, (unsigned)v, (unsigned)prev);
                g_fail++;
                break;
            }
            prev = v;
        }
        for (uint32_t p = 0; p < 32768u; p += 97u) {
            if (nc_px_engine_sin_lin(p) != nc_px_engine_sin_lin(32768u - p)) {
                printf("sin_lin 唔對稱：p=%u\n", (unsigned)p);
                g_fail++;
                break;
            }
        }
    }
}

// ── 正弦的**精度閘門**：對 double sin() 的最大誤差（0..4095 域）──
// 數字來自 sin_bench 的全相位掃描（host）；界限放寬少少，避免 libm 差異造成假紅。
// 成功唔出聲（golden 不變）；失敗印出係邊個算法、差幾多。
static void check_sin_accuracy(void)
{
    static const struct { const char *name; double limit; } lim[] = {
        { "nc_px_engine_sin_lin",   1.0 },   // 256 錨點混合（四捨五入後 0.72；截斷版係 1.23）
    };
    for (size_t k = 0; k < sizeof(lim) / sizeof(lim[0]); k++) {
        double mx = 0.0;
        uint32_t worst = 0;
        for (uint32_t p = 0; p < 65536u; p += 16u) {
            double ideal = 2048.0 + 2048.0 *
                sin(2.0 * 3.141592653589793 * (double)p / 65536.0);
            if (ideal > 4095.0) {                 // 12-bit 表示不到 4096（峰值）
                ideal = 4095.0;
            } else if (ideal < 0.0) {
                ideal = 0.0;
            }
            const double got = (double)nc_px_engine_sin_lin(p);
            const double e = fabs(got - ideal);
            if (e > mx) {
                mx = e;
                worst = p;
            }
        }
        if (mx > lim[k].limit) {
            printf("%s 精度超標：max %.2f LSB（界限 %.1f）@ p=%u\n",
                   lim[k].name, mx, lim[k].limit, (unsigned)worst);
            g_fail++;
        }
    }
}

int main(void)
{
    nc_px_test_cfg_t c;

    nc_px_test_register();            // 實作 → 引擎 registry（引擎唔知道有邊幾個效果）

    base(&c);
    run("default", &c, 64);

    base(&c); c.effect = NC_PX_EFF_OFF;
    run("off", &c, 64);

    base(&c); c.effect = NC_PX_EFF_ZONE1;
    run("zone1", &c, 64);

    base(&c); c.effect = NC_PX_EFF_BREATH; c.gain = 255;
    run("breath-gain255", &c, 64);

    base(&c); c.effect = NC_PX_EFF_BREATH; c.bits = 12; c.dither = 1; c.gain = 40;
    run("breath-12bit-dither", &c, 64);

    base(&c); c.effect = NC_PX_EFF_WAVE; c.gain = 120;
    run("wave-gain120", &c, 64);

    base(&c); c.effect = NC_PX_EFF_WAVE; c.bits = 10; c.dither = 2;
    run("wave-10bit-spatial", &c, 64);

    base(&c); c.effect = NC_PX_EFF_WAVE; c.bits = 12; c.dither = 3;
    run("wave-12bit-spatial-static", &c, 64);

    base(&c); c.effect = NC_PX_EFF_SOLID; c.r = 255; c.g = 64; c.b = 8; c.frac = 128; c.gain = 200;
    run("solid-frac", &c, 64);

    base(&c); c.effect = NC_PX_EFF_RAMP; c.bits = 12; c.dither = 1;
    run("ramp-12bit", &c, 64);

    base(&c); c.effect = NC_PX_EFF_BREATH; c.sim6 = 1;
    run("sim6", &c, 64);

    base(&c);
    c.effect = NC_PX_EFF_BREATH;
    c.zone_on = 0x0F;
    for (int z = 0; z < NC_PX_TEST_ZONES; z++) {
        c.zone_eff[z] = 0xFF;
    }
    c.zone_eff[1] = NC_PX_EFF_WAVE;
    c.zone_eff[3] = NC_PX_EFF_SOLID;
    run("zones-mixed", &c, 64);

    base(&c);
    c.effect = NC_PX_EFF_BREATH;
    c.bits = 12;
    c.dither = 1;
    for (int z = 0; z < NC_PX_TEST_ZONES; z++) {
        c.zone_bits[z] = (uint8_t)(8 + (z / 2) * 2);       // 8 / 10 / 12 / 14
        c.zone_dither[z] = (uint8_t)(z < 2 ? 0 : 1);
        c.zone_phase[z] = 0;
    }
    run("zone-bits-mixed", &c, 64);

    base(&c); c.effect = NC_PX_EFF_BREATH; c.breath_ms = 200; c.wave_ms = 200;
    run("min-speeds", &c, 64);

    base(&c); c.effect = NC_PX_EFF_RAMP;
    run("px100", &c, 100);

    check_sin_lin();                  // 純整數斷言；成功唔印嘢（golden 不變）
    check_sin_accuracy();             // 正弦對 double 的精度閘門

    printf("%s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
