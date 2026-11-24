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
    uint32_t ng = 0;
    uint32_t ns = 0;
    check(nc_px_map_count(map_txt, strlen(map_txt), &ng, &ns) == ESP_OK, "map count");
    static nc_px_group_t grp[8];
    static nc_px_group_sel_t sel[8];
    nc_px_map_pool_t pool = {
        .group = grp, .cap_group = 8,
        .sel = sel, .cap_sel = 8,
    };
    check(nc_px_map_parse(map_txt, strlen(map_txt), &pool, &map) == ESP_OK, "map 解析");
    static nc_px_mode_entry_t mode_pool[16];
    check(nc_px_mode_parse(mode_txt, strlen(mode_txt), mode_pool, 16, &mode) == ESP_OK,
          "mode 解析");

    // plan 內部指住這張表的 item，所以成個 test 都唔 free（程序完結就完）
    nc_px_asset_tbl_t *tbl = nc_px_asset_tbl_alloc((uint16_t)nc_px_asset_count());
    check(tbl != NULL, "資產表配置");
    for (int i = 0; i < nc_px_asset_count(); i++) {
        tbl->item[tbl->n++] = *nc_px_asset_at(i);
    }

    const nc_px_hw_inv_t inv[] = {
        { .hw = NC_PX_HW_WS2812, .base = 0, .px = 32 },
        { .hw = NC_PX_HW_UARTMOTOR1, .base = 32, .px = 1 },
    };
    static nc_px_plan_t plan;
    static nc_px_job_t plan_jobs[NC_PX_MODE_ENTRY_MAX];
    const esp_err_t ep = nc_px_show_plan(&mode, &map, tbl, inv,
                                         (uint8_t)(sizeof(inv) / sizeof(inv[0])), &plan,
                                         plan_jobs, NC_PX_MODE_ENTRY_MAX);
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
        check(nc_px_mode_parse(motor, strlen(motor), mode_pool, 16, &m2) == ESP_OK,
              "motor_sine 解析");
        // uart_motor_sine 用自訂段型別（唔係六工具）→ 唔會入 program 資產表；
        // 呢個 mode 要等 .nce 後端，plan 應該「搵唔到有效 job」而唔係亂畫。
        check(nc_px_show_plan(&m2, &map, tbl, inv, 2, &plan,
                              plan_jobs, NC_PX_MODE_ENTRY_MAX) == ESP_ERR_NOT_FOUND,
              "motor_sine：自訂型別未支援 → NOT_FOUND（唔會亂畫）");
        free(motor);
    }
    free(map_txt);
    free(mode_txt);

    // 配對層語義（照 mp pixel_layout._SCATTER 逐條）
    printf("[5] 配對語義（generic buffer）\n");
    {
        nc_px_job_t job;
        memset(&job, 0, sizeof(job));
        job.nranges = 1;
        job.rng[0].base = 0;
        job.rng[0].count = 2;
        const uint16_t vals[4] = { 0x0ABC, 0x0123, 0, 0 };

        // 單通道：只改自己嗰個 byte，其餘不動
        static uint8_t cv[2 * 4];
        memset(cv, 0x7F, sizeof(cv));
        job.write = NC_PX_WRITE_G;
        nc_px_pair_apply(&job, vals, 2, cv);
        bool only_g = true;
        for (int i = 0; i < 2; i++) {
            if (cv[i * 4 + 0] != 0x7F || cv[i * 4 + 2] != 0x7F || cv[i * 4 + 3] != 0x7F) {
                only_g = false;
            }
        }
        check(only_g && cv[1] == (0xABC >> 4) && cv[5] == (0x123 >> 4),
              "g：只改 G，其餘通道保留");

        // ww：12-bit 拆成 B/W 兩個 byte
        memset(cv, 0, sizeof(cv));
        job.write = NC_PX_WRITE_WW;
        nc_px_pair_apply(&job, vals, 2, cv);
        check(cv[0] == 0 && cv[1] == 0 && cv[2] == (0xABC & 0xFF) && cv[3] == (0xABC >> 8),
              "ww：o[2]=v&0xFF、o[3]=v>>8");

        // wwww：同一個值寫晒四通道
        memset(cv, 0, sizeof(cv));
        job.write = NC_PX_WRITE_WWWW;
        nc_px_pair_apply(&job, vals, 2, cv);
        check(cv[0] == (0xABC >> 4) && cv[1] == (0xABC >> 4) &&
              cv[2] == (0xABC >> 4) && cv[3] == (0xABC >> 4), "wwww：四通道同值");

        // rgb：R/G/B 同 W 清零；重疊寫保留之前通道
        static uint8_t cv2[1 * 4];
        memset(cv2, 0, sizeof(cv2));
        nc_px_job_t job2;
        memset(&job2, 0, sizeof(job2));
        job2.nranges = 1;
        job2.rng[0].base = 0;
        job2.rng[0].count = 1;
        job2.write = NC_PX_WRITE_RGB;
        const uint16_t vals3[3] = { 0xF00, 0x0F0, 0x00F };
        nc_px_pair_apply(&job2, vals3, 3, cv2);
        check(cv2[0] == 0xF0 && cv2[1] == 0x0F && cv2[2] == 0x00 && cv2[3] == 0,
              "rgb：vals[3k..] → R,G,B、W=0");

        job2.write = NC_PX_WRITE_G;
        nc_px_pair_apply(&job2, vals3, 3, cv2);
        check(cv2[0] == 0xF0 && cv2[1] == 0xF0,
              "重疊寫：g 覆寫 G，但唔會清走之前 R");

        // 未來效果器：直接交 buffer（唔經 asset/program）
        static uint8_t cv3[1 * 4];
        memset(cv3, 0, sizeof(cv3));
        job2.write = NC_PX_WRITE_W;
        const uint16_t ext_buf[1] = { 0xFFF };
        nc_px_pair_apply(&job2, ext_buf, 1, cv3);
        check(cv3[3] == 0xFF, "外部效果器交 buffer（唔經 program）都配到");
    }

    // 12-bit 原生（效果層不變量）
    printf("[6] 12-bit 原生＋抖動轉換\n");
    {
        nc_px_job_t job;
        memset(&job, 0, sizeof(job));
        job.nranges = 1;
        job.rng[0].base = 0;
        job.rng[0].count = 2;
        const uint16_t vals[2] = { 0x0ABC, 0x0123 };
        static uint16_t cv12[2 * 4];

        memset(cv12, 0, sizeof(cv12));
        job.write = NC_PX_WRITE_G;
        nc_px_pair_apply12(&job, vals, 2, cv12);
        check(cv12[1] == 0xABC && cv12[5] == 0x123, "12-bit 原值保存（唔做 >>4）");

        memset(cv12, 0, sizeof(cv12));
        job.write = NC_PX_WRITE_WWWW;
        nc_px_pair_apply12(&job, vals, 2, cv12);
        check(cv12[0] == 0xABC && cv12[1] == 0xABC && cv12[2] == 0xABC && cv12[3] == 0xABC,
              "wwww：12-bit 四通道同值");

        // 12→8：唔抖動 = 四捨五入
        uint16_t src[16];
        for (int i = 0; i < 16; i++) {
            src[i] = 8;
        }
        uint8_t out[16];
        int32_t err[4] = { 0, 0, 0, 0 };
        nc_px_canvas12_to_8(src, out, 16, false, err);
        check(out[0] == 1 && out[15] == 1, "12→8 四捨五入（8 → 1）");

        // 12→8：1D 誤差擴散，平均亮度守得住（16 × 8/16 = 8）
        memset(out, 0, sizeof(out));
        int32_t err2[4] = { 0, 0, 0, 0 };
        nc_px_canvas12_to_8(src, out, 16, true, err2);
        int sum = 0;
        bool binary = true;
        for (int i = 0; i < 16; i++) {
            sum += out[i];
            if (out[i] > 1) {
                binary = false;
            }
        }
        check(binary && sum == 8, "12→8 抖動：0/1 交錯、總和 = 8（平均 0.5）");

        // 高值：0xFFF 抖動後仍然係 255
        for (int i = 0; i < 16; i++) {
            src[i] = 0xFFF;
        }
        int32_t err3[4] = { 0, 0, 0, 0 };
        nc_px_canvas12_to_8(src, out, 16, true, err3);
        check(out[0] == 255 && out[15] == 255, "12→8 抖動：0xFFF → 255");
    }

    printf("\n%s（%d 個失敗）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
