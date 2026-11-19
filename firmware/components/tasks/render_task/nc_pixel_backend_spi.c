// nc_pixel_backend_spi.c — 渲染器 ↔ SPI 燈驅動的轉接層（task 側）。
//
// driver/drv_spi_led 只管 SPI（MOSI/SCLK、時鐘、DMA 槽）；
// 這一支負責：格式檢查（只吃時脈式燈）、codec、把位樣本打包成 MSB-first byte 流、
// 環的 acquire/release 與 slot ↔ seq 對應。
//
// 一位 = 一個 SPI 時脈。WS2801／APA102 是這個後端唯一的用途；
// WS2812 這類歸零碼燈走 I80（見 nc_pixel_backend_i80.c）。
#include <sdkconfig.h>

#if CONFIG_NETCORE_PIXEL_SPI

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nc_hw.h"
#include "nc_render.h"
#include "nc_spi_led_drv.h"

static const char *TAG = "px_spi";

#define SPI_SLOTS_MAX NC_SPI_LED_DRV_SLOTS_MAX

typedef struct {
    nc_pixel_backend_t be;
    nc_pixel_codec_t  *codec;
    uint32_t           px;
    uint32_t           bits;                  // 一幀幾個位
    uint8_t            slots;
    uint8_t           *samples;               // 位樣本（每 byte 一個位，0/1；CPU 用）
    const uint8_t     *pay[SPI_SLOTS_MAX];    // 這一格屬於環的哪一份 payload
    uint32_t           seq[SPI_SLOTS_MAX];
    uint8_t            sent[SPI_SLOTS_MAX];   // 已同步送出，等下一輪 reclaim 還環格
    nc_px_format_t     fmt;
    int                next;
} spi_be_t;

static spi_be_t s_be;

static bool spi_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    spi_be_t *d = (spi_be_t *)b->ctx;
    const uint8_t *p = nc_px_ring_peek(b->ring, NULL, seq);
    if (p == NULL || *seq == post_seq) {
        return false;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        const uint32_t k = (uint32_t)((d->next + i) % d->slots);
        if (d->seq[k] == 0) {
            d->next = (int)((k + 1u) % d->slots);
            d->seq[k] = *seq;
            d->pay[k] = p;          // 不再在 encode 裡 peek（新格配舊 seq 會對不上）
            d->sent[k] = 0;
            *slot = k;
            *payload = p;
            *px = d->px;
            return true;
        }
    }
    nc_px_ring_release(b->ring, *seq);
    return false;
}

static bool spi_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    spi_be_t *d = (spi_be_t *)b->ctx;
    if (slot >= d->slots || d->pay[slot] == NULL || d->samples == NULL) {
        return false;
    }
    nc_px_codec_frame(d->codec, d->seq[slot]);
    const uint32_t n = nc_px_codec_spi_frame(d->codec, d->pay[slot], d->px, d->samples,
                                             d->bits);
    if (n == 0 || n != d->bits) {
        return false;
    }
    // codec 交出的是一格一個 bit 的樣本（方便 host 測試逐位比對）；
    // SPI 要的是「8 個 bit 塞一個 byte」（MSB first）。打包在轉接層做。
    uint8_t *out = nc_spi_led_drv_slot_buf((int)slot);
    if (out == NULL) {
        return false;
    }
    const uint32_t tx_bytes = (d->bits + 7u) / 8u;
    memset(out, 0, tx_bytes);
    for (uint32_t i = 0; i < d->bits; i++) {
        if (d->samples[i] != 0) {
            out[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
        }
    }
    return true;
}

static bool spi_submit(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq)
{
    (void)seq;
    spi_be_t *d = (spi_be_t *)b->ctx;
    if (!nc_spi_led_drv_submit((int)slot, d->bits)) {
        d->seq[slot] = 0;
        d->pay[slot] = NULL;
        return false;
    }
    d->sent[slot] = 1;          // 同步送出：下一輪 reclaim 直接還環格
    return true;
}

static void spi_reclaim(nc_pixel_backend_t *b)
{
    spi_be_t *d = (spi_be_t *)b->ctx;
    for (uint32_t i = 0; i < d->slots; i++) {
        if (d->seq[i] != 0 && d->sent[i]) {
            nc_px_ring_release(b->ring, d->seq[i]);
            d->seq[i] = 0;
            d->pay[i] = NULL;
            d->sent[i] = 0;
        }
    }
}

static size_t spi_json(nc_pixel_backend_t *b, char *out, size_t cap)
{
    spi_be_t *d = (spi_be_t *)b->ctx;
    const int n = snprintf(out, cap,
                           "\"fmt\":\"%s\",\"px\":%u,\"bits\":%u,\"clk\":%u,"
                           "\"submitted\":%u,\"fail\":%u}",
                           nc_px_format_name(d->fmt), (unsigned)d->px, (unsigned)d->bits,
                           (unsigned)nc_spi_led_drv_clock(),
                           (unsigned)nc_spi_led_drv_submitted(),
                           (unsigned)nc_spi_led_drv_fail());
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void spi_be_free(spi_be_t *d)
{
    if (d->codec != NULL) {
        nc_px_codec_free(d->codec);
        d->codec = NULL;
    }
    if (d->samples != NULL) {
        heap_caps_free(d->samples);
        d->samples = NULL;
    }
    nc_spi_led_drv_uninstall();
}

nc_pixel_backend_t *nc_px_backend_spi(const nc_px_spi_cfg_t *cfg)
{
    if (cfg == NULL || cfg->pin_mosi < 0 || cfg->pin_sclk < 0 || cfg->px == 0) {
        return NULL;
    }
    const nc_px_formatspec_t *spec = nc_px_format_spec(cfg->fmt);
    if (spec == NULL || !spec->clk) {
        ESP_LOGE(TAG, "格式 %s 不是時脈型（SPI 只吃 WS2801／APA102）",
                 nc_px_format_name(cfg->fmt));
        return NULL;
    }
    spi_be_t *d = &s_be;
    spi_be_free(d);
    memset(d, 0, sizeof(*d));
    d->px = (cfg->px > NC_PX_MAX_LEDS) ? NC_PX_MAX_LEDS : cfg->px;
    d->slots = (uint8_t)((cfg->slots < 2) ? 2u : cfg->slots);
    if (d->slots > SPI_SLOTS_MAX) {
        d->slots = SPI_SLOTS_MAX;
    }
    d->fmt = cfg->fmt;

    nc_px_codec_cfg_t cc = {
        .fmt = cfg->fmt,
        .bits = (uint8_t)((cfg->dither_bits < 8) ? 8 : cfg->dither_bits),
        .dither_mode = cfg->dither_mode,
        .zone_px = cfg->zone_px,
        .px = d->px,
        .bright = cfg->bright,
    };
    d->codec = nc_px_codec_init(nc_px_codec_new(), &cc);
    if (d->codec == NULL) {
        return NULL;
    }
    d->bits = nc_px_codec_bits(d->codec);
    if (d->bits == 0) {
        spi_be_free(d);
        return NULL;
    }
    d->samples = heap_caps_malloc(d->bits, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (d->samples == NULL) {
        ESP_LOGE(TAG, "位樣本緩衝（%u B）配置失敗", (unsigned)d->bits);
        spi_be_free(d);
        return NULL;
    }

    if (nc_hw_reserved(cfg->pin_mosi) || nc_hw_reserved(cfg->pin_sclk)) {
        ESP_LOGE(TAG, "SPI 腳位是保留腳（MOSI=%d SCLK=%d）", cfg->pin_mosi, cfg->pin_sclk);
        spi_be_free(d);
        return NULL;
    }
    (void)nc_hw_claim(cfg->pin_mosi, "pixel", "spi_mosi", NC_HW_CLAIM_WRITE);
    (void)nc_hw_claim(cfg->pin_sclk, "pixel", "spi_sclk", NC_HW_CLAIM_WRITE);

    const nc_spi_led_drv_cfg_t dc = {
        .pin_mosi = cfg->pin_mosi,
        .pin_sclk = cfg->pin_sclk,
        .host = cfg->host,
        .clock_hz = cfg->clock_hz,
        .slot_bytes = (d->bits + 7u) / 8u,
        .slots = d->slots,
    };
    if (nc_spi_led_drv_install(&dc) != ESP_OK) {
        ESP_LOGE(TAG, "SPI LED driver 安裝失敗");
        spi_be_free(d);
        return NULL;
    }
    d->slots = (uint8_t)nc_spi_led_drv_slots();

    (void)snprintf(d->be.name, sizeof(d->be.name), "spi");
    d->be.lanes = 1;
    d->be.ctx = d;
    d->be.acquire = spi_acquire;
    d->be.encode = spi_encode;
    d->be.submit = spi_submit;
    d->be.reclaim = spi_reclaim;
    d->be.json = spi_json;
    ESP_LOGI(TAG, "SPI adapter 就緒：%s、%u 顆、clk %u Hz、一幀 %u 位",
             nc_px_format_name(cfg->fmt), (unsigned)d->px,
             (unsigned)nc_spi_led_drv_clock(), (unsigned)d->bits);
    return &d->be;
}

#else   // !CONFIG_NETCORE_PIXEL_SPI

#include "nc_render.h"

nc_pixel_backend_t *nc_px_backend_spi(const nc_px_spi_cfg_t *cfg)
{
    (void)cfg;
    return NULL;
}

#endif  // CONFIG_NETCORE_PIXEL_SPI
