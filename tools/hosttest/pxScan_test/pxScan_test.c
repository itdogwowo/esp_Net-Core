// pxScan_test — map／modes 目錄掃描、登記、排序；registry.json（host）。
//
// fixtures：tools/hosttest/pxScan_test/fs/{effects,map,modes} ＋ registry.json
// 全部係 mp_Net-Core slave/pixel 的原檔。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// gcc 13 把 (void)system(...) 當 -Werror=unused-result；用 helper 消耗回傳值。
static void sh_run(const char *cmd)
{
    if (system(cmd) != 0) {
        /* 清理／建目錄失敗唔影響測試結果 */
    }
}

#include "nc_px_asset.h"
#include "nc_px_layer_scan.h"

static int g_fail;

static void check(bool cond, const char *name)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) {
        g_fail++;
    }
}

static const char *FS = "tools/hosttest/pxScan_test/fs";

int main(void)
{
    printf("pxScan_test — 掃描／登記／排序\n");
    nc_px_layer_clear();
    nc_px_asset_clear();

    const esp_err_t ae = nc_px_asset_scan("tools/hosttest/pxScan_test/fs/effects");
    check(ae == ESP_OK, "effects 目錄掃描");
    check(nc_px_asset_count() == 6, "入表 6 個效果（uart_motor 自訂型別被拒）");
    check(nc_px_asset_find("eyes") != NULL && nc_px_asset_find("wave") != NULL,
          "eyes／wave 都搵到");

    char dir[192];
    snprintf(dir, sizeof(dir), "%s/map", FS);
    check(nc_px_map_scan(dir) == ESP_OK, "map 目錄掃描");
    const nc_px_map_tbl_t *mt = nc_px_map_table();
    check(mt->n == 3, "3 個 mapping（matrix／test／gundam）");
    check(nc_px_map_find("test") != NULL && nc_px_map_find("gundam") != NULL,
          "test／gundam 都入表");
    check(nc_px_map_find("matrix") != NULL && nc_px_map_find("3") != NULL,
          "matrix 可以用 name 或 id 搵到");

    snprintf(dir, sizeof(dir), "%s/modes", FS);
    check(nc_px_mode_scan(dir) == ESP_OK, "modes 目錄掃描");
    const nc_px_mode_tbl_t *dt = nc_px_mode_table();
    check(dt->n == 2, "2 個 mode");
    check(dt->item[0].id == 1 && strcmp(dt->item[0].name, "demo_eyes") == 0,
          "排序：index 1 = demo_eyes");
    check(dt->item[1].id == 2 && strcmp(dt->item[1].name, "motor_sine") == 0,
          "排序：index 2 = motor_sine");
    check(nc_px_mode_find("demo_eyes") != NULL && nc_px_mode_find("2") != NULL,
          "mode 可以用 name 或 id 搵到");

    snprintf(dir, sizeof(dir), "%s/registry.json", FS);
    check(nc_px_show_load(dir) == ESP_OK, "registry 讀取");
    check(nc_px_show()->auto_play == false && nc_px_show()->nlist == 0,
          "auto_play=false、list 空");

    // 一次掃齊四層（MP 開機同一個流程）
    nc_px_layer_clear();
    nc_px_asset_clear();
    check(nc_px_layer_load_all(FS) == ESP_OK, "load_all 成功");
    check(nc_px_asset_count() == 6 && nc_px_map_table()->n == 3 &&
          nc_px_mode_table()->n == 2 && nc_px_show()->nlist == 0,
          "load_all：6 effects／3 maps／2 modes／list 0");

    // 目錄唔存在
    check(nc_px_map_scan("tools/hosttest/pxScan_test/no_such") == ESP_ERR_NOT_FOUND,
          "目錄唔存在 → NOT_FOUND");

    // 重複 id／name：第二個要被拒
    const char *tmp = "tools/hosttest/pxScan_test/tmp_modes";
    sh_run("rm -rf tools/hosttest/pxScan_test/tmp_modes");
    sh_run("mkdir -p tools/hosttest/pxScan_test/tmp_modes");
    FILE *f = fopen("tools/hosttest/pxScan_test/tmp_modes/a.json", "wb");
    if (f != NULL) {
        fputs("{\"id\":9,\"name\":\"dup\",\"mapping\":\"matrix\","
              "\"map\":[{\"group\":\"matrix.full\",\"effect\":\"eyes\",\"write\":\"rgb\"}]}", f);
        fclose(f);
    }
    f = fopen("tools/hosttest/pxScan_test/tmp_modes/b.json", "wb");
    if (f != NULL) {
        fputs("{\"id\":9,\"name\":\"dup2\",\"mapping\":\"matrix\","
              "\"map\":[{\"group\":\"matrix.full\",\"effect\":\"wave\",\"write\":\"g\"}]}", f);
        fclose(f);
    }
    nc_px_layer_clear();
    check(nc_px_mode_scan(tmp) == ESP_OK, "重複測試目錄掃描");
    check(nc_px_mode_table()->n == 1 && nc_px_mode_table()->rejected == 1,
          "重複 id → 只收第一個（rejected=1）");
    sh_run("rm -rf tools/hosttest/pxScan_test/tmp_modes");

    printf("\n%s（%d 個失敗）\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
