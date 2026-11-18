// nc_pixel_encode — canonical R,G,B,W → 每個驅動的物理格式。
//
// **這一整段只有一份**：I80、SPI、direct 三個後端共用同一個編碼器。
// 所以「同一幀在三個後端上發出來的位元組一定一樣」可以用 host 測試證明
// （tools/hosttest/pixel_test）。後端只負責「怎麼送」，不負責「轉換」。
//
// 兩層：
//   ① 每顆燈：R,G,B,W → 線材位元組（3 或 4 B，次序由格式表決定）
//      抖動在這裡做：canonical 是 8-bit，WS2812 也只有 8-bit PWM；
//      想更深灰階就靠「每幀誤差回饋」，長期平均等於想要的值（8.8 定點）。
//      抖動狀態是**每區每通道**，不是每顆 —— 同一區顏色相同的燈會拿到同一串位元組，
//      所以「先 encode_px、再快取」是合法的，而且常常只算到幾顆。
//   ② 每顆燈 → 物理樣本
//      歸零碼（WS2812/SK6812/WS2815）：一個 bit = bit_cells 個時序單元，
//        前 t0h／t1h 個單元是高電平。I80 一個單元 = 一個 PCLK 的一個 byte
//        （bit0 對應 D0）；direct 一個單元 = 一個位。
//      SPI（WS2801/APA102）：一個 bit = 一個時脈。
//
// 多 lane：這裡輸出「單 lane 的單元」（每單元 1 byte，值 0/1）。
// 後端把 lane0..laneN 的單元 OR 起來就得到 I80 的一個 byte（D0..D7 各一條 lane）——
// 因為每條 lane 的時序完全同步（同一個 bit_cells、同一個 reset），不需要額外對齊。
#include "nc_pixel_core.h"

#include <stdlib.h>
#include <string.h>

// 編碼器本體（只在這一檔看得見）
struct nc_pixel_codec {
    nc_px_codec_cfg_t         cfg;
    const nc_px_formatspec_t *spec;
    uint32_t                  px;
    uint32_t                  zones;
    uint32_t                  frame;
    int32_t                   acc[NC_PX_CODEC_ZONES_MAX][4];
};

// ── 格式表 ──
// order[]：index = 通道（0=R 1=G 2=B 3=W 4=APA102 亮度頭），值 = 線材上的位置；0xFF = 沒有。
//   WS2812 / WS2815 是 GRB（W 丟棄）；WS2812W 是 GRBW；
//   APA102 是「亮度頭, B, G, R」；WS2801 的資料次序由接線決定，預設 RGB。
static const nc_px_formatspec_t s_fmt[NC_PX_FMT_COUNT] = {
    [NC_PX_FMT_WS2812] = {
        .clk = false, .bit_latch = false, .head_bytes = 0, .tail_bytes = 0, .lane_bytes = 3,
        .order = { 1, 0, 2, 0xFF, 0xFF }, .reset_us_hint = 80,
    },
    [NC_PX_FMT_WS2812W] = {
        .clk = false, .bit_latch = false, .head_bytes = 0, .tail_bytes = 0, .lane_bytes = 4,
        .order = { 1, 0, 2, 3, 0xFF }, .reset_us_hint = 80,
    },
    [NC_PX_FMT_WS2815] = {
        .clk = false, .bit_latch = false, .head_bytes = 0, .tail_bytes = 0, .lane_bytes = 3,
        .order = { 1, 0, 2, 0xFF, 0xFF }, .reset_us_hint = 280,
    },
    [NC_PX_FMT_WS2801] = {
        .clk = true, .bit_latch = false, .head_bytes = 0, .tail_bytes = 0, .lane_bytes = 3,
        .order = { 0, 1, 2, 0xFF, 0xFF }, .reset_us_hint = 500,
    },
    [NC_PX_FMT_APA102] = {
        .clk = true, .bit_latch = true, .head_bytes = 4, .tail_bytes = 4, .lane_bytes = 4,
        .order = { 2, 1, 0, 0xFF, 0 }, .reset_us_hint = 0,
    },
};

static const char *const s_fmt_name[NC_PX_FMT_COUNT] = {
    "ws2812", "ws2812w", "ws2815", "ws2801", "apa102",
};

const nc_px_formatspec_t *nc_px_format_spec(nc_px_format_t f)
{
    if ((int)f < 0 || (int)f >= NC_PX_FMT_COUNT) {
        return NULL;
    }
    return &s_fmt[f];
}

const char *nc_px_format_name(nc_px_format_t f)
{
    if ((int)f < 0 || (int)f >= NC_PX_FMT_COUNT) {
        return "?";
    }
    return s_fmt_name[f];
}

bool nc_px_format_parse(const char *s, nc_px_format_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < NC_PX_FMT_COUNT; i++) {
        if (strcmp(s, s_fmt_name[i]) == 0) {
            *out = (nc_px_format_t)i;
            return true;
        }
    }
    // 同族別名：時序相同的晶片共用一個格式（差別只在 W 與 reset）
    if (strcmp(s, "sk6812") == 0 || strcmp(s, "ws2814") == 0 || strcmp(s, "rgbw") == 0) {
        *out = NC_PX_FMT_WS2812W;
        return true;
    }
    if (strcmp(s, "rgb") == 0 || strcmp(s, "ws2811") == 0 || strcmp(s, "ws2813") == 0 ||
        strcmp(s, "ws2818") == 0) {
        *out = NC_PX_FMT_WS2812;
        return true;
    }
    return false;
}

void nc_px_wave_default(nc_px_format_t f, nc_px_wave_cfg_t *out)
{
    if (out == NULL) {
        return;
    }
    // 保守時序（對齊 rgb-matrix-dither.md 上板實測那組）：
    //   2.5 MHz × 3 個單元 = bit 1.200 µs、T0H 0.400 µs、T1H 0.800 µs。
    // 要 ~450 fps 就把 clk_hz 升到 8.889 MHz 並把 bit_cells 改成 8
    // （bit 0.900 µs、T0H 337 ns、T1H 675 ns）—— 兩者都只是 config 值。
    out->clk_hz = 2500000;
    out->t0h = 1;
    out->t1h = 2;
    out->bit_cells = 3;
    (void)f;
}

// ── 編碼器 ──

static uint8_t dither_ch(uint8_t v, uint8_t bits, int32_t *acc, uint32_t mode)
{
    if (mode == 0 || bits <= 8 || v == 0 || v == 255) {
        *acc = 0;
        return v;
    }
    if (bits > 16) {
        bits = 16;
    }
    // 8-bit → 8.8 想要值；先量化到 bits 位（模擬「只有 bits 灰階」的來源）
    const uint32_t step = 1u << (16u - bits);
    const uint32_t tgt = ((uint32_t)v << 8);
    const int32_t q = (int32_t)(((tgt + step / 2u) / step) * step);
    // 再用誤差回饋把差補回 8-bit
    const int32_t want = q + *acc;
    int32_t o = want >> 8;
    if (o < 0) {
        o = 0;
    } else if (o > 255) {
        o = 255;
    }
    // 修正：誤差 = 想要值 − 實際送出的 8-bit 碼（o × 256）。
    // 原本寫成 want - (o << 8)，但 want 已經含上一次的餘數 ⇒ 誤差會被重複累加，
    // 平均輸出會偏離目標（4096 幀會累積到數十級）。修正後長期平均精確等於目標。
    *acc = (int32_t)q - (o << 8);
    return (uint8_t)o;
}

// 配置一個編碼器（firmware 後端用；大小對後端不透明，所以由這一檔配記憶體）。
nc_pixel_codec_t *nc_px_codec_new(void)
{
    return (nc_pixel_codec_t *)calloc(1, sizeof(nc_pixel_codec_t));
}

void nc_px_codec_free(nc_pixel_codec_t *c)
{
    free(c);
}

// 建立／重建編碼器。c 必須是已經配置好的記憶體（firmware：nc_px_codec_new()；
// 測試：malloc 或堆疊）。回 NULL = 參數不對。
nc_pixel_codec_t *nc_px_codec_init(nc_pixel_codec_t *c, const nc_px_codec_cfg_t *cfg)
{
    if (c == NULL || cfg == NULL || cfg->fmt >= NC_PX_FMT_COUNT || cfg->px == 0) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    c->spec = &s_fmt[cfg->fmt];
    if (c->cfg.bits < 8) {
        c->cfg.bits = 8;
    }
    if (c->cfg.bits > 16) {
        c->cfg.bits = 16;
    }
    if (c->cfg.dither_mode > 2) {
        c->cfg.dither_mode = (uint8_t)(c->cfg.dither_mode == 0xFF) ? 1u : 0u;
    }
    if (!c->spec->clk && c->cfg.wave.bit_cells == 0) {
        nc_px_wave_default(cfg->fmt, &c->cfg.wave);
    }
    if (!c->spec->clk) {
        if (c->cfg.wave.t0h > c->cfg.wave.bit_cells) {
            c->cfg.wave.t0h = c->cfg.wave.bit_cells;
        }
        if (c->cfg.wave.t1h > c->cfg.wave.bit_cells) {
            c->cfg.wave.t1h = c->cfg.wave.bit_cells;
        }
    }
    c->px = cfg->px;
    c->zones = (c->cfg.zone_px == 0) ? 1u : (c->px + c->cfg.zone_px - 1u) / c->cfg.zone_px;
    if (c->zones > NC_PX_CODEC_ZONES_MAX) {
        c->zones = NC_PX_CODEC_ZONES_MAX;
    }
    return c;
}

const nc_px_formatspec_t *nc_px_codec_spec(const nc_pixel_codec_t *c)
{
    return (c != NULL) ? c->spec : NULL;
}

uint32_t nc_px_codec_lane_bytes(const nc_pixel_codec_t *c)
{
    return (c != NULL) ? c->spec->lane_bytes : 0;
}

uint32_t nc_px_codec_units(const nc_pixel_codec_t *c)
{
    if (c == NULL) {
        return 0;
    }
    if (c->spec->clk) {
        return nc_px_codec_bits(c);
    }
    return nc_px_codec_bits(c) * c->cfg.wave.bit_cells;
}

uint32_t nc_px_codec_bits(const nc_pixel_codec_t *c)
{
    if (c == NULL) {
        return 0;
    }
    const uint32_t head = (uint32_t)c->spec->head_bytes * 8u;
    const uint32_t body = c->px * c->spec->lane_bytes * 8u;
    return head + body + (uint32_t)c->spec->tail_bytes * 8u;
}

uint32_t nc_px_codec_total_units(const nc_pixel_codec_t *c)
{
    if (c == NULL) {
        return 0;
    }
    const uint32_t units = nc_px_codec_units(c);
    if (c->spec->clk) {
        return units;
    }
    // reset 的單元數：reset_us × clk_hz（整數向上）
    const uint32_t us = c->spec->reset_us_hint;
    const uint32_t hz = c->cfg.wave.clk_hz ? c->cfg.wave.clk_hz : 1u;
    const uint32_t reset = (us * hz + 999999u) / 1000000u;
    return units + reset;
}

void nc_px_codec_frame(nc_pixel_codec_t *c, uint32_t frame_idx)
{
    if (c == NULL) {
        return;
    }
    c->frame = frame_idx;
}

// 一顆燈的抖動後 R,G,B,W（已依 bits／模式處理）
static void dither_px(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t zone, uint8_t out[4])
{
    const uint32_t mode = c->cfg.dither_mode;
    int32_t *acc = c->acc[zone];
    out[0] = dither_ch(rgbw[0], c->cfg.bits, &acc[0], mode);
    out[1] = dither_ch(rgbw[1], c->cfg.bits, &acc[1], mode);
    out[2] = dither_ch(rgbw[2], c->cfg.bits, &acc[2], mode);
    out[3] = dither_ch(rgbw[3], c->cfg.bits, &acc[3], mode);
    if (mode == 2) {
        // spatial：4×4 Bayer 的粗版（每 4 顆換一格），逐幀旋轉
        static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
        const uint32_t ph = c->frame & 15u;
        const uint8_t b = bayer[(zone + ph) & 15u];
        for (int i = 0; i < 4; i++) {
            if (out[i] < 255) {
                // 把「已經在 acc 的餘數」用空間圖案決定要不要進一級
                const int32_t r = c->acc[zone][i];
                if (r > (int32_t)b * 16) {
                    out[i] = (uint8_t)(out[i] + 1);
                    c->acc[zone][i] = r - 256;
                }
            }
        }
    }
}

static void lane_order(const nc_pixel_codec_t *c, const uint8_t ch[5], uint8_t *lane)
{
    const uint8_t n = c->spec->lane_bytes;
    for (uint8_t i = 0; i < n; i++) {
        lane[i] = 0;
    }
    for (uint8_t ci = 0; ci < 5; ci++) {
        const uint8_t pos = c->spec->order[ci];
        if (pos < n) {
            lane[pos] = ch[ci];
        }
    }
}

uint32_t nc_px_codec_lane_px(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t zone,
                             uint8_t *lane_out)
{
    if (c == NULL || rgbw == NULL || lane_out == NULL) {
        return 0;
    }
    if (zone >= c->zones) {
        zone = c->zones - 1u;
    }
    // 註：rgbw 是 R,G,B,W 四位（NC_PX_BPP = 4）。RGB 三色燈（WS2812/WS2801）的
    // 線材位元組由 spec->lane_bytes/order 決定，W 只是被 order 表丟掉。畫面要用白色
    // ⇒ 由產生者自己把 W 換成 R=G=B（見 rgbmatrix 產生者的白）；渲染器不代勞。
    uint8_t d[5];
    dither_px(c, rgbw, zone, d);
    // APA102 的亮度頭（5-bit）由 bright 決定，格式表的 order[4] 指向它
    d[4] = (uint8_t)(0xE0u | ((uint32_t)c->cfg.bright >> 3));
    const uint8_t ch[5] = { d[0], d[1], d[2], d[3], d[4] };
    lane_order(c, ch, lane_out);
    return c->spec->lane_bytes;
}

// 線材位元組 → 時序單元（歸零碼）
static uint32_t wave_of(const nc_pixel_codec_t *c, const uint8_t *lane, uint32_t lane_bytes,
                        uint8_t *out, uint32_t cap)
{
    const nc_px_wave_cfg_t *w = &c->cfg.wave;
    const uint32_t bc = w->bit_cells;
    const uint32_t need = lane_bytes * 8u * bc;
    if (cap < need || bc == 0) {
        return 0;
    }
    // 高電平 = 前 hi 個單元。兩個碼都至少要有 1 個單元的高電平：
    // WS2812 靠「上升緣」把資料打進下一顆／結束 reset，整組 bit 全 0 電平會被
    // 當成 reset（燈會閃 / 掉格）。t0h = 0 或 t1h 不小於 bit_cells 都是配置錯誤，
    // 這裡夾回合法範圍，讓錯的 config 是「看起來偏一點」而不是「整條燈亂掉」。
    const uint32_t bit_cells = bc;
    const uint32_t t_lo = (w->t0h == 0) ? 1u : (w->t0h >= bit_cells ? bit_cells - 1u : w->t0h);
    const uint32_t t_hi = (w->t1h <= t_lo) ? bit_cells : (w->t1h >= bit_cells ? bit_cells : w->t1h);
    uint32_t o = 0;
    for (uint32_t i = 0; i < lane_bytes; i++) {
        const uint8_t v = lane[i];
        for (int bit = 7; bit >= 0; bit--) {
            const uint32_t hi = ((v >> bit) & 1u) ? t_hi : t_lo;
            for (uint32_t k = 0; k < bc; k++) {
                out[o++] = (k < hi) ? 1u : 0u;
            }
        }
    }
    return o;
}

// 「和上一顆一模一樣」就抄：**這是關鍵的省工路徑**，但它必須先經過 dither_ch。
// 教訓（2026-11-18）：第一版用「R,G,B,W 一樣就跳過」—— 抖動是**有狀態**的，
// 跳過就等於凍結它的誤差回饋（長亮 / 長暗）。所以正確的順序是：
//   每顆都呼叫 lane_px（逐通道 dither_ch，誤差回饋照樣推進）→ 得到線材位元組 →
//   只有「線材位元組和上一顆完全相同」才複製那段樣式。
// dither_ch 對 bits=8 是零成本，bits=11/12 也只是幾次整數運算。
uint32_t nc_px_codec_wave_frame(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t px,
                                uint8_t *out, uint32_t cap)
{
    if (c == NULL || rgbw == NULL || out == NULL || c->spec->clk) {
        return 0;
    }
    const uint32_t total = nc_px_codec_total_units(c);
    if (cap < total) {
        return 0;
    }
    if (px > c->px) {
        px = c->px;
    }
    const uint32_t lb = c->spec->lane_bytes;
    uint8_t lane[4];
    uint32_t first_off = 0;
    uint32_t first_len = 0;
    uint8_t first_lane[4] = { 0, 0, 0, 0 };
    uint32_t o = 0;
    for (uint32_t i = 0; i < px; i++) {
        const uint32_t zone = (c->cfg.zone_px == 0) ? 0u : (i / c->cfg.zone_px);
        (void)nc_px_codec_lane_px(c, rgbw + i * NC_PX_BPP, zone, lane);
        // 全黑（GRB 全是 0）直接生成，不參與「和上一顆一樣就抄」的比對：
        // 全黑是 reset 的形狀（全拉低），抄它會把 reset 提前／複製到畫面中間。
        // 反過來說，只要 bit 串不是全 0，抄的是「上一顆的波形段」，那是安全且唯一的省工。
        bool dark = true;
        for (uint32_t k = 0; k < lb; k++) {
            if (lane[k] != 0) {
                dark = false;
                break;
            }
        }
        if (i > 0 && !dark && memcmp(lane, first_lane, lb) == 0) {
            memcpy(out + o, out + first_off, first_len);
            o += first_len;
            continue;
        }
        const uint32_t n = wave_of(c, lane, lb, out + o, cap - o);
        if (n == 0) {
            return 0;
        }
        memcpy(first_lane, lane, lb);
        first_off = o;
        first_len = n;
        o += n;
    }
    // reset：拉低（時序單元 = 0）
    memset(out + o, 0, total - o);
    return total;
}

uint32_t nc_px_codec_spi_frame(nc_pixel_codec_t *c, const uint8_t *rgbw, uint32_t px,
                               uint8_t *out, uint32_t cap)
{
    if (c == NULL || rgbw == NULL || out == NULL || !c->spec->clk) {
        return 0;
    }
    if (px > c->px) {
        px = c->px;
    }
    // 先寫「位元組緩衝」（頭部 + 每顆燈 + 尾巴），再展開成位。
    static uint8_t bytes[NC_PX_MAX_LEDS * 4 + 16];
    const uint32_t head = c->spec->head_bytes;
    const uint32_t tail = c->spec->tail_bytes;
    const uint32_t lb = c->spec->lane_bytes;
    const uint32_t nbytes = head + px * lb + tail;
    if (nbytes == 0 || nbytes > sizeof(bytes)) {
        return 0;
    }
    if (cap < nbytes * 8u) {
        return 0;
    }
    memset(bytes, 0, nbytes);
    for (uint32_t i = 0; i < px; i++) {
        const uint32_t zone = (c->cfg.zone_px == 0) ? 0u : (i / c->cfg.zone_px);
        nc_px_codec_lane_px(c, rgbw + i * NC_PX_BPP, zone, bytes + head + i * lb);
    }
    uint32_t o = 0;
    for (uint32_t i = 0; i < nbytes; i++) {
        // SPI 預設 MSB first（WS2801：SDI 在 CKI 上升緣取樣；APA102 相同）
        for (int bit = 7; bit >= 0; bit--) {
            out[o++] = (uint8_t)((bytes[i] >> bit) & 1u);
        }
    }
    return o;
}
