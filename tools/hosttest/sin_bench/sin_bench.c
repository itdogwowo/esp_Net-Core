// sin_bench — 正弦計算的 host 基準（時間 × 精度 × 記憶體）。
//
// 目的：比較幾種「12-bit 正弦」做法在**同一台機器、同一個相位序列**下的真實差別。
//
//   ★ 三個「出貨中」的函數（直接 link firmware 的 nc_px_engine_tables.c，唔係另寫一份）：
//     eng_sin      1024 點 Q15 查表（nc_px_engine_sin，內建效果用）
//     eng_sin_lin  256 錨點＋線性插值（nc_px_engine_sin_lin，514 B）
//     eng_sin_q12  MP 多項式（nc_px_engine_sin_q12，0 表）
//
//   ☆ 對照組（只在這個檔案，方便研究表大小／算法取捨）：
//     lut_4096     4096 點 Q15 查表（8 KB）
//     hyb64_lin    64 錨點混合（130 B）
//     hyb128_lin   128 錨點混合（258 B）
//     cordic_q15   CORDIC 15 次迭代（60 B atan 表）
//     sinf_libm    libm（解釋為什麼要自己算）
//
// ⚠️ 為什麼要 link 真函數：以前這支工具自己抄了一份多項式／混合算法，
//    固件改咗而工具冇改，量到的就係另一個算法。現在三個主行**冇得走樣**。
//
// 量法：每個 case 跑 N=100000 次（≈ 一幀 192 次的 520 幀），重複 REP 次，報 min／median／p99；
//       相位由 volatile 陣列讀，保證唔會被向量化或預先算好。
//
// 用法：
//   bash tools/hosttest/sin_bench/run.sh           完整（REP=120）
//   bash tools/hosttest/sin_bench/run.sh --quick   快（REP=20；開發時用）
//   bash tools/hosttest/sin_bench/run.sh --json    機器可讀（方便存檔／比較）
//
// ⚠️ host（x86-64）數字只代表**相對**成本；實機請用固件 0xF063 SINBENCH
//    （tools/hosttest/sin_bench/dev_probe.py --sin）。
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nc_px_engine.h"

#define N         100000      // 一個 batch 幾多次
#define REP_DEF   120         // 預設重複幾多個 batch（120 → p99 有意義）
#define REP_QUICK 20
#define FPP       192         // 一幀的正弦呼叫數（64 px × 3 通道）

#define PI_D 3.141592653589793

static int16_t           s_lut4k[4096];
static int16_t           s_lin64[65];
static int16_t           s_lin128[129];
static volatile uint16_t s_ph[N];
static volatile uint32_t g_sink;
static int               g_rep = REP_DEF;

// ── 計時 ─────────────────────────────────────────────────────────────
static double now_ms(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

#if defined(_MSC_VER)
#define NO_VECTOR __pragma(loop(no_vector))
#else
#define NO_VECTOR
#endif

// Q15（-32767..32767）→ 0..4095；夾住 CORDIC 的輕微過衝（-32769 這類）。
static inline uint32_t q15_to_w(int32_t v)
{
    int32_t w = (v >> 4) + 2048;
    if (w < 0) {
        w = 0;
    }
    if (w > 4095) {
        w = 4095;
    }
    return (uint32_t)w;
}

// ── ★ 出貨中的三個（link nc_px_engine_tables.c）────────────────────────
// 回傳值統一換到 0..4095 域，量測嘅係同一個「工作」量。
// 注意：nc_px_engine_sin 的相位單位係 1/1024 圈（唔係 65536），要 >>6。
static uint32_t run_eng_sin(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += q15_to_w(nc_px_engine_sin(s_ph[i] >> 6));
    }
    return acc;
}

static uint32_t run_eng_sin_lin(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += nc_px_engine_sin_lin(s_ph[i]);
    }
    return acc;
}

static uint32_t run_eng_sin_q12(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += nc_px_engine_sin_q12(s_ph[i]);
    }
    return acc;
}

// ── ☆ 對照：4096 點查表 ───────────────────────────────────────────────
static uint32_t run_lut4k(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)(uint16_t)s_lut4k[(s_ph[i] >> 4) & 4095u];
    }
    return acc;
}

// ── ☆ 對照：混合（錨點＋線性插值），研究表大小用 ──────────────────────
// 誤差 ∝ (2π/n)²：n=64 → 2.5 LSB、n=128 → 0.69、n=256 → 0.23（0..4095 域）。
static inline int32_t hyb_lin(uint32_t p, const int16_t *tab, uint32_t n, uint32_t shift)
{
    const uint32_t mask = (1u << shift) - 1u;
    const uint32_t i = (p >> shift) & (n - 1u);
    const uint32_t f = p & mask;
    const int32_t a = tab[i];
    const int32_t b = tab[i + 1];
    return (int32_t)(((a << shift) + (b - a) * (int32_t)f) >> shift);   // Q15 插值
}

static uint32_t run_hyb64(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += q15_to_w(hyb_lin(s_ph[i], s_lin64, 64u, 10u));
    }
    return acc;
}

static uint32_t run_hyb128(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += q15_to_w(hyb_lin(s_ph[i], s_lin128, 128u, 9u));
    }
    return acc;
}

// ── ☆ 對照：CORDIC（BAM：65536 = 2π；atan 表；輸出 Q15）────────────────
static const int32_t s_atan[15] = {
    8192, 4836, 2555, 1297, 651, 326, 163, 81, 41, 20, 10, 5, 3, 1, 1
};

static inline int32_t cordic_sin_q15(uint32_t phase)
{
    int32_t z = (int32_t)(phase & 65535u);
    int32_t sgn = 1;
    if (z >= 16384 && z < 49152) {          // 折到 -16384..16383（±90°）
        z -= 32768;
        sgn = -1;
    } else if (z >= 49152) {
        z -= 65536;
    }
    int32_t x = 19899;                      // K = 0.6073 × 32768
    int32_t y = 0;
    for (int i = 0; i < 15; i++) {
        const int32_t xs = x >> i;
        const int32_t ys = y >> i;
        if (z >= 0) {
            const int32_t xn = x - ys;
            y += xs;
            z -= s_atan[i];
            x = xn;
        } else {
            const int32_t xn = x + ys;
            y -= xs;
            z += s_atan[i];
            x = xn;
        }
    }
    return sgn * y;
}

static uint32_t run_cordic(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += q15_to_w(cordic_sin_q15(s_ph[i]));
    }
    return acc;
}

// ── ☆ 對照：libm ─────────────────────────────────────────────────────
static uint32_t run_sinf(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        const float t = (float)(2.0 * PI_D * (double)s_ph[i] / 65536.0);
        acc += (uint32_t)((int32_t)(sinf(t) * 2048.0f) + 2048);
    }
    return acc;
}

// ── 統一描述：一個 case = 名 + 函數 + 表大小 + 精度評估器 ──────────────
typedef struct {
    const char *name;
    uint32_t (*fn)(void);
    uint32_t table_bytes;
    double   (*eval)(uint32_t phase);      // 0..4095 域
} case_t;

static double ev_eng_sin(uint32_t p)      { return (double)q15_to_w(nc_px_engine_sin(p >> 6)); }
static double ev_eng_sin_lin(uint32_t p)  { return (double)nc_px_engine_sin_lin(p); }
static double ev_eng_sin_q12(uint32_t p)  { return (double)nc_px_engine_sin_q12(p); }
static double ev_lut4k(uint32_t p)        { return (double)q15_to_w(s_lut4k[(p >> 4) & 4095u]); }
static double ev_hyb64(uint32_t p)
{
    return (double)q15_to_w(hyb_lin(p, s_lin64, 64u, 10u));
}
static double ev_hyb128(uint32_t p)
{
    return (double)q15_to_w(hyb_lin(p, s_lin128, 128u, 9u));
}
static double ev_cordic(uint32_t p)
{
    return (double)q15_to_w(cordic_sin_q15(p));
}
static double ev_sinf(uint32_t p)
{
    const float t = (float)(2.0 * PI_D * (double)p / 65536.0);
    return (double)((int32_t)(sinf(t) * 2048.0f) + 2048);
}

// ── driver：min / median / p99 ───────────────────────────────────────
static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

typedef struct {
    double min_ns, med_ns, p99_ns;
    double max_err, rms_err;
} result_t;

static void bench(const case_t *c, result_t *out)
{
    static double t[REP_DEF];
    const int rep = g_rep;
    g_sink += c->fn();                       // warm-up
    for (int r = 0; r < rep; r++) {
        const double t0 = now_ms();
        g_sink += c->fn();
        const double t1 = now_ms();
        t[r] = (t1 - t0) * 1e6 / (double)N;  // ns/call
    }
    qsort(t, (size_t)rep, sizeof(t[0]), cmp_double);
    out->min_ns = t[0];
    out->med_ns = t[rep / 2];
    out->p99_ns = t[rep - 1];
}

// 精度：對理想正弦（中心 2048、振幅 2048），1 LSB = 1/4096 滿刻度。
static void accuracy(const case_t *c, result_t *out)
{
    double mx = 0.0, sq = 0.0;
    for (uint32_t p = 0; p < 65536u; p++) {
        const double ang = 2.0 * PI_D * (double)p / 65536.0;
        double ideal = 2048.0 + 2048.0 * sin(ang);
        if (ideal > 4095.0) {             // 12-bit 表示不到 4096（峰值），比較前先夾
            ideal = 4095.0;
        } else if (ideal < 0.0) {
            ideal = 0.0;
        }
        const double e = fabs(c->eval(p) - ideal);
        if (e > mx) {
            mx = e;
        }
        sq += e * e;
    }
    out->max_err = mx;
    out->rms_err = sqrt(sq / 65536.0);
}

int main(int argc, char **argv)
{
    bool quick = false, json = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else {
            fprintf(stderr, "用法：%s [--quick] [--json]\n", argv[0]);
            return 2;
        }
    }
    if (quick) {
        g_rep = REP_QUICK;
    }

    nc_px_engine_tables_init();              // ★ 出貨函數的表

    for (int i = 0; i < 4096; i++) {
        s_lut4k[i] = (int16_t)lround(sin(2.0 * PI_D * (double)i / 4096.0) * 32767.0);
    }
    for (int i = 0; i <= 64; i++) {
        s_lin64[i] = (int16_t)lround(sin(2.0 * PI_D * (double)i / 64.0) * 32767.0);
    }
    for (int i = 0; i <= 128; i++) {
        s_lin128[i] = (int16_t)lround(sin(2.0 * PI_D * (double)i / 128.0) * 32767.0);
    }
    for (int i = 0; i < N; i++) {
        s_ph[i] = (uint16_t)((uint32_t)i * 65537u);      // 均勻覆蓋整個相位域
    }

    static const case_t cases[] = {
        { "eng_sin",      run_eng_sin,      2048, ev_eng_sin },
        { "eng_sin_lin",  run_eng_sin_lin,   514, ev_eng_sin_lin },
        { "eng_sin_q12",  run_eng_sin_q12,     0, ev_eng_sin_q12 },
        { "lut_4096",     run_lut4k,        8192, ev_lut4k },
        { "hyb64_lin",    run_hyb64,         130, ev_hyb64 },
        { "hyb128_lin",   run_hyb128,        258, ev_hyb128 },
        { "cordic_q15",   run_cordic,         60, ev_cordic },
        { "sinf_libm",    run_sinf,            0, ev_sinf },
    };
    const int ncase = (int)(sizeof(cases) / sizeof(cases[0]));
    result_t res[sizeof(cases) / sizeof(cases[0])];
    for (int i = 0; i < ncase; i++) {
        bench(&cases[i], &res[i]);
        accuracy(&cases[i], &res[i]);
    }

    if (json) {
        printf("{\"n\":%d,\"rep\":%d,\"fpp\":%d,\"cases\":[", N, g_rep, FPP);
        for (int i = 0; i < ncase; i++) {
            printf("%s{\"name\":\"%s\"", i ? "," : "", cases[i].name);
            printf(",\"min_ns\":%.3f,\"med_ns\":%.3f,\"p99_ns\":%.3f,"
                   "\"ms_per_10k\":%.5f,\"max_err\":%.3f,\"rms_err\":%.3f,\"table\":%u}",
                   res[i].min_ns, res[i].med_ns, res[i].p99_ns,
                   res[i].min_ns * 1e4 / 1e6, res[i].max_err, res[i].rms_err,
                   (unsigned)cases[i].table_bytes);
        }
        printf("]}\n");
    } else {
        printf("N=%d 次/batch，REP=%d（min／median／p99）；相位由 volatile 讀。\n", N, g_rep);
        printf("一幀 = %d 次；ms/10k = ns/call × 10,000；★ = 固件出貨中的函數。\n\n", FPP);
        printf("%-12s %9s %9s %9s %9s %8s %8s %7s\n",
               "case", "min ns", "med ns", "p99 ns", "ms/10k", "max err", "rms err", "table");
        for (int i = 0; i < ncase; i++) {
            const char *mark = (strncmp(cases[i].name, "eng_", 4) == 0) ? "★" : " ";
            printf("%s%-11s %9.2f %9.2f %9.2f %9.4f %8.2f %8.2f %6uB\n",
                   mark, cases[i].name, res[i].min_ns, res[i].med_ns, res[i].p99_ns,
                   res[i].min_ns * 1e4 / 1e6, res[i].max_err, res[i].rms_err,
                   (unsigned)cases[i].table_bytes);
        }
        printf("\n誤差域：0..4095（1 LSB = 1/4096 滿刻度）；rms 對 65536 個相位點。\n");
        printf("（sink=%u，防止優化；x86-64 host，唔係 ESP32；實機用 0xF063 SINBENCH）\n",
               (unsigned)g_sink);
    }
    return 0;
}
