// nc_px_layer.h — DATA 平面的第二、三、四層：mapping／mode／播放清單。
//
// 四層（權威：docs/02_guides/08_pixel_subsystem.md）：
//   effect   `pixel/effects/effects.json`     波形 program（nc_px_asset_t）
//   mapping  `pixel/map/*.json`               群組排列：group.sel[] = 每種硬體選邊啲燈
//   mode     `pixel/modes/*.json`             委派：group × effect × write × range
//   show     `pixel/registry.json`            播放清單：list[] ＋ auto_play
//
// 這一支只做「JSON → struct」：唔碰檔案系統、唔碰硬件、熱路徑唔再 parse。
// 解析失敗／未知欄位 → 回錯誤或跳過該項；呼叫端負責 log（同 asset 層一致）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PX_LAYER_NAME_MAX 24
#define NC_PX_SLICE_MAX      16
// 容量唔再用固定常數：group／sel 由 `nc_px_map_count()` 數出嚟、由呼叫端配 pool。
// 下面兩個只係「壞檔／惡意 JSON」的健全性上限（唔係正常路徑的截斷）；
// 正常超過只會令 pool 配置失敗（ESP_ERR_NO_MEM），唔會靜默丟資料。
#define NC_PX_GROUP_MAX     65535u
#define NC_PX_GROUP_SEL_MAX 4194304u
#define NC_PX_MODE_ENTRY_MAX  8   // legacy show/bench 固定 array；production paint 唔用呢個上限

// ── 硬體型別（registry 的 key；同 pixel_task.py 的 TYPE_MAP 一致）──
typedef enum {
    NC_PX_HW_NONE = 0,
    NC_PX_HW_WS2812,
    NC_PX_HW_APA102,
    NC_PX_HW_PCA9685,
    NC_PX_HW_PWM,
    NC_PX_HW_UARTMOTOR1,
    NC_PX_HW_COUNT,
} nc_px_hw_t;

// ── mapping（群組排列）──
// 一個 group = **有序的段列表**（sel）；段依書寫次序拼接 = 像素次序，
// 可以交叉型別、反序（"15:10:-1"）、隔顆（"::2"）、單顆（"7"）、重疊。
// 群組數／段數**由檔案決定**（`nc_px_map_count()`），唔再係固定陣列。
typedef struct {
    uint8_t hw;                              // nc_px_hw_t；NONE = 未認識（呼叫端跳過）
    char    slice[NC_PX_SLICE_MAX];          // ":" / "0:16" / "::2" / "15:10:-1"（Python slice 語義）
} nc_px_group_sel_t;

typedef struct {
    uint16_t id;                             // 500 組以上要用 16-bit（舊 8-bit 會撞）
    char     name[NC_PX_LAYER_NAME_MAX];
    const nc_px_group_sel_t *sel;            // 指入呼叫端的 pool（生命週期由呼叫端管）
    uint32_t nsel;
} nc_px_group_t;

typedef struct {
    uint8_t  id;
    char     name[NC_PX_LAYER_NAME_MAX];
    const nc_px_group_t *group;              // 指入呼叫端的 pool
    uint32_t ngroups;
    uint32_t hash;                           // 檔案內容 hash（快取 key 用）
} nc_px_map_t;

// 解析用 bump pool：呼叫端一次過配（大小 = 所有 map 檔 count 的總和），
// 解析時只向前推，唔 malloc、唔碎片；`nc_px_layer_clear()` 會 reset。
typedef struct {
    nc_px_group_t     *group;
    uint32_t           cap_group;
    uint32_t           used_group;
    nc_px_group_sel_t *sel;
    uint32_t           cap_sel;
    uint32_t           used_sel;
} nc_px_map_pool_t;

// ── mode（委派）──
// write 名同 pixel_layout 的 _SCATTER 一致：r/g/b/w/ww/rgb/rgbw/wwww
typedef enum {
    NC_PX_WRITE_R = 0,
    NC_PX_WRITE_G,
    NC_PX_WRITE_B,
    NC_PX_WRITE_W,
    NC_PX_WRITE_WW,
    NC_PX_WRITE_RGB,
    NC_PX_WRITE_RGBW,
    NC_PX_WRITE_WWWW,
    NC_PX_WRITE_COUNT,
} nc_px_write_t;

typedef struct {
    char    group[NC_PX_LAYER_NAME_MAX + 8];  // 原始引用："matrix.full" / "3.full" / "full"
    char    effect[NC_PX_LAYER_NAME_MAX];
    uint8_t write;                           // nc_px_write_t
    bool    has_range;
    int16_t start;                           // range 省略 = 0（整組）
    int16_t stop;                            // 0 = 到最尾（Python 的 "："）
    int16_t step;                            // 0 = 1
} nc_px_mode_entry_t;

typedef struct {
    uint8_t            id;
    char               name[NC_PX_LAYER_NAME_MAX];
    char               mapping[NC_PX_LAYER_NAME_MAX];  // mode 預設 mapping（可空）
    int16_t            play_loop;            // -1 = 無限
    uint16_t           maxF;                 // 0 = 唔限
    // 白混合（production Show 用）：0..100；JSON `"white"` 或 `"white_mix"`。
    // 缺欄位 = 0（舊 JSON 完全相容）。逐幀對四個通道做
    //   out = (out*(100-w) + 255*w) / 100
    // 即 w=80 → 「80% 白 ＋ 20% 效果」。由 pair 任務套用（effect 只算值），唔關渲染器事。
    uint8_t            white;
    uint32_t           nentry;
    const nc_px_mode_entry_t *entry;   // 指入呼叫端 pool（mode 條目數由檔案決定）
    uint32_t           hash;           // 檔案內容 hash（快取 key 用）
} nc_px_mode_t;

// ── show（播放清單）──
typedef struct {
    bool        auto_play;
    uint32_t    nlist;
    uint32_t    cap;
    char      (*list)[NC_PX_LAYER_NAME_MAX];   // 動態；nc_px_show_parse() 配置
} nc_px_show_t;

// ═══════════════════════ API ═══════════════════════
// 全部：回 ESP_OK = 至少解析到一個有效項；ESP_ERR_INVALID_ARG = JSON／schema 壞。
// map 兩步：先 `nc_px_map_count()` 數要幾多 group／sel，配好 pool，再 `nc_px_map_parse()`。
// pool 由呼叫端擁有；parse 只向前推，容量不足回 ESP_ERR_NO_MEM（唔會截斷）。
void      nc_px_map_pool_reset(nc_px_map_pool_t *pool);
void      nc_px_show_free(nc_px_show_t *show);
esp_err_t nc_px_map_count(const char *json, size_t len, uint32_t *ngroups, uint32_t *nsels);
esp_err_t nc_px_map_parse(const char *json, size_t len, nc_px_map_pool_t *pool,
                          nc_px_map_t *out);
esp_err_t nc_px_mode_count(const char *json, size_t len, uint32_t *nentry);
esp_err_t nc_px_mode_parse(const char *json, size_t len, nc_px_mode_entry_t *pool,
                           uint32_t cap, nc_px_mode_t *out);
esp_err_t nc_px_show_parse(const char *json, size_t len, nc_px_show_t *out);

// 名字 ↔ 代號（給 log／測試用）
const char *nc_px_hw_name(uint8_t hw);
bool        nc_px_hw_parse(const char *name, uint8_t *out);
const char *nc_px_write_name(uint8_t w);
bool        nc_px_write_parse(const char *name, uint8_t *out);

// Python slice 字串 → (start, stop, step)；`stop == 0` 代表「到最尾」、`step == 0` 代表 1。
bool nc_px_slice_parse(const char *s, int16_t *start, int16_t *stop, int16_t *step);

#ifdef __cplusplus
}
#endif
