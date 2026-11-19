// nc_pixel_backend_i80.c — 渲染器 ↔ I80 驅動的轉接層（task 側）。
//
// 分工（2026-11-18 分層）：
//   · driver/drv_i80：I80 bus／PCLK／DMA 槽／完成回呼（唔知像素、唔知環）
//   · 這一支：政策（腳位保留／claim、GPIO3 例外）、codec、lane 合成、環的
//     acquire/release、slot ↔ seq 對應
//   · sw/pixel：環＋編碼器＋效果
//
// 多 lane 合成：codec 每條 lane 輸出一串「單元」（每單元 1 byte，0/1）。
//   這裡把 lane0..laneN 的單元 OR 起來 → 一個 I80 byte（bit0 = D0 = lane0 的資料）。
//   各 lane 時序完全同步（同 bit_cells、同 reset），所以不需要額外對齊。
#include <sdkconfig.h>

#if CONFIG_NETCORE_PIXEL_I80

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nc_hw.h"
#include "nc_i80_drv.h"
#include "nc_render.h"

static const char *TAG = "px_i80";

#define I80_SLOTS_MAX NC_I80_DRV_SLOTS_MAX
#define I80_LANES_MAX 16                       // 8-bit = 8、16-bit = 16
#define I80_PINS_MAX  18                       // D0..D15、WR、DC（16-bit 模式）

typedef struct {
    nc_pixel_backend_t  be;
    nc_pixel_codec_t   *codec[I80_LANES_MAX]; // 每條 lane 一個（各自的抖動狀態）
    uint32_t            px;
    uint32_t            units;              // 含 reset（= 一個 DMA 槽的大小）
    uint8_t             lanes;
    uint8_t             bus_width;           // 8 或 16
    uint8_t             slots;
    uint32_t            pack_bytes;         // 每條 lane 的 packed 波形 bytes
    uint8_t            *pack[8];            // 每次 8 條 lane 一組
    const uint8_t      *pay[I80_SLOTS_MAX]; // 這一格屬於環的哪一份 payload
    uint32_t            seq[I80_SLOTS_MAX]; // 這個槽被哪一格佔住（0 = 空閒）
    int                 next;
} i80_be_t;

static i80_be_t s_be;

static bool i80_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    i80_be_t *d = (i80_be_t *)b->ctx;
    const uint8_t *p = nc_px_ring_peek(b->ring, NULL, seq);
    if (p == NULL || *seq == post_seq) {
        return false;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        const uint32_t k = (uint32_t)((d->next + i) % d->slots);
        if (d->seq[k] == 0) {
            d->next = (int)((k + 1u) % d->slots);
            d->seq[k] = *seq;
            d->pay[k] = p;          // 這一格在 release 之前不會被 writer 覆寫
            *slot = k;
            *payload = p;
            *px = d->px;
            return true;
        }
    }
    // 全部在飛：本輪就不要了（下一輪 reclaim 之後再試）。
    nc_px_ring_release(b->ring, *seq);
    return false;
}

static bool i80_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    i80_be_t *d = (i80_be_t *)b->ctx;
    if (slot >= d->slots || d->pay[slot] == NULL || d->pack[0] == NULL) {
        return false;
    }
    const uint8_t *p = d->pay[slot];
    const uint32_t seq = d->seq[slot];
    uint8_t *out = nc_i80_drv_slot_buf((int)slot);
    if (out == NULL) {
        return false;
    }
    const uint32_t stride = (d->bus_width == 16) ? 2u : 1u;
    // 唔再每幀 memset 整格：第一組（lane 0–7）用 store 直接把整格寫死（未用的 lane
    // 交 NULL → 寫 0），第二組（16-bit 的 lane 8–15）才 OR 進高位 byte。
    // 16×600 一幀 86,800 B，省掉 memset 就是省 ~2–3.6 ms（PSRAM 寫入）。
    // 每 8 條 lane 一組：packed 波形（bitstream）→ 8×8 轉置 → I80 byte-per-unit。
    for (uint8_t g = 0; g < d->lanes; g += 8) {
        const uint8_t n = (uint8_t)((d->lanes - g > 8) ? 8 : (d->lanes - g));
        for (uint8_t k = 0; k < n; k++) {
            nc_pixel_codec_t *c = d->codec[g + k];
            if (c == NULL) {
                return false;
            }
            nc_px_codec_frame(c, seq);
            if (nc_px_codec_wave_pack(c, p, d->px, d->pack[k], d->pack_bytes) == 0) {
                return false;
            }
        }
        // 未用到的 lane 一定要交 NULL：malloc 的緩衝有舊資料，否則會 OR 到
        // D6/D7（dummy）甚至下一組 lane 的位元。
        const uint8_t *lp[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
        for (uint8_t k = 0; k < n; k++) {
            lp[k] = d->pack[k];
        }
        const uint32_t off = (d->bus_width == 16 && g >= 8) ? 1u : 0u;
        if (g == 0) {
            nc_px_codec_pack_merge8_store(lp, d->pack_bytes, out, stride, off);
        } else {
            nc_px_codec_pack_merge8(lp, d->pack_bytes, out, stride, off);
        }
    }
    return true;
}

static bool i80_submit(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq)
{
    (void)seq;
    i80_be_t *d = (i80_be_t *)b->ctx;
    if (!nc_i80_drv_submit((int)slot)) {
        if (slot < d->slots) {
            d->seq[slot] = 0;
            d->pay[slot] = NULL;
        }
        return false;
    }
    return true;
}

static void i80_reclaim(nc_pixel_backend_t *b)
{
    i80_be_t *d = (i80_be_t *)b->ctx;
    for (uint32_t i = 0; i < d->slots; i++) {
        if (d->seq[i] != 0 && nc_i80_drv_slot_done((int)i)) {
            // DMA 完成了才可以還格（P6 陷阱 1：提前還 = torn frame）
            nc_px_ring_release(b->ring, d->seq[i]);
            d->seq[i] = 0;
            d->pay[i] = NULL;
        }
    }
}

static size_t i80_json(nc_pixel_backend_t *b, char *out, size_t cap)
{
    i80_be_t *d = (i80_be_t *)b->ctx;
    const int n = snprintf(out, cap,
                           "\"lanes\":%u,\"width\":%u,\"px\":%u,\"units\":%u,\"slots\":%u,"
                           "\"submitted\":%u,\"done\":%u,\"fail\":%u,\"inflight\":%u,"
                           "\"pclk\":%u}",
                           (unsigned)d->lanes, (unsigned)d->bus_width, (unsigned)d->px,
                           (unsigned)d->units, (unsigned)d->slots, (unsigned)nc_i80_drv_submitted(),
                           (unsigned)nc_i80_drv_done_count(), (unsigned)nc_i80_drv_fail(),
                           (unsigned)nc_i80_drv_inflight(), (unsigned)nc_i80_drv_pclk());
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void i80_be_free(i80_be_t *d)
{
    for (int ln = 0; ln < I80_LANES_MAX; ln++) {
        if (d->codec[ln] != NULL) {
            nc_px_codec_free(d->codec[ln]);
            d->codec[ln] = NULL;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (d->pack[i] != NULL) {
            heap_caps_free(d->pack[i]);
            d->pack[i] = NULL;
        }
    }
    nc_i80_drv_uninstall();
}

nc_pixel_backend_t *nc_px_backend_i80(const nc_px_i80_cfg_t *cfg)
{
    if (cfg == NULL || cfg->pins == NULL || cfg->px == 0) {
        return NULL;
    }
    i80_be_t *d = &s_be;
    i80_be_free(d);
    memset(d, 0, sizeof(*d));
    d->bus_width = (cfg->bus_width == 16) ? 16u : 8u;
    d->lanes = (uint8_t)((cfg->lanes == 0) ? 1u : cfg->lanes);
    if (d->lanes > d->bus_width) {
        d->lanes = d->bus_width;               // 8-bit 最多 8 lane、16-bit 最多 16
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
    for (uint8_t ln = 0; ln < d->lanes; ln++) {
        d->codec[ln] = nc_px_codec_init(nc_px_codec_new(), &cc);
        if (d->codec[ln] == NULL) {
            i80_be_free(d);
            return NULL;
        }
    }
    d->units = nc_px_codec_total_units(d->codec[0]);
    if (d->units == 0 || d->units > NC_PX_UNITS_MAX) {
        ESP_LOGE(TAG, "一幀 %u B 超出上限 %u", (unsigned)d->units, (unsigned)NC_PX_UNITS_MAX);
        i80_be_free(d);
        return NULL;
    }
    // packed 波形：px × lane_bytes × bit_cells bytes（每條 lane）；8 條一組去轉置。
    d->pack_bytes = d->px * (uint32_t)nc_px_codec_lane_bytes(d->codec[0]) * cc.wave.bit_cells;
    for (int i = 0; i < 8; i++) {
        d->pack[i] = heap_caps_malloc(d->pack_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (d->pack[i] == NULL) {
            // internal 放唔落就退 PSRAM（CPU 讀寫，DMA 唔會碰）
            d->pack[i] = heap_caps_malloc(d->pack_bytes, MALLOC_CAP_SPIRAM);
        }
        if (d->pack[i] == NULL) {
            ESP_LOGE(TAG, "packed 波形緩衝（%u B × 8）配置失敗", (unsigned)d->pack_bytes);
            i80_be_free(d);
            return NULL;
        }
    }

    // ① 政策：10 個腳位都要有效、唔可以重複、唔可以踩保留腳。
    //    GPIO3 在 S3 是 strapping（JTAG signal source），但 boot 之後當 WS2812 資料線
    //    冇問題（LED 的 DIN 是高阻抗輸入）；LED Textkit（GPIO1–5）要用它，所以只准
    //    這一個例外，其餘保留腳照樣擋。
    for (int i = 0; i < (int)d->bus_width + 2; i++) {
        if (cfg->pins[i] < 0 || cfg->pins[i] > 48) {
            ESP_LOGE(TAG, "I80 腳位第 %d 個 = %d 不是有效 GPIO", i + 1, cfg->pins[i]);
            i80_be_free(d);
            return NULL;
        }
        const bool lane_pin = (i < d->lanes);
        const bool allow_strap = lane_pin && cfg->pins[i] == 3;
        if (nc_hw_reserved(cfg->pins[i])) {
            if (!allow_strap) {
                ESP_LOGE(TAG, "GPIO%d 是保留腳（PSRAM／flash／USB／console／strapping）→ 換腳",
                         cfg->pins[i]);
                i80_be_free(d);
                return NULL;
            }
            ESP_LOGW(TAG, "GPIO3 是 strapping 腳，但當 I80 資料線使用（LED Textkit）");
        }
        for (int j = 0; j < i; j++) {
            if (cfg->pins[j] == cfg->pins[i]) {
                ESP_LOGE(TAG, "GPIO%d 重複（第 %d 與第 %d 個）", cfg->pins[i], j + 1, i + 1);
                i80_be_free(d);
                return NULL;
            }
        }
    }
    for (int i = 0; i < (int)d->bus_width + 2; i++) {
        const bool is_wr = (i == d->bus_width);
        const bool is_dc = (i == d->bus_width + 1);
        const char *label = is_wr ? "i80_wr" : (is_dc ? "i80_dc" : "i80_d");
        if (nc_hw_reserved(cfg->pins[i])) {
            continue;               // 上面的例外（GPIO3）：nc_hw 一定會拒絕，唔 claim
        }
        // dummy 腳被其他 owner 佔用只是少幾條沒用的資料線 → 警告，不擋整條 lane。
        if (nc_hw_claim(cfg->pins[i], "pixel", label, NC_HW_CLAIM_WRITE) != ESP_OK) {
            if (i < d->lanes || is_wr || is_dc) {
                ESP_LOGW(TAG, "GPIO%d（%s）已被其他 owner 佔用", cfg->pins[i], label);
            }
        }
    }

    // ② 硬體交給 driver：bus／PCLK／DMA 槽／完成回呼都在那一層。
    const nc_i80_drv_cfg_t dc = {
        .pins = cfg->pins,
        .pclk_hz = cc.wave.clk_hz,
        .slot_bytes = d->units * (uint32_t)(d->bus_width / 8),
        .slots = cfg->slots,
        .bus_width = d->bus_width,
    };
    if (nc_i80_drv_install(&dc) != ESP_OK) {
        ESP_LOGE(TAG, "I80 driver 安裝失敗");
        i80_be_free(d);
        return NULL;
    }
    d->slots = (uint8_t)nc_i80_drv_slots();


    (void)snprintf(d->be.name, sizeof(d->be.name), "i80");
    d->be.lanes = d->lanes;
    d->be.width = d->bus_width;
    d->be.ctx = d;
    d->be.acquire = i80_acquire;
    d->be.encode = i80_encode;
    d->be.submit = i80_submit;
    d->be.reclaim = i80_reclaim;
    d->be.json = i80_json;
    ESP_LOGI(TAG, "I80 adapter 就緒：%u-bit、%u lane × %u 顆、一幀 %u B、%u 槽",
             (unsigned)d->bus_width, (unsigned)d->lanes, (unsigned)d->px,
             (unsigned)(d->units * (d->bus_width / 8)), (unsigned)d->slots);
    return &d->be;
}

#else   // !CONFIG_NETCORE_PIXEL_I80

#include "nc_render.h"

// 這塊板沒開 I80：回 NULL，渲染器會退到下一個後端。
nc_pixel_backend_t *nc_px_backend_i80(const nc_px_i80_cfg_t *cfg)
{
    (void)cfg;
    return NULL;
}

#endif  // CONFIG_NETCORE_PIXEL_I80
