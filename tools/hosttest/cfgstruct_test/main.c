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

#ifndef PRESET_SYSTEM
#define PRESET_SYSTEM ""
#endif
    if (PRESET_SYSTEM[0] != '\0') {
        printf("[15] 真的出廠預置（%s）\n", PRESET_SYSTEM);
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

    printf("\n%d/%d 項通過\n", checks - fails, checks);
    return fails == 0 ? 0 : 1;
}
