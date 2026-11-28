// nc_px_layout.h — 排版配對：把自描述的 map／mode 編譯成 **destination 順序的寫入表**。
//
// 為什麼要這一層（見 docs/00_pm/pixel-layout-pairing.md）：
//   舊 `nc_px_show_plan()` 把 group 壓成「最多 4 段連續 range + base/count」，
//   反序、隔顆、重疊、500 組全部表達不到（靜默截斷）。
//   這一支按舊項目 `PixelLayout` 的語義忠實展開：
//     group.sel[] → 有序全域 pixel index → （mode range 再切）→ 每顆一個 dst
//   然後 **按 dst 分桶（counting bucket，O(px+n)，唔用 qsort）**，合併成 runs：
//     px_lrun {dst, src, count, job}
//   pair 只順序掃 runs → PSRAM 順序寫（inverse8 實測 1.872 ms / 9,600 px）。
//
// 容量：**由檔案決定**。group 數、sel 段數、job 數、runs 數、值流長度全部唔設固定上限；
//   index 用 32-bit（唔再 65,535 上限）。記憶體由本層 malloc（board 上 PSRAM）。
//
// 與舊 plan 並存：`nc_px_show.c` 的 `nc_px_plan_t`／`nc_px_pair_apply()` 保留做 A/B 對拍，
//   production 走這一支。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_px_asset.h"
#include "nc_px_layer.h"
#include "nc_px_program.h"
#include "nc_px_show.h"          // nc_px_hw_inv_t

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PX_LAYOUT_ASSET_MAX 32

// 一個用到的效果（program 只編一次；同一 asset 的多個 group 共用）
typedef struct {
    const nc_px_asset_t *asset;
    nc_px_prog_t         prog;
    uint32_t             nv;        // 值流長度（= asset->pixel_hint；唔再 cap 256）
    uint32_t             vals_off;  // 在值流池的位置
} nc_px_aprog_t;

// 一個 mode 條目 = group × asset × write（range 已經在編譯時展開）
typedef struct {
    uint16_t aprog;                 // index into layout->aprog
    uint8_t  write;                 // nc_px_write_t
    uint32_t nled;                  // 這個條目幾顆燈（run 的 count 總和；32-bit）
} nc_px_ljob_t;

// destination 升序的一段連續寫入
typedef struct {
    uint32_t dst;                   // 全域輸出位置（canvas 的 pixel index）
    uint32_t src;                   // 值流 index k（group 像素次序的第 k 顆）
    uint32_t count;                 // 連續幾顆
    uint32_t job;                   // index into layout->job
} nc_px_lrun_t;

// 6 B／顆的落點表（`nc_px_layout_flatify()` 由 runs 展開）。
// 為什麼要這張：runs 每段只有幾顆（500 組 / 9,600 px ≈ 19 顆），但每段要算 11 次
// `% nv` 加兩層 job/aprog 追蹤；展開成「每顆一筆」之後熱路徑冇除法、冇分表。
// `src` 是**值流起點**（已經用 write 的通道步長換算好），所以內圈只剩加法回繞。
typedef struct {
    uint16_t dst;                   // 落點（canvas 的 pixel index）
    uint16_t src;                   // 值流起點（第一條通道；rgb/rgbw 之後 +3／+4）
    uint8_t  write;                 // nc_px_write_t
    uint8_t  aprog;                 // index into layout->aprog
} nc_px_flat6_t;                    // 6 B／顆（見 nc_px_layout.c 的 _Static_assert）

typedef struct {
    uint32_t px;                    // 合成鏈總長（inventory 推）
    uint32_t naprog;                // 用到的效果數（值流條數）
    uint32_t njobs;
    uint32_t nruns;
    uint32_t nvals;                 // 值流池長度（Σ aprog[].nv）
    uint32_t skipped;               // 解析唔到而跳過的 entry（合計）
    // 逐個原因分開數：用戶要知「邊一條、為咩」而唔是「靜靜少咗」。
    uint16_t skip_no_asset;         // mode 引用的 effect 唔存在
    uint16_t skip_no_group;         // mode 引用的 group 唔存在
    uint16_t skip_bad_write;        // write 名唔認識
    uint16_t skip_empty;            // group／range 展開後 0 顆
    uint16_t skip_asset_full;       // 效果數超過 aprog 上限
    uint16_t skip_prog;             // program 編譯失敗
    uint16_t skip_oom;              // 暫存／容量唔夠
    uint32_t       nflat;           // 6 B 表長度（0 = 冇表 → 熱路徑退回 runs）
    nc_px_aprog_t *aprog;           // naprog 個
    nc_px_ljob_t  *job;             // njobs 個
    nc_px_lrun_t  *run;             // nruns 個（dst 升序）
    nc_px_flat6_t *flat;            // nflat 個（dst 升序；重疊 = 後面覆蓋前面）
    bool           flat_psram;      // flat 表放喺 PSRAM（量測／政策用）
    int16_t        flat_err;        // 最近一次 flatify 的結果（0=OK；非 0 = 行 runs）
    uint8_t        flat_uniform;    // flat 表全部同一 write 時的值（0 = 混合）
} nc_px_layout_t;

// mode ＋ map ＋ assets ＋ inventory → layout（內部 malloc；用完 call free）。
// 回 ESP_OK；冇任何有效 job → ESP_ERR_NOT_FOUND；參數壞 → ESP_ERR_INVALID_ARG；
// 記憶體不足 → ESP_ERR_NO_MEM。解析唔到的 entry（未知 group／effect／硬體）會跳過並記入 skipped。
esp_err_t nc_px_layout_build(const nc_px_mode_t *mode, const nc_px_map_t *map,
                             const nc_px_asset_tbl_t *assets,
                             const nc_px_hw_inv_t *inv, uint32_t ninv,
                             nc_px_layout_t *out);

void nc_px_layout_free(nc_px_layout_t *l);

// ── 永久儲存（自描述二進位；下次直接讀，唔重建）──
// key = 檔案內容 hash ＋ px ＋ 格式版本；唔夾就當快取失效（回 ESP_ERR_INVALID_STATE）。
// 讀取時用 assets 表把 asset 名稱指回真實 asset（program 已編譯，原樣帶回）。
#define NC_PX_LAYOUT_CACHE_VER 2u   // 2: program segments 動態（variable-length cache）
uint32_t nc_px_layout_cache_key(uint32_t mode_hash, uint32_t map_hash, uint32_t px,
                                uint32_t assets_hash);
size_t    nc_px_layout_cache_bytes(const nc_px_layout_t *l);
esp_err_t nc_px_layout_save(const nc_px_layout_t *l, const char *path, uint32_t key);
esp_err_t nc_px_layout_load(nc_px_layout_t *out, const char *path, uint32_t key,
                            const nc_px_asset_tbl_t *assets);

// ── 6 B 落點表（Task 7）──
// 由 runs 一次展開（換場／讀快取之後自動做；唔使呼叫端記住）。
// 唔展開的情況：`px > 65,535`、`naprog > 255`、有值流 `nv > 65,535` 或 `nv == 0`、
// rgb 值流 `nv < 3`／rgbw `nv < 4`、總顆數 > 8×px（重疊太多）、**internal 記憶體唔夠**。
// 此時 `nflat == 0`，熱路徑照舊行 runs，行為一模一樣。
//
// 記憶體擺位係**硬要求**：只可以放 internal SRAM（6 B × 顆數；9,600 px ＝ 57.6 KB）。
// 真機實測放 PSRAM 會**慢過** runs（每幀 57.6 KB PSRAM 讀取頻寬），所以唔夠 internal
// 就唔建表、行返 runs——唔會靜默降級去一條更慢的路。
esp_err_t nc_px_layout_flatify(nc_px_layout_t *l);
// 量測／政策用：place_psram = true 時 6 B 表放 PSRAM（預設 internal，實測較快）。
esp_err_t nc_px_layout_flatify_place(nc_px_layout_t *l, bool place_psram);
size_t    nc_px_layout_flat_bytes(const nc_px_layout_t *l);   // nflat × 6 B（internal）

// 一幀：vals[i] = 第 i 條值流的指標（長度 aprog[i].nv），dst = canvas（px × 4）。
// 語義逐條照 mp `pixel_layout._SCATTER`；回傳寫入咗幾多顆（有 6 B 表）或幾多段 run。
uint32_t nc_px_layout_apply(const nc_px_layout_t *l, uint16_t *const *vals, uint8_t *dst);
// 只套用 flat 表的一段（[from, from+count)）；給 chunk／預取 pipeline 用。
uint32_t nc_px_layout_apply_range(const nc_px_layout_t *l, uint16_t *const *vals,
                                  uint8_t *dst, uint32_t from, uint32_t count);

// 這一幀開始前使唔使清 canvas？write=r/g/b/w（只寫一個通道）或 flat 表未覆蓋全部 px
// 時要；全部 RGB／RGBW／WW／WWWW 且 full coverage 就唔使（可省 16 KB memset）。
bool nc_px_layout_needs_clear(const nc_px_layout_t *l);

// 時間抖動：一次掃描 runs，同時寫 subframes 份 8-bit 輸出。
// 每份 = base 或 base+1，令 subframes 份平均 = 12-bit>>4（±1）。
// phase 每幀 +1，令 +1 的子幀輪流，避免閃爍。只支援 rgb／rgbw。
void nc_px_layout_apply4(const nc_px_layout_t *l, uint16_t *const vals[],
                         uint8_t *const dst[], uint8_t subframes, uint32_t phase);

// 只寫其中一個子幀（環一次只可以 begin 一格，所以逐子幀寫）。
void nc_px_layout_apply_dither(const nc_px_layout_t *l, uint16_t *const vals[],
                               uint8_t *dst, uint8_t subframes, uint8_t sf, uint32_t phase);

// 12-bit 版（canvas = px × 4 個 uint16；唔做 >>4）。
uint32_t nc_px_layout_apply12(const nc_px_layout_t *l, uint16_t *const *vals, uint16_t *dst);

// 由 program 算一條值流（給 effect 任務；wave table 在 ap 內）
uint32_t nc_px_aprog_vals(const nc_px_aprog_t *ap, uint32_t step, uint16_t *out, uint32_t cap);

#ifdef __cplusplus
}
#endif
