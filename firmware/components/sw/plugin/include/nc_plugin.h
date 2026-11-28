// nc_plugin.h — 插件工具（解碼＋運行）的**唯一**對外介面。
//
// 一套工具做三件事：
//   ① 認得容器（.nct／.nce，見 nc_ncd.h）—— 檔頭／manifest／payload；
//   ② 把 payload 實例化成一個「插件」（open）；
//   ③ 每 tick 叫它一次（step），收工時放掉（close）。
//
// 它**唔識** pixel、**唔識** `nc_task_t`、**唔識** status，
// 亦**唔管**幾時跑、為邊個跑 —— 那些是客戶端（pixel／tasks／test）的政策。
//
// `.nct` 同 `.nce` 唔係兩種工具，係同一工具的兩種 payload：
// 分別只在 manifest 的 kind（task → `task_tick()`、effect → `nce_render()`）。
//
// 實作（backend）可以換：WAMR 係現時唯一一個（nc_plugin_wamr.c），
// 將來 AOT／其他 runtime 都係填同一張表，上層一行都唔使改。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_mod_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// 一個插件實例的把手。內容由 backend 決定（WAMR 版 = 它的 slot 指標），
// 上層只負責原封不動傳返入去。
typedef void *nc_plugin_t;

#define NC_PLUGIN_MAX      8
#define NC_PLUGIN_NAME_MAX 24

// payload 格式（同容器 flags 對應）
#define NC_PLUGIN_FMT_WASM 0
#define NC_PLUGIN_FMT_AOT  1

// payload 類型（同 nc_ncd.h 的 kind 數值一致；0 = system，一律拒絕）
#define NC_PLUGIN_KIND_TASK   1
#define NC_PLUGIN_KIND_EFFECT 2

// 開一個插件需要的全部資料。loader 由容器解碼出來，engine 唔再識 nc_mod_t。
typedef struct {
    const char    *name;      // log／診斷用（不超過 NC_PLUGIN_NAME_MAX）
    const uint8_t *payload;   // 已在 arena 內（caller 保證 lifetime ≥ 插件）
    size_t         size;
    uint8_t        fmt;       // NC_PLUGIN_FMT_*
    uint8_t        kind;      // NC_PLUGIN_KIND_*
    uint16_t       px;        // effect：輸出**值數**（1 值 = 1 B；同 program 的 pixel_hint 同義）
    uint32_t       mem_bytes; // 線性記憶體上限（manifest 宣告；0 = 不限／用預設）
} nc_plugin_src_t;

// ABI v0 的 descriptor 佈局（見 nc_mod_abi.h）：
//   0..32   descriptor（host 寫、guest 讀）
//   96      scratch 起點（預設 4 KB）
//   96+4K   framebuffer（px × 1 B，0–255；點排列由配對層 write 決定）
#define NC_PLUGIN_SCRATCH_OFF   NC_MOD_DATA_OFF
#define NC_PLUGIN_SCRATCH_BYTES (4u * 1024u)
#define NC_PLUGIN_FB_OFF        (NC_PLUGIN_SCRATCH_OFF + NC_PLUGIN_SCRATCH_BYTES)

// 砌 ABI v0 descriptor（純函式，host 可測）。回 false = px／mem 對唔上
// （fb 放唔落線性記憶體）→ 呼叫者應該拒絕開這個插件。
bool nc_plugin_desc_build(uint16_t px, uint32_t mem_bytes, nc_mod_desc_t *out);

// 執行引擎插槽。上層只識這張表；換引擎唔使改上層。
typedef struct {
    const char *name;
    // boot 內一次（arena 由 plan 給；WAMR 自己再向 heap 要）。
    esp_err_t (*init)(void *arena, size_t bytes);
    // 實例化。成功要填 *out（把手）。
    esp_err_t (*open)(nc_plugin_t *out, const nc_plugin_src_t *src);
    // 每次 tick 叫一次（效果＝一幀；任務＝一次 task_tick）。非 0 → 客戶端標 bad。
    esp_err_t (*step)(nc_plugin_t p, uint32_t frame_id);
    // 可選：效果插件交 framebuffer（guest linear memory 內，位址固定到下一幀）。
    // 回非 0 = 冇 drawable（任務模組／未 instantiate）。
    esp_err_t (*frame)(nc_plugin_t p, const uint8_t **fb, uint32_t *bytes);
    // 回收（deinstantiate／unload）。可以 NULL。
    void      (*close)(nc_plugin_t p);
    // 收工（destroy runtime）。可以 NULL。
    void      (*deinit)(void);
} nc_plugin_backend_t;

// ── backend ──
// arena 由 plan 給（caller 交一次；插件實例的 payload 住喺裡面）。
// 可以早過 backend 設定，亦可以唔交（NULL → 引擎自己向 heap 要）。
void        nc_plugin_set_arena(void *arena, size_t bytes);
// 設定引擎並用已存的 arena 初始化。be = NULL → 清除（之後 open 會失敗）。
// 初始化失敗 → 清除 backend 並回錯誤，唔會留一半狀態。
esp_err_t   nc_plugin_set_backend(const nc_plugin_backend_t *be);
bool        nc_plugin_ready(void);          // 有 backend 而且 init 成功
const char *nc_plugin_backend_name(void);   // 冇 backend → "none"

// ── 實例 ──
esp_err_t nc_plugin_open(nc_plugin_t *out, const nc_plugin_src_t *src);
esp_err_t nc_plugin_step(nc_plugin_t p, uint32_t frame_id);
// 效果插件的 framebuffer（0–255 × px × 4）。任務插件／冇 backend → ESP_ERR_NOT_SUPPORTED。
esp_err_t nc_plugin_frame(nc_plugin_t p, const uint8_t **fb, uint32_t *bytes);
void      nc_plugin_close(nc_plugin_t p);
void      nc_plugin_deinit(void);           // 所有實例已 close 之後才叫

// ── 暫停閘（OTA 期間唔跑；由呼叫方注入，工具唔識 ota_action）──
void nc_plugin_set_pause_check(bool (*fn)(void));
bool nc_plugin_paused(void);

// ── 內建 backend：WAMR（唯一一個；release 建置預設唔編）──
const nc_plugin_backend_t *nc_plugin_wamr_backend(void);

#ifdef __cplusplus
}
#endif
