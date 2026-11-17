// 模組載入器（ncm）—— 三層裡的中間那層：政策 + 實例表。
//
// 三層（見 docs/03_notes/ncm-loader.md §10）：
//   wasm   runtime 能力（init / instantiate）      boot 內單例
//   ncm    政策：arena + live 指標 + 看門狗 + task  boot → 可停   ← 這支
//   module 一個模組                                 載入 → 卸載  ← 實例表，不是服務
//
// 為什麼 module 不是服務：服務註冊表是 boot-time 扁平清單、沒有卸載語義；
//   模組是 runtime 產生的，50 個模組 = 50 個服務項。註冊表存 ID，實例隨時增減
//   （同 rx-architecture.md 的結論）。
//
// 檔案系統就是模組的註冊表：/data/pixel/effects/*.nce、/data/tasks/*.nct
// 就是「有哪些模組」。
//
// v1：每個 .nct / .nce 可以帶 manifest（nc_ncd.h）宣告「我是什麼、跑哪一核、
//     要多少記憶體」。載入時解碼 → 驗證 → 用 nc_task_attach() 登記成租戶。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

// arena 上限（從 nc_plan 要；PSRAM 優先）。載入時再依模組的 mem_bytes 切。
#define NC_MOD_ARENA_MAX   (512 * 1024)
#define NC_MOD_MAX_MODULES 8
#define NC_MOD_NAME_MAX    24
#define NC_MOD_PATH_MAX    80
#define NC_MOD_MAX_CMDS    8      // manifest 最多宣告幾條指令（模組層另有上限）

// 執行引擎插槽。上層（loader／task／狀態）唔識 WAMR，只識這張表；
// 引擎換掉（WAMR／stub／host 測試）唔需要改上層。
typedef struct nc_mod nc_mod_t;

typedef struct {
    const char *name;
    // 一次初始化（boot 內單例）。arena 係 plan 給的 payload 儲存區，
    // 引擎可以自己再要 heap（WAMR 用 custom allocator）。
    esp_err_t (*init)(void *arena, size_t bytes);
    // 可選：模組 payload 已就緒（驗證／CRC 都過了）→ 載入並實例化。
    // 回非 0 ＝ 這個模組不能跑（loader 會標 bad，但仍然留在表裡給人查）。
    esp_err_t (*load)(nc_mod_t *ncm);
    // 每次 tick 呼叫（效果＝一幀；任務＝一次 task_tick）。回非 0 → 標 bad。
    esp_err_t (*render)(nc_mod_t *ncm, uint32_t frame_id);
    // 可選：卸載前回收（destroy env／deinstantiate／unload）。
    void      (*unload)(nc_mod_t *ncm);
    void      (*deinit)(void);
} nc_mod_backend_t;

// ── 生命週期 ──
// ① 初始化（commit 之後、task start 之前）。arena 由 plan 給。
esp_err_t nc_mod_init(void *arena, size_t bytes);
// ② 掃目錄把找到的 .nct / .nce 載入（此時 FAT 仍歸裝置）
int nc_mod_scan(const char *dir);

// render task 會跑在哪一核（module 宣告的 core 必須跟它一致，除非寫 -1）。
// 在 nc_mod_init 之前呼叫；main 的 ncm 模組用這個介面固定排程。
void nc_mod_set_task_placement(int core, int prio);
int  nc_mod_task_core(void);
// 目前 live 模組的幀週期（ms）；沒有 manifest 就回預設 10 ms。
uint32_t nc_mod_period_ms(void);

// ── 實例操作 ──
esp_err_t nc_mod_load(const char *path);
esp_err_t nc_mod_select(const char *name);      // NULL = 停止
esp_err_t nc_mod_unload(const char *name);
nc_mod_t  *nc_mod_live(void);
// 這個模組是 wasm 還是 aot（NC_MOD_FMT_*）；WAMR backend 用它決定 import 路徑
uint8_t   nc_mod_module_format(const nc_mod_t *ncm);
// 給 backend 用：模組名字（log 用）＋ payload 指標（loader 已由 arena 複製好）
const char    *nc_mod_name(const nc_mod_t *ncm);
const uint8_t *nc_mod_payload(const nc_mod_t *ncm, size_t *size);
// 這顆晶片吃的 AOT target（NC_MOD_TARGET_*）；0 = 未知（只收 wasm）
int       nc_mod_arch_target(void);

// 解碼後的宣告（給狀態與診斷看；host 測試也會用）
typedef struct {
    bool     has_manifest;
    uint8_t  kind;         // NC_NCD_KIND_*
    int      core;         // -1 = 不限
    int      layer;
    int      prio;
    uint32_t stack;
    uint32_t fps;
    uint32_t px;
    uint32_t budget_us;
    uint32_t mem_bytes;
    int      ncmds;
} nc_mod_decl_t;
bool nc_mod_decl(const nc_mod_t *ncm, nc_mod_decl_t *out);
bool nc_mod_cmd_at(const nc_mod_t *ncm, int i, uint16_t *cmd);

// ── 掃描階段：只讀檔頭 + manifest，不碰 payload / arena ──
// 這是「先掃描、再讀 tasks.json、合併、最後啟動」的那個掃描。
typedef struct {
    char     name[NC_MOD_NAME_MAX];
    uint8_t  fmt;          // NC_MOD_FMT_*
    uint32_t target_id;
    nc_mod_decl_t decl;
    uint16_t cmds[NC_MOD_MAX_CMDS];
    nc_meta_t meta;        // 自我描述（name / desc / usage / requires / provides）
} nc_mod_probe_t;
esp_err_t nc_mod_probe(const char *path, nc_mod_probe_t *out);

// ── 開機框架：掃描 → 合併 tasks.json → 載入 ──
// ① 掃描：只讀 header + manifest，不碰 payload / arena（B06 declare 之前）。
// ② 合併：對 tasks.json；缺的補 [0,0]，已有的套 level / affinity。
// ③ 載入：commit + start_all 之後，逐個載入 payload 並 attach。
#define NC_MOD_MAX_ENTRIES 16

typedef struct {
    char           path[NC_MOD_PATH_MAX];
    nc_mod_probe_t probe;
    int            level;      // 合併後：-1 = 停用
    int            aff[2];     // 合併後：[core0, core1]
    bool           enabled;    // level >= 0 && (aff[0] || aff[1])
} nc_mod_entry_t;

typedef struct {
    nc_mod_entry_t entries[NC_MOD_MAX_ENTRIES];
    int            count;
    int            bad;        // 被拒的檔案數
} nc_mod_scan_t;

// 掃一個目錄，append 到 out（不 reset）。回傳成功加入的數量。
int nc_mod_scan_dir(const char *dir, nc_mod_scan_t *out);
// 掃 /data/pixel/effects + /data/tasks，reset 後重新填。
int nc_mod_scan_all(nc_mod_scan_t *out);
// 把掃描結果合併進 tasks.json（只加不改），並把 level / affinity 寫回 entry。
int nc_mod_merge_cfg(nc_mod_scan_t *scan);
// 依掃描結果載入 payload（commit + start_all 之後）。
int nc_mod_load_all(const nc_mod_scan_t *scan);
// 把掃描結果的 provides / requires 加進 nc_res 表（B06，nc_res_build 之後）。
int nc_mod_declare_res(const nc_mod_scan_t *scan);
// 指定 level 的載入（nc_mod_load 是 level=5 的包裝）。
esp_err_t nc_mod_load_ex(const char *path, int level);

// ── 執行引擎插槽（host 測試或之後的 WAMR）──
esp_err_t nc_mod_set_backend(const nc_mod_backend_t *backend);

// ── task ──
void nc_mod_task(void *arg);          // FreeRTOS 迴圈（host 測試不編）
uint32_t nc_mod_tick(void);           // 一幀：退休 + render + 預算；host 測試直接呼叫
uint32_t nc_mod_frame_id(void);

// ── 狀態 / 診斷 ──
// info：物件本身 {"arena":…}（給 provide 用）
// status：加上 "ncm": 前置（給 STATUS_GET 用）
size_t nc_mod_info_json(char *buf, size_t cap);
size_t nc_mod_status_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
