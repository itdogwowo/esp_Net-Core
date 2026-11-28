// pxLayer_test — DATA 平面第二～四層（map／modes／registry）解析的 host 測試。
//
// 用 mp_Net-Core 的真檔案做 golden（fixtures/ 係 slave/pixel 的原檔），
// 驗 struct 逐欄位；另外驗壞 JSON／未知 write／壞 range 的拒絕行為。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_px_layer.h"
#include "nc_px_layer_scan.h"
#include "nc_px_asset.h"

static int g_fail;

// mode 條目現在由檔案決定數量：測試用一塊固定 pool（parse 會檢查 cap）。
static nc_px_mode_entry_t g_mode_pool[64];
#define MODE_PARSE(txt, len, out) \
    nc_px_mode_parse((txt), (len), g_mode_pool, \
                     (uint32_t)(sizeof(g_mode_pool) / sizeof(g_mode_pool[0])), (out))

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

static void test_map(void)
{
    printf("[1] mapping（matrix.json）\n");
    size_t n = 0;
    char *txt = read_file("tools/hosttest/pxLayer_test/fixtures/matrix.json", &n);
    if (txt == NULL) {
        return;
    }
    // 兩步解析：先數、再配 pool（group／sel 由檔案決定，唔再係固定陣列）
    uint32_t ng = 0;
    uint32_t ns = 0;
    check(nc_px_map_count(txt, n, &ng, &ns) == ESP_OK, "count 成功");
    check(ng == 2 && ns == 2, "count：2 組／2 段");
    nc_px_group_t *grp = calloc(ng, sizeof(*grp));
    nc_px_group_sel_t *sel = calloc(ns ? ns : 1, sizeof(*sel));
    nc_px_map_pool_t pool = {
        .group = grp, .cap_group = ng,
        .sel = sel, .cap_sel = ns ? ns : 1,
    };
    nc_px_map_t m;
    const esp_err_t e = nc_px_map_parse(txt, n, &pool, &m);
    free(txt);
    check(e == ESP_OK, "解析成功");
    check(m.id == 3, "id = 3");
    check(strcmp(m.name, "matrix") == 0, "name = matrix");
    check(m.ngroups == 2, "2 個群組");
    check(m.group[0].id == 1 && strcmp(m.group[0].name, "full") == 0, "group[0] = 1/full");
    check(m.group[0].nsel == 1 && m.group[0].sel[0].hw == NC_PX_HW_WS2812,
          "full.sel[0] = ws2812");
    check(strcmp(m.group[0].sel[0].slice, ":") == 0, "full.sel[0].slice = \":\"");
    check(m.group[1].id == 2 && strcmp(m.group[1].name, "motors") == 0,
          "group[1] = 2/motors");
    check(m.group[1].sel[0].hw == NC_PX_HW_UARTMOTOR1, "motors.sel[0] = uartMotor1");
    free(grp);
    free(sel);
}

static void test_mode_demo_eyes(void)
{
    printf("[2] mode（demo_eyes.json）\n");
    size_t n = 0;
    char *txt = read_file("tools/hosttest/pxLayer_test/fixtures/demo_eyes.json", &n);
    if (txt == NULL) {
        return;
    }
    nc_px_mode_t m;
    const esp_err_t e = MODE_PARSE(txt, n, &m);
    free(txt);
    check(e == ESP_OK, "解析成功");
    check(m.id == 1 && strcmp(nc_px_name_dict_global_str(m.name_id), "demo_eyes") == 0,
          "id/name");
    check(strcmp(nc_px_name_dict_global_str(m.mapping_id), "matrix") == 0,
          "mapping = matrix");
    check(m.play_loop == -1, "play_loop = -1（無限）");
    check(m.maxF == 500, "maxF = 500");
    check(m.nentry == 2, "2 個委派項");
    check(strcmp(nc_px_name_dict_global_str(m.entry[0].group_id), "full") == 0 &&
          strcmp(nc_px_name_dict_global_str(m.entry[0].effect_id), "eyes") == 0 &&
          m.entry[0].write == NC_PX_WRITE_RGB, "entry[0] = matrix.full/eyes/rgb");
    check(m.entry[0].has_range && m.entry[0].start == 0 && m.entry[0].stop == 16,
          "entry[0] range = 0:16");
    check(strcmp(nc_px_name_dict_global_str(m.entry[1].group_id), "full") == 0 &&
          strcmp(nc_px_name_dict_global_str(m.entry[1].effect_id), "wave") == 0 &&
          m.entry[1].write == NC_PX_WRITE_G, "entry[1] = matrix.full/wave/g");
    check(m.entry[1].has_range && m.entry[1].start == 16 && m.entry[1].stop == 32,
          "entry[1] range = 16:32");
}

static void test_mode_motor(void)
{
    printf("[3] mode（motor_sine.json）\n");
    size_t n = 0;
    char *txt = read_file("tools/hosttest/pxLayer_test/fixtures/motor_sine.json", &n);
    if (txt == NULL) {
        return;
    }
    nc_px_mode_t m;
    const esp_err_t e = MODE_PARSE(txt, n, &m);
    free(txt);
    check(e == ESP_OK, "解析成功");
    check(m.nentry == 1, "1 個委派項");
    check(strcmp(nc_px_name_dict_global_str(m.entry[0].group_id), "motors") == 0 &&
          strcmp(nc_px_name_dict_global_str(m.entry[0].effect_id), "uart_motor_sine") == 0 &&
          m.entry[0].write == NC_PX_WRITE_W, "matrix.motors/uart_motor_sine/w");
    check(!m.entry[0].has_range, "冇 range（整組）");
}

static void test_show(void)
{
    printf("[4] registry（registry.json）\n");
    size_t n = 0;
    char *txt = read_file("tools/hosttest/pxLayer_test/fixtures/registry.json", &n);
    if (txt == NULL) {
        return;
    }
    nc_px_show_t s;
    const esp_err_t e = nc_px_show_parse(txt, n, &s);
    free(txt);
    check(e == ESP_OK, "解析成功");
    check(!s.auto_play, "auto_play = false");
    check(s.nlist == 0, "list 空");

    // 合成一份有 list 的
    const char *doc = "{\"version\":1,\"auto_play\":true,\"list\":[\"demo_eyes\",2]}";
    check(nc_px_show_parse(doc, strlen(doc), &s) == ESP_OK, "合成版解析成功");
    check(s.auto_play && s.nlist == 1 &&
          strcmp(s.list[0], "demo_eyes") == 0,
          "auto_play=true、list[0]=demo_eyes（數字項跳過）");
}

// [6] reload 語義：nc_px_layer_clear() 一定要連效果表一齊清。
// 唔清的話 nc_px_asset_scan() 會把同名效果當重複丟掉（「撞名：…（保留第一個）」），
// 結果改 effects.json 之後要 reboot 才生效，刪走的效果仍然留在 RAM。
static void test_reload_clears(void)
{
    printf("[6] reload 清表\n");
    const char *eff = "{\"effects\":[{\"name\":\"e1\",\"program\":["
                      "{\"type\":\"math_now\",\"F\":10,\"l_max\":100,\"end_Time\":32}]}]}";
    nc_px_asset_tbl_alloc(8);
    check(nc_px_asset_add_json(eff, strlen(eff), "effects.json") == ESP_OK, "加入 1 個效果");
    check(nc_px_asset_count() == 1, "效果表 = 1");
    nc_px_layer_clear();
    check(nc_px_asset_count() == 0, "layer_clear 亦清效果表（唔會殘留舊效果）");
}

static void test_negative(void)
{
    printf("[5] 拒絕／跳過\n");
    nc_px_mode_t m;
    const char *bad = "{\"name\":";
    check(MODE_PARSE(bad, strlen(bad), &m) == ESP_ERR_INVALID_ARG, "壞 JSON → INVALID_ARG");

    const char *w = "{\"name\":\"x\",\"map\":[{\"group\":\"g\",\"effect\":\"e\",\"write\":\"xyz\"}]}";
    check(MODE_PARSE(w, strlen(w), &m) == ESP_OK, "未知 write：整體仍 OK");
    check(m.nentry == 0, "未知 write → 該項跳過");

    const char *r = "{\"map\":[{\"group\":\"g\",\"effect\":\"e\",\"write\":\"rgb\",\"range\":\"a:b\"}]}";
    check(MODE_PARSE(r, strlen(r), &m) == ESP_OK, "壞 range：整體仍 OK");
    check(m.nentry == 1 && !m.entry[0].has_range, "壞 range → 該項當冇 range");

    const char *ok2 = "{\"map\":[{\"group\":\"g\",\"effect\":\"e\",\"write\":\"w\"},"
                      "{\"effect\":\"e2\",\"write\":\"w\"}]}";
    check(MODE_PARSE(ok2, strlen(ok2), &m) == ESP_OK, "缺欄位：整體仍 OK");
    check(m.nentry == 1, "缺 group 的項跳過");

    int16_t a, b, c;
    check(nc_px_slice_parse(":", &a, &b, &c) && a == 0 && b == 0 && c == 0, "slice \":\"");
    check(nc_px_slice_parse("0:16", &a, &b, &c) && a == 0 && b == 16, "slice \"0:16\"");
    check(nc_px_slice_parse("16:", &a, &b, &c) && a == 16 && b == 0, "slice \"16:\"");
    check(nc_px_slice_parse("::-1", &a, &b, &c) && c == -1, "slice \"::-1\"");
    check(!nc_px_slice_parse("a:b", &a, &b, &c), "slice 壞字串回 false");

    uint8_t hw = 0, w2 = 0;
    check(nc_px_hw_parse("ws2812", &hw) && hw == NC_PX_HW_WS2812, "hw parse ws2812");
    check(nc_px_hw_parse("uartMotor1", &hw) && hw == NC_PX_HW_UARTMOTOR1, "hw parse uartMotor1");
    check(!nc_px_hw_parse("bogus", &hw), "hw 未知回 false");
    check(nc_px_write_parse("rgbw", &w2) && w2 == NC_PX_WRITE_RGBW, "write parse rgbw");
    check(!nc_px_write_parse("rgbx", &w2), "write 未知回 false");
}

int main(void)
{
    printf("pxLayer_test — map／modes／registry（golden = mp 真檔）\n");
    test_map();
    test_mode_demo_eyes();
    test_mode_motor();
    test_show();
    test_negative();
    test_reload_clears();
    printf("\n%s（%d 個失敗）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
