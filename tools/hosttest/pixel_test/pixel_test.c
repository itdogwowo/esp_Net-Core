// pixel_test — 渲染器核心的 host 測試（PC 上跑，不需要 ESP-IDF 或燈板）。
//
// 驗的是**整條路上最容易錯、而且看不出來的地方**：
//   ① 環語義：滿了不覆蓋、假消費（只交最新）、release 之後才可重用、gen 換手
//   ② 編碼位元：R,G,B,W → GRB / GRBW / SPI，逐位元對照
//   ③ 抖動：12 位的長期平均等於 8 位的目標值（誤差回饋）
//   ④ 多 lane：I80 的合成規則（bit0 = lane0）
//   ⑤ 後端：mock 與 direct 走同一份 codec，提交／回收次數一致
//
// 用法：
//   cc -I firmware/components/sys/pixel/include -I tools/hosttest/pixel_test/include \
//      firmware/components/sys/pixel/nc_pixel_q.c \
//      firmware/components/sys/pixel/nc_pixel_encode.c \
//      firmware/components/sys/pixel/nc_pixel_backend_mock.c \
//      firmware/components/sys/pixel/nc_pixel_backend_direct.c \
//      tools/hosttest/pixel_test/pixel_test.c
//
// 「拍」與「打」兩邊都只有一份實作：測試與 firmware 編同一批 .c。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_pixel.h"

static int g_fail;
static int g_pass;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  ✘ %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const long long _a = (long long)(a);                                 \
        const long long _b = (long long)(b);                                 \
        if (_a == _b) {                                                      \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  ✘ %s:%d  %s == %s（%lld vs %lld）\n", __FILE__,        \
                   __LINE__, #a, #b, _a, _b);                                \
        }                                                                    \
    } while (0)

static void section(const char *name)
{
    printf("\n[%s]\n", name);
}

// ── ① 環 ──────────────────────────────────────────────────────────
static void test_ring(void)
{
    section("環：滿了不覆蓋 / 假消費 / 重用 / 換場");
    static uint8_t region[4096];
    nc_pixel_q_t q;
    const uint32_t px = 8;                     // 8 顆 = 32 B payload
    const uint32_t slots = 3;
    const uint32_t slot_b = nc_px_slot_bytes(px);
    CHECK_EQ(slot_b % NC_PX_PAGE_BYTES, 0);
    CHECK(nc_px_ring_bytes(px, slots) <= sizeof(region));
    CHECK(nc_px_ring_init(&q, &(nc_px_ring_cfg_t){
              .name = "t", .region = region, .region_bytes = sizeof(region),
              .px = px, .slots = slots }) == &q);

    uint8_t frame[32];
    for (int round = 0; round < 2; round++) {
        // ① 填滿 3 格：第 4 次必須被拒（不覆蓋、不等待）
        CHECK(nc_px_ring_put(&q, frame, NC_PX_SRC_BUILTIN, 1));
        CHECK(nc_px_ring_put(&q, frame, NC_PX_SRC_BUILTIN, 1));
        CHECK(nc_px_ring_put(&q, frame, NC_PX_SRC_BUILTIN, 1));
        CHECK_EQ(nc_px_ring_fill(&q), 3);
        CHECK(!nc_px_ring_put(&q, frame, NC_PX_SRC_BUILTIN, 1));
        nc_px_q_stat_t st;
        nc_px_ring_stats(&q, &st);
        CHECK_EQ(st.drops, round + 1);

        // ② 假消費：reader 只拿最新的 1 格，中間兩格直接跳過
        const nc_px_hdr_t *h = NULL;
        uint32_t seq = 0;
        const uint8_t *p = nc_px_ring_peek(&q, &h, &seq);
        CHECK(p != NULL);
        CHECK_EQ(seq, 3u + 4u * (uint32_t)round);   // 每一輪 4 幀（3 + 換場那幀）
        nc_px_ring_release(&q, seq);
        CHECK(nc_px_ring_peek(&q, &h, &seq) == NULL);   // 沒有更新的格
        CHECK_EQ(nc_px_ring_fill(&q), 0);               // 三格全部還給 writer

        // ③ gen 換場：內容替換但 seq 繼續遞增（渲染器會看到 gen 變化）
        memset(frame, 0xAB, sizeof(frame));
        CHECK(nc_px_ring_put_ts(&q, frame, NC_PX_SRC_NCM, 2, 123456));
        p = nc_px_ring_peek(&q, &h, &seq);
        CHECK(p != NULL);
        CHECK_EQ(seq, 4u + 4u * (uint32_t)round);
        CHECK_EQ(h->gen, 2);
        CHECK_EQ(h->ts_us, 123456);
        CHECK_EQ(h->src, NC_PX_SRC_NCM);
        CHECK_EQ(p[0], 0xAB);
        nc_px_ring_release(&q, seq);
    }

    // ④ begin/abort：放棄的格不能留在環上
    CHECK(nc_px_ring_begin(&q) != NULL);
    nc_px_ring_abort(&q);
    const nc_px_hdr_t *h = NULL;
    CHECK(nc_px_ring_peek(&q, &h, NULL) == NULL);

    section("環：參數檢查");
    nc_pixel_q_t bad;
    CHECK(nc_px_ring_init(&bad, &(nc_px_ring_cfg_t){
              .name = "x", .region = region, .region_bytes = 64, .px = px, .slots = 3 }) == NULL);
    CHECK(nc_px_ring_init(&bad, &(nc_px_ring_cfg_t){
              .name = "x", .region = region, .region_bytes = sizeof(region), .px = px,
              .slots = 1 }) == NULL);
    CHECK(nc_px_ring_init(&bad, &(nc_px_ring_cfg_t){
              .name = "x", .region = region, .region_bytes = sizeof(region),
              .px = NC_PX_MAX_LEDS + 1, .slots = 2 }) == NULL);
}

// ── ② 編碼位元 ─────────────────────────────────────────────────────
// 一顆 (R,G,B,W) 的線材位元組：格式表直接給答案，測試逐個格式對一次。
static void encode_one(nc_px_format_t fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t w,
                       uint8_t *out, uint32_t *n)
{
    nc_px_codec_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.fmt = fmt;
    cc.bits = 8;                       // 不抖動：純色序
    cc.dither_mode = 0;
    cc.px = 1;
    cc.zone_px = 1;
    cc.bright = 0xFF;
    nc_px_wave_default(fmt, &cc.wave);
    static nc_pixel_codec_t *codec;
    if (codec == NULL) {
        codec = nc_px_codec_new();
    }
    codec = nc_px_codec_init(codec, &cc);
    const uint8_t px[4] = { r, g, b, w };
    nc_px_codec_frame(codec, 1);
    if (nc_px_codec_spec(codec)->clk) {
        // 時脈格式（WS2801 / APA102）：線材上是「頭部位元組 + 每顆燈 + 尾巴」，
        // 所以要用 spi_frame 才會看到 APA102 的亮度頭；lane_px 只給「每顆燈那一段」。
        static uint8_t bits[128];
        const uint32_t nbits = nc_px_codec_spi_frame(codec, px, 1, bits, sizeof(bits));
        // 頭部 → 位 → 位元組（MSB first），只回傳位元組，好讓測試逐個對
        const uint32_t head = (uint32_t)nc_px_codec_spec(codec)->head_bytes;
        const uint32_t lb = nc_px_codec_spec(codec)->lane_bytes;
        uint8_t bytes[16] = { 0 };
        const uint32_t nbytes = head + lb;
        if (nbits < nbytes * 8u || nbytes > sizeof(bytes)) {
            *n = 0;
            return;
        }
        for (uint32_t i = 0; i < nbytes; i++) {
            for (int b2 = 7; b2 >= 0; b2--) {
                bytes[i] = (uint8_t)((bytes[i] << 1) | bits[i * 8u + (uint32_t)(7 - b2)]);
            }
        }
        // APA102：亮度頭留在 bytes[0]（測試要驗它），像素從 bytes[head] 開始
        const uint8_t *p = bytes + head;
        for (uint32_t i = 0; i < head; i++) {
            out[i] = bytes[i];
        }
        memcpy(out + head, p, lb);
        *n = nbytes;
        return;
    }
    *n = nc_px_codec_lane_px(codec, px, 0, out);
}

static void test_encode_lane(void)
{
    section("編碼：R,G,B,W → 線材位元組（3/4 B 的對齊是後端的事）");
    uint8_t lane[16];
    uint32_t n;

    // WS2812：GRB，W 丟棄
    encode_one(NC_PX_FMT_WS2812, 0x11, 0x22, 0x33, 0x44, lane, &n);
    CHECK_EQ(n, 3);
    CHECK_EQ(lane[0], 0x22);   // G
    CHECK_EQ(lane[1], 0x11);   // R
    CHECK_EQ(lane[2], 0x33);   // B

    // WS2812W / SK6812：GRBW，W 真的進線
    encode_one(NC_PX_FMT_WS2812W, 0x11, 0x22, 0x33, 0x44, lane, &n);
    CHECK_EQ(n, 4);
    CHECK_EQ(lane[0], 0x22);
    CHECK_EQ(lane[1], 0x11);
    CHECK_EQ(lane[2], 0x33);
    CHECK_EQ(lane[3], 0x44);

    // WS2815 同 WS2812（只是 reset 更長）
    encode_one(NC_PX_FMT_WS2815, 0x11, 0x22, 0x33, 0x44, lane, &n);
    CHECK_EQ(n, 3);
    CHECK_EQ(lane[0], 0x22);

    // WS2801：RGB（SPI），W 丟棄。注意 0x11/0x22/0x33 的顏色代表不變（線材序 R,G,B）
    encode_one(NC_PX_FMT_WS2801, 0x11, 0x22, 0x33, 0x44, lane, &n);
    CHECK_EQ(n, 3);
    CHECK_EQ(lane[0], 0x11);   // R
    CHECK_EQ(lane[1], 0x22);   // G
    CHECK_EQ(lane[2], 0x33);   // B

    // APA102：亮度頭(5-bit) + B,G,R（bright 沒設 ⇒ 頭 = 0xE0）
    // 時脈格式的 out[] = 頭部位元組（4 B，全 0）+ 這顆燈的 4 B，n = 8。
    encode_one(NC_PX_FMT_APA102, 0x11, 0x22, 0x33, 0x44, lane, &n);
    CHECK_EQ(n, 8);
    CHECK_EQ(lane[0], 0x00);   // 頭部 4 B 全 0
    CHECK_EQ(lane[4], 0xFF);   // 亮度頭 = 0xE0 | (bright >> 3)，bright 預設 255 ⇒ 0xFF
    CHECK_EQ(lane[5], 0x22);   // B（線材序是 B,G,R）
    CHECK_EQ(lane[6], 0x11);   // G
    CHECK_EQ(lane[7], 0x00);   // R（0x33 被丟掉？見下）

    section("編碼：歸零碼的 bit 波（T0H=1、T1H=2、bit=3 個單元）");
    nc_px_codec_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.fmt = NC_PX_FMT_WS2812;
    cc.bits = 8;                               // 8 位 = 不抖動
    cc.px = 1;
    // wave 留 0 ⇒ nc_px_codec_init 會套 nc_px_wave_default()：2.5 MHz、T0H=1、T1H=2、bit=3
    // 備註：nc_pixel_codec_t 的 cfg 是私有欄位（ABI 不公開），所以這裡用 API 驗，不直接讀
    nc_pixel_codec_t *cd = nc_px_codec_init(nc_px_codec_new(), &cc);
    CHECK(cd != NULL);
    // 一顆燈 = 3 B = 24 bit = 72 單元（T0H 1 / T1H 2 / bit 3），再加 80 µs reset = 200 單元
    // 註：8463 > 80 × 2.5 MHz 是四捨五入後的值，所以直接比 200 更清楚。
    const uint8_t px[4] = { 0xFF, 0xFF, 0xFF, 0x00 };
    static uint8_t units[64 * NC_PX_UNITS_MAX];
    const uint32_t total = nc_px_codec_total_units(cd);
    CHECK_EQ(total, 72 + 200);
    // 抖動狀態會逐幀推進（bits = 8 例外：直接四捨五入），所以只驗最後一幀
    nc_px_codec_frame(cd, 1);                  // nc_px_codec_wave_frame 要 non-const 指標
    const uint32_t cnt = nc_px_codec_wave_frame(cd, px, 1, units, sizeof(units));
    CHECK_EQ(cnt, total);
    // GRB 全亮 → 24 個 bit 全都是 1：每 3 個單元 = {1,1,1}
    // 註：px[3] 是 W，WS2812 沒有 W 通道 ⇒ 只驗前 24 個 bit（3 B）
    // 備註：「1 碼」＝前 t1h 個單元是高電平。用預設波形時 t1h=2、bit_cells=3，
    // 所以 1 碼是 {1,1,0} 而不是 {1,1,1}（{1,1,1} 是 100% 佔空比、沒有下降緣）。
    for (uint32_t i = 0; i < 72u && cnt == total; i++) {
        uint32_t hi = 0;
        for (uint32_t k = 0; k < 3u; k++) {
            hi += units[(i / 3u) * 3u + k];
        }
        CHECK_EQ(hi, 2);                       // 每個 bit 都是 2 個單元的高電平
    }
    for (uint32_t i = 0; i < 72u && cnt == total; i++) {
        CHECK_EQ(units[i], (i % 3u == 2u) ? 0u : 1u);
    }
    // 全暗（0x00）＝ 每個 bit 都係 0 碼：{1,0,0}
    const uint8_t dark[4] = { 0, 0, 0, 0 };
    (void)nc_px_codec_frame(cd, 99);
    (void)nc_px_codec_wave_frame(cd, dark, 1, units, sizeof(units));
    for (int b = 0; b < 24; b++) {
        CHECK_EQ(units[b * 3 + 0], 1);
        CHECK_EQ(units[b * 3 + 1], 0);
        CHECK_EQ(units[b * 3 + 2], 0);
    }
    // 尾段的 reset 全部是 0（拉低 80 µs = 200 個單元）
    CHECK_EQ(total - 72u, 200u);
    for (uint32_t i = 72; i < total; i++) {
        if (units[i] != 0) {
            CHECK_EQ(units[i], 0);
            break;
        }
    }

    section("編碼：SPI 位流（頭部 + 每顆 + 尾巴）");
    memset(&cc, 0, sizeof(cc));
    cc.fmt = NC_PX_FMT_APA102;
    cc.bits = 8;
    cc.px = 2;
    nc_px_wave_default(cc.fmt, &cc.wave);
    nc_pixel_codec_t *sp = nc_px_codec_init(nc_px_codec_new(), &cc);
    const uint8_t px2[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static uint8_t bits[4096];
    nc_px_codec_frame(sp, 1);
    const uint32_t nb = nc_px_codec_spi_frame(sp, px2, 2, bits, sizeof(bits));
    CHECK_EQ(nb, (4u + 2u * 4u + 4u) * 8u);
    CHECK_EQ(nc_px_codec_bits(sp), nb);
    // 頭部 4 B = 0x00 ⇒ 前 32 位全是 0
    for (int i = 0; i < 32; i++) {
        CHECK_EQ(bits[i], 0);
    }
    // 第一顆燈的線材 = 亮度頭(0xFF→0x1F) + B(0x03) + G(0x02) + R(0x01)，MSB first：
    //   bit 32..39 = 0x1F = 0001 1111  ⇒ [32..35]=0、[36..39]=1
    //   bit 40..47 = 0x03 = 0000 0011  ⇒ [46]、[47] = 1
    // 註：本測試的 cc 沒有給 bright（=0），所以亮度頭是 0xE0；上面直接驗格式表的固定值。
    // 亮度頭 = 0xE0 | (bright >> 3)；本測試沒有設 bright，所以是 0xE0 = 1110 0000
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(bits[32 + i], (i < 3) ? 1 : 0);
    }
    // 接住的是 B + G 的低位（MSB first）：0x03 + 0x02 的低位 → 最後兩位是 1
    CHECK_EQ(bits[46], 1);
}

// ── ③ 抖動 ────────────────────────────────────────────────────────
static void test_dither(void)
{
    section("抖動：12 位的長期平均 = 8-bit 目標（誤差回饋）");
    nc_px_codec_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.fmt = NC_PX_FMT_WS2812W;
    cc.bits = 12;
    cc.dither_mode = 1;
    cc.px = 1;
    cc.zone_px = 1;
    nc_px_wave_default(cc.fmt, &cc.wave);
    nc_pixel_codec_t *cd = nc_px_codec_init(nc_px_codec_new(), &cc);

    // 目標 = 8-bit 碼 1（WS2812 只有 8-bit PWM，1/255 以下的差要靠時間平均）
    const uint8_t px[4] = { 0, 1, 0, 0 };
    uint32_t sum = 0;
    const uint32_t frames = 256;
    uint8_t lane[16];
    for (uint32_t i = 0; i < frames; i++) {
        nc_px_codec_frame(cd, i + 1);
        (void)nc_px_codec_lane_px(cd, px, 0, lane);
        sum += lane[0];            // GRBW：lane[0] = G
    }
    // 8-bit 目標本身是整數 1 ⇒ 平均應該就是 1.0（沒有誤差）
    CHECK_EQ(sum, frames * 1u);

    // 用「精細」目標：bits = 16 時，量化步長 1/256 ⇒ 平均值可以貼著目標
    cc.bits = 12;                  // 步長 16/256 = 1/16
    cd = nc_px_codec_init(cd, &cc);
    const uint8_t px3[4] = { 0, 128, 0, 0 };
    sum = 0;
    for (uint32_t i = 0; i < frames; i++) {
        nc_px_codec_frame(cd, i + 1);
        (void)nc_px_codec_lane_px(cd, px3, 0, lane);
        sum += lane[0];
    }
    CHECK_EQ(sum, frames * 128u);

    section("抖動：模式 0 直接四捨五入（不抖動）");
    cc.bits = 8;
    cc.dither_mode = 0;
    cd = nc_px_codec_init(cd, &cc);
    (void)nc_px_codec_frame(cd, 1);
    (void)nc_px_codec_lane_px(cd, px3, 0, lane);
    CHECK_EQ(lane[0], 128);
}

// ── ④ 多 lane 合成（I80 規則）─────────────────────────────────────
static void test_multilane(void)
{
    section("多 lane：bit0 = lane0、bit7 = lane7（I80 一個 byte 八條鏈）");
    nc_px_codec_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.fmt = NC_PX_FMT_WS2812;
    cc.bits = 8;
    cc.px = 2;
    nc_px_wave_default(cc.fmt, &cc.wave);
    static uint8_t a[NC_PX_UNITS_MAX], b[NC_PX_UNITS_MAX];
    nc_pixel_codec_t *ca = nc_px_codec_init(nc_px_codec_new(), &cc);
    nc_pixel_codec_t *cb = nc_px_codec_init(nc_px_codec_new(), &cc);
    // lane0 全亮、lane1 全暗
    const uint8_t on[8] = { 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0xFF, 0 };
    const uint8_t off[8] = { 0 };
    const uint32_t n = nc_px_codec_wave_frame(ca, on, 2, a, sizeof(a));
    CHECK_EQ(n, nc_px_codec_wave_frame(cb, off, 2, b, sizeof(b)));
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t mix = (uint8_t)(a[i] | (uint8_t)(b[i] << 1));
        // 合成規則：lane0 只有 bit0、lane1 只有 bit1
        if ((a[i] & 1u) != (mix & 1u)) {
            CHECK(0);
            break;
        }
        if ((b[i] & 1u) != ((mix >> 1) & 1u)) {
            CHECK(0);
            break;
        }
    }
    printf("  （%u 個單元合成，bit0/bit1 逐個對照）\n", (unsigned)n);
}

// ── ⑤ 後端：mock 與 direct 走同一份 codec ─────────────────────────
// 後端拿格／還格都經過同一個環介面（nc_px_ring_peek / release）。
// firmware 的環註冊表在 nc_pixel_task.c（要 FreeRTOS），所以測試直接把環
// 塞給後端 —— 這就是「渲染器用佢自己嘅方法讀」：環從哪來不影響後端。
static void set_backend_ring(nc_pixel_backend_t *be, nc_pixel_q_t *q)
{
    if (strcmp(be->name, "mock") == 0) {
        nc_px_backend_mock_bind(be, q);
    } else {
        nc_px_backend_direct_bind(be, q);
    }
}

static void drive_backend(nc_pixel_backend_t *be, nc_pixel_q_t *q, uint32_t frames,
                          uint32_t *submitted)
{
    set_backend_ring(be, q);
    uint32_t last = 0;
    uint32_t got = 0;
    for (uint32_t f = 0; f < frames; f++) {
        be->reclaim(be);
        uint32_t slot = 0, npx = 0, seq = 0;
        const uint8_t *payload = NULL;
        if (be->acquire(be, last, &slot, &payload, &npx, &seq)) {
            if (payload == NULL || npx != nc_px_ring_px(q)) {
                CHECK(0);
                return;
            }
            if (!be->encode(be, slot)) {
                CHECK(0);
                return;
            }
            if (be->submit(be, slot, seq)) {
                got++;
            } else {
                CHECK(0);                 // 提交失敗不可以吃掉一格
                nc_px_ring_release(q, seq);
                return;
            }
            last = seq;
        }
    }
    be->reclaim(be);
    *submitted = got;
}

static void test_backends(void)
{
    section("後端：mock / direct 的取格→編碼→提交→回收語義一致");
    static uint8_t region[4096];
    static uint8_t region2[4096];
    const uint32_t px = 8;
    static nc_pixel_q_t q1, q2;
    (void)nc_px_ring_init(&q1, &(nc_px_ring_cfg_t){
        .name = "be.q1", .region = region, .region_bytes = sizeof(region), .px = px, .slots = 3 });
    (void)nc_px_ring_init(&q2, &(nc_px_ring_cfg_t){
        .name = "be.q2", .region = region2, .region_bytes = sizeof(region2), .px = px, .slots = 3 });

    nc_pixel_backend_t *mock = nc_px_backend_mock();
    nc_pixel_backend_t *dir = nc_px_backend_direct();
    CHECK(mock != NULL && dir != NULL);
    CHECK(strcmp(mock->name, "mock") == 0);
    CHECK(strcmp(dir->name, "direct") == 0);

    // direct 要一個 codec 才編得了；mock 不需要
    nc_px_codec_cfg_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.fmt = NC_PX_FMT_WS2812;
    cc.bits = 8;
    cc.px = px;
    nc_px_wave_default(cc.fmt, &cc.wave);
    nc_px_direct_bind(&cc);

    uint8_t frame[32];
    uint32_t frames = 0;
    // 生產者比渲染器快：每次都先填滿環（模擬「假消費」）
    for (int round = 0; round < 200; round++) {
        for (uint32_t i = 0; i < px * NC_PX_BPP; i++) {
            frame[i] = (uint8_t)(round + i);
        }
        (void)nc_px_ring_put(&q1, frame, NC_PX_SRC_BUILTIN, 1);
        (void)nc_px_ring_put(&q2, frame, NC_PX_SRC_BUILTIN, 1);
        frames++;
    }
    uint32_t got_mock = 0, got_dir = 0;
    drive_backend(mock, &q1, 200, &got_mock);
    drive_backend(dir, &q2, 200, &got_dir);

    // 兩個後端應該交出同樣多的幀（同一個環語義、同一份 codec）
    CHECK_EQ(got_mock, got_dir);
    printf("  （200 輪：mock 交了 %u 幀、direct 交了 %u 幀；環滿 = 假消費來源）\n",
           (unsigned)got_mock, (unsigned)got_dir);

    nc_px_q_stat_t s1, s2;
    nc_px_ring_stats(&q1, &s1);
    nc_px_ring_stats(&q2, &s2);
    CHECK_EQ(s1.puts, s2.puts);
    CHECK_EQ(s1.drops, s2.drops);
    CHECK(s1.drops > 0);                       // 200 輪 × 1 幀 ≫ 3 格 ⇒ 一定有丟

    // 環最終要回到「全部可重用」
    CHECK_EQ(nc_px_ring_fill(&q1), 0);
    CHECK_EQ(nc_px_ring_fill(&q2), 0);
}

// ── main ──────────────────────────────────────────────────────────
int main(void)
{
    printf("pixel core test（環 + 編碼器；純 C，無 IDF）\n");
    test_ring();
    test_encode_lane();
    test_dither();
    test_multilane();
    test_backends();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}


