// nc_pixel_bench.c — 像素編碼吞吐量測（唔需要燈板、唔需要測試映像）。
//
// 量「packed 波形 + 8×8 轉置 → I80 byte-per-unit」每幀成本；所有緩衝**先試 internal
// SRAM**，唔夠才退 PSRAM，回報 `sram`（1 = 全部 internal）。
// 用途：回答「16×600×50 做唔做得到」，同「純 SRAM 有幾快」。
#include "nc_pixel_core.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL

static void *bench_alloc(size_t n, int *is_int)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p != NULL) {
        (*is_int)++;
        return p;
    }
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

size_t nc_px_bench_json(uint8_t lanes, uint16_t px, uint8_t frames, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (lanes == 0) { lanes = 1; }
    if (lanes > 16) { lanes = 16; }
    if (px == 0) { px = 1; }
    if (px > NC_PX_MAX_LEDS) { px = NC_PX_MAX_LEDS; }
    if (frames == 0) { frames = 1; }
    if (frames > 50) { frames = 50; }

    nc_px_codec_cfg_t cc = { 0 };
    nc_px_format_t fmt = NC_PX_FMT_WS2812;
    (void)nc_px_format_parse(CONFIG_NETCORE_PIXEL_FORMAT, &fmt);
    cc.fmt = fmt;
    nc_px_wave_default(fmt, &cc.wave);
    cc.wave.clk_hz = CONFIG_NETCORE_PIXEL_PCLK_HZ;
    cc.wave.bit_cells = CONFIG_NETCORE_PIXEL_BIT_CELLS;
    cc.wave.t0h = CONFIG_NETCORE_PIXEL_T0H;
    cc.wave.t1h = CONFIG_NETCORE_PIXEL_T1H;
    cc.bits = 8;
    cc.dither_mode = 0;
    cc.zone_px = (uint16_t)CONFIG_NETCORE_PIXEL_ZONE_PX;
    cc.px = px;
    cc.bright = 255;

    nc_pixel_codec_t *c0 = nc_px_codec_init(nc_px_codec_new(), &cc);
    if (c0 == NULL) {
        return (size_t)snprintf(out, cap, "{\"err\":\"codec\"}");
    }
    const uint32_t units = nc_px_codec_total_units(c0);
    const uint32_t lb = nc_px_codec_lane_bytes(c0);
    const uint32_t bc = (cc.wave.bit_cells > 0) ? cc.wave.bit_cells : 3;
    const uint32_t pack_bytes = (uint32_t)px * lb * bc;

    nc_pixel_codec_t *cd[16] = { 0 };
    uint8_t *pack[8] = { 0 };
    int int_cnt = 0, int_expect = 0;
    uint8_t *out0 = bench_alloc(units, &int_cnt); int_expect++;
    uint8_t *out1 = bench_alloc(units, &int_cnt); int_expect++;
    bool ok = out0 != NULL && out1 != NULL;
    if (ok) {
        memset(out0, 0, units);
        memset(out1, 0, units);
        cd[0] = c0;
        for (uint8_t i = 1; i < lanes && ok; i++) {
            cd[i] = nc_px_codec_init(nc_px_codec_new(), &cc);
            ok = cd[i] != NULL;
        }
        for (int i = 0; i < 8 && ok; i++) {
            pack[i] = bench_alloc(pack_bytes, &int_cnt); int_expect++;
            ok = pack[i] != NULL;
        }
    }
    uint8_t *payload = NULL;
    if (ok) {
        payload = bench_alloc((size_t)px * NC_PX_BPP, &int_cnt); int_expect++;
        ok = payload != NULL;
        if (ok) {
            for (uint32_t i = 0; i < (uint32_t)px * NC_PX_BPP; i++) {
                payload[i] = (uint8_t)((i * 37u + (i >> 3)) & 0xFFu);
            }
        }
    }

    int64_t us = 0;
    int64_t us_pack = 0;
    int64_t us_merge = 0;
    if (ok) {
        const int64_t t0 = esp_timer_get_time();
        for (uint8_t fr = 0; fr < frames; fr++) {
            memset(out0, 0, units);
            memset(out1, 0, units);
            for (uint8_t g = 0; g < lanes; g += 8) {
                const uint8_t n = (uint8_t)((lanes - g > 8) ? 8 : (lanes - g));
                for (uint8_t k = 0; k < n; k++) {
                    nc_px_codec_frame(cd[g + k], (uint32_t)fr + 1u);
                    if (nc_px_codec_wave_pack(cd[g + k], payload, px, pack[k], pack_bytes) == 0) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) { break; }
                const uint8_t *lp[8] = { NULL };
                for (uint8_t k = 0; k < n; k++) { lp[k] = pack[k]; }
                nc_px_codec_pack_merge8(lp, pack_bytes, (g == 0) ? out0 : out1, 1, 0);
            }
            if (!ok) { break; }
        }
        us = (esp_timer_get_time() - t0) / (int64_t)frames;

        // 拆開：只 pack（寫 packed bitstream）／只 merge（轉置＋寫 I80）
        const int64_t t1 = esp_timer_get_time();
        for (uint8_t fr = 0; fr < frames; fr++) {
            for (uint8_t ln = 0; ln < lanes; ln++) {
                nc_px_codec_frame(cd[ln], (uint32_t)fr + 1u);
                if (nc_px_codec_wave_pack(cd[ln], payload, px, pack[ln & 7],
                                          pack_bytes) == 0) {
                    ok = false;
                    break;
                }
            }
            if (!ok) { break; }
        }
        us_pack = (esp_timer_get_time() - t1) / (int64_t)frames;

        memset(out0, 0, units);
        memset(out1, 0, units);
        const int64_t t2 = esp_timer_get_time();
        for (uint8_t fr = 0; fr < frames; fr++) {
            for (uint8_t g = 0; g < lanes; g += 8) {
                nc_px_codec_pack_merge8((const uint8_t **)pack, pack_bytes,
                                        (g == 0) ? out0 : out1, 1, 0);
            }
        }
        us_merge = (esp_timer_get_time() - t2) / (int64_t)frames;
    }

    size_t n = 0;
    if (!ok) {
        n = (size_t)snprintf(out, cap, "{\"err\":\"mem\"}");
    } else {
        const uint32_t per = (uint32_t)((us > 0) ? us : 1);
        const uint32_t fps10 = (uint32_t)(10000000u / per);
        n = (size_t)snprintf(out, cap,
                             "{\"lanes\":%u,\"px\":%u,\"frames\":%u,\"units\":%u,"
                             "\"us_packed\":%u,\"us_pack\":%u,\"us_merge\":%u,\"max_fps_x10\":%u,"
                             "\"sram\":%d,\"sram_blocks\":%d,\"sram_expect\":%d,"
                             "\"pass50\":%u}",
                             (unsigned)lanes, (unsigned)px, (unsigned)frames, (unsigned)units,
                             (unsigned)per, (unsigned)us_pack, (unsigned)us_merge,
                             (unsigned)fps10,
                             (int_cnt == int_expect) ? 1 : 0, int_cnt, int_expect,
                             (fps10 >= 500u) ? 1u : 0u);
    }

    for (int i = 0; i < 16; i++) {
        if (cd[i] != NULL) { nc_px_codec_free(cd[i]); }
    }
    for (int i = 0; i < 8; i++) {
        if (pack[i] != NULL) { heap_caps_free(pack[i]); }
    }
    if (out0 != NULL) { heap_caps_free(out0); }
    if (out1 != NULL) { heap_caps_free(out1); }
    if (payload != NULL) { heap_caps_free(payload); }
    return n;
}

#else   // !CONFIG_NETCORE_PIXEL ── 這塊板冇開 pixel：唔編（main 的 0xF062 亦 depends on NETCORE_PIXEL）
typedef int nc_px_bench_unused_t;

#endif  // CONFIG_NETCORE_PIXEL
