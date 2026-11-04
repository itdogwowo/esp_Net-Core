// 每核「大循環」（round）量測：一個核上面的所有 loop 各自跑完一次 = 一輪。
//
// 純邏輯、零依賴（唔用 IDF、唔用 FreeRTOS），所以 host 都跑得。
// 時間同 cycles 由呼叫端提供 —— trampoline 已經有現成的 timestamp，唔需要再讀一次。
//
// 為什麼要呢個數字：
//   · period_us = 上一輪完成到這一輪完成的 wall time（包含等 period 的時間）
//   · exec_us   = 這一輪裡面所有 loop 的 fn 執行時間總和（真正的負載）
//   · duty      = exec / period（這個核有幾忙）
//   · cycles    = 同一輪的 CPU cycles（同 exec 對比可以睇出被搶佔／量測誤差）
//
// ⚠️ 定義上的限制：round 要「所有 loop 都跑過一次」才完成，所以
//    如果有一條 loop 的 period 係 1 秒，round 就會係 1 秒 —— 唔係 bug，係定義。
//    要量一個短窗口，就要令同一核上面的 loop period 接近。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_EPOCH_CORES 2
#define NC_EPOCH_LOOPS 8          // 每個核心最多 8 條 loop（超過就 add 失敗）

typedef struct {
    uint32_t rounds;              // 完成過幾多輪（第一輪只做 base，唔計）
    uint32_t period_last_us;
    uint32_t period_avg_us;
    uint32_t period_min_us;
    uint32_t period_max_us;
    uint32_t jitter_us;           // period_max − period_min
    uint32_t exec_last_us;        // 一輪裡面所有 loop 的 fn 時間總和
    uint32_t exec_avg_us;
    uint32_t exec_max_us;
    uint32_t cycles_last;         // 同一輪的 CPU cycles（u32 差，> 10.7 s @400 MHz 會 wrap）
    uint32_t cycles_avg;
    uint16_t duty_permille;       // exec_avg / period_avg × 1000
    uint8_t  loops;               // 目前幾條 loop
    bool     collecting;          // 有冇開始收集（至少一條 loop）
} nc_epoch_stats_t;

void nc_epoch_reset(void);

// 登記一條 loop（開機／start 時）。回 token（0..NC_EPOCH_LOOPS-1），<0 = 失敗。
int  nc_epoch_add(int core);
void nc_epoch_remove(int core, int token);

// 一條 loop 跑完一輪 fn 之後呼叫。
//   now_us   : 現在時間（esp_timer）
//   cycles   : 現在 CPU cycles（esp_cpu_get_cycle_count）
//   exec_us  : 這次 fn 執行的 wall time
// 回 true = 這一刻剛好完成一個「大循環」。
bool nc_epoch_mark(int core, int token, int64_t now_us, uint32_t cycles, uint32_t exec_us);

void nc_epoch_get(int core, nc_epoch_stats_t *out);

// {"core0":{...},"core1":{...}}；永遠輸出合法 JSON（截斷時回一個最小物件）。
size_t nc_epoch_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
