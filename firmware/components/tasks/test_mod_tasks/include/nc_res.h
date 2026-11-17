// 外部任務資源表格（nc_res）—— 誰提供什麼、誰需要什麼。
//
// 三個來源：
//   ① 系統預設：/data/config/resources.json（缺檔 → 用 nc_cfg_defaults_resources()）
//   ② 模組 provides：.nct / .nce 的 meta 宣告「我提供什麼」
//   ③ 模組 requires：.nct / .nce 的 meta 宣告「我需要什麼」
//
// 開機順序（B06）：
//   nc_res_build()        ← 讀 resources.json，建 prov 表
//   nc_mod_declare_res()  ← 把掃描到的模組 provides/requires 加進來
//   nc_res_merge()        ← 標 present、解衝突
// 載入（B08）：
//   nc_res_check(name)    ← 逐條查 requires；缺就拒絕該模組
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_RES_MAX        32
#define NC_RES_NAME_MAX   24
#define NC_RES_OWNER_MAX  24

typedef enum {
    NC_RES_BUF = 0,     // 緩衝區（led.frame）
    NC_RES_CMD,         // 指令（0x3101）
    NC_RES_HW,          // 硬體（rmt / gpio）
    NC_RES_PROVIDE,     // 任務的 provide（pixel.out）
    NC_RES_MODULE,      // 另一個模組
} nc_res_type_t;

typedef struct {
    char          name[NC_RES_NAME_MAX];
    nc_res_type_t type;
    char          provider[NC_RES_OWNER_MAX];
    uint32_t      bytes;
    bool          exclusive;
    bool          present;
} nc_res_prov_t;

typedef struct {
    char          name[NC_RES_NAME_MAX];
    nc_res_type_t type;
    char          owner[NC_RES_OWNER_MAX];
    uint32_t      bytes;
    bool          exclusive;
} nc_res_req_t;

typedef struct {
    nc_res_prov_t provs[NC_RES_MAX];
    int           n_prov;
    nc_res_req_t  reqs[NC_RES_MAX];
    int           n_req;
} nc_res_table_t;

const char *nc_res_type_name(nc_res_type_t type);

void nc_res_reset(void);
int  nc_res_add_prov(const char *name, nc_res_type_t type, const char *provider,
                     uint32_t bytes, bool exclusive);
int  nc_res_add_req(const char *name, nc_res_type_t type, const char *owner,
                    uint32_t bytes, bool exclusive);
int  nc_res_build(void);   // 從 nc_cfg_resources() 建 prov 表
int  nc_res_merge(void);   // 標 present
const nc_res_prov_t *nc_res_find_prov(const char *name);
bool nc_res_check(const char *owner, char *why, size_t cap);
size_t nc_res_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
