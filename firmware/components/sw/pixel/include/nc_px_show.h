// nc_px_show.h — 把 mode 的「委派」變成可執行 plan：group→offsets、effect、write、range。
//
// 四層入面嘅「mode」係宣告（邊個效果寫邊個群組）；呢一支負責：
//   mode.map[] → 解析 group 引用（matrix.full／3.full／full）→ 用硬件 inventory 展開
//   → 每個委派項一個 job（asset＋write＋一至幾段 [base,count] 連續 offset）
// 之後 nc_px_show_frame() 逐幀合成：program interpreter 算值 → write 分派 → 寫入 4 B/px 畫布。
//
// 為什麼唔喺這裏碰硬件：inventory 由呼叫端（pixel_task / backend）提供：
//   { hw, base, px } = 「這個硬體型別喺合成鏈嘅起點同長度」。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_px_asset.h"
#include "nc_px_layer.h"
#include "nc_px_program.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PX_HW_INV_MAX   8
#define NC_PX_JOB_MAX      NC_PX_MODE_ENTRY_MAX
#define NC_PX_JOB_RANGE_MAX 4

typedef struct {
    uint8_t  hw;        // nc_px_hw_t
    uint16_t base;      // 合成鏈起點
    uint16_t px;        // 幾顆
} nc_px_hw_inv_t;

typedef struct {
    uint16_t base;
    uint16_t count;
} nc_px_job_range_t;

typedef struct {
    const nc_px_asset_t *asset;         // program 效果；NULL = 該項跳過
    nc_px_prog_t        prog;           // plan 時預編譯（plan 約 5 KB，唔好放棧）
    uint8_t             write;          // nc_px_write_t
    uint8_t             nranges;
    nc_px_job_range_t   rng[NC_PX_JOB_RANGE_MAX];
    uint16_t            nled;           // 這個 job 幾顆燈（= ranges 總和）
} nc_px_job_t;

typedef struct {
    uint8_t    njobs;
    uint8_t    skipped;                 // 解析唔到而跳過的 entry 數
    uint16_t   px;                      // 合成鏈總長（由 inventory 推）
    nc_px_job_t job[NC_PX_JOB_MAX];
} nc_px_plan_t;

// mode ＋ map ＋ assets ＋ inventory → plan。
// 回 ESP_OK；冇任何有效 job → ESP_ERR_NOT_FOUND；參數壞 → ESP_ERR_INVALID_ARG。
// 解析唔到嘅 entry（未知 group／未知 effect／無對應硬體）會跳過，並記入 out->skipped。
esp_err_t nc_px_show_plan(const nc_px_mode_t *mode, const nc_px_map_t *map,
                          const nc_px_asset_tbl_t *assets,
                          const nc_px_hw_inv_t *inv, uint8_t ninv,
                          nc_px_plan_t *out);

// 算一幀：step = 全域步數；dst = px × 4（R,G,B,W，0..255）。
// 只覆寫 plan 有涉及的像素；呼叫端負責先清畫布。
void nc_px_show_frame(const nc_px_plan_t *plan, uint32_t step, uint8_t *dst);

#ifdef __cplusplus
}
#endif
