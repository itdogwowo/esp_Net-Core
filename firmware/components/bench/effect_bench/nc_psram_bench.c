// nc_psram_bench.c — 大緩衝放 PSRAM、讀取／混合使用的實機量測（0xF067）。
//
// 目的：不是再量效果，而是量「資料搬運」本身：
//   · internal / PSRAM 的 memset、sequential read、memcpy；
//   · PSRAM cold read（每次先 M2C invalidate，確保真的由 PSRAM 讀）；
//   · PSRAM writeback（C2M）成本；
//   · internal ↔ PSRAM 混合 copy；
//   · 大 buffer 留在 PSRAM、以 1/4/16/64 KB chunk 讀入 internal scratch。
//
// 為什麼要另一條指令：0xF065 量的是效果管線（pair/conv/dither 有 modulo、逐像素），
// 不能當成純記憶體頻寬。這一支只做 memcpy/memset/read，數字可以直接對照 datasheet。
//
// ⚠ 這一支是 raw microbenchmark：直接用 heap_caps_aligned_alloc，**不是**正式 buffer 路徑。
//   正式路徑一律用 nc_buf（docs/03_notes/buffers.md）：where=S/P/DMA、slot_bytes×n 開機定死、
//   reserve 放語意 header、put_end(len) 記實際用量、get(&len,&off) 交邊界、handover 換 owner。
//
// payload [size_kb:u16 LE][iters:u16 LE]（可省；預設 256 KB、8 次，夾 16..1024 KB、1..32）
// 回覆 0xF068 JSON。只喺 CONFIG_NETCORE_EFFECT_BENCH 時編譯。

#include "sdkconfig.h"

#if CONFIG_NETCORE_EFFECT_BENCH

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nc4.h"
#include "nc_action.h"

static const char *TAG = "psram_bench";

#define NC4_CMD_PSRAMBENCH     0xF067
#define NC4_CMD_PSRAMBENCH_RSP 0xF068
#define PSRAM_SCRATCH_BYTES    (64u * 1024u)
#define PSRAM_IO_MAX_KB        1024u

static volatile uint32_t s_sink;
static bool s_busy;

static void *alloc_buf(size_t bytes, uint32_t caps)
{
    return heap_caps_aligned_alloc(64, bytes, caps | MALLOC_CAP_8BIT);
}

static uint32_t mbps(size_t bytes, uint32_t iters, int64_t us)
{
    if (us < 1) {
        us = 1;
    }
    const uint64_t total = (uint64_t)bytes * (uint64_t)iters;
    return (uint32_t)((total * 1000000ULL) / ((uint64_t)us * 1024ULL * 1024ULL));
}

static int64_t now_us(void) { return esp_timer_get_time(); }

// ── 基本操作（全部 hot path 之外量）──

static uint32_t bench_write_hot(uint8_t *p, size_t n, uint32_t iters)
{
    const int64_t t0 = now_us();
    for (uint32_t i = 0; i < iters; i++) {
        memset(p, (int)(0x5A + i), n);
    }
    const int64_t us = now_us() - t0;
    s_sink += p[0];
    return mbps(n, iters, us);
}

static uint32_t bench_write_cold(uint8_t *p, size_t n, uint32_t iters, uint32_t *err)
{
    const int64_t t0 = now_us();
    for (uint32_t i = 0; i < iters; i++) {
        memset(p, (int)(0x5A + i), n);
        if (esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK) {
            (*err)++;
        }
    }
    const int64_t us = now_us() - t0;
    s_sink += p[0];
    return mbps(n, iters, us);
}

static uint32_t bench_read_hot(const uint8_t *p, size_t n, uint32_t iters)
{
    const uint32_t *w = (const uint32_t *)p;
    const size_t nw = n / sizeof(uint32_t);
    uint32_t acc = 0;
    const int64_t t0 = now_us();
    for (uint32_t k = 0; k < iters; k++) {
        for (size_t i = 0; i < nw; i++) {
            acc += w[i];
        }
    }
    const int64_t us = now_us() - t0;
    s_sink += acc;
    return mbps(n, iters, us);
}

static uint32_t bench_read_cold(uint8_t *p, size_t n, uint32_t iters, uint32_t *err)
{
    const uint32_t *w = (const uint32_t *)p;
    const size_t nw = n / sizeof(uint32_t);
    uint32_t acc = 0;
    const int64_t t0 = now_us();
    for (uint32_t k = 0; k < iters; k++) {
        if (esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) {
            (*err)++;
        }
        for (size_t i = 0; i < nw; i++) {
            acc += w[i];
        }
    }
    const int64_t us = now_us() - t0;
    s_sink += acc;
    return mbps(n, iters, us);
}

static uint32_t bench_copy(uint8_t *dst, const uint8_t *src, size_t n, uint32_t iters)
{
    const int64_t t0 = now_us();
    for (uint32_t i = 0; i < iters; i++) {
        memcpy(dst, src, n);
    }
    const int64_t us = now_us() - t0;
    s_sink += dst[0] + src[0];
    return mbps(n, iters, us);
}

// 大 buffer 在 PSRAM，逐 chunk memcpy 入 internal scratch。
static uint32_t bench_copy_chunk(uint8_t *dst, const uint8_t *src, size_t n,
                                 size_t chunk, uint32_t iters)
{
    if (chunk == 0 || chunk > n) {
        return 0;
    }
    const int64_t t0 = now_us();
    for (uint32_t i = 0; i < iters; i++) {
        for (size_t off = 0; off < n; off += chunk) {
            size_t c = n - off;
            if (c > chunk) {
                c = chunk;
            }
            memcpy(dst, src + off, c);
        }
    }
    const int64_t us = now_us() - t0;
    s_sink += dst[0] + src[0];
    return mbps(n, iters, us);
}

static uint32_t msync_us(uint8_t *p, size_t n, int flags, uint32_t *err)
{
    const int64_t t0 = now_us();
    const esp_err_t e = esp_cache_msync(p, n, flags);
    const int64_t us = now_us() - t0;
    if (e != ESP_OK) {
        (*err)++;
    }
    return (uint32_t)us;
}

// ── 回覆 ──

static void reply_json(nc4_ctx_t *ctx, const char *json)
{
    if (ctx == NULL || ctx->send == NULL || ctx->decoder == NULL) {
        return;
    }
    uint8_t *frame = nc4_decoder_reply_buf(ctx->decoder);
    if (frame == NULL) {
        return;
    }
    size_t n = strlen(json);
    uint8_t *data = frame + NC4_HDR_LEN;
    if (n > NC4_REPLY_JSON_MAX) {
        n = (size_t)snprintf((char *)data + 2, NC4_REPLY_JSON_MAX + 1,
                             "{\"err\":\"reply_too_big\",\"need\":%u}",
                             (unsigned)strlen(json));
    } else {
        memcpy(data + 2, json, n);
    }
    data[0] = (uint8_t)n;
    data[1] = (uint8_t)(n >> 8);
    const int total = nc4_pack_into(frame, NC4_REPLY_FRAME_MAX, NC4_CMD_PSRAMBENCH_RSP,
                                    nc4_reply_addr(ctx), data, 2 + n);
    if (total > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)total);
    }
}

static void on_psrambench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t size_kb = 256;
    uint16_t iters = 8;
    if (f->len >= 2) {
        size_kb = (uint16_t)(f->data[0] | ((uint16_t)f->data[1] << 8));
    }
    if (f->len >= 4) {
        iters = (uint16_t)(f->data[2] | ((uint16_t)f->data[3] << 8));
    }
    if (size_kb < 16) {
        size_kb = 16;
    }
    if (size_kb > PSRAM_IO_MAX_KB) {
        size_kb = PSRAM_IO_MAX_KB;
    }
    if (iters < 1) {
        iters = 1;
    }
    if (iters > 32) {
        iters = 32;
    }
    if (s_busy) {
        reply_json(ctx, "{\"err\":\"busy\"}");
        return;
    }
    s_busy = true;

    const size_t n = (size_t)size_kb * 1024u;
    const size_t scratch = (n < PSRAM_SCRATCH_BYTES) ? n : PSRAM_SCRATCH_BYTES;

    uint8_t *pa = (uint8_t *)alloc_buf(n, MALLOC_CAP_SPIRAM);
    uint8_t *pb = (n <= 512u * 1024u) ? (uint8_t *)alloc_buf(n, MALLOC_CAP_SPIRAM) : NULL;
    uint8_t *ia = (uint8_t *)alloc_buf(scratch, MALLOC_CAP_INTERNAL);
    uint8_t *ib = (n <= scratch) ? (uint8_t *)alloc_buf(n, MALLOC_CAP_INTERNAL) : NULL;

    if (pa == NULL || ia == NULL) {
        if (pa) {
            heap_caps_free(pa);
        }
        if (pb) {
            heap_caps_free(pb);
        }
        if (ia) {
            heap_caps_free(ia);
        }
        if (ib) {
            heap_caps_free(ib);
        }
        s_busy = false;
        reply_json(ctx, "{\"err\":\"oom\"}");
        return;
    }

    const uint32_t e0 = iters;
    const bool int_full = (n <= scratch && ib != NULL);
    uint32_t int_wr = 0, int_rd = 0, int_cp = 0;
    uint32_t p_wr = 0, p_wr_cold = 0, p_rd = 0, p_rd_cold = 0, p_cp = 0;
    uint32_t mix_i2p = 0, mix_p2i = 0, mix_p2p = 0;
    uint32_t chunk[4] = { 0, 0, 0, 0 };
    uint32_t err = 0;
    const uint32_t ms_c2m = msync_us(pa, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M, &err);
    const uint32_t ms_m2c = msync_us(pa, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C, &err);

    // 先 hot，再 cold；每組之間讓 WDT。
    if (int_full) {
        int_wr = bench_write_hot(ia, n, e0);
        int_rd = bench_read_hot(ia, n, e0);
        int_cp = bench_copy(ib, ia, n, e0);
        vTaskDelay(2);
    }
    p_wr = bench_write_hot(pa, n, e0);
    p_rd = bench_read_hot(pa, n, e0);
    if (pb != NULL) {
        p_cp = bench_copy(pb, pa, n, e0);
    }
    vTaskDelay(2);

    p_wr_cold = bench_write_cold(pa, n, e0, &err);
    p_rd_cold = bench_read_cold(pa, n, e0, &err);
    vTaskDelay(2);

    // 混合：固定 64 KB 或更小的 chunk，internal scratch ↔ PSRAM。
    const size_t mix_chunk = scratch;
    mix_i2p = bench_copy_chunk(pa, ia, n, mix_chunk, e0);
    mix_p2i = bench_copy_chunk(ia, pa, n, mix_chunk, e0);
    if (pb != NULL) {
        mix_p2p = bench_copy(pb, pa, n, e0);
    }
    vTaskDelay(2);

    // 分塊讀：PSRAM → internal scratch，1/4/16/64 KB。
    static const size_t sizes[4] = { 1024, 4096, 16384, 65536 };
    for (int i = 0; i < 4; i++) {
        if (sizes[i] <= scratch && sizes[i] <= n) {
            chunk[i] = bench_copy_chunk(ia, pa, n, sizes[i], e0);
        }
    }

    char j[1400];
    (void)snprintf(
        j, sizeof(j),
        "{\"size\":%u,\"it\":%u,\"ok\":{\"i\":%d,\"p\":1},"
        "\"int\":{\"wr\":%u,\"rd\":%u,\"cp\":%u},"
        "\"psram\":{\"wr\":%u,\"wr_cold\":%u,\"rd\":%u,\"rd_cold\":%u,\"cp\":%u},"
        "\"mix\":{\"i2p\":%u,\"p2i\":%u,\"p2p\":%u},"
        "\"chunk\":{\"1k\":%u,\"4k\":%u,\"16k\":%u,\"64k\":%u},"
        "\"msync_us\":{\"c2m\":%u,\"m2c\":%u},\"msync_err\":%u}",
        (unsigned)n, (unsigned)e0, int_full ? 1 : 0,
        (unsigned)int_wr, (unsigned)int_rd, (unsigned)int_cp,
        (unsigned)p_wr, (unsigned)p_wr_cold, (unsigned)p_rd, (unsigned)p_rd_cold,
        (unsigned)p_cp,
        (unsigned)mix_i2p, (unsigned)mix_p2i, (unsigned)mix_p2p,
        (unsigned)chunk[0], (unsigned)chunk[1], (unsigned)chunk[2], (unsigned)chunk[3],
        (unsigned)ms_c2m, (unsigned)ms_m2c, (unsigned)err);

    heap_caps_free(pa);
    if (pb) {
        heap_caps_free(pb);
    }
    heap_caps_free(ia);
    if (ib) {
        heap_caps_free(ib);
    }
    s_busy = false;

    ESP_LOGI(TAG, "size=%u KB it=%u int=%u/%u/%u psram=%u/%u/%u/%u/%u mix=%u/%u/%u",
             (unsigned)size_kb, (unsigned)e0,
             (unsigned)int_wr, (unsigned)int_rd, (unsigned)int_cp,
             (unsigned)p_wr, (unsigned)p_wr_cold, (unsigned)p_rd, (unsigned)p_rd_cold,
             (unsigned)p_cp, (unsigned)mix_i2p, (unsigned)mix_p2i, (unsigned)mix_p2p);
    reply_json(ctx, j);
}

static const nc_action_t s_actions[] = {
    { NC4_CMD_PSRAMBENCH, on_psrambench, "psram_bench" },
};

NC_ACTION_TABLE_DEF(nc_psram_bench_table, "psram", s_actions);

#endif  // CONFIG_NETCORE_EFFECT_BENCH
