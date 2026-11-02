// 效果載入器（fx）—— 三層裡的中間那層：政策 + 實例表。
//
// 三層（見 docs/03_notes/fx-loader.md §10）：
//   wasm   runtime 能力（init / instantiate）      boot 內單例
//   fx     政策：arena + live 指標 + 看門狗 + task  boot → 可停   ← 這支
//   module 一個效果                                 載入 → 卸載  ← 實例表，不是服務
//
// 為什麼 module 不是服務：服務註冊表是 boot-time 扁平清單、沒有卸載語義；
//   效果是 runtime 產生的，50 個效果 = 50 個服務項。註冊表存 ID，實例隨時增減
//   （同 rx-architecture.md 的結論）。
//
// 檔案系統就是效果的註冊表：/data/fx/*.fxe 就是「有哪些效果」。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// arena 上限（從 nc_plan 要；PSRAM 優先）。載入時再依 .fxe 的 mem_bytes 切。
#define NC_FX_ARENA_MAX   (512 * 1024)
#define NC_FX_MAX_MODULES 8
#define NC_FX_NAME_MAX    24
#define NC_FX_PATH_MAX    80

// 執行引擎插槽。現在只有 stub；WAMR 接進來時換掉 fn 表即可（不改上層）。
typedef struct nc_fx nc_fx_t;

typedef struct {
    const char *name;
    esp_err_t (*init)(void *arena, size_t bytes);
    esp_err_t (*render)(nc_fx_t *fx, uint32_t frame_id);
    void      (*deinit)(void);
} nc_fx_backend_t;

// ── 生命週期 ──
// ① 宣告（nc_plan_commit 之前）
esp_err_t nc_fx_declare(void);
// ② 初始化（commit 之後、task start 之前）。arena 由 plan 給。
esp_err_t nc_fx_init(void *arena, size_t bytes);
// ③ 掃 /data/fx/ 把找到的 .fxe 載入（此時 FAT 仍歸裝置）
int nc_fx_scan(const char *dir);

// ── 實例操作 ──
esp_err_t nc_fx_load(const char *path);
esp_err_t nc_fx_select(const char *name);      // NULL = 停止
esp_err_t nc_fx_unload(const char *name);
nc_fx_t  *nc_fx_live(void);
// 這個模組是 wasm 還是 aot（NC_FX_FMT_*）；WAMR backend 用它決定 import 路徑
uint8_t   nc_fx_module_format(const nc_fx_t *fx);
// 這顆晶片吃的 AOT target（NC_FX_TARGET_*）；0 = 未知（只收 wasm）
int       nc_fx_arch_target(void);

// ── 執行引擎插槽（host 測試或之後的 WAMR）──
esp_err_t nc_fx_set_backend(const nc_fx_backend_t *backend);

// ── task ──
void nc_fx_task(void *arg);          // FreeRTOS 迴圈（host 測試不編）
uint32_t nc_fx_tick(void);           // 一幀：退休 + render + 預算；host 測試直接呼叫
uint32_t nc_fx_frame_id(void);

// ── 狀態 / 診斷 ──
size_t nc_fx_status_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
