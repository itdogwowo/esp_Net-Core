// gamma_bench.c — 「有 gamma 表」同「冇 gamma」有咩分別（host；唔需要硬件）。
//
// 直接 link 出貨中的 firmware/components/sw/pixel/engine/nc_px_engine_tables.c，
// 所以呢支印出嚟嘅曲線就係板上跑嘅同一張表（2,048 B、pow(x, 2.2)）。
//
//   bash tools/hosttest/gamma_bench/run.sh
//   bash tools/hosttest/gamma_bench/run.sh --csv artifacts/tmp/gamma.csv
//
// 三個問題：
//   1. 同一個輸入，有 gamma 同冇 gamma 的 8-bit 輸出差幾多？
//   2. 一條 fade 幾時才「看起來一半亮」？（有 gamma 73% 時間、冇 gamma 50%）
//   3. 呢啲數字有冇回歸？（[2] 的斷言，golden 一改就紅）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_px_engine.h"

static int g_fail;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("✘ %s\n", what);
        g_fail++;
    }
}

// 16-bit linear → 8-bit 顯示值（有 gamma 表）
static unsigned with_gamma(uint32_t lin16)
{
    return nc_px_engine_gamma((uint16_t)lin16) >> 8;
}

// 冇 gamma：直接取高位（同 12-bit 路徑的 >>4 同一個意思）
static unsigned without_gamma(uint32_t lin16)
{
    return (unsigned)(lin16 >> 8);
}

// 16-bit linear 由 8-bit level 砌返（level 255 → 65535）
static uint32_t lin_of(unsigned level255)
{
    return (uint32_t)level255 * 257u;
}

// 找「輸出第一次 ≥ half」的那個輸入比例（0..100%）
static unsigned percent_to_reach(unsigned half, int gamma)
{
    for (uint32_t lin = 0; lin <= 65535u; lin += 16u) {
        const unsigned v = gamma ? with_gamma(lin) : without_gamma(lin);
        if (v >= half) {
            return (unsigned)(((uint64_t)lin * 100u) / 65535u);
        }
    }
    return 100;
}

int main(int argc, char **argv)
{
    const char *csv = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv = argv[++i];
        }
    }

    nc_px_engine_tables_init();

    // ── [1] 同一批輸入，兩個輸出 ─────────────────────────────────────
    printf("[1] 8-bit 輸出：有 gamma vs 冇 gamma（linear 值 = level × 257）\n");
    printf("  level   linear  冇 gamma  有 gamma   差（有−冇）\n");
    static const unsigned rows[] = { 0, 16, 32, 48, 64, 80, 96, 112, 128,
                                     144, 160, 176, 192, 208, 224, 240, 255 };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const unsigned lv = rows[i];
        const uint32_t lin = lin_of(lv);
        const unsigned a = without_gamma(lin);
        const unsigned b = with_gamma(lin);
        printf("  %5u   %6u   %6u   %7u   %+6d\n",
               lv, (unsigned)lin, a, b, (int)b - (int)a);
    }

    // ── [2] 斷言（回歸閘門）──────────────────────────────────────────
    printf("\n[2] 斷言\n");
    check(nc_px_engine_gamma(0) == 0, "gamma(0) = 0");
    check(with_gamma(65535u) == 255, "gamma(65535) = 255（白唔會縮水）");

    int mono = 1;
    unsigned prev = 0;
    for (uint32_t lin = 0; lin <= 65535u; lin++) {
        const unsigned v = nc_px_engine_gamma((uint16_t)lin);
        if (v < prev) {
            mono = 0;
            break;
        }
        prev = v;
    }
    check(mono, "gamma 單調（0 → 65535 逐點）");

    int le = 1;
    for (uint32_t lin = 0; lin <= 65535u; lin++) {
        if (with_gamma(lin) > without_gamma(lin)) {
            le = 0;
            break;
        }
    }
    check(le, "gamma ≤ 線性（暗部唔會被推光）");

    const unsigned mid = with_gamma(32768u);            // 50% 線性
    printf("  gamma(50%% 線性) = %u（線性會出 128；比例 %.0f%%）\n",
           mid, (double)mid * 100.0 / 128.0);
    check(mid >= 54 && mid <= 56, "gamma(50% 線性) 係 55±1");

    // ── [3] 一條 fade 的分別（正弦 0..4095 → 16-bit，300 步一個循環）──
    printf("\n[3] 一條 fade（正弦 300 步；輸入 = sin_lin × 16）\n");
    unsigned max_d = 0;
    unsigned long sum_d = 0;
    unsigned n = 0;
    unsigned dark_frames = 0;                          // 有 gamma 時 < 25% 輸出的步數
    for (int step = 0; step < 300; step++) {
        const uint32_t ph = (uint32_t)step * 65536u / 300u;
        const uint32_t lin = (uint32_t)nc_px_engine_sin_lin(ph) * 16u;
        const unsigned a = without_gamma(lin);
        const unsigned b = with_gamma(lin);
        const unsigned d = (b > a) ? (b - a) : (a - b);
        if (d > max_d) {
            max_d = d;
        }
        sum_d += d;
        // 用「有 gamma」做量測基準：暗部（< 25%）佔幾多時間
        if (b < 64) {
            dark_frames++;
        }
        n++;
    }
    printf("  最大差 %u／255、平均差 %.1f／255\n", max_d, (double)sum_d / (double)n);

    const unsigned t50g = percent_to_reach(128, 1);
    const unsigned t50n = percent_to_reach(128, 0);
    printf("  「看起來一半亮」（輸出 ≥ 128）發生在：有 gamma %u%% 時間、冇 gamma %u%%\n",
           t50g, t50n);
    printf("  有 gamma 時最暗的一半時間（輸出 < 64）佔 %u／300 幀\n", dark_frames);

    // 12-bit 生產路徑：全程唔經 gamma（>>4），所以上面嘅差距唔會出現
    printf("\n[4] 12-bit 生產路徑：值直接 >>4 → 等同「冇 gamma」那一行\n");

    if (csv != NULL) {
        FILE *f = fopen(csv, "w");
        if (f == NULL) {
            printf("✘ 開唔到 %s\n", csv);
            g_fail++;
        } else {
            fprintf(f, "level,lin16,linear8,gamma8\n");
            for (unsigned lv = 0; lv <= 255u; lv++) {
                const uint32_t lin = lin_of(lv);
                fprintf(f, "%u,%u,%u,%u\n", lv, (unsigned)lin,
                        without_gamma(lin), with_gamma(lin));
            }
            fclose(f);
            printf("\nCSV：%s（level,lin16,linear8,gamma8）\n", csv);
        }
    }

    if (g_fail != 0) {
        printf("\n%d 個斷言失敗\n", g_fail);
        return 1;
    }
    printf("\nOK\n");
    return 0;
}
