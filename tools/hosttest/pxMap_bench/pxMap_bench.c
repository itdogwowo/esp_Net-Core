// pxMap_bench — 量「配對／排版」成本：plan 建立（排版管理）＋逐幀 scatter（配對），
// 同純效果計算分開報，方便決定 map 應唔應該獨立一層。
//
// 量法（host x86；同一份 C 實作）：
//   [1] nc_px_show_plan()  換場時一次：group 解析、slice 展開、program 編譯
//   [2] nc_px_show_frame() 逐幀：vals（program）＋ scatter 到 px×4 畫布
//   [3] 純效果計算        ：只 loop program_value（唔 scatter）→ 相減 = scatter 成本
//   [4] 記憶體            ：plan／表 的 sizeof
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nc_px_asset.h"
#include "nc_px_layer.h"
#include "nc_px_show.h"

#define BENCH_PX_MAX 10000

static double now_us(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static void bench_size(uint16_t px, int jobs, const nc_px_asset_tbl_t *tbl)
{
    static nc_px_map_t map;
    static nc_px_mode_t mode;
    static nc_px_plan_t plan;
    memset(&map, 0, sizeof(map));
    memset(&mode, 0, sizeof(mode));
    strcpy(map.name, "bench");
    map.id = 1;
    map.ngroups = 1;
    map.group[0].id = 1;
    strcpy(map.group[0].name, "full");
    map.group[0].nsel = 1;
    map.group[0].sel[0].hw = NC_PX_HW_WS2812;
    strcpy(map.group[0].sel[0].slice, ":");

    strcpy(mode.name, "bench");
    strcpy(mode.mapping, "bench");
    mode.nentry = (uint8_t)jobs;
    for (int i = 0; i < jobs; i++) {
        nc_px_mode_entry_t *e = &mode.entry[i];
        strcpy(e->group, "bench.full");
        strcpy(e->effect, (i % 2 == 0) ? "eyes" : "wave");
        e->write = (i % 2 == 0) ? NC_PX_WRITE_RGB : NC_PX_WRITE_G;
        e->has_range = true;
        e->start = (int16_t)(px * i / jobs);
        e->stop = (int16_t)(px * (i + 1) / jobs);
        e->step = 1;
    }
    const nc_px_hw_inv_t inv[] = { { .hw = NC_PX_HW_WS2812, .base = 0, .px = px } };

    // [1] plan（排版）
    const int reps = 200;
    double t0 = now_us();
    nc_px_job_t plan_jobs[NC_PX_LEGACY_JOB_MAX];
    for (int i = 0; i < reps; i++) {
        (void)nc_px_show_plan(&mode, &map, tbl, inv, 1, &plan,
                              plan_jobs, NC_PX_LEGACY_JOB_MAX);
    }
    const double plan_us = (now_us() - t0) / reps;
    if (plan.njobs == 0) {
        printf("%5u px %d jobs：plan 失敗\n", (unsigned)px, jobs);
        return;
    }

    // [2] 逐幀（效果＋scatter）
    static uint8_t canvas[BENCH_PX_MAX * 4];
    const int frames = (px > 1000) ? 200 : 2000;
    for (int i = 0; i < 50; i++) {
        nc_px_show_frame(&plan, (uint32_t)i, canvas);      // warm
    }
    t0 = now_us();
    for (int i = 0; i < frames; i++) {
        nc_px_show_frame(&plan, (uint32_t)(100 + i), canvas);
    }
    const double frame_us = (now_us() - t0) / frames;

    // [3] 純效果計算（同樣的 vals 數量：每 job pixel_hint）
    uint32_t sink = 0;
    t0 = now_us();
    for (int i = 0; i < frames; i++) {
        for (uint8_t j = 0; j < plan.njobs; j++) {
            const nc_px_prog_t *p = &plan.job[j].prog;
            const uint16_t nv = plan.job[j].asset->pixel_hint ? plan.job[j].asset->pixel_hint : 1;
            for (uint16_t k = 0; k < nv; k++) {
                sink += nc_px_engine_program_value(p, (uint32_t)((i + k) % p->total));
            }
        }
    }
    const double eval_us = (now_us() - t0) / frames;
    const double scatter_us = frame_us - eval_us;

    printf("%5u px %2d jobs  plan %8.1f us    frame %8.2f us  (eval %.2f + scatter %.2f)  "
           "%.1f ns/px  budget50 %.1f%%\n",
           (unsigned)px, jobs, plan_us, frame_us, eval_us,
           (scatter_us > 0 ? scatter_us : 0.0),
           frame_us * 1000.0 / px, frame_us / 20000.0 * 100.0);
    (void)sink;
}

int main(void)
{
    size_t n = 0;
    char *eff = read_file("tools/hosttest/pxLayer_test/fixtures/effects.json", &n);
    if (eff == NULL) {
        printf("搵唔到 effects.json fixture\n");
        return 1;
    }
    nc_px_asset_clear();
    if (nc_px_asset_add_json(eff, n, "effects") != ESP_OK) {
        printf("effects.json 解析失敗\n");
        return 1;
    }
    free(eff);

    nc_px_asset_tbl_t *tbl = nc_px_asset_tbl_alloc((uint16_t)nc_px_asset_count());
    if (tbl == NULL) {
        printf("資產表配置失敗\n");
        return 1;
    }
    for (int i = 0; i < nc_px_asset_count(); i++) {
        tbl->item[tbl->n++] = *nc_px_asset_at(i);
    }

    printf("=== 配對／排版成本（host x86，同一份 C 實作）===\n");
    printf("plan = 換場一次（group 解析＋slice＋program 編譯）；frame = 每幀\n\n");
    bench_size(64, 1, tbl);
    bench_size(64, 4, tbl);
    bench_size(960, 1, tbl);
    bench_size(960, 4, tbl);
    bench_size(9600, 1, tbl);
    bench_size(9600, 4, tbl);

    printf("\n記憶體：plan %u B（8 jobs × prog）、map %u B、mode %u B、asset %u B、"
           "表頭 %u B＋每項 %u B（舊版固定 32 項 ＝ 8,386 B）\n",
           (unsigned)sizeof(nc_px_plan_t), (unsigned)sizeof(nc_px_map_t),
           (unsigned)sizeof(nc_px_mode_t), (unsigned)sizeof(nc_px_asset_t),
           (unsigned)sizeof(nc_px_asset_tbl_t), (unsigned)sizeof(nc_px_asset_t));
    nc_px_asset_tbl_free(tbl);
    return 0;
}
