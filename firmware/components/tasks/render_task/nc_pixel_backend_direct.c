// nc_pixel_direct.c — software relay 後端（不碰任何周邊）。
//
// 為什麼要有這一支：**沒有燈板的板子，整條路也要跑得起來**。
//   direct 不碰暫存器：它用同一份 codec 把 canonical 幀編成單 lane 的時序單元，
//   放進自己的 frame buffer 並計數「已送」。另一個消費者
//   （將來的 RMT、P4 的 LED 面板、host 測試）可以直接讀 `nc_px_direct_frame()`。
//
// 它同時是「DMA 槽 → 環格」登記表的最小實作：
//   acquire → encode → submit →（完成）→ reclaim。
//   i80／spi 只是把「送」換成真的硬體，登記表語義完全一樣。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nc_render.h"

static const char *TAG = "px_direct";

#define DIR_SLOTS 3

typedef struct {
    nc_pixel_backend_t be;
    nc_pixel_q_t      *q;
    nc_pixel_codec_t  *codec;
    uint32_t           px;
    uint32_t           units;                // 一幀幾個時序單元（含 reset）
    uint8_t           *buf;                  // DIR_SLOTS × units（internal）
    const uint8_t     *pay[DIR_SLOTS];       // 這一格屬於環的哪一份 payload
    uint32_t           seq[DIR_SLOTS];       // 0 = 空閒
    uint32_t           len[DIR_SLOTS];
    nc_px_format_t     fmt;
    uint32_t           submitted;
    uint32_t           reclaimed;
    uint32_t           busy_rows;            // 已提交、未還
    uint32_t           last_seq;
} dir_t;

static dir_t  s_dir;

static bool dir_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    dir_t *d = (dir_t *)b->ctx;
    // 取格政策見 nc_px_ring_take()：多子幀＝逐格 FIFO，單子幀＝最新優先。
    const uint8_t *p = nc_px_ring_take(d->q, post_seq, NULL, seq);
    if (p == NULL) {
        return false;                        // 沒有新格（或已經交過同一格）
    }
    for (int i = 0; i < DIR_SLOTS; i++) {
        if (d->seq[i] == 0) {
            // 記住 payload 指標：encode 不可以再 peek（生產者也許已交出新格，
            // 那會拿到新內容配舊 seq）。這一格在 release 之前 writer 不會覆寫。
            d->seq[i] = *seq;
            d->pay[i] = p;
            *slot = (uint32_t)i;
            *payload = p;
            *px = d->px;
            return true;
        }
    }
    nc_px_ring_untake(d->q, *seq);           // mailbox：唔還格會阻住最新格
    return false;                            // 全部在飛（direct 不該發生：reclaim 每輪都清）
}

static bool dir_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    dir_t *d = (dir_t *)b->ctx;
    if (slot >= DIR_SLOTS || d->pay[slot] == NULL || d->buf == NULL) {
        return false;
    }
    nc_px_codec_frame(d->codec, d->seq[slot]);
    uint8_t *out = d->buf + (size_t)slot * d->units;
    const uint32_t n = nc_px_codec_wave_frame(d->codec, d->pay[slot], d->px, out, d->units);
    if (n == 0) {
        return false;
    }
    d->len[slot] = n;
    return true;
}

static bool dir_submit(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq)
{
    dir_t *d = (dir_t *)b->ctx;
    if (slot >= DIR_SLOTS || seq == 0) {
        return false;
    }
    d->last_seq = seq;
    d->submitted++;
    d->busy_rows++;
    return true;
}

static void dir_reclaim(nc_pixel_backend_t *b)
{
    dir_t *d = (dir_t *)b->ctx;
    // direct 的「送出」是同步的：下一次進到這裡就代表上一輪已經被消費（或不需要）。
    for (int i = 0; i < DIR_SLOTS; i++) {
        if (d->seq[i] != 0) {
            nc_px_ring_release(d->q, d->seq[i]);
            d->seq[i] = 0;
            d->pay[i] = NULL;
            d->reclaimed++;
            if (d->busy_rows > 0) {
                d->busy_rows--;
            }
        }
    }
}

static size_t dir_json(nc_pixel_backend_t *b, char *out, size_t cap)
{
    dir_t *d = (dir_t *)b->ctx;
    const int n = snprintf(out, cap,
                           "\"direct\":{\"fmt\":\"%s\",\"submitted\":%u,\"reclaimed\":%u,"
                           "\"busy\":%u,\"seq\":%u}",
                           nc_px_format_name(d->fmt), (unsigned)d->submitted,
                           (unsigned)d->reclaimed, (unsigned)d->busy_rows,
                           (unsigned)d->last_seq);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

nc_pixel_backend_t *nc_px_backend_direct(void)
{
    dir_t *d = &s_dir;
    // codec／緩衝是 nc_px_direct_bind() 配的，這裡只重設執行期帳面。
    nc_pixel_codec_t *codec = d->codec;
    uint8_t *buf = d->buf;
    const uint32_t px = d->px;
    const uint32_t units = d->units;
    const nc_px_format_t fmt = d->fmt;
    memset(d, 0, sizeof(*d));
    d->codec = codec;
    d->buf = buf;
    d->px = px;
    d->units = units;
    d->fmt = fmt;
    (void)snprintf(d->be.name, sizeof(d->be.name), "direct");
    d->be.lanes = 1;
    d->be.ctx = d;
    d->be.acquire = dir_acquire;
    d->be.encode = dir_encode;
    d->be.submit = dir_submit;
    d->be.reclaim = dir_reclaim;
    d->be.json = dir_json;
    ESP_LOGI(TAG, "direct 後端就緒（software relay，%d 槽）", DIR_SLOTS);
    return &d->be;
}

// 給另一個消費者用：最近一次「已提交」的單 lane 時序單元。
//   out=NULL 只回長度。renderer 還格之前這份 buffer 一定有效
//   （renderer 每輪先 reclaim 再 acquire，所以「上一輪交的」會被這一輪改寫）。
bool nc_px_direct_frame(const uint8_t **units, uint32_t *len, uint32_t *seq)
{
    dir_t *d = &s_dir;
    if (d->be.ctx == NULL) {
        return false;
    }
    int best = -1;
    uint32_t best_seq = 0;
    for (int i = 0; i < DIR_SLOTS; i++) {
        if (d->seq[i] != 0 && d->seq[i] >= best_seq) {
            best = i;
            best_seq = d->seq[i];
        }
    }
    if (best < 0) {
        return false;
    }
    if (units != NULL) {
        *units = d->buf + (size_t)best * d->units;
    }
    if (len != NULL) {
        *len = d->len[best];
    }
    if (seq != NULL) {
        *seq = best_seq;
    }
    return true;
}

// renderer 的 on_start 會叫它：把 direct 使用的 codec 綁上（格式／顆數由呼叫端給）
void nc_px_direct_bind(const nc_px_codec_cfg_t *codec_cfg)
{
    dir_t *d = &s_dir;
    if (codec_cfg == NULL) {
        return;
    }
    if (d->codec != NULL) {
        nc_px_codec_free(d->codec);
        d->codec = NULL;
    }
    d->codec = nc_px_codec_init(nc_px_codec_new(), codec_cfg);
    if (d->codec == NULL) {
        return;
    }
    d->px = codec_cfg->px;
    d->fmt = codec_cfg->fmt;
    const uint32_t units = nc_px_codec_total_units(d->codec);
    // 緩衝按這塊板真的幾顆／幾條 lane 配置，不是按 NC_PX_MAX_LEDS
    // （2048 顆 × 3 槽的靜態版本要 393 KB，S3 根本放不落）。
    if (d->buf != NULL && units == d->units) {
        return;
    }
    if (d->buf != NULL) {
        free(d->buf);
        d->buf = NULL;
    }
    d->units = units;
    if (units == 0) {
        return;
    }
    // malloc 在 firmware 一樣是 internal heap；用 libc 的配置器是為了 host 測試也編同一份。
    d->buf = calloc(DIR_SLOTS, units);
    if (d->buf == NULL) {
        ESP_LOGE(TAG, "direct 緩衝（%u × %u B）配置失敗", DIR_SLOTS, (unsigned)units);
    }
}

// 綁環（同 mock：環從哪來由呼叫端決定）
void nc_px_backend_direct_bind(nc_pixel_backend_t *b, nc_pixel_q_t *q)
{
    dir_t *d = (dir_t *)b->ctx;
    d->q = q;
    d->px = nc_px_ring_px(q);
}