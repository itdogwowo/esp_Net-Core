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
    uint32_t base;      // 合成鏈起點（32-bit：唔再 65,535 上限）
    uint32_t px;        // 幾顆
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

// 舊 plan（A/B 對拍＋板上自測用；production 走 nc_px_layout）。
// **唔再有 `job[8]` 固定上限**：job 陣列由呼叫端提供（容量 = mode->nentry 就夠），
// 超容量會計入 skipped，唔會靜靜 drop。
typedef struct {
    uint8_t     njobs;
    uint8_t     skipped;                // 解析唔到／容量唔夠而跳過的 entry 數
    uint16_t    px;                     // 合成鏈總長（由 inventory 推）
    uint16_t    jobcap;                 // 呼叫端給的 job 容量
    nc_px_job_t *job;                   // jobcap 個（呼叫端配置，生命週期由呼叫端管）
} nc_px_plan_t;

// mode ＋ map ＋ assets ＋ inventory → plan。
// 回 ESP_OK；冇任何有效 job → ESP_ERR_NOT_FOUND；參數壞 → ESP_ERR_INVALID_ARG。
// 解析唔到嘅 entry（未知 group／未知 effect／無對應硬體）會跳過，並記入 out->skipped。
esp_err_t nc_px_show_plan(const nc_px_mode_t *mode, const nc_px_map_t *map,
                          const nc_px_asset_tbl_t *assets,
                          const nc_px_hw_inv_t *inv, uint8_t ninv,
                          nc_px_plan_t *out, nc_px_job_t *jobs, uint16_t jobcap);

// ── 配對層（generic）：效果交「值緩衝」，唔理佢係六工具／.nce／未來效果器 ──
// 語義逐條照 mp `pixel_layout._SCATTER`：
//   r/g/b/w  只寫該通道（其餘不動；重疊寫可以疊加）
//   ww       寫 o[0]=0,o[1]=0, o[2]=v&0xFF, o[3]=v>>8（12-bit 拆兩個 byte）
//   wwww     同一個 v（>>4）寫晒四通道
//   rgb      寫 R,G,B，W 清零
//   rgbw     寫 R,G,B,W
//   nv==0    按上面規則寫 0（單通道只清自己嗰個）
void nc_px_pair_apply(const nc_px_job_t *job, const uint16_t *vals, uint16_t nv, uint8_t *dst);

// ── 12-bit 原生配對（效果層的真身）──
// 不變量：**效果狀態永遠 12-bit 0..4095**；位深轉換／抖動只喺最尾（encode）做。
// dst12 = px × 4 個 uint16（每通道 0..4095，2 B/channel；9,600 px = 76.8 KB）。
// 寫法語義同 8-bit 版一樣，只係唔做 `>>4`（ww 仍然照 MP 的 byte 打包）。
void nc_px_pair_apply12(const nc_px_job_t *job, const uint16_t *vals, uint16_t nv,
                        uint16_t *dst12);

// 12-bit → 8-bit：dither=false 四捨五入；true 用 1D 誤差擴散（err 要 4 個 int32，
// 跨像素保留），令低 4 位唔會白丟。未來 >8-bit 輸出（apa102 16-bit、pca9685 12-bit）
// 就唔使轉，直接交 12-bit。
void nc_px_canvas12_to_8(const uint16_t *src, uint8_t *dst, uint32_t nch, bool dither,
                         int32_t err[4]);

// 由 program asset 算一個 job 的 vals（給六工具；未來效果器自己出 buffer 就跳過呢個）
uint16_t nc_px_job_vals(const nc_px_job_t *job, uint32_t step, uint16_t *out, uint16_t cap);

// 算一幀：step = 全域步數；dst = px × 4（R,G,B,W，0..255）。
// 只覆寫 plan 有涉及的像素；呼叫端負責先清畫布。
void nc_px_show_frame(const nc_px_plan_t *plan, uint32_t step, uint8_t *dst);

#ifdef __cplusplus
}
#endif
