// nc_px_layer_scan.h — 掃描 DATA 平面的 map／modes 目錄同 registry.json → 表。
//
// 行為對齊 MP `pixel_task._init_layout()` / `_init_modes()`：
//   · map/*.json   → 全部 mapping（檔名排序；同 id／同名跳過，保留第一個）
//   · modes/*.json → 全部 mode（按 `index` 排序；id／name 重複跳過）
//   · registry.json → 播放清單（auto_play ＋ list）
// 解析在 nc_px_layer.c（PC 可測）；呢一支只做 I/O＋登記＋排序。
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "nc_px_layer.h"

#ifdef __cplusplus
extern "C" {
#endif

// 2026-11-28：唔再用固定 8 maps／16 modes。表由掃描到的檔案數決定，item 由
// nc_px_layer_scan.c 動態配置；上限只剩 RAM／FAT。呼叫端用 tbl->n／.item。
typedef struct {
    uint32_t     n;
    uint32_t     rejected;     // 讀唔到／解析失敗／重複
    uint32_t     cap;
    nc_px_map_t *item;
} nc_px_map_tbl_t;

typedef struct {
    uint32_t      n;
    uint32_t      rejected;
    uint32_t      cap;
    nc_px_mode_t *item;
} nc_px_mode_tbl_t;

// 掃一個目錄（例如 `…/pixel/map`）；目錄唔存在 → ESP_ERR_NOT_FOUND。
void                        nc_px_layer_clear(void);
esp_err_t                   nc_px_map_scan(const char *dir);
esp_err_t                   nc_px_mode_scan(const char *dir);
esp_err_t                   nc_px_show_load(const char *path);
const nc_px_map_tbl_t      *nc_px_map_table(void);
const nc_px_mode_tbl_t     *nc_px_mode_table(void);
const nc_px_show_t         *nc_px_show(void);

// 一次掃齊四層（base = `…/pixel`）：effects／map／modes／registry.json。
// 行為同 MP pixel_task 開機一樣：effects 失敗 → 回錯誤；其餘失敗只 warn。
esp_err_t nc_px_layer_load_all(const char *base);

// 找 mapping／mode（name 或數字 id）；搵唔到回 NULL。
const nc_px_map_t  *nc_px_map_find(const char *ref);
const nc_px_mode_t *nc_px_mode_find(const char *ref);

#ifdef __cplusplus
}
#endif
