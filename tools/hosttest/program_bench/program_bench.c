// program_bench — C 側 program interpreter 的 10,000 步量測（host，x86）。
//
// 同 mp 的 test/pixel/bench_wave_segments.py 對齊：每個波形型別跑 10,000 步，
// 量 value_at（單 pixel）同 64 px 一幀（spacing=1）。
// 量到的是 nc_px_engine_program_value()，即板上真正跑的 C 實作（不是另寫一份）。
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "nc_px_asset.h"
#include "nc_px_engine.h"
#include "nc_px_program.h"

#define STEPS 10000
#define PIXELS 64

static double now_us(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static nc_px_asset_t make_asset(uint8_t type)
{
    nc_px_asset_t a;
    memset(&a, 0, sizeof(a));
    strcpy(a.name, "bench");
    a.kind = NC_PX_ASSET_PROGRAM;
    a.nseg = 1;
    a.pixel_hint = 64;
    a.seg[0].type = type;
    a.seg[0].f = 10;
    a.seg[0].l_max = 4095;
    a.seg[0].l_lim = 100;
    a.seg[0].phi = 0;
    a.seg[0].end_f = 100;
    a.seg[0].pulse = 2047;
    return a;
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case NC_PX_SEG_KEEP: return "keep";
    case NC_PX_SEG_MATH_NOW: return "math_now";
    case NC_PX_SEG_SQUARE_WAVE_NOW: return "square_wave_now";
    case NC_PX_SEG_PULSE_WAVE: return "pulse_wave";
    case NC_PX_SEG_PULSE: return "pulse";
    case NC_PX_SEG_STARTER: return "starter";
    default: return "?";
    }
}

int main(void)
{
    nc_px_engine_tables_init();
    printf("=== C program interpreter：%d 步量測（host x86）===\n", STEPS);
    printf("%-16s%12s%12s%14s\n", "type", "ns/step", "us/step", "us/frame64");
    printf("------------------------------------------------------\n");

    static const uint8_t types[] = {
        NC_PX_SEG_KEEP, NC_PX_SEG_MATH_NOW, NC_PX_SEG_SQUARE_WAVE_NOW,
        NC_PX_SEG_PULSE_WAVE, NC_PX_SEG_PULSE, NC_PX_SEG_STARTER,
    };
    for (size_t k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
        nc_px_asset_t a = make_asset(types[k]);
        nc_px_prog_t p;
        if (nc_px_engine_program_compile(&a, &p) != ESP_OK) {
            printf("%-16s compile failed\n", type_name(types[k]));
            continue;
        }
        volatile uint32_t sink = 0;
        double t0 = now_us();
        for (int i = 0; i < STEPS; i++) {
            sink += nc_px_engine_program_value(&p, (uint32_t)(i % (int)p.total));
        }
        double dt1 = now_us() - t0;

        // 一幀 64 px（spacing=1）：phase 走 64 格
        t0 = now_us();
        for (int i = 0; i < STEPS; i++) {
            uint32_t base = (uint32_t)(i % (int)p.total);
            for (int x = 0; x < PIXELS; x++) {
                sink += nc_px_engine_program_value(&p, base + (uint32_t)x);
            }
        }
        double dt64 = now_us() - t0;
        printf("%-16s%12.2f%12.3f%14.2f\n", type_name(types[k]),
               dt1 * 1000.0 / STEPS, dt1 / STEPS, dt64 / STEPS);
    }

    // compile 一次性成本
    nc_px_asset_t a = make_asset(NC_PX_SEG_MATH_NOW);
    double t0 = now_us();
    for (int i = 0; i < STEPS; i++) {
        nc_px_prog_t p;
        (void)nc_px_engine_program_compile(&a, &p);
    }
    double dtc = now_us() - t0;
    printf("\ncompile()          %d 次 → %.2f ms（%.2f us/次）\n",
           STEPS, dtc / 1000.0, dtc / STEPS);
    printf("（程式中斷點：value_at = 段查找 + sin_q12 + 加減；frame64 = %d 次 value）\n", PIXELS);
    return 0;
}
