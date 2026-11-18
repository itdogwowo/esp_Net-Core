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
#include <string.h>

#include "esp_log.h"
#include "nc_pixel.h"

static const char *TAG = "px_direct";

#define DIR_SLOTS 3
#define DIR_UNITS NC_PX_UNITS_MAX

typedef struct {
    nc_pixel_backend_t be;
    nc_pixel_q_t      *q;
    nc_pixel_codec_t  *codec;
    uint32_t           px;
    uint32_t           seq[DIR_SLOTS];       // 0 = 空閒
    uint32_t           units[DIR_SLOTS];
    nc_px_format_t     fmt;
    uint32_t           submitted;
    uint32_t           reclaimed;
    uint32_t           busy_rows;            // 已提交、未還
    uint32_t           last_seq;
} dir_t;

static dir_t  s_dir;
static uint8_t s_dir_buf[DIR_SLOTS][DIR_UNITS];
static nc_pixel_codec_t *s_dir_codec;

static bool dir_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    dir_t *d = (dir_t *)b->ctx;
    const uint8_t *p = nc_px_ring_peek(d->q, NULL, seq);
    if (p == NULL) {
        return false;                        // 沒有新格（或已經交過同一格）
    }
    if (*seq == post_seq) {
        return false;
    }
    for (int i = 0; i < DIR_SLOTS; i++) {
        if (d->seq[i] == 0) {
            // 記住「這格屬於哪個 seq」：encode 會再用 peek 拿指標，
            // 所以 slot 與 seq 的對應只在渲染器這一輪有效。
            d->seq[i] = *seq;
            *slot = (uint32_t)i;
            *payload = p;
            *px = d->px;
            return true;
        }
    }
    return false;                            // 全部在飛（direct 不該發生：reclaim 每輪都清）
}

static bool dir_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    dir_t *d = (dir_t *)b->ctx;
    if (slot >= DIR_SLOTS) {
        return false;
    }
    const uint8_t *p = nc_px_ring_peek(d->q, NULL, NULL);
    if (p == NULL) {
        return false;
    }
    nc_px_codec_frame(d->codec, d->submitted + 1u);
    const uint32_t n = nc_px_codec_wave_frame(d->codec, p, d->px, s_dir_buf[slot], DIR_UNITS);
    if (n == 0) {
        return false;
    }
    d->units[slot] = n;
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
    memset(d, 0, sizeof(*d));
    memset(s_dir_buf, 0, sizeof(s_dir_buf));
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
        *units = s_dir_buf[best];
    }
    if (len != NULL) {
        *len = d->units[best];
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
    d->codec = nc_px_codec_init(nc_px_codec_new(), codec_cfg);
    d->px = codec_cfg->px;
    d->fmt = codec_cfg->fmt;
}

// 綁環（同 mock：環從哪來由呼叫端決定）
void nc_px_backend_direct_bind(nc_pixel_backend_t *b, nc_pixel_q_t *q)
{
    dir_t *d = (dir_t *)b->ctx;
    d->q = q;
    d->px = nc_px_ring_px(q);
}