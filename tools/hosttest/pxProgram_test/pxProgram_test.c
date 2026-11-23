// pxProgram_test — program interpreter 的 host 測試（唔需要 ESP-IDF／板子）。
//
// 驗的是「段落表 → 每幀值」這一段。**期望值全部由 MicroPython 版跑出來**
// （mp_Net-Core/slave：lib/sw/PixelMathMethod.py 的 compile／value_at、
//  lib/sw/effect_core.py 的 Effect.frame），C 這邊唔可以自己解釋一次。
//
// ⚠️ 2026-11-23：正弦收斂成單一 `nc_px_engine_sin_lin()`（status §7.194），
// 所以 MATH_NOW／SQUARE_WAVE_NOW／PULSE_WAVE 的值唔再同 MicroPython（多項式）位元級一致。
// 下面所有「有 sine 的期望值」同 `fnv16()` 指紋要重影一次（有 host 編譯器的機器跑
// `run.bat`，佢會逐項印出 got vs want）。純段邏輯的期望值（start／end／gap／width）不受影響。
//
// 若 golden 失敗：先問「這個改動應該改變波形嗎？」——
// 應該變 → 重跑 Python 更新期望值；唔應該變 → 修 C。
#include <stdio.h>
#include <string.h>

#include "nc_px_asset.h"
#include "nc_px_engine.h"
#include "nc_px_program.h"

static int g_fail;

static void check(bool cond, const char *name)
{
    if (cond) {
        printf("  PASS %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
        g_fail++;
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *name)
{
    if (got == want) {
        printf("  PASS %s\n", name);
    } else {
        printf("  FAIL %s（want %lu, got %lu）\n", name,
               (unsigned long)want, (unsigned long)got);
        g_fail++;
    }
}

// FNV-1a（低 byte 先）：同 Python oracle 的 fnv() 逐位一致，用一串值當指紋。
static uint32_t fnv16(const uint16_t *v, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint32_t)(v[i] & 0xFFu);
        h *= 16777619u;
        h ^= (uint32_t)((v[i] >> 8) & 0xFFu);
        h *= 16777619u;
    }
    return h;
}

static const nc_px_asset_t *add_asset(const char *json, const char *hint)
{
    if (nc_px_asset_add_json(json, strlen(json), hint) != ESP_OK) {
        return NULL;
    }
    return nc_px_asset_find(hint);
}

static void expect_seg(const nc_px_prog_t *p, int i, uint32_t start, uint32_t end,
                       uint8_t kind, uint16_t l_range, uint16_t l_lim, uint16_t phi4,
                       uint32_t step_phase, uint32_t gap, uint32_t width, const char *what)
{
    char buf[96];
    if (p == NULL || i >= p->n) {
        printf("  FAIL %s：冇第 %d 段\n", what, i);
        g_fail++;
        return;
    }
    const nc_px_prog_seg_t *s = &p->seg[i];
    snprintf(buf, sizeof(buf), "%s[%d] 段區間 %lu..%lu", what, i,
             (unsigned long)start, (unsigned long)end);
    bool ok = (s->start == start) && (s->end == end);
    check(ok, buf);

    snprintf(buf, sizeof(buf), "%s[%d] l_range／l_lim", what, i);
    check(s->l_range == l_range && s->l_lim == l_lim, buf);

    snprintf(buf, sizeof(buf), "%s[%d] kind／phi4", what, i);
    check(s->kind == kind && s->phi4 == phi4, buf);

    snprintf(buf, sizeof(buf), "%s[%d] step_phase／gap／width", what, i);
    check(s->step_phase == step_phase && s->gap == gap && s->width == width, buf);
}

// ── 1. 編譯：eyes（keep + 三段 math_now）──────────────────────────────
static const char *JSON_EYES =
    "{\"name\":\"eyes\",\"program\":["
    "{\"type\":\"keep\",\"F\":1,\"l_max\":0,\"l_lim\":0,\"phi\":0,\"end_Time\":60},"
    "{\"type\":\"math_now\",\"F\":5,\"l_max\":100,\"l_lim\":20,\"phi\":3071,\"end_Time\":100},"
    "{\"type\":\"math_now\",\"F\":5,\"l_max\":1023,\"l_lim\":100,\"phi\":3071,\"end_Time\":200},"
    "{\"type\":\"math_now\",\"F\":5,\"l_max\":1023,\"l_lim\":200,\"phi\":1023,\"end_Time\":320}]}";

// ── 2. 編譯：breathing（math_now + square_wave_now）──────────────────
static const char *JSON_BREATH =
    "{\"name\":\"breathing\",\"program\":["
    "{\"type\":\"math_now\",\"F\":2,\"l_max\":4095,\"l_lim\":100,\"phi\":0,\"end_Time\":200},"
    "{\"type\":\"square_wave_now\",\"F\":1,\"l_max\":3000,\"l_lim\":0,\"phi\":1024,\"end_Time\":300}]}";

// ── 3. 編譯：四種「非正弦」型別（starter／keep／pulse／pulse_wave）────
static const char *JSON_KINDS =
    "{\"name\":\"kinds\",\"program\":["
    "{\"type\":\"starter\",\"F\":1,\"l_max\":0,\"l_lim\":0,\"phi\":0,\"end_Time\":16},"
    "{\"type\":\"keep\",\"F\":1,\"l_max\":500,\"l_lim\":10,\"phi\":0,\"end_Time\":32},"
    "{\"type\":\"pulse\",\"F\":2,\"l_max\":4000,\"l_lim\":0,\"phi\":7,\"pulse\":900,\"end_Time\":64},"
    "{\"type\":\"pulse_wave\",\"F\":1,\"l_max\":2048,\"l_lim\":16,\"phi\":300,"
    " \"pulse\":1200,\"end_Time\":128}]}";

static void test_compile(void)
{
    printf("[1] compile：段落 → 預算值\n");
    nc_px_asset_clear();

    const nc_px_asset_t *a = add_asset(JSON_EYES, "eyes");
    check(a != NULL, "eyes：解析成功");
    nc_px_prog_t p;
    check(nc_px_engine_program_compile(a, &p) == ESP_OK, "eyes：compile 成功");
    check_u32(p.n, 4, "eyes：4 段");
    check_u32(p.total, 320, "eyes：波長 320（最後一段 end_Time）");
    expect_seg(&p, 0, 0, 60, NC_PX_SEG_KEEP, 0, 0, 0, 109, 60, 7, "eyes");
    expect_seg(&p, 1, 60, 100, NC_PX_SEG_MATH_NOW, 80, 20, 49136, 819, 8, 7, "eyes");
    expect_seg(&p, 2, 100, 200, NC_PX_SEG_MATH_NOW, 923, 100, 49136, 327, 20, 7, "eyes");
    expect_seg(&p, 3, 200, 320, NC_PX_SEG_MATH_NOW, 823, 200, 16368, 273, 24, 7, "eyes");

    a = add_asset(JSON_BREATH, "breathing");
    check(a != NULL, "breathing：解析成功");
    check(nc_px_engine_program_compile(a, &p) == ESP_OK, "breathing：compile 成功");
    expect_seg(&p, 0, 0, 200, NC_PX_SEG_MATH_NOW, 3995, 100, 0, 65, 100, 47, "breath");
    expect_seg(&p, 1, 200, 300, NC_PX_SEG_SQUARE_WAVE_NOW, 3000, 0, 16384, 65, 100, 47, "breath");

    a = add_asset(JSON_KINDS, "kinds");
    check(a != NULL, "kinds：解析成功");
    check(nc_px_engine_program_compile(a, &p) == ESP_OK, "kinds：compile 成功");
    expect_seg(&p, 0, 0, 16, NC_PX_SEG_STARTER, 0, 0, 0, 409, 16, 15, "kinds");
    expect_seg(&p, 1, 16, 32, NC_PX_SEG_KEEP, 490, 10, 0, 409, 16, 15, "kinds");
    expect_seg(&p, 2, 32, 64, NC_PX_SEG_PULSE, 4000, 0, 112, 409, 16, 4, "kinds");
    expect_seg(&p, 3, 64, 128, NC_PX_SEG_PULSE_WAVE, 2032, 16, 4800, 102, 64, 48, "kinds");
}

// ── 2. 取值：整張波表逐點對 oracle（用指紋＋關鍵取樣）────────────────
static void test_values(void)
{
    printf("[2] value：逐幀取值（oracle = MicroPython）\n");
    static uint16_t wave[400];

    const nc_px_asset_t *a = nc_px_asset_find("eyes");
    nc_px_prog_t p;
    if (a == NULL || nc_px_engine_program_compile(a, &p) != ESP_OK) {
        check(false, "eyes：重新 compile");
        return;
    }
    for (uint32_t g = 0; g < p.total; g++) {
        wave[g] = nc_px_engine_program_value(&p, g);
    }
    check_u32(fnv16(wave, p.total), 0xF23054DDu, "eyes：320 點指紋 0xF23054DD");
    check_u32(nc_px_engine_program_value(&p, 0), 0, "eyes：g=0（keep 0）");
    check_u32(nc_px_engine_program_value(&p, 59), 0, "eyes：g=59（keep 尾）");
    check_u32(nc_px_engine_program_value(&p, 60), 20, "eyes：g=60（第二段起點 = l_lim）");
    check_u32(nc_px_engine_program_value(&p, 99), 99, "eyes：g=99（第二段尾）");
    check_u32(nc_px_engine_program_value(&p, 100), 100, "eyes：g=100（第三段起點）");
    check_u32(nc_px_engine_program_value(&p, 159), 687, "eyes：g=159");
    check_u32(nc_px_engine_program_value(&p, 160), 701, "eyes：g=160");
    check_u32(nc_px_engine_program_value(&p, 199), 1022, "eyes：g=199");
    check_u32(nc_px_engine_program_value(&p, 200), 1022, "eyes：g=200（第四段起點）");
    check_u32(nc_px_engine_program_value(&p, 319), 200, "eyes：g=319（最後一點）");
    check_u32(nc_px_engine_program_value(&p, 320), 0, "eyes：g=320 繞回 0");
    check_u32(nc_px_engine_program_value(&p, 700), 20, "eyes：g=700 繞回（700%320=60）");

    a = nc_px_asset_find("breathing");
    if (a == NULL || nc_px_engine_program_compile(a, &p) != ESP_OK) {
        check(false, "breathing：重新 compile");
        return;
    }
    for (uint32_t g = 0; g < p.total; g++) {
        wave[g] = nc_px_engine_program_value(&p, g);
    }
    check_u32(fnv16(wave, p.total), 0xF0E06D64u, "breathing：300 點指紋 0xF0E06D64");
    check_u32(nc_px_engine_program_value(&p, 0), 2097, "breathing：g=0");
    check_u32(nc_px_engine_program_value(&p, 199), 3986, "breathing：g=199（正弦段尾）");
    check_u32(nc_px_engine_program_value(&p, 200), 3000, "breathing：g=200（方波亮）");
    check_u32(nc_px_engine_program_value(&p, 299), 3000, "breathing：g=299（方波）");

    a = nc_px_asset_find("kinds");
    if (a == NULL || nc_px_engine_program_compile(a, &p) != ESP_OK) {
        check(false, "kinds：重新 compile");
        return;
    }
    for (uint32_t g = 0; g < p.total; g++) {
        wave[g] = nc_px_engine_program_value(&p, g);
    }
    check_u32(fnv16(wave, p.total), 0xA218F0C5u, "kinds：128 點指紋 0xA218F0C5");
    check_u32(nc_px_engine_program_value(&p, 15), 0, "kinds：starter 全黑");
    check_u32(nc_px_engine_program_value(&p, 16), 500, "kinds：keep = l_range＋l_lim");
    check_u32(nc_px_engine_program_value(&p, 32), 4000, "kinds：pulse 亮");
    check_u32(nc_px_engine_program_value(&p, 47), 0, "kinds：pulse 暗");
    check_u32(nc_px_engine_program_value(&p, 64), 2048, "kinds：pulse_wave（l_range＋l_lim）");
}

// ── 3. 一幀：時間 × 空間（Effect.frame 的索引方式）──────────────────
static void test_frame(void)
{
    printf("[3] levels：一幀的空間分布（step／spacing／offset／speed／reverse）\n");
    const nc_px_asset_t *a = nc_px_asset_find("breathing");
    nc_px_prog_t p;
    if (a == NULL || nc_px_engine_program_compile(a, &p) != ESP_OK) {
        check(false, "breathing：compile");
        return;
    }

    // 期望值 = Effect("b", {...}).frame(7)，pixel_n=16、step=1、spacing=3、offset=5
    static const uint16_t want_s1[16] = {
        2244, 2280, 2318, 2355, 2392, 2429, 2465, 2502,
        2539, 2575, 2611, 2647, 2683, 2718, 2754, 2789,
    };
    static const uint16_t want_s2[16] = {
        2195, 2232, 2269, 2306, 2343, 2380, 2416, 2453,
        2489, 2526, 2562, 2599, 2635, 2671, 2707, 2743,
    };

    uint16_t lv[16];
    nc_px_prog_play_t pl = { .step = 1, .spacing = 3, .offset = 5, .speed = 1, .reverse = false };
    nc_px_engine_program_levels(&p, &pl, 7, lv, 16);
    check(memcmp(lv, want_s1, sizeof(lv)) == 0, "t=7、speed=1：16 點逐值一致");

    pl.speed = 2;
    nc_px_engine_program_levels(&p, &pl, 7, lv, 16);
    check(memcmp(lv, want_s2, sizeof(lv)) == 0, "t=7、speed=2：16 點逐值一致");

    pl.speed = 1;
    pl.reverse = true;
    nc_px_engine_program_levels(&p, &pl, 7, lv, 16);
    bool rev_ok = true;
    for (int i = 0; i < 16; i++) {
        if (lv[i] != want_s1[15 - i]) {
            rev_ok = false;
        }
    }
    check(rev_ok, "reverse：波形倒轉（同順向逐點相反）");

    pl.reverse = false;
    check_u32(nc_px_engine_program_phase(&p, &pl, 7), 12, "phase：t=7 → g0 = (7·1+5)%300");

    pl.speed = 0;
    check_u32(nc_px_engine_program_phase(&p, &pl, 7), 12, "phase：speed=0 當 1（唔會除零）");

    nc_px_prog_t empty = {0};
    memset(lv, 0xAA, sizeof(lv));
    nc_px_engine_program_levels(&empty, &pl, 7, lv, 16);
    bool all_zero = true;
    for (int i = 0; i < 16; i++) {
        if (lv[i] != 0) {
            all_zero = false;
        }
    }
    check(all_zero, "levels：空 program → 全 0（唔會亂寫）");
}

// ── 4. 畫布：12-bit → 8.8（gain／gamma／顏色）────────────────────────
static void test_canvas(void)
{
    printf("[4] canvas：等級 → 8.8 顯示值\n");
    nc_px_engine_tables_init();

    nc_px_asset_clear();
    const nc_px_asset_t *a = add_asset(
        "{\"name\":\"full\",\"program\":["
        "{\"type\":\"keep\",\"l_max\":4095,\"l_lim\":0,\"end_Time\":10}]}", "full");
    nc_px_prog_t p;
    if (a == NULL || nc_px_engine_program_compile(a, &p) != ESP_OK) {
        check(false, "full：compile");
        return;
    }
    nc_px_prog_play_t pl = { .step = 1, .spacing = 1, .offset = 0, .speed = 1, .reverse = false };

    uint16_t rgb[8 * 3];
    nc_px_engine_program_canvas(&p, &pl, 0, 8, 255, 255, 255, 255, rgb);
    const uint16_t white = nc_px_engine_gamma(65535);
    bool ok = true;
    for (int i = 0; i < 8 * 3; i++) {
        if (rgb[i] != white) {
            ok = false;
        }
    }
    check(ok, "全亮 4095、白、gain 255 → gamma(65535)");

    nc_px_engine_program_canvas(&p, &pl, 0, 8, 255, 0, 0, 255, rgb);
    check(rgb[0] == white && rgb[1] == 0 && rgb[2] == 0, "顏色：只亮 R 通道");

    nc_px_engine_program_canvas(&p, &pl, 0, 8, 255, 255, 255, 128, rgb);
    const uint16_t half = nc_px_engine_gamma((255u * 128u * 257u) / 255u);
    check(rgb[0] == half && half < white, "gain 128：亮度減半（gamma 曲線）");

    a = add_asset("{\"name\":\"dark\",\"program\":["
                  "{\"type\":\"keep\",\"l_max\":0,\"l_lim\":0,\"end_Time\":10}]}", "dark");
    if (a == NULL || nc_px_engine_program_compile(a, &p) != ESP_OK) {
        check(false, "dark：compile");
        return;
    }
    memset(rgb, 0xAA, sizeof(rgb));
    nc_px_engine_program_canvas(&p, &pl, 0, 8, 255, 255, 255, 255, rgb);
    ok = true;
    for (int i = 0; i < 8 * 3; i++) {
        if (rgb[i] != 0) {
            ok = false;
        }
    }
    check(ok, "等級 0 → 全黑（唔會留舊值）");
}

// ── 5. 拒絕：壞資產唔可以畫出半截波形 ────────────────────────────────
static void test_reject(void)
{
    printf("[5] 拒絕：compile 只收合法的 PROGRAM\n");
    nc_px_prog_t p;
    p.n = 7;                                  // 故意留舊值，驗 compile 會清
    p.total = 7;

    nc_px_asset_t a;
    memset(&a, 0, sizeof(a));
    a.kind = NC_PX_ASSET_PROGRAM;
    strcpy(a.name, "empty");
    check(nc_px_engine_program_compile(&a, &p) == ESP_ERR_INVALID_ARG, "冇段落 → INVALID_ARG");
    check(p.n == 0 && p.total == 0, "失敗時清空輸出");

    memset(&a, 0, sizeof(a));
    a.kind = NC_PX_ASSET_NCE;
    a.nseg = 1;
    a.seg[0].end_f = 10;
    check(nc_px_engine_program_compile(&a, &p) == ESP_ERR_INVALID_ARG, ".nce 唔入 program");

    memset(&a, 0, sizeof(a));
    a.kind = NC_PX_ASSET_PROGRAM;
    a.nseg = 2;
    a.seg[0].end_f = 100;
    a.seg[1].end_f = 100;                      // 冇遞增
    check(nc_px_engine_program_compile(&a, &p) == ESP_ERR_INVALID_ARG, "end_Time 唔遞增 → 拒絕");

    a.seg[0].end_f = 0;                        // 0 都唔收
    a.nseg = 1;
    check(nc_px_engine_program_compile(&a, &p) == ESP_ERR_INVALID_ARG, "end_Time = 0 → 拒絕");

    check(nc_px_engine_program_compile(NULL, &p) == ESP_ERR_INVALID_ARG, "NULL 資產 → 拒絕");
    check_u32(nc_px_engine_program_value(&p, 5), 0, "空 program 取值 = 0");
}

int main(void)
{
    printf("pxProgram_test — program interpreter（期望值 = MicroPython 版）\n");
    test_compile();
    test_values();
    test_frame();
    test_canvas();
    test_reject();
    if (g_fail != 0) {
        printf("✘ %d 個檢查失敗\n", g_fail);
    } else {
        printf("✓ 全部通過\n");
    }
    return g_fail ? 1 : 0;
}
