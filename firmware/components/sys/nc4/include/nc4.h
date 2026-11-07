// NC4 解碼器（傳輸無關）
//
// 架構（與 mp_Net-Core 的 BusSources → bus_decode → dispatch 同形）：
//
//   transport A ─┐
//   transport B ─┼─► nc4_parser_feed() ─► 解碼 ─► dispatch 表 ─► handler
//   transport C ─┘        （每個通道一個 parser）      （全域共用一個 decoder）
//
// 為什麼 parser 是「每個通道一個」而不是全域共用：
//   黏包／拆包的狀態屬於**單一連線**。兩個通道的半個幀混在同一個緩衝裡，
//   重同步會互相污染 —— MP 版也是每個 bus 各一個 StreamParser，共用同一個 Dispatcher。
//
// 解碼器不知道 bytes 從哪來；來源只透過 ctx 傳給 handler（logging / 回覆用）。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC4_SOF_0            0x4E   // 'N'
#define NC4_SOF_1            0x43   // 'C'
#define NC4_VERSION          4
#define NC4_HDR_LEN          9      // SOF(2) + VER(1) + ADDR(2) + CMD(2) + LEN(2)
#define NC4_CRC_LEN          4      // CRC32：覆蓋 VER..DATA，不含 SOF
// 單幀 DATA 上限。**經驗參數（magic number），不是協議限制** ——
// 協議的 LEN 是 uint16，最大 65535。8192 = 上游實測甜蜜點再加少少出界餘量。
// 它的作用是配 parser 緩衝（NC4_MAX_FRAME = 8205）。改它要同步上游 proto.py 的 MAX_PAYLOAD。
#define NC4_MAX_PAYLOAD      8192
#define NC4_MAX_FRAME        (NC4_HDR_LEN + NC4_MAX_PAYLOAD + NC4_CRC_LEN)
// 指令表的格數。**要對得起實際註冊量**：core 5 + test 11 + test bench 9 +
// ota 9 + file 2 = 36（測試映像）。曾經是 32 → 最後 4 條靜靜咁註冊失敗
// （STATUS_SET / FILE_QUERY 永遠答不到）。塞爆會印 ESP_LOGE（見 nc4.c）。
// 回覆幀的 JSON 上限。完整的 STATUS_GET（sys+mem+tasks+provides+storage+fx）
// 大約 2.4 KB，用小了會**靜靜截斷成無效 JSON**。
// 這塊緩衝由 decoder 持有（每個 decoder 一份，預設放 PSRAM），**不再放任務棧**：
// 以前它是 handler 的區域變數，一個 decode 任務的 5,120 B 棧裡面有 2,615 B 就是它。
#define NC4_REPLY_JSON_MAX   2600
#define NC4_REPLY_FRAME_MAX  (NC4_HDR_LEN + 2 + NC4_REPLY_JSON_MAX + NC4_CRC_LEN)
#define NC4_MAX_HANDLERS     48
#define NC4_ADDR_BROADCAST   0xFFFF

// ── 指令碼（與 slave/schema/*.json 對齊）──
#define NC4_CMD_REBOOT             0x100F
#define NC4_CMD_STATUS_GET         0x1101
#define NC4_CMD_STATUS_RSP         0x1102
// 0x1019 是空號，保留給 ASSET_RELOAD（見 docs/01_protocol/commands.md §3）。
// 它一度被定義成 RESET_TO_DOWNLOAD，從未實作，已刪。

typedef struct {
    uint16_t addr;
    uint16_t cmd;
    const uint8_t *data;   // payload 視圖（handler 返回後失效）
    uint16_t len;
} nc4_frame_t;

typedef struct nc4_decoder nc4_decoder_t;

// 回覆函式：transport 提供，負責把已打包好的整幀送出去
typedef esp_err_t (*nc4_send_fn)(void *arg, const uint8_t *frame, size_t len);

typedef struct {
    nc4_decoder_t *decoder;
    nc4_send_fn send;      // 可為 NULL（純接收的通道）
    void *send_arg;
    const char *source;    // "CDC" / "UART" / "WS" …
} nc4_ctx_t;

typedef void (*nc4_handler_t)(nc4_ctx_t *ctx, const nc4_frame_t *frame);

typedef struct nc4_parser nc4_parser_t;

// ── decoder：指令表（全域一個）──
nc4_decoder_t *nc4_decoder_create(void);
void nc4_decoder_destroy(nc4_decoder_t *decoder);

// 指令表是**靜態**的：新增指令請寫在模組的 const 表（見 nc_action.h），
// 由 nc_action_init() 在開機時統計＋一次配置。
// 以前有 nc4_decoder_on() / on_owner() / off_owner() 三個執行期註冊 API ——
// 已刪：留著只會讓人以為可以註冊，而實際上行不通（靜靜失敗正是之前最貴的 bug）。

// 給帳本／狀態用：目前註冊了幾條、第 i 條是誰的、某條 cmd 有沒有主人
size_t nc4_decoder_count(const nc4_decoder_t *decoder);
bool nc4_decoder_at(const nc4_decoder_t *decoder, size_t i, uint16_t *cmd,
                    const char **owner, nc4_handler_t *handler);
bool nc4_decoder_has(const nc4_decoder_t *decoder, uint16_t cmd);

// ── 位址模型（與 mp 版 §5 一致）──
// cid        我自己（協議層 2B）；未設定 = 0xFFFF = 什麼都收
// master_cid 我的上級；回覆預設填它，未設定 = 0xFFFF（廣播）
void nc4_decoder_set_cid(nc4_decoder_t *decoder, uint16_t cid);
uint16_t nc4_decoder_cid(const nc4_decoder_t *decoder);
void nc4_decoder_set_master_cid(nc4_decoder_t *decoder, uint16_t master_cid);
uint16_t nc4_decoder_master_cid(const nc4_decoder_t *decoder);

// 回覆位址：有 master_cid 用 master_cid，否則廣播
uint16_t nc4_reply_addr(const nc4_ctx_t *ctx);

// 這個 decoder 的回覆幀緩衝（NC4_REPLY_FRAME_MAX B，建立時配置好）。
// 由 decoder 獨佔：同一個 decoder 一次只跑一個 handler（有鎖），所以安全。
uint8_t *nc4_decoder_reply_buf(nc4_decoder_t *decoder);

// ── parser：每個通道一個 ──
// max_frame = 這條通道的 parser 緩衝大小；0 → NC4_MAX_FRAME
nc4_parser_t *nc4_parser_create(nc4_decoder_t *decoder, nc4_send_fn send, void *send_arg,
                                const char *source, size_t max_frame);

// 在「外部已配好的區域」上建 parser（plan 給的記憶體走這條）
nc4_parser_t *nc4_parser_create_in(void *region, size_t region_bytes,
                                   nc4_decoder_t *decoder, nc4_send_fn send,
                                   void *send_arg, const char *source);
void nc4_parser_destroy(nc4_parser_t *parser);

// 丟入任意長度的位元組流；處理黏包／拆包／錯位重同步
esp_err_t nc4_parser_feed(nc4_parser_t *parser, const uint8_t *data, size_t len);

// ── 零複製（frame-aligned）──
// 看 9 B 表頭判斷整幀長度；不是合法表頭（SOF/VER/LEN）回 0。
// transport 可以「先 peek 表頭、再 recv 剛好一整幀」，於是 parser 不必有 ring。
size_t nc4_header_total(const uint8_t *hdr);

// 直接解析一個完整 frame：buf 必須從 SOF 開始，len == 13 + LEN。
// 不複製、不進 ring；handler 執行期間 frame.data 指向 buf+9，回傳後失效。
// 回傳 ESP_OK / ESP_ERR_INVALID_SIZE（不是一幀）/ ESP_ERR_INVALID_CRC。
esp_err_t nc4_parser_frame(nc4_parser_t *parser, const uint8_t *buf, size_t len);

// 清掉「半個 frame」的殘留。
// TCP 斷線時若正好停在一幀中間，parser 會留著那個殘幀等後續 bytes；
// 下一條連線的資料會被接在它後面，於是小指令全被吞掉（看起來像「卡住」）。
// 新連線 = 新位元組流 → 這條一定要呼叫。
void nc4_parser_reset(nc4_parser_t *parser);

// 壞幀統計（指標可為 NULL）。以前 CRC 錯了只會靜靜前移 1 byte，
// 外面完全看不出來 —— 現在至少數得到。
//   crc_err：CRC32 對不上（雜訊 / 撕裂 / 對端算錯）
//   bad_hdr：VER 不對或 LEN 超上限（多半是根本沒對上 SOF）
void nc4_parser_errors(const nc4_parser_t *parser, uint32_t *crc_err, uint32_t *bad_hdr);

// 尺寸量測：這條線收過的最大 payload（max_dlen 含被丟棄的超大幀，max_ok 只算成功 dispatch 的）。
// 用途：parser region = 13 + max_dlen + 餘裕 —— 用實測取代「先給 8 KB」。
void nc4_parser_peaks(const nc4_parser_t *parser, uint32_t *max_dlen, uint32_t *max_ok);

// ── 打包（給 transport 當 send 用；也可給 handler 產生回覆）──
// 回傳實際長度；容量不足回 -1（不寫、不 raise）
int nc4_pack_into(uint8_t *buf, size_t cap, uint16_t cmd, uint16_t addr,
                  const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
