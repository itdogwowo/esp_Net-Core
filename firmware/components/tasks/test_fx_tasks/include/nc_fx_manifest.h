// .fxe manifest（模組自我宣告）—— 純 C、無依賴，所以可以 host 測試
//
// 為什麼要有這東西：
//   檔頭（48 B）只回答「這個 payload 怎麼載入」（格式 / 大小 / crc / target）。
//   manifest 回答的是「這個模組是什麼、要怎麼活在系統裡」：
//     類型（task / service / effect）、跑哪一核、哪一層、優先權、
//     要多少線性記憶體、螢幕幾 px、每幀預算、擁有哪幾條指令。
//
// 分開放而不是塞進檔頭的理由：檔頭是「載入器契約」，改一次所有工具都要跟；
// manifest 是「模組契約」，可以獨立加欄位（version + flags + reserved）。
//
// 位置：檔頭的 manifest_off / manifest_size（相對檔案起點），flags bit1 設起來。
// 位元組序：little-endian（與檔頭一致）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_FXM_MAGIC      0x314D5846u   // 'F','X','M','1'
#define NC_FXM_VERSION    1
#define NC_FXM_HDR_SIZE   40

// 上限（超過 = 拒絕載入；不要讓 manifest 決定系統上限）
#define NC_FXM_MAX_PX       4096
#define NC_FXM_MAX_MEM      (128 * 1024)
#define NC_FXM_MAX_FPS      240
#define NC_FXM_MAX_CMDS     32
#define NC_FXM_MAX_STACK    (32 * 1024)
#define NC_FXM_MAX_BUDGET_US 100000

// kind：數值與 nc_task_kind_t 相同（nc_fx.c 有 static_assert 對齊）。
// system（0）一律拒絕：載入的檔案不能自稱「不可卸載」的那一層。
// 舊說法的 "service" / "task" 都是 NORMAL —— 兩者是同一件事。
#define NC_FXM_KIND_SYSTEM  0
#define NC_FXM_KIND_NORMAL  1
#define NC_FXM_KIND_EFFECT  2
#define NC_FXM_KIND_SERVICE NC_FXM_KIND_NORMAL
#define NC_FXM_KIND_TASK    NC_FXM_KIND_NORMAL

typedef enum {
    NC_FXM_OK = 0,
    NC_FXM_ERR_MAGIC,
    NC_FXM_ERR_VERSION,
    NC_FXM_ERR_KIND,
    NC_FXM_ERR_CORE,
    NC_FXM_ERR_LAYER,
    NC_FXM_ERR_PRIO,
    NC_FXM_ERR_STACK,
    NC_FXM_ERR_FPS,
    NC_FXM_ERR_PX,
    NC_FXM_ERR_MEM,
    NC_FXM_ERR_BUDGET,
    NC_FXM_ERR_CMDS,
} nc_fxm_err_t;

// 解碼後的視圖（host 端填給 nc_fx 用）
typedef struct {
    uint8_t  kind;        // NC_FXM_KIND_*
    int8_t   core;        // -1 = 不限；0 / 1
    uint8_t  layer;
    uint8_t  prio;
    uint16_t fps;
    uint16_t px;
    uint32_t stack;
    uint32_t budget_us;
    uint32_t mem_bytes;
    uint32_t cmds_off;    // 相對檔案起點的 u16 陣列；0 = 沒有
    uint16_t ncmds;
    uint16_t flags;
} nc_fxm_t;

const char *nc_fxm_err_name(nc_fxm_err_t err);

// 解析並驗證 manifest。buf 是 manifest 區塊的起點（不是整個檔案）。
// file_size 用於檢查 cmds_off（指令表必須落在檔案內）。
nc_fxm_err_t nc_fxm_parse(const void *buf, size_t len, uint32_t file_size, nc_fxm_t *out);

// 讀第 i 條指令（cmd 表在檔案裡，所以要傳整個檔案的起點與長度）
bool nc_fxm_cmd_at(const uint8_t *file, size_t file_size, const nc_fxm_t *m,
                   int i, uint16_t *cmd);

#ifdef __cplusplus
}
#endif
