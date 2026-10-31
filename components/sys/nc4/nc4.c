#include "nc4.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "nc4";

typedef struct {
    uint16_t cmd;
    nc4_handler_t handler;
} nc4_entry_t;

struct nc4_decoder {
    nc4_entry_t table[NC4_MAX_HANDLERS];
    size_t count;
    uint16_t cid;          // 我自己（0xFFFF = 未設定）
    uint16_t master_cid;   // 我的上級（0xFFFF = 未設定）
    uint32_t dropped_addr; // 丟掉的「不是給我的」幀
};

struct nc4_parser {
    uint8_t *buf;
    size_t cap;
    size_t start;
    size_t end;
    nc4_ctx_t ctx;
};

// ───────────────────────── decoder ─────────────────────────

nc4_decoder_t *nc4_decoder_create(void)
{
    nc4_decoder_t *d = calloc(1, sizeof(nc4_decoder_t));
    if (d != NULL) {
        d->cid = NC4_ADDR_BROADCAST;
        d->master_cid = NC4_ADDR_BROADCAST;
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
    free(decoder);
}

esp_err_t nc4_decoder_on(nc4_decoder_t *decoder, uint16_t cmd, nc4_handler_t handler)
{
    if (decoder == NULL || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < decoder->count; i++) {
        if (decoder->table[i].cmd == cmd) {
            decoder->table[i].handler = handler;   // 覆寫（與 MP 版「後載入者勝」一致）
            return ESP_OK;
        }
    }
    if (decoder->count >= NC4_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    decoder->table[decoder->count++] = (nc4_entry_t){ .cmd = cmd, .handler = handler };
    return ESP_OK;
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

void nc4_parser_destroy(nc4_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    free(parser->buf);
    free(parser);
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
            parser->start += total;
            if (parser->start == parser->end) {
                parser->start = parser->end = 0;
            }
            continue;
        }

        nc4_handler_t h = decoder_lookup(parser->ctx.decoder, f.cmd);
        if (h != NULL) {
            h(&parser->ctx, &f);
        } else {
            ESP_LOGD(TAG, "[%s] 未註冊的 cmd 0x%04X (len=%u)", parser->ctx.source, f.cmd, f.len);
        }

        parser->start += total;
        if (parser->start == parser->end) {
            parser->start = parser->end = 0;
        }
    }
}
