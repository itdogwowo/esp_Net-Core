// log 管道：把兩個核的 ESP_LOG 收進各自的 ring，由**一個** consumer 統一輸出。
//
// 為什麼每核一個 ring：
//   · 生產者永遠只寫自己那個 ring → 保護只需要「本核」的 critical section，
//     另一核永遠不會碰同一個 mux（唔會跨核自旋、唔會有 priority inversion）。
//   · 消費者只有一個，按 t_us 合併兩邊，輸出順序接近真實時間。
//
// 為什麼 consumer 釘在 CDC 核：
//   輸出要走 CDC 出口，而 CDC 出口只有一個 writer 在跑（見 main.c 的 arbiter）。
//
// 生命週期（**兩段**，分開是有原因的）：
//   ① nc_log_capture()  app_main 一開頭就呼叫：只裝 hook，只寫 ring。
//      為什麼要咁早：S3 的 USB-Serial-JTAG 同 OTG 共用 PHY，usb_init() 之後
//      JTAG 的 port 會消失，之前的 log 主機永遠收唔到 → 一定要先抄入 RAM。
//   ② nc_log_start()    usb_init() 之後才呼叫：建 consumer、接 CDC 出口。
//      ring 滿了會丟新行，所以留下的是**最早期**那幾行（正是開機最想看的）。
//
// 這一版刻意很小：
//   · 每核 NC_LOG_SLOTS 格 × 176 B；格數可逐級調（先量，唔好盲加）
//   · 唔做 history ring：ring 只係 backlog；滿了就丟（有計數）
//   · 唔做續行拼合：一行超過 LINE_MAX 就截斷 + flag
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_LOG_LINE_MAX  160     // 一行文字上限（唔含 NUL）
// 環的大小（bytes，**只存文字**＋4 B header，可變長）。
//   量出嚟：開機 burst ≈ 75–106 行、平均 57 B/行 → 加 4 B header ≈ 61 B/行。
//   8 KB 可裝 ~130 行；2 KB（簡模式）約 30 行，夠看到服務／bus 啟動。
//   時間戳同 seq 都唔存（順序差一行無所謂），所以 header 由 10 B 壓到 4 B。
// core1 開機只有 4–5 行，唔需要大環。
#ifndef NC_LOG_BYTES_0
#define NC_LOG_BYTES_0   8192    // core0（app_main / decode0 / CDC 組）
#endif
#ifndef NC_LOG_BYTES_1
#define NC_LOG_BYTES_1   1024    // core1（decode1 / UART producer）
#endif

// 合批：一次 CDC write 最多帶幾多行／幾多 bytes。
// 每行平均 57 B，8 行 ≈ 456 B；再大就要兩三個 USB 包，收益遞減。
#ifndef NC_LOG_BATCH_LINES
#define NC_LOG_BATCH_LINES 8
#endif
#ifndef NC_LOG_BATCH_BYTES
#define NC_LOG_BATCH_BYTES 512
#endif
#define NC_LOG_PREFIX    '\x1e'  // 每行輸出前綴（RS）：主機用來快速跳過 log 行

// 出口的三種結果（要分清楚，因為處理方式完全不同）：
//   OK      = 寫入成功
//   BUSY    = 暫時寫唔到（回覆正在用出口／FIFO 滿）→ 稍後再試
//   OFFLINE = 主機未開這個 port → **唔好消費 ring**，否則那些行會白白掉走
//             （實測：port 關著時寫入 CDC，主機之後永遠收唔到那些 bytes）
typedef enum {
    NC_LOG_TX_OK = 0,
    NC_LOG_TX_BUSY,
    NC_LOG_TX_OFFLINE,
} nc_log_tx_result_t;

typedef nc_log_tx_result_t (*nc_log_tx_fn)(const char *buf, size_t len);

// 等級位（IDF v1 的 format 第一個字元就是等級字母）：過濾在 hook 最前面做，
// 被過濾的行連 vsnprintf 都唔會跑。
#define NC_LOG_LVL_ERROR    0x01u
#define NC_LOG_LVL_WARN     0x02u
#define NC_LOG_LVL_INFO     0x04u
#define NC_LOG_LVL_DEBUG    0x08u
#define NC_LOG_LVL_VERBOSE  0x10u
#define NC_LOG_LVL_ALL      0x1Fu
#define NC_LOG_LVL_DEFAULT  (NC_LOG_LVL_ERROR | NC_LOG_LVL_WARN | NC_LOG_LVL_INFO)

// ── 只有兩個級別（已經實測：7 KB 同 8 KB 冇分別，所以合併）──
//   0 = 簡：error/warn/info、2 KB（core1 512 B）、USB 起來之後才裝 hook（只記開機之後）
//   1 = 詳：全部等級（含 DEBUG/VERBOSE）、8 KB（core1 1 KB）、由 app_main 第一行開始記
#define NC_LOG_LEVELS_QUIET 0
#define NC_LOG_LEVELS_FULL  1

// 設定存在 NVS（boot/log）：開機最早期就要讀（FAT 未掛）。
// 由 config 的 system.json "log_debug_mode" 覆寫；即時生效（必要時遷移環），並寫回 NVS。
esp_err_t nc_log_set_levels(uint8_t levels);
void nc_log_get_levels(uint8_t *levels);

// ① 只裝 hook（最早呼叫）。之後所有 ESP_LOG 會寫入 RAM ring，等 consumer 起來才送。
esp_err_t nc_log_capture(void);

// ② 建立 consumer（釘 core）＋接上出口。core 應該是 CDC 所屬核。
//    會順便確保 hook 已安裝（idempotent）。
esp_err_t nc_log_start(int core, nc_log_tx_fn tx);

// 還原 hook（測試用）
void nc_log_stop(void);

// STATUS 用：
// {"cap":1,"run":1,"core":0,"slots":8,"pending":[0,1],"produced":[12,4],
//  "sent":20,"dropped":33,"busy":0,"offline":2}
void nc_log_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
