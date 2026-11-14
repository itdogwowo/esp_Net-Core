// Plan v2 host 測試：逐項失敗、level 順序、每 class 只 carve 一次
#include <stdio.h>
#include <string.h>

#include "nc_plan.h"
#include "plan_host.h"

static int fails;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { printf("  PASS  %s\n", msg); }                             \
        else { printf("  FAIL  %s\n", msg); fails++; }                         \
    } while (0)

static void decl(const char *svc, const char *n, int lvl, nc_plan_class_t c, size_t b)
{
    nc_plan_decl_t d = {.service = svc, .name = n, .level = lvl, .cls = c, .bytes = b};
    if (nc_plan_declare(&d) != ESP_OK) {
        printf("  宣告失敗: %s\n", n);
        fails++;
    }
}

static void decl_system(void)
{
    decl("nc4",   "usb.rx",   0, NC_PLAN_INTERNAL, 8205);
    decl("probe", "probe.hub",3, NC_PLAN_INTERNAL, 512);
    decl("pixel", "pixel.slots", 2, NC_PLAN_DMA, 10816);
    decl("asset", "assets",   3, NC_PLAN_PSRAM, 65536);
}

int main(void)
{
    size_t avail[NC_PLAN_CLASSES];

    printf("\n[1] 容量充足：逐項成功、每 class 只 carve 一次\n");
    nc_plan_reset();
    plan_host_reset(64 * 1024, 128 * 1024, 256 * 1024);
    decl_system();
    avail[NC_PLAN_DMA] = 64 * 1024; avail[NC_PLAN_INTERNAL] = 128 * 1024; avail[NC_PLAN_PSRAM] = 256 * 1024;
    CHECK(nc_plan_commit(avail, false) == ESP_OK, "commit 成功");
    CHECK(nc_plan_report()->fail_count == 0, "沒有失敗項");
    CHECK(nc_plan_get("usb.rx") != NULL, "拿得到 usb.rx");
    CHECK(nc_plan_get("pixel.slots") != NULL, "拿得到 pixel.slots");
    CHECK(plan_host_allocs() == 3, "三個 class 各 carve 一次（共 3 次 alloc）");

    printf("\n[2] 內部 RAM 不足：逐項失敗，其他人照跑\n");
    nc_plan_reset();
    plan_host_reset(64 * 1024, 8500, 256 * 1024);
    decl_system();
    avail[NC_PLAN_DMA] = 64 * 1024; avail[NC_PLAN_INTERNAL] = 8500; avail[NC_PLAN_PSRAM] = 256 * 1024;
    CHECK(nc_plan_commit(avail, false) == ESP_OK, "commit 仍成功（不是整批失敗）");
    CHECK(nc_plan_report()->fail_count == 1, "剛好一項失敗");
    CHECK(nc_plan_get("usb.rx") != NULL, "level 0 先拿到");
    CHECK(nc_plan_get("probe.hub") == NULL, "後面的 probe 被標記失敗");
    CHECK(nc_plan_was_shed("probe.hub"), "was_shed 回報正確");
    CHECK(nc_plan_get("pixel.slots") != NULL, "其他 class 不受影響");

    printf("\n[3] level 小的優先（順序不影響結果）\n");
    nc_plan_reset();
    plan_host_reset(64 * 1024, 8500, 256 * 1024);
    decl("probe", "probe.hub", 3, NC_PLAN_INTERNAL, 512);   // 先宣告
    decl("nc4",   "usb.rx",    0, NC_PLAN_INTERNAL, 8205);  // 後宣告但 level 小
    CHECK(nc_plan_commit(avail, false) == ESP_OK, "commit 成功");
    CHECK(nc_plan_get("usb.rx") != NULL, "level 0 仍然拿到");
    CHECK(nc_plan_get("probe.hub") == NULL, "level 3 失敗");

    printf("\n[4] 宣告窗口 / 重名\n");
    nc_plan_decl_t late = {.service = "x", .name = "late", .level = 0,
                           .cls = NC_PLAN_INTERNAL, .bytes = 16};
    CHECK(nc_plan_declare(&late) == ESP_ERR_INVALID_STATE, "commit 後拒絕宣告");
    nc_plan_reset();
    plan_host_reset(64 * 1024, 64 * 1024, 64 * 1024);
    decl("a", "dup", 0, NC_PLAN_INTERNAL, 32);
    nc_plan_decl_t again = {.service = "b", .name = "dup", .level = 0,
                            .cls = NC_PLAN_INTERNAL, .bytes = 32};
    CHECK(nc_plan_declare(&again) == ESP_ERR_INVALID_STATE, "同名被拒");

    printf("\n%s（%d 個失敗）\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
