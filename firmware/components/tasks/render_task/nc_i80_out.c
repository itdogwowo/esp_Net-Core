// nc_i80_out.c — I80 輸出轉接層（renderer 側；唔含 pixel 命名、唔碰 driver 內部）。
//
// 分層（2026-11-19 更名）：
//   · driver/drv_i80：I80 bus／PCLK／DMA 槽／完成回呼 —— 通用，完全唔識 pixel
//   · 這一支：I80 輸出政策（lane 合成、腳位 claim、slot↔seq、環的 acquire/release）
//   · sw/pixel：環＋編碼器＋效果；render_task：節拍與後端編排
//   · 新增第二個 I80 使用者時，改這裡與 drv_i80，不用動編碼器
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

static const char *TAG = "i80_out";

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
    const uint8_t      *pay[I80_SLOTS_MAX]; // 這一格屬於環的哪一份 payload
    uint32_t            seq[I80_SLOTS_MAX]; // 這個槽被哪一格佔住（0 = 空閒）
    int                 next;
} i80_be_t;

static i80_be_t s_be;

static bool i80_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                        const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    i80_be_t *d = (i80_be_t *)b->ctx;
    // 取格政策見 nc_px_ring_take()：多子幀＝逐格 FIFO，單子幀＝最新優先。
    const uint8_t *p = nc_px_ring_take(b->ring, post_seq, NULL, seq);
    if (p == NULL) {
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
    // mailbox 要還格（否則阻住最新格）；FIFO 唔還（下一輪照樣攞返同一格）。
    nc_px_ring_untake(b->ring, *seq);
    return false;
}

static bool i80_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    i80_be_t *d = (i80_be_t *)b->ctx;
    if (slot >= d->slots || d->pay[slot] == NULL) {
        return false;
    }
    const uint8_t *p = d->pay[slot];
    const uint32_t seq = d->seq[slot];
    uint8_t *out = nc_i80_drv_slot_buf((int)slot);
    if (out == NULL) {
        return false;
    }
    for (uint8_t k = 0; k < d->lanes; k++) {
        if (d->codec[k] == NULL) {
            return false;
        }
        nc_px_codec_frame(d->codec[k], seq);
    }
    // 單 lane／8-bit：直接用「線材 byte → 波形 LUT」的 memcpy 路徑。
    // 廣播路徑係逐 tick 寫；1 lane 時舊 merge8 一次寫 8 tick 反而快（實測 64 px：
    // pack_bus 840 µs vs 舊路徑 357 µs）。所以 1 lane 保留最直接的那條。
    if (d->lanes == 1 && d->bus_width == 8) {
        return nc_px_codec_wave_frame(d->codec[0], p, d->px, out, d->units) != 0;
    }
    // Task 6：一次過「展開＋轉置」，直接寫 DMA 槽；唔再經每條 lane 的 packed
    // 中間緩衝（16×600 舊路徑 16 × 5,400 B ＝ 86.4 KB 寫＋讀，新路徑 0）。
    // 8-bit／無抖動行廣播快路徑；12-bit dither 行完整 8×8 轉置（host 逐 byte 對拍）。
    return nc_px_codec_pack_bus(d->codec, p, d->px, out, d->lanes, d->bus_width) != 0;
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
    d->px = cfg->px;

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
    if (d->units == 0) {
        ESP_LOGE(TAG, "一幀 0 B（px=%u）", (unsigned)d->px);
        i80_be_free(d);
        return NULL;
    }
    // Task 6：一次過轉置，唔再配置每條 lane 的 packed 中間緩衝
    //（16×600 舊版 8 × 5,400 B ＝ 43.2 KB PSRAM；新版 0）。
    ESP_LOGI(TAG, "預算：px=%u lanes=%u bus=%u-bit → 一次過轉置（冇 packed 中間緩衝）、"
                  "DMA 槽 %u B × %u（internal）",
             (unsigned)d->px, (unsigned)d->lanes, (unsigned)d->bus_width,
             (unsigned)(d->units * (uint32_t)(d->bus_width / 8)), (unsigned)d->slots);

    // ① 政策：要用的腳都要有效、唔可以重複、唔可以踩保留腳。
    //    GPIO3 在 S3 是 strapping（JTAG signal source），但 boot 之後當 WS2812 資料線
    //    冇問題（LED 的 DIN 是高阻抗輸入）；LED Textkit（GPIO1–5）要用它，所以只准
    //    這一個例外，其餘保留腳照樣擋。
    //
    //    DIRECT 版只 route 真正接燈的 lane → 只需要檢查／claim lanes 支；
    //    esp_lcd 版仍然要 bus_width+2 支（data＋WR＋DC）。
#if CONFIG_NETCORE_PIXEL_I80_DIRECT
    const int pin_n = (int)d->lanes;
#else
    const int pin_n = (int)d->bus_width + 2;
#endif
    for (int i = 0; i < pin_n; i++) {
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
    for (int i = 0; i < pin_n; i++) {
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
#if CONFIG_NETCORE_PIXEL_I80_DIRECT
    // 直驅：只給 lane_pins（lanes 支）；WR／DC／dummy 由驅動層完全忽略
    const nc_i80_drv_cfg_t dc = {
        .pins = NULL,
        .lane_pins = cfg->pins,
        .lanes = d->lanes,
        .pclk_hz = cc.wave.clk_hz,
#else
    const nc_i80_drv_cfg_t dc = {
        .pins = cfg->pins,
        .pclk_hz = cc.wave.clk_hz,
#endif
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
