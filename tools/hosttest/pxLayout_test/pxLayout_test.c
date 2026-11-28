// pxLayout_test — 排版編譯器（nc_px_layout）：group.sel → dst 順序 runs 的 host 測試。
//
// 覆蓋：
//   1. 單一 group（連續鏈）與舊 plan＋pair 逐 byte 對拍
//   2. 反序 slice "31:15:-1"（舊 path 表達不到）
//   3. 隔顆 slice "::2"
//   4. 重疊寫（兩個 group 寫同一批燈的不同通道）
//   5. 500 個 group × 16×600 任意組合（run 數、覆蓋率、抽樣值）
//   6. 編譯／配對的 host 時間（僅供參考，唔係真機數字）
// clock_gettime／CLOCK_MONOTONIC 要 POSIX feature macro（gcc 13 預設 -std=c17 會收埋）
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nc_px_layout.h"
#include "nc_px_show.h"

static int g_fail;

static void check(bool cond, const char *name)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) {
        g_fail++;
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// 一個簡單的 program 資產：pixel_n 顆、math_now、波長 256
static void make_asset(nc_px_asset_t *a, const char *name, uint16_t pixel_n)
{
    memset(a, 0, sizeof(*a));
    a->name_id = nc_px_name_dict_global_intern(name, strlen(name));
    a->name = nc_px_name_dict_global_str(a->name_id);
    a->kind = NC_PX_ASSET_PROGRAM;
    a->pixel_hint = pixel_n;
    a->step = 1;
    a->spacing = 0;
    a->offset = 0;
    a->speed = 1;
    a->reverse = false;
    nc_px_seg_t *seg = nc_px_asset_seg_add(a);
    seg->type = NC_PX_SEG_MATH_NOW;
    seg->f = 10;
    seg->l_max = 384;
    seg->l_lim = 0;
    seg->phi = 0;
    seg->end_f = 256;
}

// 值流：固定公式（方便抽樣核對）
static void fill_vals(uint16_t *v, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) {
        v[i] = (uint16_t)((i * 37u + seed * 101u) & 0xFFFu);
    }
}

// ── [1] 單一 group 連續鏈：新舊 path 對拍 ──
static void test_single_chain(void)
{
    printf("[1] 單一 group（0:32）新舊 path 對拍\n");
    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 64);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ asset } };

    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 32 } };

    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_t group = { .id = 1, .name = "full", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "matrix", .group = &group, .ngroups = 1 };

    nc_px_mode_entry_t entry = {
        .group = "matrix.full", .effect = "breathe", .write = NC_PX_WRITE_RGB,
        .has_range = false, .start = 0, .stop = 0, .step = 0,
    };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &entry };

    nc_px_layout_t l;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &l) == ESP_OK, "layout 建立");
    check(l.px == 32, "px = 32");
    check(l.njobs == 1, "1 個 job");
    check(l.nruns == 1 && l.run[0].dst == 0 && l.run[0].src == 0 && l.run[0].count == 32,
          "1 條 run：dst 0..31 / src 0..31");
    check(l.aprog[0].nv == 64, "nv = pixel_n 64（唔再 cap 256 以下的 64 也照用）");

    uint16_t vals[64];
    fill_vals(vals, 64, 3);
    uint16_t *vptrs[1] = { vals };
    uint8_t canvas[32 * 4];
    memset(canvas, 0, sizeof(canvas));
    nc_px_layout_apply(&l, vptrs, canvas);

    // 舊 path：plan＋pair_apply
    static nc_px_plan_t plan;
    static nc_px_job_t plan_jobs[1];
    check(nc_px_show_plan(&mode, &map, &assets, inv, 1, &plan, plan_jobs, 1) == ESP_OK,
          "舊 plan 建立");
    uint8_t old_canvas[32 * 4];
    memset(old_canvas, 0, sizeof(old_canvas));
    nc_px_pair_apply(&plan.job[0], vals, 64, old_canvas);
    check(memcmp(canvas, old_canvas, sizeof(canvas)) == 0, "新舊 path 逐 byte 相同");
    nc_px_layout_free(&l);
}

// ── [2] 反序 slice ──
static void test_reverse(void)
{
    printf("[2] 反序 slice（31:15:-1）\n");
    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 64);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ asset } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 32 } };

    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = "31:15:-1" } };
    nc_px_group_t group = { .id = 1, .name = "rev", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "matrix", .group = &group, .ngroups = 1 };
    nc_px_mode_entry_t entry = {
        .group = "rev", .effect = "breathe", .write = NC_PX_WRITE_R,
    };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &entry };

    nc_px_layout_t l;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &l) == ESP_OK, "layout 建立");
    check(l.njobs == 1, "1 個 job（反序段唔會被丟棄）");
    // 反序：dst 升序時 src 遞減，兩者唔可以合併成一段 → 16 條 run（每條 1 顆）
    check(l.nruns == 16 && l.run[0].dst == 16 && l.run[0].src == 15 &&
          l.run[15].dst == 31 && l.run[15].src == 0,
          "16 條 run：dst 16..31 ← src 15..0（反序保留）");

    uint16_t vals[64];
    fill_vals(vals, 64, 1);
    uint16_t *vptrs[1] = { vals };
    uint8_t canvas[32 * 4];
    memset(canvas, 0, sizeof(canvas));
    nc_px_layout_apply(&l, vptrs, canvas);
    check(canvas[31 * 4 + 0] == (uint8_t)(vals[0] >> 4), "pixel 31 = vals[0]（反序第一顆）");
    check(canvas[16 * 4 + 0] == (uint8_t)(vals[15] >> 4), "pixel 16 = vals[15]");
    nc_px_layout_free(&l);
}

// ── [3] 隔顆 slice ──
static void test_step(void)
{
    printf("[3] 隔顆 slice（::2）\n");
    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 64);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ asset } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 32 } };

    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = "::2" } };
    nc_px_group_t group = { .id = 1, .name = "even", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "matrix", .group = &group, .ngroups = 1 };
    nc_px_mode_entry_t entry = {
        .group = "even", .effect = "breathe", .write = NC_PX_WRITE_G,
    };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &entry };

    nc_px_layout_t l;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &l) == ESP_OK, "layout 建立");
    check(l.job[0].nled == 16, "16 顆（唔再只取 4 粒）");
    check(l.nruns == 16, "16 條 run（每顆一段）");
    uint16_t vals[64];
    fill_vals(vals, 64, 5);
    uint16_t *vptrs[1] = { vals };
    uint8_t canvas[32 * 4];
    memset(canvas, 0, sizeof(canvas));
    nc_px_layout_apply(&l, vptrs, canvas);
    bool ok = true;
    for (int k = 0; k < 16; k++) {
        if (canvas[k * 2 * 4 + 1] != (uint8_t)(vals[k] >> 4)) {
            ok = false;
        }
        if (canvas[(k * 2 + 1) * 4 + 1] != 0) {
            ok = false;
        }
    }
    check(ok, "偶數顆 = vals[k]，奇數顆不動");
    nc_px_layout_free(&l);
}

// ── [4] 重疊寫（R 與 G 兩個 job 打同一批燈）──
static void test_overlap(void)
{
    printf("[4] 重疊寫（同一批燈：job0 寫 R、job1 寫 G）\n");
    nc_px_asset_t a0;
    nc_px_asset_t a1;
    make_asset(&a0, "red", 16);
    make_asset(&a1, "green", 16);
    nc_px_asset_tbl_t assets = { .n = 2, .cap = 2,
                                      .item = (nc_px_asset_t[]){ a0, a1 } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 8 } };

    nc_px_group_sel_t s0[] = { { .hw = NC_PX_HW_WS2812, .slice = "0:8" } };
    nc_px_group_sel_t s1[] = { { .hw = NC_PX_HW_WS2812, .slice = "0:8" } };
    nc_px_group_t groups[2] = {
        { .id = 1, .name = "g0", .sel = s0, .nsel = 1 },
        { .id = 2, .name = "g1", .sel = s1, .nsel = 1 },
    };
    nc_px_map_t map = { .id = 1, .name = "m", .group = groups, .ngroups = 2 };
    nc_px_mode_entry_t entries[2] = {
        { .group = "g0", .effect = "red", .write = NC_PX_WRITE_R },
        { .group = "g1", .effect = "green", .write = NC_PX_WRITE_G },
    };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 2, .entry = entries };

    nc_px_layout_t l;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &l) == ESP_OK, "layout 建立");
    uint16_t v0[16];
    uint16_t v1[16];
    fill_vals(v0, 16, 2);
    fill_vals(v1, 16, 9);
    uint16_t *vptrs[2] = { v0, v1 };
    uint8_t canvas[8 * 4];
    memset(canvas, 0, sizeof(canvas));
    nc_px_layout_apply(&l, vptrs, canvas);
    bool ok = true;
    for (int k = 0; k < 8; k++) {
        if (canvas[k * 4 + 0] != (uint8_t)(v0[k] >> 4)) {
            ok = false;
        }
        if (canvas[k * 4 + 1] != (uint8_t)(v1[k] >> 4)) {
            ok = false;
        }
    }
    check(ok, "R 來自 job0、G 來自 job1，互不清走");
    nc_px_layout_free(&l);
}

// ── [5] 500 個 group × 16×600 任意組合 ──
static void test_500_groups(void)
{
    printf("[5] 500 個 group（16×600 = 9,600 px，隨機組合）\n");
    enum { PX = 9600 };
    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 64);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ asset } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = PX } };

    enum { NG = 500 };
    static char slices[NG][NC_PX_SLICE_MAX];
    static nc_px_group_sel_t sels[NG];
    static nc_px_group_t groups[NG];
    static nc_px_mode_entry_t entries[NG];
    static char names[NG][NC_PX_LAYER_NAME_MAX];
    static char eff[NG][NC_PX_LAYER_NAME_MAX];

    // 把 9,600 px 由頭到尾切成 500 段（長度 2–120，總和 = PX），次序打亂
    uint32_t lens[NG];
    uint32_t rem = 9600;
    uint32_t seed = 12345;
    for (int i = 0; i < NG - 1; i++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t choices[] = { 2, 3, 12, 19, 40, 120 };
        uint32_t ln = choices[(seed >> 16) % 6];
        if (ln > rem - (uint32_t)(NG - 1 - i)) {
            ln = 1;
        }
        lens[i] = ln;
        rem -= ln;
    }
    lens[NG - 1] = rem;
    // 隨機排列段落次序（group 之間任意跳）
    for (int i = NG - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        const int j = (int)((seed >> 8) % (uint32_t)(i + 1));
        const uint32_t t = lens[i];
        lens[i] = lens[j];
        lens[j] = t;
    }
    uint32_t pos = 0;
    for (int i = 0; i < NG; i++) {
        const uint32_t ln = lens[i];
        snprintf(slices[i], sizeof(slices[i]), "%u:%u", (unsigned)pos, (unsigned)(pos + ln));
        sels[i].hw = NC_PX_HW_WS2812;
        memcpy(sels[i].slice, slices[i], sizeof(sels[i].slice));
        snprintf(names[i], sizeof(names[i]), "g%d", i);
        snprintf(eff[i], sizeof(eff[i]), "breathe");
        groups[i].id = (uint8_t)(i & 0xFF);
        groups[i].name_id = nc_px_name_dict_global_intern(names[i], strlen(names[i]));
        groups[i].name = nc_px_name_dict_global_str(groups[i].name_id);
        groups[i].sel = &sels[i];
        groups[i].nsel = 1;
        entries[i].group_id = nc_px_name_dict_global_intern(names[i], strlen(names[i]));
        entries[i].group = nc_px_name_dict_global_str(entries[i].group_id);
        entries[i].effect_id = nc_px_name_dict_global_intern(eff[i], strlen(eff[i]));
        entries[i].effect = nc_px_name_dict_global_str(entries[i].effect_id);
        entries[i].write = NC_PX_WRITE_RGB;
        pos += ln;
    }
    check(pos == PX, "切段總和 = 9,600");
    nc_px_map_t map = { .id = 1, .name = "big", .group = groups, .ngroups = NG };
    nc_px_mode_t mode = { .id = 1, .name = "big", .nentry = NG, .entry = entries };

    nc_px_layout_t l;
    const double t0 = now_ms();
    const esp_err_t e = nc_px_layout_build(&mode, &map, &assets, inv, 1, &l);
    const double t1 = now_ms();
    check(e == ESP_OK, "layout 建立（500 個條目）");
    check(l.px == PX, "px = 9,600");
    check(l.njobs == NG, "500 個 job（唔再截 8）");
    check(l.aprog[0].nv == 64, "值流 64 值（共用一個 asset）");
    uint32_t covered = 0;
    for (uint32_t r = 0; r < l.nruns; r++) {
        covered += l.run[r].count;
    }
    check(covered == PX, "run 覆蓋 9,600 顆");
    check(l.nruns >= 500 && l.nruns <= PX, "run 數合理（>=500 段）");

    // 抽樣：每條 run 的 dst/src 對應正確（dst 升序、src 與 group 次序一致）
    bool mono = true;
    for (uint32_t r = 1; r < l.nruns; r++) {
        if (l.run[r].dst < l.run[r - 1].dst) {
            mono = false;
        }
    }
    check(mono, "runs dst 嚴格非遞減");

    static uint16_t vals[64];
    fill_vals(vals, 64, 7);
    uint16_t *vptrs[1] = { vals };
    static uint8_t canvas[PX * 4];
    memset(canvas, 0, sizeof(canvas));
    const double t2 = now_ms();
    nc_px_layout_apply(&l, vptrs, canvas);
    const double t3 = now_ms();
    // 抽 20 顆核對
    bool spot = true;
    for (uint32_t r = 0; r < l.nruns && spot; r += (l.nruns / 20 + 1)) {
        const nc_px_lrun_t *run = &l.run[r];
        const uint32_t x = run->count / 2;
        const uint32_t k = run->src + x;
        const uint8_t want = (uint8_t)(vals[(3u * k) % 64] >> 4);   // rgb：R = vals[3k]
        if (canvas[(run->dst + x) * 4 + 0] != want) {
            spot = false;
        }
    }
    check(spot, "抽樣 20 顆：dst ← vals[src] 正確");

    printf("  build %.2f ms、apply %.2f ms（host x86，含 500 段隨機次序）\n",
           t1 - t0, t3 - t2);

    // 6 B 表 vs run 路徑（同一份 layout、同一個 canvas；host 數字只睇方向，
    // 真機要用 tools/layout_bench.py 量）
    check(l.nflat == PX, "9,600 px 全部展開（1 顆 1 筆）");
    nc_px_layout_t lr = l;
    lr.flat = NULL;
    lr.nflat = 0;
    const uint32_t reps = 50;
    double f_ms = 0.0;
    double r_ms = 0.0;
    for (int round = 0; round < 2; round++) {
        const double a0 = now_ms();
        for (uint32_t i = 0; i < reps; i++) {
            nc_px_layout_apply(&l, vptrs, canvas);
        }
        const double a1 = now_ms();
        for (uint32_t i = 0; i < reps; i++) {
            nc_px_layout_apply(&lr, vptrs, canvas);
        }
        const double a2 = now_ms();
        f_ms = (a1 - a0) / (double)reps;
        r_ms = (a2 - a1) / (double)reps;
    }
    printf("  apply：6 B 表 %.3f ms、runs %.3f ms（%.2f×，host 參考）\n",
           f_ms, r_ms, (f_ms > 0.0) ? (r_ms / f_ms) : 0.0);
    nc_px_layout_free(&l);
}


// ── [6] 由真實 map／mode JSON（500 組）走完整條：count → pool → parse → compile ──
static void test_json_500(void)
{
    printf("[6] 500 組 map／mode JSON → parse → compile\n");
    enum { NG = 500, PX = 9600 };
    // 組 JSON：500 個 group，每組一段連續 slice（長度由 2..120 變化）
    size_t cap = 64u + NG * 220u;
    char *map_js = malloc(cap);
    char *mode_js = malloc(cap);
    if (map_js == NULL || mode_js == NULL) {
        free(map_js);
        free(mode_js);
        check(false, "malloc");
        return;
    }
    size_t n = 0;
    n += (size_t)snprintf(map_js + n, cap - n, "{\"id\":1,\"name\":\"big\",\"groups\":[");
    uint32_t pos = 0;
    uint32_t expect_jobs = 0;
    static const uint32_t lens[6] = { 2, 3, 12, 19, 40, 120 };
    for (int i = 0; i < NG; i++) {
        uint32_t ln = lens[i % 6];
        if (pos + ln > PX) {
            ln = PX - pos;
        }
        if (ln > 0) {
            expect_jobs++;
        }
        n += (size_t)snprintf(map_js + n, cap - n,
                              "%s{\"id\":%d,\"name\":\"g%d\",\"sel\":[{\"type\":\"ws2812\","
                              "\"sel\":\"%u:%u\"}]}",
                              (i == 0) ? "" : ",", i + 1, i, (unsigned)pos,
                              (unsigned)(pos + ln));
        pos += ln;
    }
    snprintf(map_js + n, cap - n, "]}");
    n = 0;
    n += (size_t)snprintf(mode_js + n, cap - n, "{\"id\":1,\"name\":\"big\",\"map\":[");
    for (int i = 0; i < NG; i++) {
        n += (size_t)snprintf(mode_js + n, cap - n,
                              "%s{\"group\":\"g%d\",\"effect\":\"breathe\",\"write\":\"rgb\"}",
                              (i == 0) ? "" : ",", i);
    }
    snprintf(mode_js + n, cap - n, "]}");

    uint32_t ng = 0;
    uint32_t ns = 0;
    check(nc_px_map_count(map_js, strlen(map_js), &ng, &ns) == ESP_OK, "map count");
    check(ng == NG && ns == NG, "count = 500 組／500 段");
    nc_px_group_t *grp = calloc(ng ? ng : 1, sizeof(*grp));
    nc_px_group_sel_t *sel = calloc(ns ? ns : 1, sizeof(*sel));
    nc_px_map_pool_t pool = { .group = grp, .cap_group = ng, .sel = sel, .cap_sel = ns ? ns : 1 };
    nc_px_map_t map;
    check(nc_px_map_parse(map_js, strlen(map_js), &pool, &map) == ESP_OK, "map parse");
    check(map.ngroups == NG, "map 500 組");

    uint32_t ne = 0;
    check(nc_px_mode_count(mode_js, strlen(mode_js), &ne) == ESP_OK, "mode count");
    check(ne == NG, "mode count = 500");
    nc_px_mode_entry_t *ent = calloc(ne ? ne : 1, sizeof(*ent));
    nc_px_mode_t mode;
    check(nc_px_mode_parse(mode_js, strlen(mode_js), ent, ne, &mode) == ESP_OK, "mode parse");
    check(mode.nentry == NG, "mode 500 條目");

    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 64);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ asset } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = PX } };

    nc_px_layout_t l;
    const double t0 = now_ms();
    const esp_err_t e = nc_px_layout_build(&mode, &map, &assets, inv, 1, &l);
    const double t1 = now_ms();
    check(e == ESP_OK, "由 JSON 直接 compile");
    check(l.njobs == expect_jobs, "job 數 = 非空組數（500 組內）");
    check(l.px == PX, "px = 9,600");
    uint32_t covered = 0;
    for (uint32_t r = 0; r < l.nruns; r++) {
        covered += l.run[r].count;
    }
    check(covered == PX, "run 覆蓋 9,600");
    printf("  JSON→compile %.2f ms（%u 組→%u job、%u runs，含 parse）\n",
           t1 - t0, (unsigned)NG, (unsigned)l.njobs, (unsigned)l.nruns);
    nc_px_layout_free(&l);
    free(grp);
    free(sel);
    free(ent);
    free(map_js);
    free(mode_js);
}


// ── [7] 組合快取：存檔／讀回（同一 key）逐項一致；key 唔夾要拒絕 ──
static void test_cache_roundtrip(void)
{
    printf("[7] 組合快取：save → load\n");
    nc_px_asset_t asset;
    make_asset(&asset, "breathe", 64);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ asset } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 64 } };

    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_t group = { .id = 1, .name = "full", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "m", .group = &group, .ngroups = 1, .hash = 0x1111 };
    nc_px_mode_entry_t entry = { .group = "full", .effect = "breathe",
                                 .write = NC_PX_WRITE_RGB };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &entry, .hash = 0x2222 };

    nc_px_layout_t a;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &a) == ESP_OK, "建立");
    // 效果表指紋（0 = 測試用常數；生產路徑由 paint 計真 hash）
    const uint32_t key = nc_px_layout_cache_key(mode.hash, map.hash, 64, 0u);
    const char *path = "/tmp/pxlayout_cache_test.bin";
    check(nc_px_layout_save(&a, path, key) == ESP_OK, "存檔");

    nc_px_layout_t b;
    check(nc_px_layout_load(&b, path, key, &assets) == ESP_OK, "讀回");
    check(b.px == a.px && b.njobs == a.njobs && b.nruns == a.nruns && b.nvals == a.nvals,
          "尺寸／條目／段數一致");
    check(b.job[0].write == a.job[0].write && b.job[0].nled == a.job[0].nled,
          "job 一致");
    check(b.run[0].dst == a.run[0].dst && b.run[0].count == a.run[0].count &&
          b.run[0].src == a.run[0].src, "run 一致");
    check(b.aprog[0].nv == a.aprog[0].nv, "值流長度一致");

    uint16_t vals[64];
    fill_vals(vals, 64, 4);
    uint16_t *vp[1] = { vals };
    uint8_t ca[64 * 4], cb[64 * 4];
    memset(ca, 0, sizeof(ca));
    memset(cb, 0, sizeof(cb));
    nc_px_layout_apply(&a, vp, ca);
    nc_px_layout_apply(&b, vp, cb);
    check(memcmp(ca, cb, sizeof(ca)) == 0, "配對結果逐 byte 相同");

    nc_px_layout_t c;
    check(nc_px_layout_load(&c, path, key ^ 1u, &assets) == ESP_ERR_INVALID_STATE,
          "key 唔夾 → 拒絕（由呼叫端重建）");
    nc_px_layout_free(&a);
    nc_px_layout_free(&b);
}


// ── [9] 群組重疊：同一顆被兩個 entry 寫到，現在是疊加還是最後贏？ ──
static void test_overlap_same_channel(void)
{
    printf("[9] 群組重疊（A=0:4、B=2:5，重疊 2、3）\n");
    nc_px_asset_t assetA, assetB;
    make_asset(&assetA, "A", 4);
    make_asset(&assetB, "B", 4);
    nc_px_asset_tbl_t assets = { .n = 2, .cap = 2,
                                      .item = (nc_px_asset_t[]){ assetA, assetB } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 5 } };

    nc_px_group_sel_t selA[] = { { .hw = NC_PX_HW_WS2812, .slice = "0:4" } };
    nc_px_group_sel_t selB[] = { { .hw = NC_PX_HW_WS2812, .slice = "2:5" } };
    nc_px_group_t groups[2] = {
        { .id = 1, .name = "A", .sel = selA, .nsel = 1 },
        { .id = 2, .name = "B", .sel = selB, .nsel = 1 },
    };
    nc_px_map_t map = { .id = 1, .name = "m", .group = groups, .ngroups = 2 };
    nc_px_mode_entry_t entries[2] = {
        { .group = "A", .effect = "A", .write = NC_PX_WRITE_RGB },
        { .group = "B", .effect = "B", .write = NC_PX_WRITE_RGB },
    };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 2, .entry = entries };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &lay) == ESP_OK, "建立");
    printf("  jobs=%u runs=%u\n", (unsigned)lay.njobs, (unsigned)lay.nruns);

    // A 全部 0x100、B 全部 0x200
    uint16_t va[8], vb[8];
    for (int i = 0; i < 8; i++) { va[i] = 0x100; vb[i] = 0x200; }
    uint16_t *vp[2] = { va, vb };
    uint8_t out[5 * 4] = { 0 };
    nc_px_layout_apply(&lay, vp, out);
    for (int px = 0; px < 5; px++) {
        printf("  px%d: R=%u G=%u B=%u\n", px, out[px * 4 + 0], out[px * 4 + 1], out[px * 4 + 2]);
    }
    nc_px_layout_free(&lay);
}


// ── [11] apply4 抖動：4 子幀平均 ≈ 12-bit>>4 ──
static void test_apply4(void)
{
    printf("[11] apply4 抖動（4 子幀）\n");
    nc_px_asset_t a;
    make_asset(&a, "w", 4);
    nc_px_asset_tbl_t assets = { .n = 1, .cap = 1,
                                      .item = (nc_px_asset_t[]){ a } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 4 } };
    nc_px_group_sel_t sel[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_t g = { .id = 1, .name = "full", .sel = sel, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "m", .group = &g, .ngroups = 1 };
    nc_px_mode_entry_t en = { .group = "full", .effect = "w", .write = NC_PX_WRITE_RGB };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &en };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &lay) == ESP_OK, "建立");
    uint16_t v[4] = { 4095, 2048, 17, 1234 };
    uint16_t *vp[1] = { v };
    uint8_t o[4][16];
    uint8_t *outs[4] = { o[0], o[1], o[2], o[3] };
    memset(o, 0, sizeof(o));
    nc_px_layout_apply4(&lay, vp, outs, 4, 0);
    bool ok = true;
    for (int px = 0; px < 4; px++) {
        const uint32_t idx = (3u * (uint32_t)px) % 4u;   // rgb stride = 3，nv = 4
        const uint32_t want = ((uint32_t)v[idx] + 8u) >> 4;
        uint32_t sum = 0;
        for (int sf = 0; sf < 4; sf++) {
            sum += o[sf][px * 4 + 0];
        }
        const uint32_t avg = (sum + 2u) >> 2;
        if (avg + 1 < want || avg > want + 1) {
            ok = false;
        }
    }
    check(ok, "4 子幀平均 = 12-bit>>4（±1）");
    nc_px_layout_free(&lay);
}

// ── [12] 6 B 落點表：同 run 路徑逐 byte 相同（混合 write／重疊／stride）──
// 這張表是衍生資料：runs 係真相，表只係把「每顆一筆」攤平。任何一個 byte 唔同
// 就係回歸——用戶見到嘅畫面要靠它。
static void test_flat6_matches_runs(void)
{
    printf("[12] 6 B 落點表（對拍 run 路徑）\n");
    nc_px_asset_t a, b;
    make_asset(&a, "A", 24);          // rgb 8 顆剛好 24 個值
    make_asset(&b, "B", 8);
    nc_px_asset_tbl_t assets = { .n = 2, .cap = 2,
                                      .item = (nc_px_asset_t[]){ a, b } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 8 } };
    // g0 全鏈寫 rgb；g1 隔顆只寫紅（同 g0 重疊 → 後寫贏）；g2 前半寫 rgbw
    nc_px_group_sel_t s0[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_sel_t s1[] = { { .hw = NC_PX_HW_WS2812, .slice = "::2" } };
    nc_px_group_sel_t s2[] = { { .hw = NC_PX_HW_WS2812, .slice = "0:8:2" } };
    nc_px_group_t groups[3] = {
        { .id = 1, .name = "g0", .sel = s0, .nsel = 1 },
        { .id = 2, .name = "g1", .sel = s1, .nsel = 1 },
        { .id = 3, .name = "g2", .sel = s2, .nsel = 1 },
    };
    nc_px_map_t map = { .id = 1, .name = "m", .group = groups, .ngroups = 3 };
    nc_px_mode_entry_t en[3] = {
        { .group = "g0", .effect = "A", .write = NC_PX_WRITE_RGB },
        { .group = "g1", .effect = "B", .write = NC_PX_WRITE_R },
        { .group = "g2", .effect = "A", .write = NC_PX_WRITE_RGBW },
    };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 3, .entry = en };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &lay) == ESP_OK, "建立");
    check(sizeof(nc_px_flat6_t) == 6, "nc_px_flat6_t 就係 6 B");
    check(lay.nflat > 0, "build 之後自動展開 6 B 表");
    uint32_t total = 0;
    for (uint32_t r = 0; r < lay.nruns; r++) {
        total += lay.run[r].count;
    }
    check(lay.nflat == total, "表長度 = Σ run.count（重疊的顆都保留）");
    check(nc_px_layout_flat_bytes(&lay) == (size_t)lay.nflat * 6u, "flat_bytes = nflat × 6");

    uint16_t va[24], vb[8];
    for (int i = 0; i < 24; i++) {
        va[i] = (uint16_t)(0x100 + i * 7);
    }
    for (int i = 0; i < 8; i++) {
        vb[i] = (uint16_t)(0x800 + i * 3);
    }
    uint16_t *vp[2] = { va, vb };

    uint8_t out_flat[8 * 4], out_runs[8 * 4];
    memset(out_flat, 0xAA, sizeof(out_flat));
    memset(out_runs, 0xAA, sizeof(out_runs));
    nc_px_layout_apply(&lay, vp, out_flat);       // 6 B 表
    nc_px_layout_t lr = lay;                     // 同一份 layout，強制行 run 路徑
    lr.flat = NULL;
    lr.nflat = 0;
    nc_px_layout_apply(&lr, vp, out_runs);
    check(memcmp(out_flat, out_runs, sizeof(out_flat)) == 0, "8-bit 寫入：逐 byte 一樣");

    bool same = true;
    for (uint32_t ph = 0; ph < 4; ph++) {
        for (uint8_t sf = 0; sf < 4; sf++) {
            uint8_t d1[8 * 4], d2[8 * 4];
            memset(d1, 0x5A, sizeof(d1));
            memset(d2, 0x5A, sizeof(d2));
            nc_px_layout_apply_dither(&lay, vp, d1, 4, sf, ph);
            nc_px_layout_apply_dither(&lr, vp, d2, 4, sf, ph);
            if (memcmp(d1, d2, sizeof(d1)) != 0) {
                same = false;
            }
        }
    }
    check(same, "抖動寫入：4 子幀 × 4 phase 全部一樣");

    // 邊界：px 超過 16-bit 就唔展開（唔可以靜靜截斷），apply 照行 runs
    nc_px_layout_t t;
    if (nc_px_layout_build(&mode, &map, &assets, inv, 1, &t) == ESP_OK) {
        t.px = 70000;
        check(nc_px_layout_flatify(&t) == ESP_ERR_INVALID_SIZE, "px > 65,535 → 拒絕展開");
        check(t.nflat == 0 && t.flat == NULL, "拒絕之後冇表（nflat = 0）");
        uint8_t o1[8 * 4], o2[8 * 4];
        memset(o1, 0x33, sizeof(o1));
        memset(o2, 0x33, sizeof(o2));
        nc_px_layout_apply(&t, vp, o1);
        nc_px_layout_apply(&lay, vp, o2);
        check(memcmp(o1, o2, sizeof(o1)) == 0, "冇表時 apply 照行 runs，結果一樣");
        nc_px_layout_free(&t);
    }
    nc_px_layout_free(&lay);
}

// ── [13] 40 個效果（舊上限 32）：資產表／aprog 都按實際數量配置 ──
// 每個效果寫自己 4 顆，40 × 4 = 160 顆 ＝ 160 筆表（冇重疊）。
static void test_many_effects(void)
{
    printf("[13] 40 個效果（舊上限 32）\n");
    const uint16_t n = 40;
    nc_px_asset_tbl_t *t = nc_px_asset_tbl_alloc(n);
    check(t != NULL, "資產表：按 40 個配置（表頭 ＋ 40 × 每項）");
    if (t == NULL) {
        return;
    }
    t->n = n;
    for (uint16_t i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof(name), "e%u", (unsigned)i);
        make_asset(&t->item[i], name, 4);
    }
    nc_px_group_t *grp = calloc(n, sizeof(*grp));
    nc_px_group_sel_t *sels = calloc(n, sizeof(*sels));
    nc_px_mode_entry_t *en = calloc(n, sizeof(*en));
    if (grp == NULL || sels == NULL || en == NULL) {
        check(false, "暫存配置");
        nc_px_asset_tbl_free(t);
        free(grp);
        free(sels);
        free(en);
        return;
    }
    for (uint16_t i = 0; i < n; i++) {
        grp[i].id = (uint16_t)(i + 1);
        char nb[24], eb[24];
        snprintf(nb, sizeof(nb), "g%u", (unsigned)i);
        snprintf(eb, sizeof(eb), "e%u", (unsigned)i);
        grp[i].name = nc_px_name_dict_global_intern_cstr(nb);
        grp[i].name_id = nc_px_name_dict_global_intern(nb, strlen(nb));
        grp[i].sel = &sels[i];
        grp[i].nsel = 1;
        sels[i].hw = NC_PX_HW_WS2812;
        snprintf(sels[i].slice, sizeof(sels[i].slice), "%u:%u",
                 (unsigned)(i * 4u), (unsigned)(i * 4u + 4u));
        en[i].group = nc_px_name_dict_global_intern_cstr(nb);
        en[i].group_id = nc_px_name_dict_global_intern(nb, strlen(nb));
        en[i].effect = nc_px_name_dict_global_intern_cstr(eb);
        en[i].effect_id = nc_px_name_dict_global_intern(eb, strlen(eb));
        en[i].write = NC_PX_WRITE_RGB;
    }
    nc_px_map_t map = { .id = 1, .name = "m", .group = grp, .ngroups = n };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = n, .entry = en };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = n * 4u } };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, t, inv, 1, &lay) == ESP_OK, "建立");
    check(lay.naprog == n, "aprog 數 = 40（唔再截 32）");
    check(lay.skipped == 0, "冇 skip");
    check(lay.nflat == (uint32_t)n * 4u, "6 B 表 = 40 × 4 = 160 筆");
    nc_px_layout_free(&lay);
    nc_px_asset_tbl_free(t);
    free(grp);
    free(sels);
    free(en);
}


// ── [13] apply_range 分段 == 一次過 apply ──
static void test_apply_range(void)
{
    printf("[13] apply_range 分段\n");
    nc_px_asset_t a, b;
    make_asset(&a, "A", 24);
    make_asset(&b, "B", 8);
    nc_px_asset_tbl_t assets = { .n = 2, .cap = 2,
                                 .item = (nc_px_asset_t[]){ a, b } };
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = 8 } };
    nc_px_group_sel_t s0[] = { { .hw = NC_PX_HW_WS2812, .slice = ":" } };
    nc_px_group_t g = { .id = 1, .name = "g0", .sel = s0, .nsel = 1 };
    nc_px_map_t map = { .id = 1, .name = "m", .group = &g, .ngroups = 1 };
    nc_px_mode_entry_t en = { .group = "g0", .effect = "A", .write = NC_PX_WRITE_RGB };
    nc_px_mode_t mode = { .id = 1, .name = "m", .nentry = 1, .entry = &en };
    nc_px_layout_t lay;
    check(nc_px_layout_build(&mode, &map, &assets, inv, 1, &lay) == ESP_OK, "建立");
    check(lay.nflat > 0, "有 flat 表");
    uint16_t va[24];
    for (int i = 0; i < 24; i++) {
        va[i] = (uint16_t)(0x300 + i * 5);
    }
    uint16_t *vp[1] = { va };
    uint8_t whole[8 * 4], parts[8 * 4];
    memset(whole, 0x11, sizeof(whole));
    memset(parts, 0x22, sizeof(parts));
    (void)nc_px_layout_apply(&lay, vp, whole);
    uint32_t off = 0;
    for (uint32_t k = 0; k < 5; k++) {
        const uint32_t n = (lay.nflat + 4u) / 5u;
        (void)nc_px_layout_apply_range(&lay, vp, parts, off, n);
        off += n;
    }
    check(memcmp(whole, parts, sizeof(whole)) == 0, "分段 == 一次過（逐 byte）");
    nc_px_layout_free(&lay);
}

int main(void)
{
    test_single_chain();
    test_reverse();
    test_step();
    test_overlap_same_channel();
    test_apply4();
    test_apply_range();
    test_flat6_matches_runs();
    test_many_effects();
    test_500_groups();
    test_json_500();
    test_cache_roundtrip();
    test_overlap();
    printf("\n%s（%d 個失敗）\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
