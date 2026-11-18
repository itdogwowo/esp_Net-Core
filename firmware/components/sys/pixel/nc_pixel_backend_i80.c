// nc_pixel_backend_i80.c — I80（LCD_CAM 的 8080 並口）後端：用 GDMA 推波形。
//
// 為什麼是 I80（而不是 RMT 或 SPI）：
//   · S3 的 LCD_CAM 把「一個 PCLK 一個 byte」推到 D0..D7 **八條資料線**，
//     等於一次送八條 lane（16 lane 燈板 = 兩顆 I80）。
//   · 波形由 GDMA 推，CPU 只負責編碼；實測一幀 1.4–2.0 ms、~450 fps（見 rgb-matrix-dither.md）。
//   · RMT 每條 lane 一個 channel（S3 只有 8 個），而且 RMT DMA 讀不到 PSRAM。
//
// 時序（一個 WS2812 bit = bit_cells 個 PCLK 週期）：
//   2.5 MHz × 3 格 ⇒ bit 1.200 µs、T0H 0.400 µs（1 格）、T1H 0.800 µs（2 格）
//   8.889 MHz × 8 格 ⇒ bit 0.900 µs、T0H 0.300 µs、T1H 0.600 µs（約 450 fps）
// 兩個都只是 cfg 值；wave 由共用 codec 產生，所以和 direct 後端發出來的完全一樣。
//
// 多 lane 合成：codec 每條 lane 輸出一串「單元」（每單元 1 byte，0/1）。
//   這裡把 lane0..laneN 的單元 OR 起來 → 一個 I80 byte（bit0 = D0 = lane0 的資料）。
//   各 lane 時序完全同步（同 bit_cells、同 reset），所以不需要額外對齊。
//   **這是唯一需要理解多 lane 的地方**；編碼、環、節奏都不用改。
#include <sdkconfig.h>

#if CONFIG_NETCORE_PIXEL_I80

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nc_hw.h"
#include "nc_pixel.h"

static const char *TAG = "px_i80";

#define I80_SLOTS_MAX 3
#define I80_LANES_MAX NC_PX_UNITS_PER_CELL     // 一個 DMA byte 塞 8 條 lane

typedef struct {
    nc_pixel_backend_t be;
    nc_pixel_q_t      *q;
    nc_pixel_codec_t  *codec;
    uint32_t           px;
    uint8_t            lanes;
    uint8_t            slots;
    uint32_t           units;              // 含 reset（= 一個 DMA 槽的大小）
    uint8_t           *tx[I80_SLOTS_MAX];
    uint32_t           seq[I80_SLOTS_MAX]; // 這個槽被哪一格佔住（0 = 空閒）
    uint32_t           done_cnt[I80_SLOTS_MAX];
    uint32_t           in_flight;
    uint32_t           submitted;
    uint32_t           done_evt;
    uint32_t           submit_fail;
    uint32_t           last_seq;
    uint32_t           pclk_hz;
    int                next;
    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t  done;
} i80_t;

static i80_t s_i80;
static nc_pixel_codec_t *s_i80_codec[I80_LANES_MAX];
static uint8_t s_i80_lane[I80_LANES_MAX][NC_PX_UNITS_MAX];
static uint8_t s_i80_mix[NC_PX_UNITS_MAX];

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    i80_t *d = (i80_t *)ctx;
    // 完成的次序 = 提交的次序（esp_lcd 對同一 panel_io 是 FIFO）；
    // 把「最早一個還在飛」的槽標記完成。
    for (uint32_t i = 0; i < I80_SLOTS_MAX; i++) {
        if (d->seq[i] != 0 && d->done_cnt[i] == 0) {
            d->done_cnt[i] = 1;
            break;
        }
    }
    d->done_evt++;
    BaseType_t hp = pdFALSE;
    if (d->done != NULL) {
        xSemaphoreGiveFromISR(d->done, &hp);
    }
    return hp == pdTRUE;
}

static bool i80_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    i80_t *d = (i80_t *)b->ctx;
    const uint8_t *p = nc_px_ring_peek(d->q, NULL, seq);
    if (p == NULL || *seq == post_seq) {
        return false;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        const uint32_t k = (uint32_t)((d->next + i) % d->slots);
        if (d->seq[k] == 0) {
            d->next = (int)((k + 1u) % d->slots);
            d->seq[k] = *seq;
            d->done_cnt[k] = 0;
            *slot = k;
            *payload = p;
            *px = d->px;
            return true;
        }
    }
    // 全部在飛：本輪就不要了（下一輪 reclaim 之後再試）。
    nc_px_ring_release(d->q, *seq);
    return false;
}

static bool i80_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    i80_t *d = (i80_t *)b->ctx;
    if (slot >= d->slots) {
        return false;
    }
    const uint8_t *p = nc_px_ring_peek(d->q, NULL, NULL);
    if (p == NULL) {
        return false;
    }
    const uint32_t seq = d->seq[slot];
    for (uint8_t ln = 0; ln < d->lanes; ln++) {
        nc_px_codec_frame(d->codec[ln], seq);
        if (nc_px_codec_wave_frame(d->codec[ln], p, d->px, s_i80_lane[ln], NC_PX_UNITS_MAX) == 0) {
            return false;
        }
    }
    // 合成：lane0 → bit0、lane1 → bit1 …（I80 byte 的位元就是資料線）
    uint8_t *out = d->tx[slot];
    for (uint32_t u = 0; u < d->units; u++) {
        uint8_t m = 0;
        for (uint8_t ln = 0; ln < d->lanes; ln++) {
            m |= (uint8_t)(s_i80_lane[ln][u] << ln);
        }
        out[u] = m;
    }
    return true;
}

static bool i80_submit(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq)
{
    i80_t *d = (i80_t *)b->ctx;
    if (slot >= d->slots) {
        return false;
    }
    // esp_lcd 的 tx_color 排進佇列就返回，完成時才回呼 —— 所以可以連續提交幾格。
    const esp_err_t e = esp_lcd_panel_io_tx_color(d->io, -1, d->tx[slot], d->units);
    if (e != ESP_OK) {
        d->seq[slot] = 0;             // 沒有提交：立刻還格
        d->submit_fail++;
        return false;
    }
    d->in_flight++;
    d->submitted++;
    d->last_seq = seq;
    return true;
}

static void i80_reclaim(nc_pixel_backend_t *b)
{
    i80_t *d = (i80_t *)b->ctx;
    for (uint32_t i = 0; i < d->slots; i++) {
        if (d->seq[i] != 0 && d->done_cnt[i] != 0) {
            // DMA 完成了才可以還格（P6 陷阱 1：提前還 = torn frame）
            nc_px_ring_release(d->q, d->seq[i]);
            d->seq[i] = 0;
            d->done_cnt[i] = 0;
            if (d->in_flight > 0) {
                d->in_flight--;
            }
        }
    }
}

static size_t i80_json(nc_pixel_backend_t *b, char *out, size_t cap)
{
    i80_t *d = (i80_t *)b->ctx;
    const int n = snprintf(out, cap,
                           "\"lanes\":%u,\"px\":%u,\"units\":%u,\"slots\":%u,"
                           "\"submitted\":%u,\"done\":%u,\"fail\":%u,\"pclk\":%u,\"seq\":%u}",
                           (unsigned)d->lanes, (unsigned)d->px, (unsigned)d->units,
                           (unsigned)d->slots, (unsigned)d->submitted, (unsigned)d->done_evt,
                           (unsigned)d->submit_fail, (unsigned)d->pclk_hz,
                           (unsigned)d->last_seq);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

nc_pixel_backend_t *nc_px_backend_i80(const nc_px_i80_cfg_t *cfg)
{
    if (cfg == NULL || cfg->pins == NULL || cfg->px == 0) {
        return NULL;
    }
    i80_t *d = &s_i80;
    memset(d, 0, sizeof(*d));
    d->lanes = (uint8_t)((cfg->lanes == 0) ? 1u : cfg->lanes);
    if (d->lanes > I80_LANES_MAX) {
        d->lanes = I80_LANES_MAX;
    }
    d->slots = (uint8_t)((cfg->slots < 2) ? 2u : cfg->slots);
    if (d->slots > I80_SLOTS_MAX) {
        d->slots = I80_SLOTS_MAX;
    }
    d->px = (cfg->px > NC_PX_MAX_LEDS) ? NC_PX_MAX_LEDS : cfg->px;

    nc_px_codec_cfg_t cc = {
        .fmt = cfg->fmt,
        .wave = cfg->wave,
        .bits = (uint8_t)((cfg->dither_bits < 8) ? 8 : cfg->dither_bits),
        .dither_mode = cfg->dither_mode,
        .zone_px = cfg->zone_px,
        .px = d->px,
        .bright = 255,
    };
    if (cc.wave.bit_cells == 0) {
        nc_px_wave_default(cfg->fmt, &cc.wave);
    }
    if (cfg->pclk_hz != 0) {
        cc.wave.clk_hz = cfg->pclk_hz;
    }
    d->codec = nc_px_codec_init(nc_px_codec_new(), &cc);
    if (d->codec == NULL) {
        return NULL;
    }
    for (uint8_t ln = 1; ln < d->lanes; ln++) {
        s_i80_codec[ln] = nc_px_codec_init(nc_px_codec_new(), &cc);
    }
    d->units = nc_px_codec_total_units(d->codec);
    if (d->units == 0 || d->units > NC_PX_UNITS_MAX) {
        return NULL;
    }
    d->pclk_hz = cc.wave.clk_hz;

    // ① pins：D0..D(lanes-1) 必填；D(lanes)..D7 用 D0 代替（I80 只要求「是有效 GPIO」）
    for (int i = 0; i < cfg->lanes; i++) {
        if (cfg->pins[i] < 0 || cfg->pins[0] < 0) {
            return NULL;
        }
    }
    for (int i = 0; i < (int)d->lanes; i++) {
        if (nc_hw_reserved(cfg->pins[i])) {
            ESP_LOGE(TAG, "D%d = GPIO%d 是保留腳 → 換腳", i, cfg->pins[i]);
            return NULL;
        }
        (void)nc_hw_claim(cfg->pins[i], "pixel", "i80_d", NC_HW_CLAIM_WRITE);
    }
    const int wr = cfg->pins[d->lanes];
    const int dc = cfg->pins[d->lanes + 1];
    if (wr >= 0) {
        (void)nc_hw_claim(wr, "pixel", "i80_wr", NC_HW_CLAIM_WRITE);
    }
    if (dc >= 0) {
        (void)nc_hw_claim(dc, "pixel", "i80_dc", NC_HW_CLAIM_WRITE);
    }

    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = (gpio_num_t)dc,
        .wr_gpio_num = (gpio_num_t)wr,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .bus_width = 8,
        .max_transfer_bytes = d->units,
        .dma_burst_size = 0,
    };
    for (int i = 0; i < 8; i++) {
        // 沒有用到的資料線一律重複 D0：同一支腳重複填是合法的（已經上板驗證過 dummy 腳）。
        bus_cfg.data_gpio_nums[i] = (gpio_num_t)cfg->pins[(i < d->lanes) ? i : 0];
    }
    if (esp_lcd_new_i80_bus(&bus_cfg, &d->bus) != ESP_OK) {
        ESP_LOGE(TAG, "I80 bus 建立失敗");
        return NULL;
    }
    d->done = xSemaphoreCreateBinary();
    if (d->done == NULL) {
        return NULL;
    }
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1,
        .pclk_hz = cc.wave.clk_hz,
        .trans_queue_depth = (int)d->slots + 1,
        .on_color_trans_done = on_color_done,
        .user_ctx = d,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = { 0, 0, 0, 0 },
        .flags = { .pclk_active_neg = 0, .pclk_idle_low = 1 },
    };
    if (esp_lcd_new_panel_io_i80(d->bus, &io_cfg, &d->io) != ESP_OK) {
        ESP_LOGE(TAG, "I80 panel IO 建立失敗");
        return NULL;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        d->tx[i] = heap_caps_calloc(1, d->units, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (d->tx[i] == NULL) {
            ESP_LOGE(TAG, "DMA 槽 %u（%u B）配置失敗", (unsigned)i, (unsigned)d->units);
            return NULL;
        }
    }
    (void)snprintf(d->be.name, sizeof(d->be.name), "i80");
    d->be.lanes = d->lanes;
    d->be.ctx = d;
    d->be.acquire = i80_acquire;
    d->be.encode = i80_encode;
    d->be.submit = i80_submit;
    d->be.reclaim = i80_reclaim;
    d->be.json = i80_json;
    ESP_LOGI(TAG, "I80 就緒：%u lane × %u 顆、pclk %u Hz、一幀 %u B（%.0f µs）",
             (unsigned)d->lanes, (unsigned)d->px, (unsigned)cc.wave.clk_hz, (unsigned)d->units,
             (double)d->units * 1000000.0 / (double)cc.wave.clk_hz);
    return &d->be;
}

#else   // !CONFIG_NETCORE_PIXEL_I80

#include "nc_pixel.h"

// 這塊板沒開 I80：回 NULL，渲染器會退到下一個後端。
nc_pixel_backend_t *nc_px_backend_i80(const nc_px_i80_cfg_t *cfg)
{
    (void)cfg;
    return NULL;
}

#endif  // CONFIG_NETCORE_PIXEL_I80
