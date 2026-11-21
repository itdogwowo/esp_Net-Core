// nc_pipeline.h — effect → pair 的窄介面。
//
// 三任務：effect（只算波形值）→ pair（map/range/write/white）→ render（I2C/I80）。
// 這個 header 只放兩個方向：
//   · pair 擁有的 plan → effect 要一份 snapshot（effect 不讀 pair 的 static）；
//   · effect 的最新值 frame → pair 要一份 copy（pair 不讀 effect 的 static）。
// 兩邊都用 mutex；frame 內容 fixed layout，唔帶 pointer（asset 已 repoint 到 snapshot 的副本）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nc_px_asset.h"
#include "nc_px_show.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PIPE_VAL_MAX 256

typedef struct {
    uint32_t seq;
    uint32_t plan_gen;
    uint8_t  njobs;
    uint16_t nv[NC_PX_JOB_MAX];
    uint16_t vals[NC_PX_JOB_MAX][NC_PIPE_VAL_MAX];
} nc_pipe_effect_frame_t;

// pair → effect：copy 一份 plan；assets 係 caller 的 array（最少 njobs 個），
// 會把每個 job.asset 指去 assets[i] 的副本，effect 之後唔再碰 pair 的 asset 表。
bool nc_pipe_plan_snapshot(nc_px_plan_t *out, nc_px_asset_t *assets,
                           uint8_t cap, uint32_t *gen);

// effect → pair：copy 最新完整 frame；未有 frame 回 false。
bool nc_pipe_effect_copy(nc_pipe_effect_frame_t *out);

#ifdef __cplusplus
}
#endif
