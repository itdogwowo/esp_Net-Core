#include "nc_bench.h"

#include <stdint.h>

#include "esp_cpu.h"

// 燒幾多 cycles。100,000 @360 MHz ≈ 278 µs；@400 MHz ≈ 250 µs。
// 想測更細的解析度就改細，想更明顯就改大（但不要超過 period，否則會 over-budget）。
#define BENCH_BURN_CYCLES 100000u
#define BENCH_PERIOD_MS   5

static volatile uint32_t s_sink;

// 用 CPU cycle counter 做忙等：同 CPU 頻率無關的「固定工作量」。
// __asm__ volatile 令編譯器唔可以把整個迴圈優化掉（GCC 係 xtensa / riscv 都支援）。
static void bench_burn(void *arg)
{
    const uint32_t cycles = (uint32_t)(uintptr_t)arg;
    const uint32_t start = (uint32_t)esp_cpu_get_cycle_count();
    uint32_t v = s_sink + 1u;
    while ((uint32_t)esp_cpu_get_cycle_count() - start < cycles) {
        v = v * 1664525u + 1013904223u;
        __asm__ volatile("" : "+r"(v));
    }
    s_sink = v;
}

#define BENCH_DEF(NM, CORE)                                                    \
    static const nc_task_loop_t s_bench_##NM##_loop = {                        \
        .name = "bench_" #NM, .fn = bench_burn,                                \
        .arg = (void *)(uintptr_t)BENCH_BURN_CYCLES,                           \
        .stack = 3072, .prio = 2, .core = CORE, .layer = 4,                    \
        .period_ms = BENCH_PERIOD_MS, .budget_us = 2000,                       \
    };                                                                         \
    static const nc_task_t s_bench_##NM = {                                    \
        .name = "bench_" #NM, .kind = NC_TASK_NORMAL, .level = 4,              \
        .loops = &s_bench_##NM##_loop, .nloops = 1,                            \
    };                                                                         \
    const nc_task_t *nc_bench_task_##NM(void)                                  \
    {                                                                          \
        return &s_bench_##NM;                                                  \
    }

// 預設全部 core1；用 tasks.json 的 affinity 改去 core0。
BENCH_DEF(a, 1)
BENCH_DEF(b, 1)
BENCH_DEF(c, 1)
