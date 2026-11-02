#include "nc4.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// handler 的互斥**屬於 decoder，不是全域**：
//   一個 decoder 一次執行一個 handler；多個 decoder（每核一個）可以同時各跑一個。
// 這是「一個解碼器執行一個指令」的模型，也是 Network.nc4.decode[] 那張表的意思。
// ⚠️ 要開第二個 decoder 之前，handler 必須是 reentrant（不能共寫 static 回覆緩衝）。
static const char *TAG = "nc4";

typedef struct {
    uint16_t cmd;
    nc4_handler_t handler;
    const char *owner;     // 誰註冊的（模組名）；NULL = 核心或舊式註冊
} nc4_entry_t;

struct nc4_decoder {
    nc4_entry_t table[NC4_MAX_HANDLERS];
    size_t count;
    SemaphoreHandle_t lock;   // 這個 decoder 的執行鎖（一個 decoder 一次一個 handler）
    uint16_t cid;          // 我自己（0xFFFF = 未設定）
    uint16_t master_cid;   // 我的上級（0xFFFF = 未設定）
    uint32_t dropped_addr; // 丟掉的「不是給我的」幀
};

static inline void decoder_lock(nc4_decoder_t *d)
{
    if (d != NULL && d->lock != NULL) {
        xSemaphoreTake(d->lock, portMAX_DELAY);
    }
}

static inline void decoder_unlock(nc4_decoder_t *d)
{
    if (d != NULL && d->lock != NULL) {
        xSemaphoreGive(d->lock);
    }
}

struct nc4_parser {
    uint8_t *buf;
    bool external;      // true = buf 由外部提供，destroy 不 free
    size_t cap;
    size_t start;
    size_t end;
    nc4_ctx_t ctx;
};

// ───────────────────────── decoder ─────────────────────────

nc4_decoder_t *nc4_decoder_create(void)
{
    nc4_decoder_t *d = calloc(1, sizeof(nc4_decoder_t));
    if (d == NULL) {
        return NULL;
    }
    d->cid = NC4_ADDR_BROADCAST;
    d->master_cid = NC4_ADDR_BROADCAST;
    d->lock = xSemaphoreCreateMutex();
    if (d->lock == NULL) {
        free(d);
        return NULL;
    }
    return d;
}

void nc4_decoder_set_cid(nc4_decoder_t *decoder, uint16_t cid)
{
    if (decoder != NULL) {
        decoder->cid = cid;
    }
}

uint16_t nc4_decoder_cid(const nc4_decoder_t *decoder)
{
    return decoder ? decoder->cid : NC4_ADDR_BROADCAST;
}

void nc4_decoder_set_master_cid(nc4_decoder_t *decoder, uint16_t master_cid)
{
    if (decoder != NULL) {
        decoder->master_cid = master_cid;
    }
}

uint16_t nc4_decoder_master_cid(const nc4_decoder_t *decoder)
{
    return decoder ? decoder->master_cid : NC4_ADDR_BROADCAST;
}

uint16_t nc4_reply_addr(const nc4_ctx_t *ctx)
{
    if (ctx == NULL || ctx->decoder == NULL) {
        return NC4_ADDR_BROADCAST;
    }
    return ctx->decoder->master_cid;
}

void nc4_decoder_destroy(nc4_decoder_t *decoder)
{
    if (decoder == NULL) {
        return;
    }
    if (decoder->lock != NULL) {
        vSemaphoreDelete(decoder->lock);
    }
    free(decoder);
}

esp_err_t nc4_decoder_on(nc4_decoder_t *decoder, uint16_t cmd, nc4_handler_t handler)
{
    return nc4_decoder_on_owner(decoder, cmd, handler, NULL);
}

// owner 是「誰擁有這條指令」：模組卸載時用它整批收回（見 nc4_decoder_off_owner）。
// 同名 owner 重複註冊同一條 cmd = 覆寫（後載入者勝）；不同 owner 蓋掉別人的 cmd
// 會印警告 —— 那是帳本該在開機就擋下的衝突，不該到執行期才悄悄發生。
esp_err_t nc4_decoder_on_owner(nc4_decoder_t *decoder, uint16_t cmd, nc4_handler_t handler,
                               const char *owner)
{
    if (decoder == NULL || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < decoder->count; i++) {
        if (decoder->table[i].cmd == cmd) {
            const char *prev = decoder->table[i].owner;
            if (owner != NULL && prev != NULL && strcmp(owner, prev) != 0) {
                ESP_LOGW(TAG, "cmd 0x%04X 被 %s 覆寫（原本是 %s）",
                         (unsigned)cmd, owner, prev);
            }
            decoder->table[i].handler = handler;
            decoder->table[i].owner = owner;
            return ESP_OK;
        }
    }
    if (decoder->count >= NC4_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    decoder->table[decoder->count++] = (nc4_entry_t){
        .cmd = cmd, .handler = handler, .owner = owner,
    };
    return ESP_OK;
}

int nc4_decoder_off_owner(nc4_decoder_t *decoder, const char *owner)
{
    if (decoder == NULL || owner == NULL) {
        return 0;
    }
    int removed = 0;
    for (size_t i = 0; i < decoder->count;) {
        const char *o = decoder->table[i].owner;
        if (o != NULL && strcmp(o, owner) == 0) {
            memmove(&decoder->table[i], &decoder->table[i + 1],
                    (decoder->count - i - 1) * sizeof(decoder->table[0]));
            decoder->count--;
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

size_t nc4_decoder_count(const nc4_decoder_t *decoder)
{
    return decoder ? decoder->count : 0;
}

bool nc4_decoder_at(const nc4_decoder_t *decoder, size_t i, uint16_t *cmd,
                    const char **owner, nc4_handler_t *handler)
{
    if (decoder == NULL || i >= decoder->count) {
        return false;
    }
    if (cmd != NULL) {
        *cmd = decoder->table[i].cmd;
    }
    if (owner != NULL) {
        *owner = decoder->table[i].owner;
    }
    if (handler != NULL) {
        *handler = decoder->table[i].handler;
    }
    return true;
}

bool nc4_decoder_has(const nc4_decoder_t *decoder, uint16_t cmd)
{
    if (decoder == NULL) {
        return false;
    }
    for (size_t i = 0; i < decoder->count; i++) {
        if (decoder->table[i].cmd == cmd) {
            return true;
        }
    }
    return false;
}

static nc4_handler_t decoder_lookup(nc4_decoder_t *decoder, uint16_t cmd)
{
    for (size_t i = 0; i < decoder->count; i++) {
        if (decoder->table[i].cmd == cmd) {
            return decoder->table[i].handler;
        }
    }
    return NULL;
}

// ───────────────────────── pack ─────────────────────────

static void wr_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

int nc4_pack_into(uint8_t *buf, size_t cap, uint16_t cmd, uint16_t addr,
                  const uint8_t *payload, size_t payload_len)
{
    if (buf == NULL || payload_len > NC4_MAX_PAYLOAD) {
        return -1;
    }
    const size_t total = NC4_HDR_LEN + payload_len + NC4_CRC_LEN;
    if (cap < total) {
        return -1;
    }

    buf[0] = NC4_SOF_0;
    buf[1] = NC4_SOF_1;
    buf[2] = NC4_VERSION;
    wr_u16(buf + 3, addr);
    wr_u16(buf + 5, cmd);
    wr_u16(buf + 7, (uint16_t)payload_len);
    if (payload_len) {
        memcpy(buf + NC4_HDR_LEN, payload, payload_len);
    }

    const uint32_t crc = esp_rom_crc32_le(0, buf + 2, NC4_HDR_LEN - 2 + payload_len);
    uint8_t *p = buf + NC4_HDR_LEN + payload_len;
    p[0] = (uint8_t)crc;
    p[1] = (uint8_t)(crc >> 8);
    p[2] = (uint8_t)(crc >> 16);
    p[3] = (uint8_t)(crc >> 24);
    return (int)total;
}

static uint32_t rd_u32le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// ───────────────────────── parser ─────────────────────────

nc4_parser_t *nc4_parser_create(nc4_decoder_t *decoder, nc4_send_fn send, void *send_arg,
                                const char *source, size_t max_frame)
{
    if (decoder == NULL) {
        return NULL;
    }
    if (max_frame == 0 || max_frame > NC4_MAX_FRAME) {
        max_frame = NC4_MAX_FRAME;
    }
    nc4_parser_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->cap = max_frame;
    p->buf = malloc(max_frame);
    if (p->buf == NULL) {
        free(p);
        return NULL;
    }
    p->ctx.decoder = decoder;
    p->ctx.send = send;
    p->ctx.send_arg = send_arg;
    p->ctx.source = source ? source : "?";
    return p;
}

nc4_parser_t *nc4_parser_create_in(void *region, size_t region_bytes,
                                   nc4_decoder_t *decoder, nc4_send_fn send,
                                   void *send_arg, const char *source)
{
    if (region == NULL || decoder == NULL || region_bytes < NC4_HDR_LEN + NC4_CRC_LEN) {
        return NULL;
    }
    nc4_parser_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->buf = region;            // 外部區域：destroy 不會 free
    p->cap = region_bytes;
    p->external = true;
    p->ctx.decoder = decoder;
    p->ctx.send = send;
    p->ctx.send_arg = send_arg;
    p->ctx.source = source ? source : "?";
    return p;
}

void nc4_parser_destroy(nc4_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    if (!parser->external) {
        free(parser->buf);
    }
    free(parser);
}

// 幀已驗過 CRC：ADDR 過濾後 dispatch。
// base = 幀起點（SOF），plen 已驗證 ≤ NC4_MAX_PAYLOAD。
// 只借用呼叫端的緩衝，不複製 —— handler 回傳後 frame.data 即失效。
static void parser_dispatch(nc4_parser_t *parser, const uint8_t *base, uint16_t plen)
{
    nc4_frame_t f = {
        .addr = (uint16_t)(base[3] | (base[4] << 8)),
        .cmd = (uint16_t)(base[5] | (base[6] << 8)),
        .data = base + NC4_HDR_LEN,
        .len = plen,
    };

    // ADDR 過濾：只收廣播或定址到本機 cid 的幀（與 mp 版 app.handle_stream 一致）
    const uint16_t my_cid = parser->ctx.decoder->cid;
    if (f.addr != NC4_ADDR_BROADCAST && f.addr != my_cid) {
        parser->ctx.decoder->dropped_addr++;
        return;
    }

    nc4_handler_t h = decoder_lookup(parser->ctx.decoder, f.cmd);
    if (h != NULL) {
        decoder_lock(parser->ctx.decoder);
        h(&parser->ctx, &f);
        decoder_unlock(parser->ctx.decoder);
    } else {
        ESP_LOGD(TAG, "[%s] 未註冊的 cmd 0x%04X (len=%u)", parser->ctx.source, f.cmd, f.len);
    }
}

void nc4_parser_reset(nc4_parser_t *parser)
{
    if (parser != NULL) {
        parser->start = 0;
        parser->end = 0;
    }
}

// 看 9 B 表頭判斷整幀長度；不是合法表頭回 0。
// 給「先看表頭、再決定這次要 recv 多少」的傳輸層用（frame-aligned 收包）。
size_t nc4_header_total(const uint8_t *hdr)
{
    if (hdr == NULL || hdr[0] != NC4_SOF_0 || hdr[1] != NC4_SOF_1 ||
        hdr[2] != NC4_VERSION) {
        return 0;
    }
    const uint16_t plen = (uint16_t)(hdr[7] | (hdr[8] << 8));
    if (plen > NC4_MAX_PAYLOAD) {
        return 0;
    }
    return NC4_HDR_LEN + (size_t)plen + NC4_CRC_LEN;
}

// 直接解析「一個完整 frame」：buf 從 SOF 開始、len 必須等於 13+plen。
// **零複製**：不進 ring、不 memcpy；dispatch 時 frame.data 指向 buf+9。
// 呼叫端（frame-aligned 的 transport）負責提供連續且完整的一幀。
esp_err_t nc4_parser_frame(nc4_parser_t *parser, const uint8_t *buf, size_t len)
{
    if (parser == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t total = nc4_header_total(buf);
    if (total == 0 || len != total) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t plen = (uint16_t)(total - NC4_HDR_LEN - NC4_CRC_LEN);

    // CRC32（esp_rom_crc32_le(0,…) 與 MP 版 binascii.crc32(x,0) 等價）
    const uint32_t want = rd_u32le(buf + NC4_HDR_LEN + plen);
    const uint32_t got = esp_rom_crc32_le(0, buf + 2, NC4_HDR_LEN - 2 + plen);
    if (want != got) {
        return ESP_ERR_INVALID_CRC;
    }

    parser_dispatch(parser, buf, plen);
    return ESP_OK;
}

esp_err_t nc4_parser_feed(nc4_parser_t *parser, const uint8_t *data, size_t len)
{
    if (parser == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 空間不夠先搬移未消費段（與 MP 版 StreamParser.compact 同語義）
    if (parser->end + len > parser->cap) {
        size_t keep = parser->end - parser->start;
        if (keep + len > parser->cap) {
            ESP_LOGW(TAG, "[%s] rx 溢位，丟棄 %u bytes", parser->ctx.source, (unsigned)keep);
            parser->start = parser->end = 0;
            return ESP_ERR_NO_MEM;
        }
        memmove(parser->buf, parser->buf + parser->start, keep);
        parser->start = 0;
        parser->end = keep;
    }
    memcpy(parser->buf + parser->end, data, len);
    parser->end += len;

    for (;;) {
        size_t avail = parser->end - parser->start;
        if (avail < NC4_HDR_LEN) {
            return ESP_OK;
        }

        // 1. 找 SOF
        uint8_t *base = parser->buf + parser->start;
        size_t i = 0;
        while (i + 1 < avail && !(base[i] == NC4_SOF_0 && base[i + 1] == NC4_SOF_1)) {
            i++;
        }
        if (i + 1 >= avail) {
            parser->start = parser->end = 0;
            return ESP_OK;
        }
        if (i != 0) {
            parser->start += i;
            avail -= i;
            base = parser->buf + parser->start;
            if (avail < NC4_HDR_LEN) {
                return ESP_OK;
            }
        }

        // 2. 標頭（不合就前移 1 byte 重找 —— 與 MP 版一致的寬鬆重同步）
        const uint8_t ver = base[2];
        const uint16_t plen = (uint16_t)(base[7] | (base[8] << 8));

        if (ver != NC4_VERSION || plen > NC4_MAX_PAYLOAD) {
            parser->start += 1;
            continue;
        }

        const size_t total = NC4_HDR_LEN + plen + NC4_CRC_LEN;
        if (avail < total) {
            return ESP_OK;   // 等更多資料
        }

        // 3. CRC32（esp_rom_crc32_le(0,…) 與 MP 版 binascii.crc32(x,0) 等價）
        const uint32_t want = rd_u32le(base + NC4_HDR_LEN + plen);
        const uint32_t got = esp_rom_crc32_le(0, base + 2, NC4_HDR_LEN - 2 + plen);
        if (want != got) {
            parser->start += 1;
            continue;
        }

        parser_dispatch(parser, base, plen);

        parser->start += total;
        if (parser->start == parser->end) {
            parser->start = parser->end = 0;
        }
    }
}
