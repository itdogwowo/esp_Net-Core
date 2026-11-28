// pxAsset_test — DATA 平面資產解析的 host 測試（PC 上跑，唔需要 ESP-IDF／FAT／燈板）。
//
// 驗的是「JSON 文字 → 結構體」這一段：載入規則（撞名／上限／未知段／壞檔）同狀態 JSON。
// 檔案系統的掃描（nc_px_asset_scan.c）唔在這支的範圍——它只有目錄走訪，冇解析邏輯。
#include <stdio.h>
#include <string.h>

#include "nc_px_asset.h"

#ifdef _MSC_VER
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

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

static esp_err_t add(const char *json, const char *hint)
{
    return nc_px_asset_add_json(json, strlen(json), hint);
}

// ── 1. effects.json 批次形式（MP 的格式）────────────────────────────
static void test_batch(void)
{
    nc_px_asset_clear();
    const char *json =
        "{\"version\":1,\"effects\":["
        "{\"id\":1,\"name\":\"breathing\",\"pixel_n\":64,"
        " \"program\":[{\"type\":\"math_now\",\"F\":2,\"l_max\":4095,\"l_lim\":100,"
        "               \"phi\":0,\"end_Time\":200}],"
        " \"step\":1,\"spacing\":0,\"reverse\":false},"
        "{\"id\":3,\"name\":\"wave\","
        " \"program\":[{\"type\":\"math_now\",\"F\":10,\"l_max\":4095,\"l_lim\":0,"
        "               \"phi\":3071,\"end_Time\":100}]}"
        "]}";
    check(add(json, "effects") == ESP_OK, "batch：回 ESP_OK");
    check(nc_px_asset_count() == 2, "batch：入表 2 個");

    const nc_px_asset_t *a = nc_px_asset_find("breathing");
    check(a != NULL, "batch：找得到 breathing");
    if (a != NULL) {
        check(a->kind == NC_PX_ASSET_PROGRAM, "batch：kind = PROGRAM");
        check(a->nseg == 1, "batch：breathing 1 段");
        check(nc_px_asset_seg(a, 0)->type == NC_PX_SEG_MATH_NOW, "batch：段型別 math_now");
        check(nc_px_asset_seg(a, 0)->f == 2, "batch：F = 2");
        check(nc_px_asset_seg(a, 0)->l_max == 4095, "batch：l_max = 4095");
        check(nc_px_asset_seg(a, 0)->l_lim == 100, "batch：l_lim = 100");
        check(nc_px_asset_seg(a, 0)->end_f == 200, "batch：end_Time = 200");
        check(a->pixel_hint == 64, "batch：pixel_n = 64（只作提示）");
    }
    const nc_px_asset_t *w = nc_px_asset_find("wave");
    check(w != NULL && nc_px_asset_seg(w, 0)->phi == 3071, "batch：wave 的 phi = 3071");
    check(w != NULL && nc_px_asset_seg(w, 0)->l_lim == 0, "batch：wave 的 l_lim = 0");
}

// ── 2. 單一效果檔（一檔一效果）──────────────────────────────────────
static void test_single(void)
{
    nc_px_asset_clear();
    check(add("{\"name\":\"fire\",\"program\":[{\"type\":\"square_wave_now\",\"F\":1,"
              "\"l_max\":3000,\"end_Time\":16}]}", "fire") == ESP_OK,
          "single：回 ESP_OK");
    const nc_px_asset_t *f = nc_px_asset_find("fire");
    check(f != NULL && f->nseg == 1, "single：fire 1 段");
    check(f != NULL && nc_px_asset_seg(f, 0)->type == NC_PX_SEG_SQUARE_WAVE_NOW, "single：段型別 square_wave_now");

    // 冇寫 name → 用檔名（name_hint）
    nc_px_asset_clear();
    check(add("{\"program\":[{\"type\":\"keep\",\"end_Time\":5}]}", "fromfile") == ESP_OK,
          "single 無 name：回 ESP_OK");
    check(nc_px_asset_find("fromfile") != NULL, "single 無 name：用檔名補上");
}

// ── 3. 壞檔／未知段 → 整個資產拒絕（唔會半桶水畫錯）─────────────────
static void test_reject(void)
{
    nc_px_asset_clear();
    check(add("{\"name\":", "x") == ESP_ERR_INVALID_ARG, "壞 JSON：拒絕");
    check(nc_px_asset_count() == 0, "壞 JSON：表不變");

    check(add("{}", "x") == ESP_ERR_INVALID_ARG, "空 object：拒絕");

    check(add("{\"name\":\"nope\",\"program\":[{\"type\":\"fancy_new\",\"F\":1}]}", "nope")
              == ESP_ERR_INVALID_ARG,
          "未知段型別：拒絕");
    check(nc_px_asset_count() == 0, "未知段型別：唔會入表");

    check(add("{\"name\":\"huge\",\"pixel_n\":70000}", "huge") == ESP_ERR_INVALID_ARG,
          "pixel_n 超範圍：拒絕");

    check(add("{\"name\":\"oka\",\"program\":[{\"F\":1}]}", "oka") == ESP_ERR_INVALID_ARG,
          "段冇 type：拒絕");
}

// ── 4. 撞名：保留第一個 ─────────────────────────────────────────────
static void test_dup(void)
{
    nc_px_asset_clear();
    check(add("{\"name\":\"a\",\"program\":[{\"type\":\"keep\",\"end_Time\":10},"
              "{\"type\":\"keep\",\"end_Time\":20}]}", "a") == ESP_OK,
          "dup：第一個加入");
    check(add("{\"name\":\"a\",\"program\":[{\"type\":\"math_now\",\"end_Time\":10}]}", "a")
              == ESP_ERR_INVALID_STATE,
          "dup：第二個回 INVALID_STATE");
    check(nc_px_asset_count() == 1, "dup：表仍然 1 個");
    const nc_px_asset_t *a = nc_px_asset_find("a");
    check(a != NULL && a->nseg == 2, "dup：保留第一個（2 段）");

    nc_px_asset_stat_t st;
    nc_px_asset_stats(&st);
    check(st.dup == 1 && st.loaded == 1, "dup：stats 記錄 dup=1");
}

// ── 5. 未知鍵跳過（向前兼容）＋ BOM ─────────────────────────────────
static void test_extra_keys(void)
{
    nc_px_asset_clear();
    const char *json =
        "{\"name\":\"x\",\"step\":1,\"spacing\":10,\"speed\":3,\"reverse\":false,"
        " \"weird\":{\"a\":[1,2,{\"b\":null}]},\"f\":-1.5e3,\"program\":[]}";
    check(add(json, "x") == ESP_OK, "未知鍵：照收（跳過）");
    const nc_px_asset_t *x = nc_px_asset_find("x");
    check(x != NULL && x->nseg == 0, "未知鍵：program 空 → 0 段");

    nc_px_asset_clear();
    const char bom[] = "\xEF\xBB\xBF{\"name\":\"bom\",\"program\":[]}";
    check(nc_px_asset_add_json(bom, sizeof(bom) - 1, "bom") == ESP_OK, "UTF-8 BOM：照收");
    check(nc_px_asset_find("bom") != NULL, "UTF-8 BOM：入表");
}

// ── 6. 上限：段落數、資產數 ─────────────────────────────────────────
static void test_limits(void)
{
    nc_px_asset_clear();
    char buf[4096];
    size_t u = (size_t)snprintf(buf, sizeof(buf), "{\"name\":\"many\",\"program\":[");
    for (int i = 0; i < 20; i++) {
        u += (size_t)snprintf(buf + u, sizeof(buf) - u, "%s{\"type\":\"keep\",\"end_Time\":%d}",
                              (i == 0) ? "" : ",", i + 1);
    }
    snprintf(buf + u, sizeof(buf) - u, "]}");
    check(add(buf, "many") == ESP_OK, "20 段：照收");
    const nc_px_asset_t *m = nc_px_asset_find("many");
    check(m != NULL && m->nseg == 20, "20 段：全部保留（動態 pool）");

    // 冇固定上限（舊版 32）：要 200 個就收 200 個，而且係倍增配置
    nc_px_asset_clear();
    const int want = 200;
    int added = 0;
    for (int i = 0; i < want; i++) {
        char one[128];
        snprintf(one, sizeof(one), "{\"name\":\"e%d\",\"program\":[]}", i);
        if (add(one, one) == ESP_OK) {
            added++;
        }
    }
    check(added == want, "表冇上限：200 個全部收");
    check(nc_px_asset_count() == want, "表冇上限：count 一致");
    check(nc_px_asset_find("e199") != NULL, "第 200 個搵得到");
    check(nc_px_asset_find("e0") != NULL, "第一個仲喺（倍增搬過都保住）");

    nc_px_asset_stat_t st;
    nc_px_asset_stats(&st);
    check(st.rejected == 0, "冇人被拒");
    check(st.loaded == (uint16_t)want, "loaded = 200");
}

// ── 7. 狀態 JSON ───────────────────────────────────────────────────
static void test_status(void)
{
    nc_px_asset_clear();
    check(add("{\"name\":\"breathing\",\"program\":[{\"type\":\"math_now\",\"end_Time\":10}]}",
              "breathing") == ESP_OK,
          "status：加入第一個");
    check(add("{\"name\":\"wave\",\"program\":[]}", "wave") == ESP_OK, "status：加入第二個");

    char out[256];
    const size_t n = nc_px_asset_json(out, sizeof(out));
    check(n > 0, "status：有輸出");
    check(strstr(out, "\"n\":2") != NULL, "status：n = 2");
    check(strstr(out, "\"breathing\"") != NULL, "status：列出 breathing");
    check(strstr(out, "\"seg\":1") != NULL, "status：breathing 1 段");

    char tiny[8];
    check(nc_px_asset_json(tiny, sizeof(tiny)) == 0, "status：buffer 太細回 0");
}

// ── 9. program 契約：pulse 缺省、end_Time 必須遞增 ───────────────────
static void test_program_rules(void)
{
    nc_px_asset_clear();
    check(add("{\"name\":\"pulse_def\",\"program\":[{\"type\":\"math_now\",\"end_Time\":10}]}",
              "pulse_def") == ESP_OK,
          "pulse：冇寫照收");
    const nc_px_asset_t *a = nc_px_asset_find("pulse_def");
    check(a != NULL && nc_px_asset_seg(a, 0)->pulse == 2047, "pulse：缺省 2047（同 MP 一致）");

    nc_px_asset_clear();
    check(add("{\"name\":\"pulse_set\",\"program\":[{\"type\":\"pulse_wave\","
              "\"pulse\":1200,\"end_Time\":10}]}", "pulse_set") == ESP_OK,
          "pulse：明寫照收");
    a = nc_px_asset_find("pulse_set");
    check(a != NULL && nc_px_asset_seg(a, 0)->pulse == 1200, "pulse：明寫 = 1200");

    nc_px_asset_clear();
    check(add("{\"name\":\"flat\",\"program\":[{\"type\":\"keep\",\"end_Time\":10},"
              "{\"type\":\"keep\",\"end_Time\":10}]}", "flat") == ESP_ERR_INVALID_ARG,
          "end_Time 冇遞增：拒絕");
    check(nc_px_asset_count() == 0, "end_Time 冇遞增：唔會入表");
    check(add("{\"name\":\"zero\",\"program\":[{\"type\":\"keep\",\"end_Time\":0}]}", "zero")
              == ESP_ERR_INVALID_ARG,
          "end_Time = 0：拒絕");
}

// ── 8. 掃一個目錄（檔案系統外殼）────────────────────────────────────
#define TMPDIR_NAME "pxAsset_tmp"

static void write_file(const char *dir, const char *name, const char *text)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f == NULL) {
        printf("  FAIL 開唔到 %s\n", p);
        g_fail++;
        return;
    }
    (void)fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void drop_file(const char *dir, const char *name)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    (void)remove(p);
}

static void test_scan(void)
{
    nc_px_asset_clear();
    (void)RMDIR(TMPDIR_NAME);                 // 上次跑剩（可能唔存在）
    if (MKDIR(TMPDIR_NAME) != 0) {
        printf("  FAIL 建唔到臨時目錄\n");
        g_fail++;
        return;
    }

    write_file(TMPDIR_NAME, "effects.json",
               "{\"version\":1,\"effects\":["
               "{\"name\":\"batch_a\",\"program\":[{\"type\":\"keep\",\"end_Time\":10}]},"
               "{\"name\":\"batch_b\",\"program\":[]}]}");
    write_file(TMPDIR_NAME, "solo.json",
               "{\"name\":\"batch_a\",\"program\":[]}");   // 撞名：批次先到，應該贏
    write_file(TMPDIR_NAME, "broken.json", "{oops");
    write_file(TMPDIR_NAME, "notes.txt", "唔係效果");
    write_file(TMPDIR_NAME, "fire.nce", "");

    const esp_err_t e = nc_px_asset_scan(TMPDIR_NAME);
    check(e == ESP_OK, "scan：回 ESP_OK");
    check(nc_px_asset_count() == 3, "scan：3 個資產（batch_a／batch_b／fire）");
    check(nc_px_asset_find("solo") == NULL, "scan：有寫 name 就唔用檔名");

    const nc_px_asset_t *a = nc_px_asset_find("batch_a");
    check(a != NULL && a->nseg == 1, "scan：批次先到者勝（1 段，唔係 solo 的 0 段）");
    const nc_px_asset_t *fire = nc_px_asset_find("fire");
    check(fire != NULL && fire->kind == NC_PX_ASSET_NCE, "scan：.nce 登記成 NCE");

    nc_px_asset_stat_t st;
    nc_px_asset_stats(&st);
    check(st.dup == 1, "scan：撞名計入 dup");
    check(st.rejected == 1, "scan：壞檔計入 rejected");

    drop_file(TMPDIR_NAME, "effects.json");
    drop_file(TMPDIR_NAME, "solo.json");
    drop_file(TMPDIR_NAME, "broken.json");
    drop_file(TMPDIR_NAME, "notes.txt");
    drop_file(TMPDIR_NAME, "fire.nce");
    (void)RMDIR(TMPDIR_NAME);
}

int main(void)
{
    test_batch();
    test_single();
    test_reject();
    test_dup();
    test_extra_keys();
    test_limits();
    test_status();
    test_program_rules();
    test_scan();

    printf("%s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
