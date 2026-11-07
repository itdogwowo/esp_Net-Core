// 基準任務（bench）：純 CPU 負載產生器（燒固定 cycles，量吞吐／延遲用）。
//
// 三條獨立任務 bench_a / bench_b / bench_c，預設喺 tasks.json 係 (0,0) = 唔跑；
// 要測試就在 tasks.json 開 affinity，例如：
//   { "name": "bench_a", "level": 4, "affinity": [0, 1] }   // 跑 core1
//
// 每條每 5 ms 燒固定 cycles（預設 100,000 ≈ 0.28 ms @360 MHz）。
// 開 0/1/2/3 條，看外部的吞吐／RTT 是否大約線性下降。
// （每核「大循環」量測 epoch 已於 2026-11-06 移除，不再提供 exec/duty。）
#pragma once

#include "nc_task.h"

#ifdef __cplusplus
extern "C" {
#endif

const nc_task_t *nc_bench_task_a(void);
const nc_task_t *nc_bench_task_b(void);
const nc_task_t *nc_bench_task_c(void);

#ifdef __cplusplus
}
#endif
