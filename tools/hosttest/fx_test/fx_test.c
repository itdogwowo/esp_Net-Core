// fx_test — 效果引擎的 host 測試（PC 上跑，不需要 ESP-IDF 或燈板）。
//
// 驗的是**重構前後行為一樣**這件事，不是「看起來差不多」：
//   同一組 (params, 時間序列) → 同一串 frame 的 payload → 同一個 hash。
//   golden.txt 係重構前用同一支測試影低的；之後任何改動只要畫面變了就會 diff 出嚟。
//
// 為什麼 hash 而不是逐格比對檔案：一幀 64 px × 4 B，15 幀 × 14 個案例就 53 KB，
// hash 令 golden 一眼看完，而 FNV-1a 對「一格像素差 1」一樣敏感。
//
// 用法：
//   bash tools/hosttest/fx_test/run.sh           跑並對 golden
//   bash tools/hosttest/fx_test/run.sh --update  重影 golden（只可以在「已知行為變更」時用）
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fx_api.h"

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
static void run(const char *name, const fx_cfg_t *cfg, uint32_t px)
{
    g_fake_us = T0;
    fx_set_cfg(cfg);

    if (fx_bind(px) != 0) {
        printf("%-30s BIND-FAIL\n", name);
        g_fail++;
        return;
    }
    fx_reset();

    uint64_t h = 1469598103934665603ull;
    for (int f = 0; f < FRAMES; f++) {
        const int64_t t = T0 + (int64_t)(f + 1) * STEP_US;
        g_fake_us = t;
        if (!fx_frame(g_payload, g_hdr, t)) {
            printf("%-30s FRAME-FAIL\n", name);
            g_fail++;
            return;
        }
        h = fnv1a(h, g_payload, (size_t)px * 4u);
    }
    printf("%-30s %016llx\n", name, (unsigned long long)h);
}

// 由預設值出發，改幾項就成為一個案例。
static void base(fx_cfg_t *c)
{
    fx_defaults();
    *c = fx_cfg();
}

int main(void)
{
    fx_cfg_t c;

    fx_register();                    // 實作 → 引擎 registry（引擎唔知道有邊幾個效果）

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
    for (int z = 0; z < FX_ZONES; z++) {
        c.zone_eff[z] = 0xFF;
    }
    c.zone_eff[1] = NC_PX_EFF_WAVE;
    c.zone_eff[3] = NC_PX_EFF_SOLID;
    run("zones-mixed", &c, 64);

    base(&c);
    c.effect = NC_PX_EFF_BREATH;
    c.bits = 12;
    c.dither = 1;
    for (int z = 0; z < FX_ZONES; z++) {
        c.zone_bits[z] = (uint8_t)(8 + (z / 2) * 2);       // 8 / 10 / 12 / 14
        c.zone_dither[z] = (uint8_t)(z < 2 ? 0 : 1);
        c.zone_phase[z] = 0;
    }
    run("zone-bits-mixed", &c, 64);

    base(&c); c.effect = NC_PX_EFF_BREATH; c.breath_ms = 200; c.wave_ms = 200;
    run("min-speeds", &c, 64);

    base(&c); c.effect = NC_PX_EFF_RAMP;
    run("px100", &c, 100);

    printf("%s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
