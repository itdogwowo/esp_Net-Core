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
// 檔案系統就是效果的註冊表：/data/pixel/effects/*.fxe 就是「有哪些效果」。
//
// v1：每個 .fxe 可以帶 manifest（nc_fx_manifest.h）宣告「我是什麼、跑哪一核、
//     要多少記憶體」。載入時解碼 → 驗證 → 用 nc_task_attach() 登記成租戶。
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
#define NC_FX_MAX_CMDS    8      // manifest 最多宣告幾條指令（模組層另有上限）

// 執行引擎插槽。現在只有 stub；WAMR 接進來時換掉 fn 表即可（不改上層）。
typedef struct nc_fx nc_fx_t;

typedef struct {
    const char *name;
    esp_err_t (*init)(void *arena, size_t bytes);
    esp_err_t (*render)(nc_fx_t *fx, uint32_t frame_id);
    void      (*deinit)(void);
} nc_fx_backend_t;

// ── 生命週期 ──
// ① 初始化（commit 之後、task start 之前）。arena 由 plan 給。
esp_err_t nc_fx_init(void *arena, size_t bytes);
// ② 掃 /data/pixel/effects/ 把找到的 .fxe 載入（此時 FAT 仍歸裝置）
int nc_fx_scan(const char *dir);

// render task 會跑在哪一核（module 宣告的 core 必須跟它一致，除非寫 -1）。
// 在 nc_fx_init 之前呼叫；main 的 fx 模組用這個介面固定排程。
void nc_fx_set_task_placement(int core, int prio);
int  nc_fx_task_core(void);
// 目前 live 模組的幀週期（ms）；沒有 manifest 就回預設 10 ms。
uint32_t nc_fx_period_ms(void);

// ── 實例操作 ──
esp_err_t nc_fx_load(const char *path);
esp_err_t nc_fx_select(const char *name);      // NULL = 停止
esp_err_t nc_fx_unload(const char *name);
nc_fx_t  *nc_fx_live(void);
// 這個模組是 wasm 還是 aot（NC_FX_FMT_*）；WAMR backend 用它決定 import 路徑
uint8_t   nc_fx_module_format(const nc_fx_t *fx);
// 這顆晶片吃的 AOT target（NC_FX_TARGET_*）；0 = 未知（只收 wasm）
int       nc_fx_arch_target(void);

// 解碼後的宣告（給狀態與診斷看；host 測試也會用）
typedef struct {
    bool     has_manifest;
    uint8_t  kind;         // NC_FXM_KIND_*
    int      core;         // -1 = 不限
    int      layer;
    int      prio;
    uint32_t stack;
    uint32_t fps;
    uint32_t px;
    uint32_t budget_us;
    uint32_t mem_bytes;
    int      ncmds;
} nc_fx_decl_t;
bool nc_fx_decl(const nc_fx_t *fx, nc_fx_decl_t *out);
bool nc_fx_cmd_at(const nc_fx_t *fx, int i, uint16_t *cmd);

// ── 執行引擎插槽（host 測試或之後的 WAMR）──
esp_err_t nc_fx_set_backend(const nc_fx_backend_t *backend);

// ── task ──
void nc_fx_task(void *arg);          // FreeRTOS 迴圈（host 測試不編）
uint32_t nc_fx_tick(void);           // 一幀：退休 + render + 預算；host 測試直接呼叫
uint32_t nc_fx_frame_id(void);

// ── 狀態 / 診斷 ──
// info：物件本身 {"arena":…}（給 provide 用）
// status：加上 "fx": 前置（給 STATUS_GET 用）
size_t nc_fx_info_json(char *buf, size_t cap);
size_t nc_fx_status_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
