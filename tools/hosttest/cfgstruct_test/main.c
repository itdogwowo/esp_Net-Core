// config 型別化（struct = schema）的 host 測試。
//
// 不需要板子、不需要 IDF：真的 cJSON ＋ 真的 nc_cfg_load.c，
// 只把 esp_log 與 esp_err 換成 stdout。
//
// 驗的是「格式的規則」而不是檔案系統：
//   缺欄位 → 預設、型別錯／超範圍 → 預設、未知鍵 → 計數、version 不符 → 整層預設、
//   陣列上限、chunk > slot_size 的夾法、tasks 的 has_level。
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "nc_cfg.h"

static int fails;
static int checks;

static void ok(const char *name, bool cond)
{
    checks++;
    if (!cond) {
        printf("  FAIL %s\n", name);
        fails++;
    } else {
        printf("  PASS %s\n", name);
    }
}

// 解析一段 JSON 並填 system；回傳 stat（root 為 NULL 就是「缺檔」）
static nc_cfg_stat_t parse_sys(const char *json, nc_system_cfg_t *out)
{
    nc_cfg_stat_t st;
    cJSON *root = (json != NULL) ? cJSON_Parse(json) : NULL;
    (void)nc_cfg_apply_system(root, out, &st);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return st;
}

static nc_cfg_stat_t parse_tasks(const char *json, nc_tasks_cfg_t *out)
{
    nc_cfg_stat_t st;
    cJSON *root = (json != NULL) ? cJSON_Parse(json) : NULL;
    (void)nc_cfg_apply_tasks(root, out, &st);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return st;
}

// 解析一段 JSON 並填 hardware；回傳 stat
static nc_cfg_stat_t parse_hw(const char *json, nc_hardware_cfg_t *out)
{
    nc_cfg_stat_t st;
    cJSON *root = (json != NULL) ? cJSON_Parse(json) : NULL;
    (void)nc_cfg_apply_hardware(root, out, &st);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return st;
}

// 真的出廠 hardware.json：零 invalid／零 unknown，ETH 的 enable 要符合預期。
static void check_preset_hw(const char *path, int expect_eth)
{
    printf("  —— 預置 %s\n", path);
    FILE *f = fopen(path, "rb");
    ok("開得到", f != NULL);
    if (f == NULL) {
        return;
    }
    static char buf[64 * 1024];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    ok("合法 JSON", root != NULL);
    if (root == NULL) {
        return;
    }
    nc_hardware_cfg_t hw;
    nc_cfg_stat_t ps;
    (void)nc_cfg_apply_hardware(root, &hw, &ps);
    ok("零 invalid", ps.invalid == 0);
    ok("零 unknown", ps.unknown == 0);
    ok("ETH 段有寫（present）", hw.eth.present);
    ok("ETH enable 符合預期", hw.eth.enable == (expect_eth != 0));
    if (hw.eth.enable) {
        ok("啟用時腳位齊", hw.eth.mdc >= 0 && hw.eth.mdio >= 0 && hw.eth.ref_clk >= 0);
    }
    ok("phy_type 非空", hw.eth.phy_type[0] != '\0');
    cJSON_Delete(root);
}

static const nc_link_cfg_t *link(const nc_system_cfg_t *s, const char *name)
{
    for (uint8_t i = 0; i < s->n_links; i++) {
        if (strcmp(s->links[i].name, name) == 0) {
            return &s->links[i];
        }
    }
    return NULL;
}

int main(void)
{
    nc_system_cfg_t sys;
    nc_cfg_stat_t st;

    printf("[1] 缺檔（root=NULL）→ 全部用最基礎的形狀\n");
    st = parse_sys(NULL, &sys);
    ok("missing=1", st.missing == 1);
    ok("2 條預設 link（CDC/UART）", sys.n_links == 2);
    ok("CDC 是 cdc kind", sys.links[0].kind == NC_LINK_CDC);
    ok("4 列預設 route", sys.n_routes == 4);
    ok("cID 空", sys.cid[0] == '\0');
    ok("frame_interval_ms=20", sys.frame_interval_ms == 20);

    printf("[2] 正常 config\n");
    st = parse_sys(
        "{"
        "\"version\":1,\"cID\":\"A57C\",\"frame_interval_ms\":33,\"num_pixels\":676,"
        "\"nc4\":{"
        "  \"links\":["
        "    {\"name\":\"CDC\",\"kind\":\"cdc\",\"enable\":1},"
        "    {\"name\":\"UART\",\"kind\":\"uart\",\"enable\":0,"
        "     \"cfg\":{\"rx\":{\"region\":2048,\"chunk\":256,\"budget\":4,"
        "                     \"slots\":8,\"slot_size\":512}}}"
        "  ],"
        "  \"routes\":["
        "    {\"in\":\"nctx\",\"out\":[\"decode0\"]},"
        "    {\"in\":\"UART\",\"out\":[\"decode1\",\"TCP\"]},"
        "    {\"in\":\"outbound\",\"out\":[]}"
        "  ]"
        "}}",
        &sys);
    ok("2 條 link", sys.n_links == 2);
    ok("cID=A57C", strcmp(sys.cid, "A57C") == 0);
    ok("frame=33", sys.frame_interval_ms == 33);
    ok("num_pixels=676", sys.num_pixels == 676);
    const nc_link_cfg_t *l = link(&sys, "UART");
    ok("UART 存在", l != NULL);
    ok("UART enable=0", l != NULL && !l->enable);
    ok("UART rx.region=2048", l != NULL && l->rx.region == 2048);
    ok("UART rx.chunk=256", l != NULL && l->rx.chunk == 256);
    ok("UART rx.budget=4", l != NULL && l->rx.budget == 4);
    ok("UART rx.slots=8", l != NULL && l->rx.slots == 8);
    ok("UART rx.slot_size=512", l != NULL && l->rx.slot_size == 512);
    ok("UART rx_mask 五項齊", l != NULL && l->rx_mask == 0x1Fu);
    ok("3 列 route", sys.n_routes == 3);
    ok("UART 有 2 個 out", sys.routes[1].n_out == 2);
    ok("out[1]=TCP", strcmp(sys.routes[1].out[1], "TCP") == 0);
    ok("無 invalid", st.invalid == 0);

    printf("[3] chunk 大過 slot_size → 夾到一格\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"links\":["
        "{\"name\":\"UART\",\"kind\":\"uart\",\"cfg\":{\"rx\":{\"chunk\":900,\"slot_size\":256}}}]}}",
        &sys);
    l = link(&sys, "UART");
    ok("chunk 夾到 256", l != NULL && l->rx.chunk == 256);

    printf("[4] 超範圍 → 用預設，且 rx_mask 唔可以話「生效」\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"links\":["
        "{\"name\":\"UART\",\"kind\":\"uart\",\"cfg\":{\"rx\":{\"region\":999999}}}]}}",
        &sys);
    l = link(&sys, "UART");
    ok("region 回到 UART 預設 1024", l != NULL && l->rx.region == 1024);
    ok("REGION 位冇 set", l != NULL && (l->rx_mask & NC_CFG_RX_REGION) == 0);
    ok("invalid>0", st.invalid > 0);

    printf("[5] 型別錯（enable 不是 bool/0-1）→ 預設 true\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"links\":[{\"name\":\"CDC\",\"kind\":\"cdc\",\"enable\":\"yes\"}]}}",
        &sys);
    l = link(&sys, "CDC");
    ok("enable 回到 true", l != NULL && l->enable);
    ok("invalid>0", st.invalid > 0);

    printf("[6] 未知鍵 → 只計數，不影響解析\n");
    st = parse_sys(
        "{\"version\":1,\"unknown_top\":1,"
        "\"nc4\":{\"links\":[{\"name\":\"CDC\",\"kind\":\"cdc\",\"weird\":2}],"
        "\"routes\":[{\"in\":\"CDC\",\"out\":[\"decode0\"]}]}}",
        &sys);
    ok("unknown>0", st.unknown > 0);
    ok("仍然解析到 link", sys.n_links == 1);

    printf("[7] 線名太長 → 跳過該條（不截斷）\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"links\":["
        "{\"name\":\"A-VERY-LONG-LINK-NAME\",\"kind\":\"cdc\"}]}}",
        &sys);
    ok("n_links=0", sys.n_links == 0);
    ok("invalid>0", st.invalid > 0);

    printf("[8] version 不符 → 整層用預設（但仍然成功）\n");
    st = parse_sys(
        "{\"version\":2,\"nc4\":{\"links\":[{\"name\":\"CDC\",\"kind\":\"cdc\"}]}}",
        &sys);
    ok("bad_version=1", st.bad_version == 1);
    ok("回到 2 條預設 link", sys.n_links == 2);

    printf("[9] links[] 空陣列 = 用戶明示全部關閉\n");
    st = parse_sys("{\"version\":1,\"nc4\":{\"links\":[]}}", &sys);
    ok("n_links=0", sys.n_links == 0);
    ok("不是 missing", st.missing == 0);

    printf("[10] 超過上限 → 只收上限數，超出的記 invalid\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"links\":["
        "{\"name\":\"L0\",\"kind\":\"tcp\"},{\"name\":\"L1\",\"kind\":\"tcp\"},"
        "{\"name\":\"L2\",\"kind\":\"tcp\"},{\"name\":\"L3\",\"kind\":\"tcp\"},"
        "{\"name\":\"L4\",\"kind\":\"tcp\"},{\"name\":\"L5\",\"kind\":\"tcp\"},"
        "{\"name\":\"L6\",\"kind\":\"tcp\"},{\"name\":\"L7\",\"kind\":\"tcp\"},"
        "{\"name\":\"L8\",\"kind\":\"tcp\"}]}}",
        &sys);
    ok("只收 8 條", sys.n_links == 8);
    ok("invalid>0", st.invalid > 0);

    printf("[11] routes 的 out 超過 4 個 → 只收 4 個\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"routes\":["
        "{\"in\":\"CDC\",\"out\":[\"decode0\",\"decode1\",\"TCP\",\"WS\",\"UDP\"]}]}}",
        &sys);
    ok("n_out=4", sys.routes[0].n_out == 4);
    ok("invalid>0", st.invalid > 0);

    printf("[12] kind 冇寫 → 由名字推\n");
    st = parse_sys(
        "{\"version\":1,\"nc4\":{\"links\":[{\"name\":\"UART\"}]}}", &sys);
    l = link(&sys, "UART");
    ok("推定 uart", l != NULL && l->kind == NC_LINK_UART);
    ok("rx 用 uart 預設 1024", l != NULL && l->rx.region == 1024);

    printf("[13] tasks：has_level 要分得出「冇寫」與「寫 0」\n");
    nc_tasks_cfg_t tasks;
    nc_cfg_stat_t ts = parse_tasks(
        "{\"version\":1,\"tasks\":["
        "{\"name\":\"hello\",\"level\":6,\"affinity\":[1,0]},"
        "{\"name\":\"bench_a\",\"affinity\":[0,1]},"
        "{\"name\":\"bad_aff\",\"level\":2,\"affinity\":[2,0]}"
        "]}",
        &tasks);
    ok("3 條 task", tasks.n_tasks == 3);
    ok("hello level=6", tasks.tasks[0].has_level && tasks.tasks[0].level == 6);
    ok("hello aff=(1,0)", tasks.tasks[0].aff[0] && !tasks.tasks[0].aff[1]);
    ok("bench_a 冇 level", !tasks.tasks[1].has_level);
    ok("bench_a aff=(0,1)", !tasks.tasks[1].aff[0] && tasks.tasks[1].aff[1]);
    ok("bad_aff 2 → 當 0 且 invalid", !tasks.tasks[2].aff[0] && ts.invalid > 0);

    printf("[14] tasks 空 → 全部用固件宣告\n");
    ts = parse_tasks("{\"version\":1,\"tasks\":[]}", &tasks);
    ok("n_tasks=0", tasks.n_tasks == 0);

    // ── hardware：ETH（2026-11-08 起真的會被讀）──
    nc_hardware_cfg_t hw;
    nc_cfg_stat_t hs;

    printf("[15] hardware：缺檔 → eth.present=0（呼叫端改用內建腳位）\n");
    hs = parse_hw(NULL, &hw);
    ok("missing=1", hs.missing == 1);
    ok("present=0", !hw.eth.present);
    ok("enable=0", !hw.eth.enable);
    ok("mdc=-1", hw.eth.mdc == -1);
    ok("phy_type 預設 IP101", strcmp(hw.eth.phy_type, "IP101") == 0);

    printf("[16] hardware：正常 ETH 段（mdio=52 在 P4 的 GPIO 範圍內）\n");
    hs = parse_hw("{\"version\":1,\"ETH\":{\"enable\":1,\"mdc\":31,\"mdio\":52,"
                  "\"ref_clk\":50,\"phy_addr\":1,\"phy_type\":\"IP101\","
                  "\"spi\":-1,\"cs\":-1,\"rst\":-1,\"int\":-1}}", &hw);
    ok("present=1", hw.eth.present);
    ok("enable=1", hw.eth.enable);
    ok("mdc=31", hw.eth.mdc == 31);
    ok("mdio=52（唔可以當超範圍）", hw.eth.mdio == 52);
    ok("ref_clk=50", hw.eth.ref_clk == 50);
    ok("phy_addr=1", hw.eth.phy_addr == 1);
    ok("phy_type=IP101", strcmp(hw.eth.phy_type, "IP101") == 0);
    ok("spi/cs/rst/int 認得（unknown=0）", hs.unknown == 0);
    ok("零 invalid", hs.invalid == 0);

    printf("[17] hardware：enable=0 是「明確關閉」，與「缺段」唔同\n");
    hs = parse_hw("{\"version\":1,\"ETH\":{\"enable\":0}}", &hw);
    ok("present=1（有寫就算）", hw.eth.present);
    ok("enable=0", !hw.eth.enable);

    printf("[18] hardware：enable 型別錯／GPIO 超範圍／phy_type 型別錯 → 用預設 + invalid\n");
    hs = parse_hw("{\"version\":1,\"ETH\":{\"enable\":\"yes\",\"mdc\":99,\"phy_type\":123}}",
                  &hw);
    ok("present=1", hw.eth.present);
    ok("enable 回 false", !hw.eth.enable);
    ok("mdc 回 -1", hw.eth.mdc == -1);
    ok("phy_type 回 IP101", strcmp(hw.eth.phy_type, "IP101") == 0);
    ok("invalid>=3（三項都係型別／範圍錯）", hs.invalid >= 3);

    printf("[19] hardware：未知鍵只計數，仍然解析\n");
    hs = parse_hw("{\"version\":1,\"ETH\":{\"enable\":1,\"mdc_gpio\":31}}", &hw);
    ok("unknown>0", hs.unknown > 0);
    ok("仍然 present + enable", hw.eth.present && hw.eth.enable);

#ifndef PRESET_SYSTEM
#define PRESET_SYSTEM ""
#endif
    if (PRESET_SYSTEM[0] != '\0') {
        printf("[20] 真的出廠 system.json（%s）\n", PRESET_SYSTEM);
        FILE *f = fopen(PRESET_SYSTEM, "rb");
        ok("開得到預置", f != NULL);
        if (f != NULL) {
            static char buf[64 * 1024];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = '\0';
            cJSON *root = cJSON_Parse(buf);
            ok("預置是合法 JSON", root != NULL);
            if (root != NULL) {
                nc_cfg_stat_t ps;
                (void)nc_cfg_apply_system(root, &sys, &ps);
                ok("預置解析零 invalid", ps.invalid == 0);
                ok("預置有 2 條 link", sys.n_links == 2);
                ok("預置有 4 列 route", sys.n_routes == 4);
                ok("預置 CDC 啟用", sys.links[0].enable);
                cJSON_Delete(root);
            }
        }
    }

    printf("[21] 真的出廠 hardware.json\n");
#ifdef PRESET_HARDWARE_P4
    check_preset_hw(PRESET_HARDWARE_P4, 1);   // P4 這塊板有 ETH
#endif
#ifdef PRESET_HARDWARE_S3
    check_preset_hw(PRESET_HARDWARE_S3, 0);   // S3 沒有 ETH
#endif

    printf("\n%d/%d 項通過\n", checks - fails, checks);
    return fails == 0 ? 0 : 1;
}
