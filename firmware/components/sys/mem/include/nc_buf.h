// 緩衝區（nc_buf）—— 全項目唯一一個緩衝工具
//
// 一個 buffer = N 個等長「格」＋ 每格一個 state。設計與用法：
//   docs/03_notes/buffers.md          為什麼只有一個工具、三條鐵律、S/P 四組合
//   docs/03_notes/buffer-patterns.md  十個用法模式與 16 條小技巧
//
// 這裡只做「機制」，唔做「語意」：
//   · 格的大小、數量、位置由建立時定死（開機一次要齊）
//   · 每格記 off（起點，用於對齊／你自己的表頭）＋ len（實際用幾多）
//   · 兩個模式：FIFO（嚴格先入先出、單寫單讀）／隨機（任意空格）
//   · 換手（handover）：記憶體一動不動，只換 owner 與 generation
//
// **冇 destroy**：冇 free 就冇碎片。要「重用」就 nc_buf_handover()，
// 前提係所有格都回到 FREE。
//
// 執行緒：FIFO 模式是單寫單讀（SPSC）。多個 writer 要自己序列化（mutex）。
// 熱路徑只做 put/get，唔做配置；建立與換手只可以開機或換場時做。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_BUF_MAX        32      // 同時最多幾條 buffer（registry 滿 → 回 NULL，唔靜默）
#define NC_BUF_NAME_MAX   16
#define NC_BUF_ALIGN      64      // cache line：DMA／PSRAM 都夠

// 格放哪裡（決定由哪個 heap 要，以及能否 DMA）
typedef enum {
    NC_BUF_SRAM = 0,   // internal
    NC_BUF_PSRAM,      // PSRAM（大而冷；S3 的 I80 DMA 讀唔到）
    NC_BUF_DMA,        // internal 且 DMA 可用
} nc_buf_where_t;

typedef enum {
    NC_BUF_ST_FREE = 0,
    NC_BUF_ST_WRITING,   // 有人揸住寫
    NC_BUF_ST_READY,     // 寫完，等 consumer
    NC_BUF_ST_READING,   // consumer 揸住讀
} nc_buf_state_t;

// 形狀只有兩種：預設 FIFO（嚴格先入先出、單寫單讀），或 STREAM（見下）。
// **冇「隨機／任意空格」模式**：冇真實用戶，而且它要多一個 WRITING 狀態。
#define NC_BUF_F_FIFO  (1u << 0)   // 保留給明確標注；預設已經係 FIFO
// STREAM：唔係一格一格，係一條連續位元組流（解析器環、位元組流水）。
// 用 slot_bytes 當「環容量」，n 必須係 1。唔支援 put/get（格 API）。
// 預設係「線性＋必要時 memmove 壓實」：read_ptr 永遠係一段連續。
#define NC_BUF_F_STREAM (1u << 1)
// RING（要同時帶 STREAM）：真正環形，in/out 單調計數，**唔會壓實／搬資料**。
// SPSC 契約：producer 只寫 in／wr 側，consumer 只寫 out／rd 側；任何一邊
// **唔會 reset** 對方嘅計數（計數可以自然 wrap，`st_in - st_out` 仍然正確）。
// consumer 揸住 out 側的指標時 producer 可以繼續寫，資料唔會被移動。
// read_ptr 只回第一段；跨環請用 nc_buf_read_segments()（兩段）。
// 量測（2026-11-14，S3）：backlog 一半時，線性版 17.6 MB/s、ring 346 MB/s。
#define NC_BUF_F_RING   (1u << 3)
// RUNTIME：執行期建立（例如 parser 隨通道開關）。只有帶這個 flag 的 buffer
// 可以用 nc_buf_release() 還返；開機建立的一律唔可以（鐵律 R2）。
#define NC_BUF_F_RUNTIME (1u << 2)

typedef struct nc_buf nc_buf_t;

// ── 建立（開機一次要齊；失敗回 NULL 並印 log）──
nc_buf_t *nc_buf_create(const char *name, nc_buf_where_t where,
                        uint32_t slot_bytes, uint16_t n, uint32_t flags);

// 在「外部已配好的區域」上建立（plan／arena 給的記憶體走這條）
nc_buf_t *nc_buf_at(const char *name, void *region, size_t region_bytes,
                    nc_buf_where_t where, uint32_t slot_bytes, uint16_t n,
                    uint32_t flags);

// ── 寫（FIFO 模式：只能按次序寫，唔可以跳格）──
// reserve = 格首留幾多 bytes 給你自己放表頭（例如 frame id）。
// 回傳的指標已經跳過 reserve；放唔落 → NULL（唔會等待）。
void     *nc_buf_put_begin(nc_buf_t *b, uint32_t reserve);
// 一次過寫入（自動處理跨空間 copy，並記 s2s/s2p/p2s/p2p 帳）
esp_err_t nc_buf_put(nc_buf_t *b, const void *src, uint32_t len);
// 寫完：記實際用量並發佈
esp_err_t nc_buf_put_end(nc_buf_t *b, uint32_t len);

// ── 讀（非阻塞；冇資料 → NULL）──
// len / off 可選：off 是這一格的資料起點（你放表頭的偏移）
void     *nc_buf_get(nc_buf_t *b, uint32_t *len, uint32_t *off);
esp_err_t nc_buf_get_end(nc_buf_t *b);      // 用完還格

// 放棄正在寫的那一格（producer 中途失敗、或長度超限）。
// FIFO 模式下唔會有「卡住的格」（見 nc_buf.c 的說明），這個呼叫主要給隨機模式。
esp_err_t nc_buf_put_abort(nc_buf_t *b);

// ── 串流（NC_BUF_F_STREAM）：連續位元組，可以部分寫／部分讀 ──
// 回實際寫入的 bytes：空間唔夠會**部分寫入**（並計 fail_full），唔會覆蓋未讀資料。
uint32_t nc_buf_write(nc_buf_t *b, const void *src, uint32_t len);
// 看得到的那一段（線性模式一定連續；RING 模式只回第一段）。
const uint8_t *nc_buf_read_ptr(nc_buf_t *b, uint32_t *len);
// RING／線性通用：回傳最多兩段（p0/n0 一定有；p1 可以 NULL）。
// 回傳值 = 總 pending bytes。線性模式的 n1 永遠 0。
uint32_t nc_buf_read_segments(nc_buf_t *b, const uint8_t **p0, uint32_t *n0,
                              const uint8_t **p1, uint32_t *n1);
// 消費 n bytes（重同步 = nc_buf_skip(b, 1)）。n 大過可讀量就全部消費。
esp_err_t nc_buf_skip(nc_buf_t *b, uint32_t n);
uint32_t nc_buf_pending(const nc_buf_t *b);   // 已寫未讀
uint32_t nc_buf_space(const nc_buf_t *b);     // 仲可以寫幾多（壓實之後）

// ── 切一塊（執行期用；呼叫端自己保管 cursor）──
// 在 [region, region+bytes) 內由 *cursor 起切一條 buffer，cursor 自動前進（64 B 對齊）。
// 唔夠位 → 回 NULL（唔會部分配置）。這是 `.fxe` 換場時由 arena 切記憶體的唯一入口。
nc_buf_t *nc_buf_carve(void *region, size_t bytes, size_t *cursor,
                       const char *name, nc_buf_where_t where,
                       uint32_t slot_bytes, uint16_t n, uint32_t flags);

// ── 換手：記憶體一動不動，只換 owner 與 generation ──
// 前提：所有格都 FREE。否則回 ESP_ERR_INVALID_STATE，violations++。
esp_err_t nc_buf_handover(nc_buf_t *b, const char *new_owner);

// ── 查詢 ──
const char *nc_buf_name(const nc_buf_t *b);
const char *nc_buf_owner(const nc_buf_t *b);
uint32_t     nc_buf_slot_bytes(const nc_buf_t *b);
uint16_t     nc_buf_slots(const nc_buf_t *b);
size_t       nc_buf_fill(const nc_buf_t *b);       // READY 的格數
uint32_t     nc_buf_generation(const nc_buf_t *b);
// 診斷：某一格的狀態、有冇 producer／consumer 揸住（STATUS 用）
uint8_t      nc_buf_state_at(const nc_buf_t *b, uint16_t idx);
bool         nc_buf_writing(const nc_buf_t *b);
bool         nc_buf_reading(const nc_buf_t *b);
void        *nc_buf_raw(nc_buf_t *b, uint16_t idx); // 直接拿一格（DMA 用，唔經狀態機）
nc_buf_t    *nc_buf_find(const char *name);

// 全部 buffer 的報表（JSON 物件本體，給 STATUS keys=mem 的 "buf"）
size_t nc_buf_json(char *out, size_t cap);

// ★ 只可以釋放「執行期建立」的 buffer（帶 NC_BUF_F_RUNTIME）。
// 開機建立的回 ESP_ERR_INVALID_STATE（鐵律 R2：冇 free 就冇碎片）。
// 外部區域（nc_buf_at）只會除名，唔會 free；自己配置的（nc_buf_create）才會還返 heap。
esp_err_t nc_buf_release(nc_buf_t *b);

// 清空（測試用：釋放全部）
void   nc_buf_reset(void);
size_t nc_buf_count(void);
size_t nc_buf_total_bytes(void);

#ifdef __cplusplus
}
#endif
