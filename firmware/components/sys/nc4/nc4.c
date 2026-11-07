#include "nc4.h"
#include "nc_action.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// handler 的互斥**屬於 decoder，不是全域**：
//   一個 decoder 一次執行一個 handler；多個 decoder（每核一個）可以同時各跑一個。
// 這是「一個解碼器執行一個指令」的模型，也是 Network.nc4.decode[] 那張表的意思。
// ⚠️ 要開第二個 decoder 之前，handler 必須是 reentrant（不能共寫 static 回覆緩衝）。
static const char *TAG = "nc4";

struct nc4_decoder {
    // 指令表唔再住喺呢個 struct：由 nc_action 管理器統計＋一次配置＋封存
    //（見 nc_action.h）。decoder 只保留「誰是我、一次跑一個」這些狀態。
    SemaphoreHandle_t lock;   // 這個 decoder 的執行鎖（一個 decoder 一次一個 handler）
    uint16_t cid;          // 我自己（0xFFFF = 未設定）
    uint16_t master_cid;   // 我的上級（0xFFFF = 未設定）
    uint32_t dropped_addr; // 丟掉的「不是給我的」幀
    uint8_t *reply;        // 回覆幀（NC4_REPLY_FRAME_MAX B，見 nc4_reply_buf）
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

    // 壞幀統計：以前 CRC 錯了只會靜靜前移 1 byte，外面完全看不出來。
    uint32_t n_crc_err;     // CRC32 對不上
    uint32_t n_bad_hdr;     // VER 不對 / LEN 超上限
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
    // 回覆幀：優先 PSRAM（它只被 CPU 讀寫，不經 DMA），冇 PSRAM 才落內部。
    // 這 2.6 KB 以前在每個 decode 任務的棧上 —— 搬出來之後棧可以細一半。
    d->reply = heap_caps_malloc(NC4_REPLY_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (d->reply == NULL) {
        d->reply = malloc(NC4_REPLY_FRAME_MAX);
    }
    if (d->reply == NULL) {
        vSemaphoreDelete(d->lock);
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

uint8_t *nc4_decoder_reply_buf(nc4_decoder_t *decoder)
{
    return decoder ? decoder->reply : NULL;
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
    free(decoder->reply);
    free(decoder);
}

static nc4_handler_t decoder_lookup(nc4_decoder_t *decoder, uint16_t cmd)
{
    (void)decoder;
    return nc_action_find(cmd);      // 已排序 → 二分搜尋
}

size_t nc4_decoder_count(const nc4_decoder_t *decoder)
{
    return decoder ? nc_action_count() : 0;
}

bool nc4_decoder_at(const nc4_decoder_t *decoder, size_t i, uint16_t *cmd,
                    const char **owner, nc4_handler_t *handler)
{
    return decoder ? nc_action_at(i, cmd, owner, handler) : false;
}

bool nc4_decoder_has(const nc4_decoder_t *decoder, uint16_t cmd)
{
    return decoder ? (nc_action_find(cmd) != NULL) : false;
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
        // memmove 而不是 memcpy：容許就地打包（payload 指向 buf + NC4_HDR_LEN，
        // 也就是把 DATA 直接砌在自己的幀裡，省掉一塊獨立的工作緩衝）。
        memmove(buf + NC4_HDR_LEN, payload, payload_len);
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
// base = 幀起點（SOF），dlen 已驗證 ≤ NC4_MAX_PAYLOAD。
// 只借用呼叫端的緩衝，不複製 —— handler 回傳後 frame.data 即失效。
static void parser_dispatch(nc4_parser_t *parser, const uint8_t *base, uint16_t dlen)
{
    nc4_frame_t f = {
        .addr = (uint16_t)(base[3] | (base[4] << 8)),
        .cmd = (uint16_t)(base[5] | (base[6] << 8)),
        .data = base + NC4_HDR_LEN,
        .len = dlen,
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

void nc4_parser_errors(const nc4_parser_t *parser, uint32_t *crc_err, uint32_t *bad_hdr)
{
    if (crc_err != NULL) {
        *crc_err = (parser != NULL) ? parser->n_crc_err : 0;
    }
    if (bad_hdr != NULL) {
        *bad_hdr = (parser != NULL) ? parser->n_bad_hdr : 0;
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
    const uint16_t dlen = (uint16_t)(hdr[7] | (hdr[8] << 8));
    if (dlen > NC4_MAX_PAYLOAD) {
        return 0;
    }
    return NC4_HDR_LEN + (size_t)dlen + NC4_CRC_LEN;
}

// 直接解析「一個完整 frame」：buf 從 SOF 開始、len 必須等於 13+dlen。
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
    const uint16_t dlen = (uint16_t)(total - NC4_HDR_LEN - NC4_CRC_LEN);

    // CRC32（esp_rom_crc32_le(0,…) 與 MP 版 binascii.crc32(x,0) 等價）
    const uint32_t want = rd_u32le(buf + NC4_HDR_LEN + dlen);
    const uint32_t got = esp_rom_crc32_le(0, buf + 2, NC4_HDR_LEN - 2 + dlen);
    if (want != got) {
        parser->n_crc_err++;
        return ESP_ERR_INVALID_CRC;
    }

    parser_dispatch(parser, buf, dlen);
    return ESP_OK;
}

// 溢位／過大幀的警告限速：每秒最多一行。唔限速的話，洪水測試會用 log 淹死
// 自己的 CDC TX（實測：8 KB 幀 × 8 MB → 三萬行，回覆全部排唔到隊）。
static void nc4_warn_throttled(const char *src, const char *fmt, ...)
{
    static int64_t  s_last_us;
    static uint32_t s_count;
    s_count++;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_us < 1000000) {
        return;
    }
    s_last_us = now;
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "[%s] %s（累計 %u 次，每秒最多 1 行）", src, msg, (unsigned)s_count);
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
            nc4_warn_throttled(parser->ctx.source, "rx 溢位，丟棄 %u bytes", (unsigned)keep);
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
        const uint16_t dlen = (uint16_t)(base[7] | (base[8] << 8));

        if (ver != NC4_VERSION || dlen > NC4_MAX_PAYLOAD) {
            parser->n_bad_hdr++;
            parser->start += 1;
            continue;
        }

        const size_t total = NC4_HDR_LEN + dlen + NC4_CRC_LEN;
        // 幀比 parser 緩衝大：永遠放唔落。以前會一直緩衝到溢位再整段丟，
        // 一個 8 KB 幀（CDC 的 region 只有 4,192）就變成每 256 B 一次「rx 溢位」
        // —— 洪水測試時 22 秒噴 627 行 log，塞爆 CDC TX，decode 的回覆全部被丟。
        // 正確做法：認出「放唔落」就跳過這個 SOF，繼續找下一個，而且 log 要限速。
        if (total > parser->cap) {
            parser->n_bad_hdr++;
            parser->start += 1;
            nc4_warn_throttled(parser->ctx.source, "幀過大（>%u B）", (unsigned)parser->cap);
            continue;
        }
        if (avail < total) {
            return ESP_OK;   // 等更多資料
        }

        // 3. CRC32（esp_rom_crc32_le(0,…) 與 MP 版 binascii.crc32(x,0) 等價）
        const uint32_t want = rd_u32le(base + NC4_HDR_LEN + dlen);
        const uint32_t got = esp_rom_crc32_le(0, base + 2, NC4_HDR_LEN - 2 + dlen);
        if (want != got) {
            parser->n_crc_err++;
            parser->start += 1;
            continue;
        }

        parser_dispatch(parser, base, dlen);

        parser->start += total;
        if (parser->start == parser->end) {
            parser->start = parser->end = 0;
        }
    }
}
