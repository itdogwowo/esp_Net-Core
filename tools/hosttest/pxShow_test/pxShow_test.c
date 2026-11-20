// pxShow_test — mode 委派 → plan → 逐幀合成（host，唔需要硬件）。
//
// 用 mp 的真檔（effects.json／matrix.json／demo_eyes.json）＋假 inventory：
//   ws2812  base 0  px 32
//   uartMotor1 base 32 px 1
// 驗 plan 的 group/range/write 解析，同 nc_px_show_frame 的通道分派。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_px_asset.h"
#include "nc_px_layer.h"
#include "nc_px_program.h"
#include "nc_px_show.h"

static int g_fail;

static void check(bool cond, const char *name)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) {
        g_fail++;
    }
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  FAIL 開唔到 %s\n", path);
        g_fail++;
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

int main(void)
{
    printf("pxShow_test — 委派 plan＋逐幀合成\n");

    size_t n = 0;
    char *eff = read_file("tools/hosttest/pxLayer_test/fixtures/effects.json", &n);
    if (eff == NULL) {
        return 1;
    }
    nc_px_asset_clear();
    check(nc_px_asset_add_json(eff, n, "effects") == ESP_OK, "effects.json 解析");
    free(eff);

    char *map_txt = read_file("tools/hosttest/pxLayer_test/fixtures/matrix.json", &n);
    char *mode_txt = read_file("tools/hosttest/pxLayer_test/fixtures/demo_eyes.json", &n);
    if (map_txt == NULL || mode_txt == NULL) {
        return 1;
    }
    nc_px_map_t map;
    nc_px_mode_t mode;
    check(nc_px_map_parse(map_txt, strlen(map_txt), &map) == ESP_OK, "map 解析");
    check(nc_px_mode_parse(mode_txt, strlen(mode_txt), &mode) == ESP_OK, "mode 解析");

    nc_px_asset_tbl_t tbl;
    memset(&tbl, 0, sizeof(tbl));
    for (int i = 0; i < nc_px_asset_count(); i++) {
        tbl.item[tbl.n++] = *nc_px_asset_at(i);
    }

    const nc_px_hw_inv_t inv[] = {
        { .hw = NC_PX_HW_WS2812, .base = 0, .px = 32 },
        { .hw = NC_PX_HW_UARTMOTOR1, .base = 32, .px = 1 },
    };
    static nc_px_plan_t plan;
    const esp_err_t ep = nc_px_show_plan(&mode, &map, &tbl, inv,
                                         (uint8_t)(sizeof(inv) / sizeof(inv[0])), &plan);
    check(ep == ESP_OK, "plan 成功");
    check(plan.px == 33, "合成鏈長度 33（ws2812 32 ＋ uartMotor1 1）");
    check(plan.njobs == 2, "2 個 job");
    check(plan.skipped == 0, "冇跳過");
    check(plan.job[0].write == NC_PX_WRITE_RGB && plan.job[0].nled == 16 &&
          plan.job[0].rng[0].base == 0 && plan.job[0].rng[0].count == 16,
          "job0 = eyes(rgb) 0..15");
    check(plan.job[1].write == NC_PX_WRITE_G && plan.job[1].nled == 16 &&
          plan.job[1].rng[0].base == 16 && plan.job[1].rng[0].count == 16,
          "job1 = wave(g) 16..31");
    check(strcmp(plan.job[0].asset->name, "eyes") == 0, "job0 asset = eyes");

    static uint8_t canvas[33 * 4];
    memset(canvas, 0, sizeof(canvas));
    nc_px_show_frame(&plan, 20, canvas);
    bool job1_g_only = true;
    bool job1_any = false;
    for (int i = 16; i < 32; i++) {
        if (canvas[i * 4 + 0] != 0 || canvas[i * 4 + 2] != 0 || canvas[i * 4 + 3] != 0) {
            job1_g_only = false;
        }
        if (canvas[i * 4 + 1] != 0) {
            job1_any = true;
        }
    }
    check(job1_g_only, "job1 只寫 G 通道");
    check(job1_any, "job1 有非零值（wave 喺 step 20）");

    nc_px_prog_t prog;
    check(nc_px_engine_program_compile(plan.job[0].asset, &prog) == ESP_OK, "job0 編譯");
    const nc_px_asset_t *a = plan.job[0].asset;
    const uint16_t nvals = a->pixel_hint ? a->pixel_hint : 1;
    const uint32_t speed = a->speed ? a->speed : 1;
    const uint32_t ph = ((uint32_t)(20 / speed) * a->step + a->offset) % prog.total;
    bool same = true;
    for (uint16_t k = 0; k < 16 && same; k++) {
        const uint16_t i0 = (uint16_t)((3u * k) % nvals);
        const uint16_t i1 = (uint16_t)((3u * k + 1u) % nvals);
        const uint16_t i2 = (uint16_t)((3u * k + 2u) % nvals);
        const uint8_t r = (uint8_t)(nc_px_engine_program_value(
            &prog, (ph + i0 * a->spacing) % prog.total) >> 4);
        const uint8_t g = (uint8_t)(nc_px_engine_program_value(
            &prog, (ph + i1 * a->spacing) % prog.total) >> 4);
        const uint8_t b = (uint8_t)(nc_px_engine_program_value(
            &prog, (ph + i2 * a->spacing) % prog.total) >> 4);
        if (canvas[k * 4] != r || canvas[k * 4 + 1] != g || canvas[k * 4 + 2] != b) {
            same = false;
        }
    }
    check(same, "job0 的 RGB = program vals[3k..]（同 scatter 語義一致）");

    char *motor = read_file("tools/hosttest/pxLayer_test/fixtures/motor_sine.json", &n);
    if (motor != NULL) {
        nc_px_mode_t m2;
        check(nc_px_mode_parse(motor, strlen(motor), &m2) == ESP_OK, "motor_sine 解析");
        // uart_motor_sine 用自訂段型別（唔係六工具）→ 唔會入 program 資產表；
        // 呢個 mode 要等 .nce 後端，plan 應該「搵唔到有效 job」而唔係亂畫。
        check(nc_px_show_plan(&m2, &map, &tbl, inv, 2, &plan) == ESP_ERR_NOT_FOUND,
              "motor_sine：自訂型別未支援 → NOT_FOUND（唔會亂畫）");
        free(motor);
    }
    free(map_txt);
    free(mode_txt);

    printf("\n%s（%d 個失敗）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
