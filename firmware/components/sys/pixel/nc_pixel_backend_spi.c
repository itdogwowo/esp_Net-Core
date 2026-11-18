// nc_pixel_spi.c — SPI 後端：WS2801 / APA102（有獨立時脈的燈串）。
//
// 為什麼要留這一支：
//   你寫的 ws28xx 裡，**WS2812／WS2812B 是單線歸零碼**（沒有時脈），
//   **WS2801 是兩線**（SDI + CKI，一個 clock 一個 bit）—— 兩者物理層不同。
//   WS2802 與 WS2811 也都是單線（歸零碼）；真正「SPI 直接送」的是 WS2801／APA102。
//   所以格式解析時，ws2801 走 SPI、ws2812 走 I80／RMT；選錯會完全沒有光。
//
// 送法：把 codec 展開的位元（每 byte 一個位，bit0 = MOSI）包成 SPI 交易：
//   時脈 8 MHz ⇒ 一顆 24 bit 燈 = 3 µs，1,000 顆 = 3 ms（比 WS2812 快很多）。
//   多條 lane 就是多個 SPI 後端（一條 SPI 只有一個 MOSI）。
#include <sdkconfig.h>

#if CONFIG_NETCORE_PIXEL_SPI

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nc_hw.h"
#include "nc_pixel.h"

static const char *TAG = "px_spi";

#define SPI_SLOTS_MAX 3

typedef struct {
    nc_pixel_backend_t be;
    nc_pixel_q_t      *q;
    nc_pixel_codec_t  *codec;
    uint32_t           px;
    uint8_t            slots;
    uint8_t           *tx[SPI_SLOTS_MAX];     // 位流（每 byte 一個位）
    uint32_t           bits;                  // 一幀幾個位
    uint32_t           seq[SPI_SLOTS_MAX];
    uint32_t           submitted;
    uint32_t           reclaimed;
    uint32_t           last_seq;
    spi_device_handle_t dev;
    int                next;
    uint32_t           clock_hz;
    nc_px_format_t     fmt;
} spi_t;

static spi_t s_spi;
static nc_pixel_codec_t *s_spi_codec;
// 最大一幀的位數：px × (head + 4B + tail) × 8（上限由 header 的 NC_PX_MAX_LEDS 保證）
static uint8_t s_spi_bits[(NC_PX_MAX_LEDS * 4 + 16) * 8];

static bool spi_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    spi_t *d = (spi_t *)b->ctx;
    const uint8_t *p = nc_px_ring_peek(d->q, NULL, seq);
    if (p == NULL || *seq == post_seq) {
        return false;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        const uint32_t k = (uint32_t)((d->next + i) % d->slots);
        if (d->seq[k] == 0) {
            d->next = (int)((k + 1u) % d->slots);
            d->seq[k] = *seq;
            *slot = k;
            *payload = p;
            *px = d->px;
            return true;
        }
    }
    nc_px_ring_release(d->q, *seq);
    return false;
}

static bool spi_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    spi_t *d = (spi_t *)b->ctx;
    if (slot >= d->slots) {
        return false;
    }
    const uint8_t *p = nc_px_ring_peek(d->q, NULL, NULL);
    if (p == NULL) {
        return false;
    }
    nc_px_codec_frame(d->codec, d->seq[slot]);
    const uint32_t n = nc_px_codec_spi_frame(d->codec, p, d->px, d->tx[slot],
                                             (uint32_t)sizeof(s_spi_bits));
    if (n == 0) {
        return false;
    }
    d->bits = n;
    return true;
}

static bool spi_submit(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq)
{
    spi_t *d = (spi_t *)b->ctx;
    if (slot >= d->slots) {
        return false;
    }
    spi_transaction_t t = {
        .length = d->bits,           // 單位：bit（buffer 每 byte 一個位）
        .tx_buffer = d->tx[slot],
    };
    const esp_err_t e = spi_device_transmit(d->dev, &t);
    if (e != ESP_OK) {
        d->seq[slot] = 0;
        return false;
    }
    d->submitted++;
    d->last_seq = seq;
    return true;
}

static void spi_reclaim(nc_pixel_backend_t *b)
{
    spi_t *d = (spi_t *)b->ctx;
    // spi_device_transmit 是同步的：回來就代表送完了。
    for (uint32_t i = 0; i < d->slots; i++) {
        if (d->seq[i] != 0) {
            nc_px_ring_release(d->q, d->seq[i]);
            d->seq[i] = 0;
            d->reclaimed++;
        }
    }
}

static size_t spi_json(nc_pixel_backend_t *b, char *out, size_t cap)
{
    spi_t *d = (spi_t *)b->ctx;
    const int n = snprintf(out, cap,
                           "\"fmt\":\"%s\",\"px\":%u,\"bits\":%u,\"clk\":%u,"
                           "\"submitted\":%u,\"seq\":%u}",
                           nc_px_format_name(d->fmt), (unsigned)d->px, (unsigned)d->bits,
                           (unsigned)d->clock_hz, (unsigned)d->submitted,
                           (unsigned)d->last_seq);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
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
    spi_t *d = &s_spi;
    memset(d, 0, sizeof(*d));
    d->px = (cfg->px > NC_PX_MAX_LEDS) ? NC_PX_MAX_LEDS : cfg->px;
    d->slots = (uint8_t)((cfg->slots < 2) ? 2u : cfg->slots);
    if (d->slots > SPI_SLOTS_MAX) {
        d->slots = SPI_SLOTS_MAX;
    }
    d->clock_hz = (cfg->clock_hz == 0) ? 8000000u : cfg->clock_hz;
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
    const uint32_t need_bits = nc_px_codec_bits(d->codec);
    if (need_bits == 0 || need_bits * (uint32_t)d->slots > sizeof(s_spi_bits)) {
        ESP_LOGE(TAG, "一幀 %u 位 × %u 槽超出上限 %u", (unsigned)need_bits, (unsigned)d->slots,
                 (unsigned)sizeof(s_spi_bits));
        return NULL;
    }

    if (nc_hw_reserved(cfg->pin_mosi) || nc_hw_reserved(cfg->pin_sclk)) {
        ESP_LOGE(TAG, "SPI 腳位是保留腳（MOSI=%d SCLK=%d）", cfg->pin_mosi, cfg->pin_sclk);
        return NULL;
    }
    (void)nc_hw_claim(cfg->pin_mosi, "pixel", "spi_mosi", NC_HW_CLAIM_WRITE);
    (void)nc_hw_claim(cfg->pin_sclk, "pixel", "spi_sclk", NC_HW_CLAIM_WRITE);

    const spi_host_device_t host = (cfg->host < 0) ? SPI2_HOST : (spi_host_device_t)cfg->host;
    spi_bus_config_t bus = {
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = cfg->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(need_bits / 8u + 8u),
    };
    if (spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        // 已經初始化過（同一條 bus 有兩個消費者）就沿用 —— 屬於 owner 的責任。
        ESP_LOGW(TAG, "spi_bus_initialize 失敗（可能已初始化），嘗試沿用");
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)d->clock_hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = (int)d->slots,
    };
    if (spi_bus_add_device(host, &dev, &d->dev) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device 失敗");
        return NULL;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        d->tx[i] = s_spi_bits + i * need_bits;
    }
    (void)snprintf(d->be.name, sizeof(d->be.name), "spi");
    d->be.lanes = 1;
    d->be.ctx = d;
    d->be.acquire = spi_acquire;
    d->be.encode = spi_encode;
    d->be.submit = spi_submit;
    d->be.reclaim = spi_reclaim;
    d->be.json = spi_json;
    ESP_LOGI(TAG, "SPI 就緒：%s、%u 顆、clk %u Hz、一幀 %u 位",
             nc_px_format_name(cfg->fmt), (unsigned)d->px, (unsigned)d->clock_hz,
             (unsigned)need_bits);
    return &d->be;
}

#else   // !CONFIG_NETCORE_PIXEL_SPI

#include "nc_pixel.h"

nc_pixel_backend_t *nc_px_backend_spi(const nc_px_spi_cfg_t *cfg)
{
    (void)cfg;
    return NULL;
}

#endif  // CONFIG_NETCORE_PIXEL_SPI
