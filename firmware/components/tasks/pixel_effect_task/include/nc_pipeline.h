// nc_pipeline.h — effect → pair 的窄介面（layout 版）。
//
// 四任務：pixel_effect_task（只算波形值）→ pixel_pair_task（抖動／配對／white mix／寫環）
// → pixel_encode_task（畫波）→ render_task（公用推送）。
//
// 這一版配合 `nc_px_layout`（data-driven 排版）：pair 把編譯好的 `aprog[]`
// （每個效果一條值流）發佈出去，effect 只負責定時把值流算好；pair 拿最新一幀
// 的**指標**（唔複製 57 KB 的值流），順序掃 runs 寫 canvas。
//
// 值流的雙緩衝：effect 有兩個 pool，交替寫；pair 處理完一幀後 ack(seq)，
// effect 只會寫「pair 已經消化咗」的 slot（pair 慢就等一下，唔會撕裂）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nc_px_asset.h"
#include "nc_px_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

// 冇固定上限：陣列由 effect 按 naprog 配置（PSRAM），下面只係 view。
// 一幀：naprog 條值流（vals[i] 指向 effect 的 pool；長度 nv[i]）
typedef struct {
    uint32_t seq;
    uint32_t plan_gen;
    uint32_t t_pub_us;         // effect 完成時間（pair 算 latency）
    uint32_t naprog;
    const uint32_t *nv;        // [naprog]（effect 擁有；plan 唔變就穩定）
    uint16_t *const *vals;     // [naprog]（effect 擁有）
} nc_pipe_effect_frame_t;

// pair → effect：plan 換了，effect 要重建 wave table／pool
typedef struct {
    uint32_t plan_gen;
    uint32_t naprog;
    uint32_t cap;              // nv／aprog 容量（effect 管理）
    uint32_t *nv;              // [cap]
    nc_px_aprog_t *aprog;      // [cap]
} nc_pipe_aprog_set_t;

typedef struct {
    uint32_t eff_n, eff_avg_us, eff_max_us, eff_last_us;
    uint32_t pair_n, pair_avg_us, pair_max_us, pair_last_us;
    uint32_t clear_avg_us, clear_max_us, clear_last_us;   // canvas memset（px×4）
    uint32_t apply_avg_us, apply_max_us, apply_last_us;
    uint32_t white_avg_us, white_max_us, white_last_us;
    uint32_t commit_avg_us, commit_max_us, commit_last_us;
    uint32_t lat_avg_us, lat_max_us, lat_last_us;
    uint32_t full;
} nc_pipe_stats_t;

// pair：plan 重建後發佈 aprog（gen 由 pair 遞增）
void nc_pipe_aprog_publish(const nc_px_aprog_t *aprog, uint32_t naprog, uint32_t gen);
// effect：拿一份 aprog 副本；未有 → false
bool nc_pipe_aprog_snapshot(nc_pipe_aprog_set_t *out);
// effect → pair：copy 最新一幀（只複製指標）；未有 → false
bool nc_pipe_effect_copy(nc_pipe_effect_frame_t *out);
// pair：處理完 seq 這一幀（effect 可以重用該 slot）
void nc_pipe_effect_ack(uint32_t seq);

// 分段計時／極限 burn。
void nc_pipe_stats_reset(void);
void nc_pipe_stats_get(nc_pipe_stats_t *out);
void nc_pipe_stats_effect(uint32_t us);
void nc_pipe_stats_pair(uint32_t clear_us, uint32_t apply_us, uint32_t white_us,
                        uint32_t commit_us, uint32_t lat_us);
void nc_pipe_stats_full(void);
void nc_pipe_stats_burn(uint32_t seconds);
bool nc_pipe_stats_burn_active(void);

#ifdef __cplusplus
}
#endif
