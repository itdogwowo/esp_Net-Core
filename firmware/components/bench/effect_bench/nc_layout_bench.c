// nc_layout_bench.c — 排版編譯器（nc_px_layout）的真機量測（0xF077 → 0xF078）。
//
// 為什麼要這條：0xF069 量的是「既有 plan（連續 range）」的配對；新的 production 路徑
// 是 `nc_px_layout_build()` → dst 升序 runs → `nc_px_layout_apply()`。host 數字
// （500 組 0.66 ms）唔可以外推到 S3，所以在同一顆板、同一份 C 實作量：
//
//   build   ：group.sel → index → counting bucket → run 合併（換場一次）
//   apply   ：8-bit canvas（production 形式；runs 在 PSRAM）
//   apply12 ：12-bit canvas（效果輸出仍然是 12-bit 的路徑）
//
// payload [px:u16 LE][groups:u16 LE][variant:u8][reps:u8]
//   variant 0 = 每組一段連續 run
//   variant 1 = 每組兩段（跨區，模擬「零件散落幾條燈帶」）
// 回覆 0xF078 JSON。
#include "sdkconfig.h"

#if CONFIG_NETCORE_EFFECT_BENCH

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "nc_pixel_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nc4.h"
#include "nc_action.h"
#include "nc_px_asset.h"
#include "nc_px_layout.h"
#include "nc_px_program.h"

static const char *TAG = "layout_bench";

#define NC4_CMD_LAYOUTBENCH     0xF077
#define NC4_CMD_LAYOUTBENCH_RSP 0xF078

#define LB_GROUP_MAX 16384
#define LB_SEL_MAX   16384
#define LB_VALS_MAX  256

static volatile uint32_t s_sink;
static bool s_busy;

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
    const int total = nc4_pack_into(frame, NC4_REPLY_FRAME_MAX, NC4_CMD_LAYOUTBENCH_RSP,
                                    nc4_reply_addr(ctx), data, 2 + n);
    if (total > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)total);
    }
}

static void fill_asset(nc_px_asset_t *a, uint16_t nvals)
{
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "lbench");
    a->kind = NC_PX_ASSET_PROGRAM;
    a->pixel_hint = nvals;
    a->step = 3;
    a->spacing = 7;
    a->offset = 11;
    a->speed = 1;
    a->nseg = 2;
    a->seg[0] = (nc_px_seg_t){ .type = NC_PX_SEG_MATH_NOW, .f = 10, .l_max = 4095,
                               .l_lim = 100, .phi = 0, .end_f = 128, .pulse = 2047 };
    a->seg[1] = (nc_px_seg_t){ .type = NC_PX_SEG_SQUARE_WAVE_NOW, .f = 17, .l_max = 3000,
                               .l_lim = 0, .phi = 700, .end_f = 256, .pulse = 2047 };
}

// 把 px 平均切成 2*nchunk 段，variant 0 只用前一半、variant 1 用兩半
static void build_groups(nc_px_group_t *groups, nc_px_group_sel_t *sels,
                         uint32_t ngroups, uint32_t px, uint8_t variant)
{
    const uint32_t per = px / (ngroups * ((variant == 0) ? 1u : 2u));
    uint32_t sel = 0;
    for (uint32_t i = 0; i < ngroups; i++) {
        nc_px_group_t *g = &groups[i];
        memset(g, 0, sizeof(*g));
        g->id = (uint16_t)(i + 1);
        snprintf(g->name, sizeof(g->name), "g%u", (unsigned)i);
        g->sel = &sels[sel];
        g->nsel = 0;

        const uint32_t a0 = i * per;
        const uint32_t a1 = (i + 1) * per;
        sels[sel].hw = NC_PX_HW_WS2812;
        snprintf(sels[sel].slice, sizeof(sels[sel].slice), "%u:%u",
                 (unsigned)(uint16_t)a0, (unsigned)(uint16_t)a1);
        g->nsel++;
        sel++;

        if (variant == 1) {
            const uint32_t b0 = px / 2u + i * per;
            const uint32_t b1 = b0 + per;
            if (b1 <= px) {
                sels[sel].hw = NC_PX_HW_WS2812;
                snprintf(sels[sel].slice, sizeof(sels[sel].slice), "%u:%u",
                         (unsigned)(uint16_t)b0, (unsigned)(uint16_t)b1);
                g->nsel++;
                sel++;
            }
        }
    }
}


// 一次掃 runs、同時寫 4 個輸出（抖動批次）。只量記憶體／掃表成本：
// 4 個子幀寫同一個 8-bit 值（真正的抖動另外加門檻比較）。
static void apply4_one_pass(const nc_px_layout_t *l, const uint16_t *vals,
                            uint8_t *const out[4])
{
    for (uint32_t r = 0; r < l->nruns; r++) {
        const nc_px_lrun_t *run = &l->run[r];
        const nc_px_ljob_t *job = &l->job[run->job];
        const nc_px_aprog_t *ap = &l->aprog[job->aprog];
        const uint32_t nv = ap->nv;
        uint32_t c0 = (nv > 0) ? ((3u * run->src) % nv) : 0;
        uint32_t c1 = (nv > 0) ? ((3u * run->src + 1u) % nv) : 0;
        uint32_t c2 = (nv > 0) ? ((3u * run->src + 2u) % nv) : 0;
        for (uint32_t x = 0; x < run->count; x++) {
            const size_t o = (size_t)(run->dst + x) << 2;
            const uint8_t r8 = (nv > 0) ? (uint8_t)(vals[c0] >> 4) : 0;
            const uint8_t g8 = (nv > 0) ? (uint8_t)(vals[c1] >> 4) : 0;
            const uint8_t b8 = (nv > 0) ? (uint8_t)(vals[c2] >> 4) : 0;
            for (int s = 0; s < 4; s++) {
                out[s][o + 0] = r8;
                out[s][o + 1] = g8;
                out[s][o + 2] = b8;
                out[s][o + 3] = 0;
            }
            if (nv > 0) {
                if (++c0 == nv) c0 = 0;
                if (++c1 == nv) c1 = 0;
                if (++c2 == nv) c2 = 0;
            }
        }
    }
}


// 8×8 bit-matrix transpose（Hacker's Delight §7-3）：x 的 byte k = lane k 的 8 個 tick，
// 轉置後 byte i 的 bit k = lane k 的 bit i。即係 merge8 做的事。
static inline uint64_t swar_t8(uint64_t x)
{
    uint64_t t;
    t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL; x = x ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL; x = x ^ t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL; x = x ^ t ^ (t << 28);
    return x;
}

static void on_layoutbench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    const uint8_t *p = (f != NULL) ? f->data : NULL;
    const size_t n = (f != NULL) ? f->len : 0;
    if (ctx == NULL) {
        return;
    }
    if (s_busy) {
        reply_json(ctx, "{\"err\":\"busy\"}");
        return;
    }
    s_busy = true;

    uint16_t px = 9600;
    uint16_t ngroups = 500;
    uint8_t variant = 1;
    uint8_t reps = 8;
    if (n >= 2) {
        px = (uint16_t)(p[0] | (p[1] << 8));
    }
    if (n >= 4) {
        ngroups = (uint16_t)(p[2] | (p[3] << 8));
    }
    if (n >= 5) {
        variant = p[4];
    }
    if (n >= 6) {
        reps = p[5];
    }
    const uint16_t groups_in = ngroups;      // 0 = 完全隨機（variant 5 用）
    if (px < 2) {
        px = 2;
    }
    if (ngroups < 1) {
        ngroups = 1;
    }
    if (ngroups > LB_GROUP_MAX) {
        ngroups = LB_GROUP_MAX;
    }
    if (reps < 1) {
        reps = 1;
    }
    if (variant == 4 && ngroups == 0) {
        ngroups = 500;
    }
    if (variant == 5 && groups_in > 0) {
        ngroups = (groups_in > LB_GROUP_MAX) ? LB_GROUP_MAX : groups_in;
    }
    if (variant == 2 || ((variant == 5 || variant == 6 || variant == 7 || variant == 8 || variant == 9 || variant == 11 || variant == 12 || variant == 13 || variant == 15) && groups_in == 0)) {
        // 完全隨機：每個像素一條單顆 run（9,600 條）。用「每組 9 個單顆 sel」
        // 打包成 ~px/9 個 group，效果一樣（dst 隨機、run 全部 1 顆），但記憶體可控。
        ngroups = (px + 8u) / 9u;
        if (ngroups < 1) {
            ngroups = 1;
        }
        if (ngroups > LB_GROUP_MAX) {
            ngroups = LB_GROUP_MAX;
        }
    }

    const uint32_t alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    nc_px_group_t *groups = heap_caps_malloc((size_t)ngroups * sizeof(*groups), alloc_caps);
    nc_px_group_sel_t *sels = heap_caps_malloc((size_t)LB_SEL_MAX * sizeof(*sels), alloc_caps);
    nc_px_mode_entry_t *entries = heap_caps_malloc((size_t)ngroups * sizeof(*entries),
                                                   alloc_caps);
    // 表頭 ＋ 1 個效果一齊配置（舊版表係內建 32 個 item 的固定陣列）
    nc_px_asset_tbl_t *assets = heap_caps_malloc(sizeof(nc_px_asset_tbl_t) +
                                                 sizeof(nc_px_asset_t), alloc_caps);
    uint8_t *c8 = heap_caps_malloc((size_t)px * 4u, alloc_caps);
    uint16_t *c12 = heap_caps_malloc((size_t)px * 8u, alloc_caps);
    uint16_t *vals = heap_caps_malloc((size_t)LB_VALS_MAX * sizeof(uint16_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (groups == NULL || sels == NULL || entries == NULL || assets == NULL ||
        c8 == NULL || c12 == NULL || vals == NULL) {
        heap_caps_free(groups);
        heap_caps_free(sels);
        heap_caps_free(entries);
        heap_caps_free(assets);
        heap_caps_free(c8);
        heap_caps_free(c12);
        heap_caps_free(vals);
        s_busy = false;
        reply_json(ctx, "{\"err\":\"oom\"}");
        return;
    }

    if (variant == 2 || ((variant == 5 || variant == 6 || variant == 7 || variant == 8 || variant == 9 || variant == 11 || variant == 12 || variant == 13 || variant == 15) && groups_in == 0)) {
        // 9,600 個像素的固定種子 permutation，再每 9 個塞入一個 group
        uint32_t *perm = heap_caps_malloc((size_t)px * sizeof(uint32_t), alloc_caps);
        if (perm == NULL) {
            heap_caps_free(groups);
            heap_caps_free(sels);
            heap_caps_free(entries);
            heap_caps_free(assets);
            heap_caps_free(c8);
            heap_caps_free(c12);
            heap_caps_free(vals);
            s_busy = false;
            reply_json(ctx, "{\"err\":\"oom\"}");
            return;
        }
        for (uint32_t i = 0; i < px; i++) {
            perm[i] = i;
        }
        uint32_t seed = 12345u;
        for (uint32_t i = px; i > 1; i--) {
            seed = seed * 1103515245u + 12345u;
            const uint32_t j = (seed >> 8) % i;
            const uint32_t t = perm[i - 1];
            perm[i - 1] = perm[j];
            perm[j] = t;
        }
        uint32_t k = 0;
        for (uint32_t i = 0; i < ngroups; i++) {
            nc_px_group_t *g = &groups[i];
            memset(g, 0, sizeof(*g));
            g->id = (uint16_t)(i + 1);
            snprintf(g->name, sizeof(g->name), "g%u", (unsigned)i);
            g->sel = &sels[k];
            g->nsel = 0;
            for (int j = 0; j < 9 && k < px && k < LB_SEL_MAX; j++, k++) {
                sels[k].hw = NC_PX_HW_WS2812;
                snprintf(sels[k].slice, sizeof(sels[k].slice), "%u:%u",
                         (unsigned)(uint16_t)perm[k], (unsigned)(uint16_t)(perm[k] + 1u));
                g->nsel++;
            }
        }
        heap_caps_free(perm);
    } else {
        const uint8_t shape = (variant == 4 || variant == 5) ? 1 : variant;
        build_groups(groups, sels, ngroups, px, shape);
    }
    for (uint32_t i = 0; i < ngroups; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        snprintf(entries[i].group, sizeof(entries[i].group), "g%u", (unsigned)i);
        snprintf(entries[i].effect, sizeof(entries[i].effect), "lbench");
        entries[i].write = NC_PX_WRITE_RGB;
        entries[i].has_range = false;
    }

    nc_px_asset_t asset;
    fill_asset(&asset, 64);
    nc_px_asset_tbl_use(assets, (nc_px_asset_t *)(assets + 1), 1);
    assets->n = 1;
    assets->item[0] = asset;
    nc_px_mode_t mode;
    memset(&mode, 0, sizeof(mode));
    mode.nentry = ngroups;
    mode.entry = entries;
    nc_px_map_t map;
    memset(&map, 0, sizeof(map));
    map.ngroups = ngroups;
    map.group = groups;
    nc_px_hw_inv_t inv = { .hw = NC_PX_HW_WS2812, .base = 0, .px = px };

    nc_px_layout_t lay;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t e = nc_px_layout_build(&mode, &map, assets, &inv, 1, &lay);
    const int64_t t1 = esp_timer_get_time();

    if (e != ESP_OK) {
        heap_caps_free(groups);
        heap_caps_free(sels);
        heap_caps_free(entries);
        heap_caps_free(assets);
        heap_caps_free(c8);
        heap_caps_free(c12);
        heap_caps_free(vals);
        s_busy = false;
        char j[160];
        snprintf(j, sizeof(j), "{\"err\":%d}", (int)e);
        reply_json(ctx, j);
        return;
    }

    // 值流：64 個 12-bit 值（效果層仍然是 12-bit）
    for (uint32_t i = 0; i < 64; i++) {
        vals[i] = (uint16_t)((i * 71u + 13u) & 0xFFFu);
    }
    uint16_t *vptrs[NC_PX_LAYOUT_ASSET_MAX] = { vals };

    uint32_t merge8_us = 0, swar_us = 0, swar_ok = 0;
    if (variant == 10) {
        const uint32_t lane_px = (px >= 16u) ? (px / 16u) : px;   // 16 條線，每條 600 px
        const uint32_t lb = lane_px * 3u * 3u;   // packed bytes / lane（600 px → 5,400）
        uint8_t *lanes[16] = { NULL };
        uint8_t *o1 = (uint8_t *)heap_caps_malloc((size_t)lb * 16u, alloc_caps);
        uint8_t *o2 = (uint8_t *)heap_caps_malloc((size_t)lb * 16u, alloc_caps);
        bool ok = (o1 != NULL && o2 != NULL);
        for (int k = 0; k < 16 && ok; k++) {
            lanes[k] = (uint8_t *)heap_caps_malloc(lb, alloc_caps);
            ok = ok && (lanes[k] != NULL);
            for (uint32_t j = 0; ok && j < lb; j++) {
                lanes[k][j] = (uint8_t)((j * 31u + (uint32_t)k * 17u) & 0xFFu);
            }
        }
        if (ok) {
            // (a) 現況：兩組 merge8（store + OR）
            const int64_t a0 = esp_timer_get_time();
            memset(o1, 0, (size_t)lb * 16u);
            nc_px_codec_pack_merge8_store((const uint8_t **)&lanes[0], lb, o1, 2, 0);
            nc_px_codec_pack_merge8((const uint8_t **)&lanes[8], lb, o1, 2, 1);
            merge8_us = (uint32_t)(esp_timer_get_time() - a0);
            // (b) SWAR：每個 byte index 做兩次 8×8 轉置，組成 16-bit 字
            const int64_t b0 = esp_timer_get_time();
            for (uint32_t j = 0; j < lb; j++) {
                uint64_t xl = 0, xh = 0;
                for (int k = 0; k < 8; k++) {
                    xl |= (uint64_t)lanes[k][j] << (8 * k);
                    xh |= (uint64_t)lanes[8 + k][j] << (8 * k);
                }
                xl = swar_t8(xl);
                xh = swar_t8(xh);
                uint8_t *dst = o2 + (size_t)j * 16u;
                for (int i = 0; i < 8; i++) {
                    dst[2 * i + 0] = (uint8_t)(xl >> (8 * i));
                    dst[2 * i + 1] = (uint8_t)(xh >> (8 * i));
                }
            }
            swar_us = (uint32_t)(esp_timer_get_time() - b0);
            swar_ok = (memcmp(o1, o2, (size_t)lb * 16u) == 0) ? 1u : 0u;
            s_sink += o2[0];
        }
        for (int k = 0; k < 16; k++) {
            if (lanes[k] != NULL) heap_caps_free(lanes[k]);
        }
        if (o1 != NULL) heap_caps_free(o1);
        if (o2 != NULL) heap_caps_free(o2);
    }

    uint32_t flat_build_us = 0, flat_apply_us = 0;
    if (variant == 9 || variant == 11 || variant == 12 || variant == 13 || variant == 15) {
        // 原型：先把每一顆的落點＋三個通道起點算成 8 B（dst16, c0, c1, c2），
        // 再跑「讀 8 B、寫 4 B」的最小白迴圈。呢個就係計劃 Task 7 的形狀。
        uint16_t *flat = (uint16_t *)heap_caps_malloc((size_t)px * 8u, alloc_caps);
        uint8_t *dst = (uint8_t *)heap_caps_malloc((size_t)px * 4u, alloc_caps);
        if (flat != NULL && dst != NULL) {
            const int64_t f0 = esp_timer_get_time();
            uint32_t k = 0;
            for (uint32_t r = 0; r < lay.nruns && k < px; r++) {
                const nc_px_lrun_t *run = &lay.run[r];
                const nc_px_ljob_t *job = &lay.job[run->job];
                const nc_px_aprog_t *ap = &lay.aprog[job->aprog];
                const uint32_t n = (run->count < (px - k)) ? run->count : (px - k);
                uint32_t c0 = (ap->nv > 0) ? ((3u * run->src) % ap->nv) : 0;
                uint32_t c1 = (ap->nv > 0) ? ((3u * run->src + 1u) % ap->nv) : 0;
                uint32_t c2 = (ap->nv > 0) ? ((3u * run->src + 2u) % ap->nv) : 0;
                for (uint32_t x = 0; x < n; x++, k++) {
                    flat[k * 4 + 0] = (uint16_t)(run->dst + x);
                    flat[k * 4 + 1] = (uint16_t)c0;
                    flat[k * 4 + 2] = (uint16_t)c1;
                    flat[k * 4 + 3] = (uint16_t)c2;
                    if (ap->nv > 0) {
                        if (++c0 == ap->nv) c0 = 0;
                        if (++c1 == ap->nv) c1 = 0;
                        if (++c2 == ap->nv) c2 = 0;
                    }
                }
            }
            flat_build_us = (uint32_t)(esp_timer_get_time() - f0);
            memset(dst, 0, (size_t)px * 4u);
            const int64_t a0 = esp_timer_get_time();
            if (variant == 11) {
                uint32_t acc = 0;                 // 只讀表＋算值，唔寫入
                for (uint32_t i = 0; i < k; i++) {
                    const uint16_t *e = &flat[i * 4];
                    acc += e[0] + vals[e[1]] + vals[e[2]] + vals[e[3]];
                }
                s_sink += acc;
            } else {
                for (uint32_t i = 0; i < k; i++) {
                    const uint16_t *e = &flat[i * 4];
                    uint8_t *o = dst + ((size_t)e[0] << 2);
                    o[0] = (uint8_t)(vals[e[1]] >> 4);
                    o[1] = (uint8_t)(vals[e[2]] >> 4);
                    o[2] = (uint8_t)(vals[e[3]] >> 4);
                    o[3] = 0;
                }
            }
            flat_apply_us = (uint32_t)(esp_timer_get_time() - a0);

            if (variant == 15) {
                // 6 B 單表：dst2 + src2 + write1 + aprog1；一次掃完，冇分表
                uint8_t *flat6 = (uint8_t *)heap_caps_malloc((size_t)k * 6u, alloc_caps);
                if (flat6 != NULL) {
                    const uint32_t nv = lay.aprog[0].nv ? lay.aprog[0].nv : 1;
                    for (uint32_t i = 0; i < k; i++) {
                        const uint16_t *e = &flat[i * 4];
                        uint8_t *p6 = flat6 + (size_t)i * 6u;
                        p6[0] = (uint8_t)e[0];
                        p6[1] = (uint8_t)(e[0] >> 8);
                        p6[2] = (uint8_t)e[1];
                        p6[3] = (uint8_t)(e[1] >> 8);
                        p6[4] = NC_PX_WRITE_RGB;
                        p6[5] = 0;
                    }
                    memset(dst, 0, (size_t)px * 4u);
                    const int64_t t0 = esp_timer_get_time();
                    for (uint32_t i = 0; i < k; i++) {
                        const uint8_t *p6 = flat6 + (size_t)i * 6u;
                        const uint32_t d = (uint32_t)p6[0] | ((uint32_t)p6[1] << 8);
                        const uint32_t src = (uint32_t)p6[2] | ((uint32_t)p6[3] << 8);
                        const uint32_t c0 = (3u * src) % nv;
                        uint8_t *o = dst + ((size_t)d << 2);
                        o[0] = (uint8_t)(vals[c0] >> 4);
                        o[1] = (uint8_t)(vals[(c0 + 1u) % nv] >> 4);
                        o[2] = (uint8_t)(vals[(c0 + 2u) % nv] >> 4);
                        o[3] = 0;
                    }
                    flat_apply_us = (uint32_t)(esp_timer_get_time() - t0);
                    s_sink += dst[0];
                    heap_caps_free(flat6);
                }
            } else if (variant == 13) {
                // 兩張表：前半 rgb、後半 rgbw；每張表用專用緊迴圈（唔用內層 ch 迴圈）
                const uint32_t half = k / 2u;
                memset(dst, 0, (size_t)px * 4u);
                const int64_t s0 = esp_timer_get_time();
                for (uint32_t i = 0; i < half; i++) {          // rgb 表
                    const uint16_t *e = &flat[i * 4];
                    uint8_t *o = dst + ((size_t)e[0] << 2);
                    o[0] = (uint8_t)(vals[e[1]] >> 4);
                    o[1] = (uint8_t)(vals[e[2]] >> 4);
                    o[2] = (uint8_t)(vals[e[3]] >> 4);
                    o[3] = 0;
                }
                for (uint32_t i = half; i < k; i++) {          // rgbw 表
                    const uint16_t *e = &flat[i * 4];
                    uint8_t *o = dst + ((size_t)e[0] << 2);
                    o[0] = (uint8_t)(vals[e[1]] >> 4);
                    o[1] = (uint8_t)(vals[e[2]] >> 4);
                    o[2] = (uint8_t)(vals[e[3]] >> 4);
                    o[3] = 0;
                }
                flat_apply_us = (uint32_t)(esp_timer_get_time() - s0);
                s_sink += dst[0];
            } else if (variant == 12) {
                // 再用 4 B/顆（dst, src）試一次：表細一半，但每顆要多 3 次 %
                uint16_t *flat4 = (uint16_t *)heap_caps_malloc((size_t)px * 4u, alloc_caps);
                if (flat4 != NULL) {
                    const int64_t g0 = esp_timer_get_time();
                    uint32_t n4 = 0;
                    for (uint32_t i = 0; i < k; i++) {
                        const uint16_t *e = &flat[i * 4];
                        flat4[n4 * 2 + 0] = e[0];
                        flat4[n4 * 2 + 1] = (uint16_t)((e[1] * 64u) / (lay.aprog[0].nv ? lay.aprog[0].nv : 1));
                        n4++;
                    }
                    const uint32_t build4 = (uint32_t)(esp_timer_get_time() - g0);
                    memset(dst, 0, (size_t)px * 4u);
                    const int64_t h0 = esp_timer_get_time();
                    const uint32_t nv = lay.aprog[0].nv ? lay.aprog[0].nv : 1;
                    for (uint32_t i = 0; i < n4; i++) {
                        const uint32_t d = flat4[i * 2 + 0];
                        const uint32_t src = flat4[i * 2 + 1];
                        const uint32_t c0 = (3u * src) % nv;
                        uint8_t *o = dst + ((size_t)d << 2);
                        o[0] = (uint8_t)(vals[c0] >> 4);
                        o[1] = (uint8_t)(vals[(c0 + 1u) % nv] >> 4);
                        o[2] = (uint8_t)(vals[(c0 + 2u) % nv] >> 4);
                        o[3] = 0;
                    }
                    flat_build_us += build4;
                    flat_apply_us = (uint32_t)(esp_timer_get_time() - h0);
                    s_sink += dst[0];
                    heap_caps_free(flat4);
                }
            }
            s_sink += dst[0];
        }
        if (flat != NULL) heap_caps_free(flat);
        if (dst != NULL) heap_caps_free(dst);
    }

    uint32_t direct_us = 0, canvas_us = 0, direct4_us = 0, direct_noclear_us = 0;
    uint32_t batch4_us = 0, batch4_1pass_us = 0, batch4_1pass_noclear_us = 0;
    if (variant == 8) {
        uint8_t *outs[4] = { NULL, NULL, NULL, NULL };
        bool ok = true;
        for (int s = 0; s < 4; s++) {
            outs[s] = (uint8_t *)heap_caps_malloc((size_t)px * 4u, alloc_caps);
            ok = ok && (outs[s] != NULL);
        }
        if (ok) {
            // (a) 4 次獨立掃表（現況）
            const int64_t a0 = esp_timer_get_time();
            for (int s = 0; s < 4; s++) {
                memset(outs[s], 0, (size_t)px * 4u);
                (void)nc_px_layout_apply(&lay, vptrs, outs[s]);
            }
            batch4_us = (uint32_t)(esp_timer_get_time() - a0);
            // (b) 一次掃表 → 寫 4 個輸出
            const int64_t b0 = esp_timer_get_time();
            for (int s = 0; s < 4; s++) {
                memset(outs[s], 0, (size_t)px * 4u);
            }
            apply4_one_pass(&lay, vals, outs);
            batch4_1pass_us = (uint32_t)(esp_timer_get_time() - b0);
            // (c) 一次掃表、寫 4 份、唔清（全覆蓋時）
            const int64_t c0b = esp_timer_get_time();
            apply4_one_pass(&lay, vals, outs);
            batch4_1pass_noclear_us = (uint32_t)(esp_timer_get_time() - c0b);
            s_sink += outs[0][0];
        }
        for (int s = 0; s < 4; s++) {
            if (outs[s] != NULL) {
                heap_caps_free(outs[s]);
            }
        }
    }
    if (variant == 6 || variant == 7) {
        uint8_t *rb = (uint8_t *)heap_caps_malloc((size_t)px * 4u, alloc_caps);
        if (rb != NULL) {
            // (a) 現況：清畫布 + 寫畫布 + 複製去交貨區
            uint32_t best_canvas = 0xFFFFFFFFu;
            for (int r = 0; r < 5; r++) {
                const int64_t a = esp_timer_get_time();
                memset(c8, 0, (size_t)px * 4u);
                (void)nc_px_layout_apply(&lay, vptrs, c8);
                memcpy(rb, c8, (size_t)px * 4u);
                const uint32_t us = (uint32_t)(esp_timer_get_time() - a);
                if (us < best_canvas) {
                    best_canvas = us;
                }
            }
            // (b) 合併：直接寫交貨區（唔經畫布、唔使複製）
            uint32_t best_direct = 0xFFFFFFFFu;
            for (int r = 0; r < 5; r++) {
                const int64_t a = esp_timer_get_time();
                memset(rb, 0, (size_t)px * 4u);
                (void)nc_px_layout_apply(&lay, vptrs, rb);
                const uint32_t us = (uint32_t)(esp_timer_get_time() - a);
                if (us < best_direct) {
                    best_direct = us;
                }
            }
            // (b2) 合併＋唔清畫布（layout 全覆蓋時可以省）
            uint32_t best_noclear = 0xFFFFFFFFu;
            for (int r = 0; r < 5; r++) {
                const int64_t a = esp_timer_get_time();
                (void)nc_px_layout_apply(&lay, vptrs, rb);
                const uint32_t us = (uint32_t)(esp_timer_get_time() - a);
                if (us < best_noclear) {
                    best_noclear = us;
                }
            }
            direct_noclear_us = best_noclear;

            // (c) 抖動：4 個子幀都直接寫交貨區（每子幀仍是 8-bit）
            const int64_t a4 = esp_timer_get_time();
            for (int sf = 0; sf < 4; sf++) {
                memset(rb, 0, (size_t)px * 4u);
                (void)nc_px_layout_apply(&lay, vptrs, rb);
            }
            direct4_us = (uint32_t)(esp_timer_get_time() - a4);
            direct_us = best_direct;
            canvas_us = best_canvas;
            s_sink += rb[0];
            heap_caps_free(rb);
        }
    }

    uint32_t save_us = 0, load_us = 0;
    long cache_bytes = -1;
    esp_err_t load_rc = ESP_OK;
    if (variant == 5) {
        const uint32_t key = nc_px_layout_cache_key(0x1234u, 0x5678u, px);
        const char *cpath = "/data/bench_layout.pxlt";
        const int64_t q0 = esp_timer_get_time();
        const esp_err_t s_rc = nc_px_layout_save(&lay, cpath, key);
        save_us = (uint32_t)(esp_timer_get_time() - q0);
        if (s_rc == ESP_OK) {
            FILE *f = fopen(cpath, "rb");
            if (f != NULL) {
                (void)fseek(f, 0, SEEK_END);
                cache_bytes = ftell(f);
                fclose(f);
            }
            nc_px_layout_t l2;
            const int64_t q1 = esp_timer_get_time();
            load_rc = nc_px_layout_load(&l2, cpath, key, assets);
            load_us = (uint32_t)(esp_timer_get_time() - q1);
            if (load_rc == ESP_OK) {
                nc_px_layout_free(&l2);
            }
        }
    }

    // variant 4：模擬 10-bit 抖動的 4 個子幀（每子幀仍是 8-bit 畫布）
    uint8_t *ring = NULL;
    uint32_t sub_us[4] = { 0, 0, 0, 0 };
    if (variant == 4) {
        ring = (uint8_t *)heap_caps_malloc((size_t)px * 4u, alloc_caps);
        if (ring == NULL) {
            nc_px_layout_free(&lay);
            heap_caps_free(groups);
            heap_caps_free(sels);
            heap_caps_free(entries);
            heap_caps_free(assets);
            heap_caps_free(c8);
            heap_caps_free(c12);
            heap_caps_free(vals);
            s_busy = false;
            reply_json(ctx, "{\"err\":\"oom\"}");
            return;
        }
        for (int sf = 0; sf < 4; sf++) {
            const int64_t s0 = esp_timer_get_time();
            memset(c8, 0, (size_t)px * 4u);
            (void)nc_px_layout_apply(&lay, vptrs, c8);
            memcpy(ring, c8, (size_t)px * 4u);
            sub_us[sf] = (uint32_t)(esp_timer_get_time() - s0);
            s_sink += ring[sf];
        }
    }

    uint32_t apply_us = 0xFFFFFFFFu;
    uint32_t apply12_us = 0xFFFFFFFFu;
    for (uint8_t r = 0; r < reps; r++) {
        const int64_t a0 = esp_timer_get_time();
        (void)nc_px_layout_apply(&lay, vptrs, c8);
        const int64_t a1 = esp_timer_get_time();
        const uint32_t us = (uint32_t)(a1 - a0);
        if (us < apply_us) {
            apply_us = us;
        }
        s_sink += c8[r & 3u];
    }
    for (uint8_t r = 0; r < reps; r++) {
        const int64_t a0 = esp_timer_get_time();
        (void)nc_px_layout_apply12(&lay, vptrs, c12);
        const int64_t a1 = esp_timer_get_time();
        const uint32_t us = (uint32_t)(a1 - a0);
        if (us < apply12_us) {
            apply12_us = us;
        }
        s_sink += c12[r & 3u];
    }

    // Task 7：6 B 落點表 vs 舊 run 路徑。同一份 layout，copy 一份清走 flat 就係舊路徑，
    // 所以兩個數字係同一次上板、同一份資料量出嚟（唔使分兩次燒）。
    uint32_t apply_runs_us = 0xFFFFFFFFu;
    {
        nc_px_layout_t lr = lay;
        lr.flat = NULL;
        lr.nflat = 0;
        for (uint8_t r = 0; r < reps; r++) {
            const int64_t r0 = esp_timer_get_time();
            (void)nc_px_layout_apply(&lr, vptrs, c8);
            const int64_t r1 = esp_timer_get_time();
            const uint32_t us = (uint32_t)(r1 - r0);
            if (us < apply_runs_us) {
                apply_runs_us = us;
            }
            s_sink += c8[r & 3u];
        }
    }

    // production 真正行的係「抖動層逐子幀寫」：4 個子幀 ＝ 4 次掃表。同一份 layout
    // 兩種表各量一次，先知換表對實際 output 有幾多幫助。
    uint32_t dither_us = 0xFFFFFFFFu;
    uint32_t dither_runs_us = 0xFFFFFFFFu;
    for (int round = 0; round < 2; round++) {
        nc_px_layout_t lr = lay;
        lr.flat = NULL;
        lr.nflat = 0;
        const int64_t f0 = esp_timer_get_time();
        for (uint8_t sf = 0; sf < 4; sf++) {
            nc_px_layout_apply_dither(&lay, vptrs, c8, 4, sf, (uint32_t)sf);
        }
        const int64_t f1 = esp_timer_get_time();
        for (uint8_t sf = 0; sf < 4; sf++) {
            nc_px_layout_apply_dither(&lr, vptrs, c8, 4, sf, (uint32_t)sf);
        }
        const int64_t f2 = esp_timer_get_time();
        const uint32_t a = (uint32_t)(f1 - f0);
        const uint32_t b = (uint32_t)(f2 - f1);
        if (a < dither_us) {
            dither_us = a;
        }
        if (b < dither_runs_us) {
            dither_runs_us = b;
        }
        s_sink += c8[round];
    }

    char j[800];
    snprintf(j, sizeof(j),
             "{\"px\":%u,\"groups\":%u,\"variant\":%u,\"jobs\":%u,\"runs\":%u,"
             "\"nv\":%u,\"build_us\":%u,\"apply_us\":%u,\"apply12_us\":%u,"
             "\"dither_us\":%u,\"dither_runs_us\":%u,"
             "\"flat_n\":%u,\"apply_runs_us\":%u,\"flat_int\":%d,"
             "\"sub_us\":[%u,%u,%u,%u],\"sub_total_us\":%u,"
             "\"save_us\":%u,\"load_us\":%u,\"cache_bytes\":%ld,\"load_rc\":%d,"
             "\"merge8_us\":%u,\"swar_us\":%u,\"swar_ok\":%u,"
             "\"flat_build_us\":%u,\"flat_apply_us\":%u,"
             "\"canvas_path_us\":%u,\"direct_us\":%u,\"direct_noclear_us\":%u,"
             "\"direct4_us\":%u,\"batch4_us\":%u,\"batch4_1pass_us\":%u,"
             "\"batch4_1pass_noclear_us\":%u,"
             "\"runs_psram\":%d,\"c8_psram\":%d,\"c12_psram\":%d}",
             (unsigned)px, (unsigned)ngroups, (unsigned)variant,
             (unsigned)lay.njobs, (unsigned)lay.nruns, 64u,
             (unsigned)(t1 - t0), (unsigned)apply_us, (unsigned)apply12_us,
             (unsigned)dither_us, (unsigned)dither_runs_us,
             (unsigned)lay.nflat, (unsigned)apply_runs_us,
             lay.flat ? (esp_ptr_internal(lay.flat) ? 1 : 0) : -1,
             (unsigned)sub_us[0], (unsigned)sub_us[1],
             (unsigned)sub_us[2], (unsigned)sub_us[3],
             (unsigned)(sub_us[0] + sub_us[1] + sub_us[2] + sub_us[3]),
             (unsigned)save_us, (unsigned)load_us, cache_bytes, (int)load_rc,
             (unsigned)merge8_us, (unsigned)swar_us, (unsigned)swar_ok,
             (unsigned)flat_build_us, (unsigned)flat_apply_us,
             (unsigned)canvas_us, (unsigned)direct_us, (unsigned)direct_noclear_us,
             (unsigned)direct4_us,
             (unsigned)batch4_us, (unsigned)batch4_1pass_us,
             (unsigned)batch4_1pass_noclear_us,
             lay.run ? (esp_ptr_external_ram(lay.run) ? 1 : 0) : -1,
             esp_ptr_external_ram(c8) ? 1 : 0,
             esp_ptr_external_ram(c12) ? 1 : 0);

    ESP_LOGI(TAG, "px=%u groups=%u v=%u jobs=%u runs=%u build=%u us apply=%u us apply12=%u us",
             (unsigned)px, (unsigned)ngroups, (unsigned)variant, (unsigned)lay.njobs,
             (unsigned)lay.nruns, (unsigned)(t1 - t0), (unsigned)apply_us,
             (unsigned)apply12_us);

    if (ring != NULL) {
        heap_caps_free(ring);
    }
    nc_px_layout_free(&lay);
    heap_caps_free(groups);
    heap_caps_free(sels);
    heap_caps_free(entries);
    heap_caps_free(assets);
    heap_caps_free(c8);
    heap_caps_free(c12);
    heap_caps_free(vals);
    s_busy = false;
    reply_json(ctx, j);
}

static const nc_action_t s_actions[] = {
    { NC4_CMD_LAYOUTBENCH, on_layoutbench, "layout_bench" },
};

NC_ACTION_TABLE_DEF(nc_layout_bench_table, "lbench", s_actions);

#endif  // CONFIG_NETCORE_EFFECT_BENCH
