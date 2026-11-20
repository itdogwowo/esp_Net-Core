// sin_bench — 正弦計算的 host 基準（多種算法 + 精度 + 分佈）。
//
// 目的：比較「查表」同「多項式」在**同一台機器、同一個相位序列**下的真實差別，
// 並且分開量：時間、精度、記憶體、決定性。
//
//   A. lut_1024      —— 現時固件做法：1024 點 Q15 查表（nc_px_engine_sin）
//   B. lut_4096      —— 4096 點 Q15 查表（8 KB，換 4 倍角度解析度）
//   C. poly_q12      —— MP（你的）做法：拋物線 + 二次修正 922*(y²−y)>>12，輸出 0..4095
//   D. cordic_q15    —— CORDIC 15 次迭代（無表、整數，另一個方向）
//   E. sinf_libm     —— libm 參考（解釋為什麼要自己算）
//
// 量法：每個 case 跑 N=100000 次（= 一幀 192 次的 520 幀），重複 REP 次，
// 報 min / median / p99；相位由 volatile 陣列讀，保證唔會被向量化或預先算好。
//
// ⚠️ host（x86-64）數字；用來看**相對**成本與精度，絕對值要上板（見同目錄 README）。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N        100000       // 一個 batch 幾多次
#define REP      120          // 重複幾多個 batch（120 → p99 有意義）
#define FPP      192          // 一幀的正弦呼叫數（64 px × 3 通道）

static int16_t          s_lut[1024];
static int16_t          s_lut4k[4096];
static int16_t          s_lin64[65];        // 混合：64 個錨點 + 線性插值
static int16_t          s_lin128[129];      // 混合：128 個錨點 + 線性插值
static int16_t          s_lin256[257];      // 混合：256 個錨點 + 線性插值
static volatile uint16_t s_ph[N];
static volatile uint32_t g_sink;

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

// ── A/B. 查表 ────────────────────────────────────────────────────────
static uint32_t run_lut(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)(uint16_t)s_lut[(s_ph[i] >> 6) & 1023u];
    }
    return acc;
}

static uint32_t run_lut4k(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)(uint16_t)s_lut4k[(s_ph[i] >> 4) & 4095u];
    }
    return acc;
}

// ── C. MP 的多項式（0..65535 相位 → 0..4095）────────────────────────────
static inline int32_t wave01_q12(uint32_t phase)
{
    const uint32_t p = phase & 65535u;
    int32_t x, sgn;
    if (p < 32768u) {
        x = (int32_t)p;
        sgn = 1;
    } else {
        x = (int32_t)(p - 32768u);
        sgn = -1;
    }
    int32_t s = (int32_t)(((uint32_t)x * (uint32_t)(32768 - x)) >> 16);
    const int32_t s2 = (s * s) >> 12;
    s += (922 * (s2 - s)) >> 12;
    if (sgn < 0) {
        s = -s;
    }
    const int32_t w = (s + 4096) >> 1;
    return (w > 4095) ? 4095 : w;
}

static uint32_t run_poly(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)wave01_q12(s_ph[i]);
    }
    return acc;
}

// ── C. 混合：我的「表」做錨點 ＋ 你的「多項式」做錨點之間（線性插值）──────
// 表只存節點值（正弦本身），節點之間用一次式補：a + ((b-a)*frac >> shift)。
// 精度 ~ (2π/n)²/8 × 32768：n=128 → 0.7 LSB、n=256 → 0.25 LSB（0..4095 域）。
static inline int32_t hyb_lin(uint32_t p, const int16_t *tab, uint32_t n, uint32_t shift)
{
    const uint32_t mask = (1u << shift) - 1u;
    const uint32_t i = (p >> shift) & (n - 1u);
    const uint32_t f = p & mask;
    const int32_t a = tab[i];
    const int32_t b = tab[i + 1];
    return (int32_t)(a + (((b - a) * (int32_t)f) >> shift));
}

static uint32_t run_hyb64(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)(uint16_t)hyb_lin(s_ph[i], s_lin64, 64u, 10u);
    }
    return acc;
}

static uint32_t run_hyb128(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)(uint16_t)hyb_lin(s_ph[i], s_lin128, 128u, 9u);
    }
    return acc;
}

static uint32_t run_hyb256(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        acc += (uint32_t)(uint16_t)hyb_lin(s_ph[i], s_lin256, 256u, 8u);
    }
    return acc;
}

// ── D. CORDIC（BAM：65536 = 2π；atan 表；輸出 Q15）────────────────────
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
        acc += (uint32_t)(uint16_t)cordic_sin_q15(s_ph[i]);
    }
    return acc;
}

// ── E. libm ──────────────────────────────────────────────────────────
static uint32_t run_sinf(void)
{
    uint32_t acc = 0;
    NO_VECTOR
    for (int i = 0; i < N; i++) {
        const float t = 6.283185307179586f * (float)s_ph[i] / 65536.0f;
        acc += (uint32_t)(int32_t)(sinf(t) * 32767.0f);
    }
    return acc;
}

// ── driver：min / median / p99 ───────────────────────────────────────
typedef uint32_t (*fn_t)(void);

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void bench(const char *name, fn_t fn)
{
    static double t[REP];
    g_sink += fn();                          // warm-up
    for (int r = 0; r < REP; r++) {
        const double t0 = now_ms();
        g_sink += fn();
        const double t1 = now_ms();
        t[r] = (t1 - t0) * 1e6 / (double)N;  // ns/call
    }
    qsort(t, REP, sizeof(t[0]), cmp_double);
    const double mn = t[0];
    const double med = t[REP / 2];
    const double p99 = t[(int)(REP * 0.99)];
    printf("%-12s %9.2f %9.2f %9.2f %9.3f %9.4f\n",
           name, mn, med, p99, mn * 1e4 / 1e6, mn * FPP / 1e3);
}

// ── 精度（對理想正弦；1 LSB = 0..4095 的一級）──────────────────────────
static void accuracy(void)
{
    double lut_max = 0, lut_rms = 0, lut4_max = 0, lut4_rms = 0;
    double poly_max = 0, poly_rms = 0, cord_max = 0, cord_rms = 0;
    double h64_max = 0, h64_rms = 0;
    double h128_max = 0, h128_rms = 0, h256_max = 0, h256_rms = 0;

    for (uint32_t p = 0; p < 65536u; p++) {
        const double ang = 2.0 * 3.141592653589793 * (double)p / 65536.0;
        // 統一在「0..4095 顯示域」比較（中心 2048、振幅 2048）；Q15 方法按 32767 正規化，
        // 否則 32767 vs 32768 的 1/32768 增益差會假裝成 0.94 LSB 誤差。
        const double ideal_w = 2048.0 + 2048.0 * sin(ang);
        const double k15 = 2048.0 / 32767.0;

        const double e1 = fabs((double)s_lut[(p >> 6) & 1023u] * k15 + 2048.0 - ideal_w);
        const double e2 = fabs((double)s_lut4k[(p >> 4) & 4095u] * k15 + 2048.0 - ideal_w);
        const double e3 = fabs((double)wave01_q12(p) - ideal_w);
        const double e4 = fabs((double)cordic_sin_q15(p) * k15 + 2048.0 - ideal_w);
        const double e0 = fabs((double)hyb_lin(p, s_lin64, 64u, 10u) * k15 + 2048.0 - ideal_w);
        const double e5 = fabs((double)hyb_lin(p, s_lin128, 128u, 9u) * k15 + 2048.0 - ideal_w);
        if (e0 > h64_max) h64_max = e0;
        h64_rms += e0 * e0;
        const double e6 = fabs((double)hyb_lin(p, s_lin256, 256u, 8u) * k15 + 2048.0 - ideal_w);
        if (e5 > h128_max) h128_max = e5;
        if (e6 > h256_max) h256_max = e6;
        h128_rms += e5 * e5;
        h256_rms += e6 * e6;

        if (e1 > lut_max) lut_max = e1;
        if (e2 > lut4_max) lut4_max = e2;
        if (e3 > poly_max) poly_max = e3;
        if (e4 > cord_max) cord_max = e4;
        lut_rms += e1 * e1;
        lut4_rms += e2 * e2;
        poly_rms += e3 * e3;
        cord_rms += e4 * e4;
    }
    const double k = 1.0 / 65536.0;
    printf("\n精度（對理想正弦，1 LSB = 1/4096 滿刻度）：\n");
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "lut_1024", lut_max, sqrt(lut_rms * k), lut_max / 4095.0 * 100.0);
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "lut_4096", lut4_max, sqrt(lut4_rms * k), lut4_max / 4095.0 * 100.0);
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "poly_q12", poly_max, sqrt(poly_rms * k), poly_max / 4095.0 * 100.0);
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "cordic_q15", cord_max, sqrt(cord_rms * k), cord_max / 4095.0 * 100.0);
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "hyb64_lin", h64_max, sqrt(h64_rms * k), h64_max / 4095.0 * 100.0);
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "hyb128_lin", h128_max, sqrt(h128_rms * k), h128_max / 4095.0 * 100.0);
    printf("  %-12s max %6.2f  rms %5.2f  (%.3f%% FS)\n", "hyb256_lin", h256_max, sqrt(h256_rms * k), h256_max / 4095.0 * 100.0);
}

int main(void)
{
    for (int i = 0; i < 1024; i++) {
        s_lut[i] = (int16_t)lround(sin(2.0 * 3.141592653589793 * (double)i / 1024.0) * 32767.0);
    }
    for (int i = 0; i < 4096; i++) {
        s_lut4k[i] = (int16_t)lround(sin(2.0 * 3.141592653589793 * (double)i / 4096.0) * 32767.0);
    }
    for (int i = 0; i <= 64; i++) {
        s_lin64[i] = (int16_t)lround(sin(2.0 * 3.141592653589793 * (double)i / 64.0) * 32767.0);
    }
    for (int i = 0; i <= 128; i++) {
        s_lin128[i] = (int16_t)lround(sin(2.0 * 3.141592653589793 * (double)i / 128.0) * 32767.0);
    }
    for (int i = 0; i <= 256; i++) {
        s_lin256[i] = (int16_t)lround(sin(2.0 * 3.141592653589793 * (double)i / 256.0) * 32767.0);
    }
    for (int i = 0; i < N; i++) {
        s_ph[i] = (uint16_t)((uint32_t)i * 65537u);      // 均勻覆蓋整個相位域
    }

    printf("N=%d 次/batch，REP=%d（min / median / p99）；相位由 volatile 讀。\n", N, REP);
    printf("一幀 = %d 次；ms/10k = ns/call × 10,000。\n\n", FPP);
    printf("%-12s %9s %9s %9s %9s %9s\n", "case", "min ns", "med ns", "p99 ns", "ms/10k", "ms/frame");
    bench("lut_1024", run_lut);
    bench("lut_4096", run_lut4k);
    bench("poly_q12", run_poly);
    bench("hyb64_lin", run_hyb64);
    bench("hyb128_lin", run_hyb128);
    bench("hyb256_lin", run_hyb256);
    bench("cordic_q15", run_cordic);
    bench("sinf_libm", run_sinf);
    accuracy();

    printf("\n記憶體：lut_1024 = %d B、lut_4096 = %d B、poly = 0 B、cordic = %d B（atan 表）。\n",
           1024 * 2, 4096 * 2, 15 * 4);
    printf("（sink=%u，防止優化；x86-64 host，唔係 ESP32）\n", (unsigned)g_sink);
    return 0;
}
