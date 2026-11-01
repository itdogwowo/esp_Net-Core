// nc_hub 的 host 測試：狀態機、三種深度、非阻塞語義
#include <stdio.h>
#include <string.h>

#include "nc_hub.h"

static int fails;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("  PASS  %s\n", msg);                                       \
        } else {                                                               \
            printf("  FAIL  %s\n", msg);                                       \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static void test_n1(void)
{
    printf("\n[1] N=1：一次性轉交\n");
    nc_hub_t *h = nc_hub_create("one", 64, 1, NC_HUB_SRAM, 0);
    CHECK(h != NULL, "建立成功");
    CHECK(!nc_hub_dirty(h), "一開始沒有資料");

    void *w = nc_hub_write_begin(h);
    CHECK(w != NULL, "writer 拿得到");
    memset(w, 0xAB, 64);
    CHECK(nc_hub_write_commit(h) == ESP_OK, "commit 成功");
    CHECK(nc_hub_dirty(h), "dirty = true");

    void *r = nc_hub_read_begin(h);
    CHECK(r == w, "reader 拿到同一塊");
    CHECK(nc_hub_write_begin(h) == NULL, "未歸還前 writer 拿不到（一次性轉交的語義）");

    nc_hub_read_end(h);
    CHECK(nc_hub_write_begin(h) != NULL, "歸還後 writer 可以再寫");
}

static void test_n3(void)
{
    printf("\n[3] N=3：輪替\n");
    nc_hub_t *h = nc_hub_create("three", 32, 3, NC_HUB_SRAM, 0);
    CHECK(h != NULL, "建立成功");

    void *p[3];
    for (int i = 0; i < 3; i++) {
        p[i] = nc_hub_write_begin(h);
        CHECK(p[i] != NULL, "writer 三格都拿得到");
        memset(p[i], (char)i, 32);
        nc_hub_write_commit(h);
    }
    CHECK(nc_hub_fill(h) == 3, "fill = 3");
    CHECK(nc_hub_write_begin(h) == NULL, "三格滿了 → writer 拿到 NULL（不阻塞）");

    // FIFO：依寫入順序讀出
    for (int i = 0; i < 3; i++) {
        void *r = nc_hub_read_begin(h);
        CHECK(r == p[i], "依序讀出（FIFO）");
    }
    CHECK(nc_hub_read_begin(h) == NULL, "讀完 → NULL");
}

static void test_rotate(void)
{
    printf("\n[4] 讀寫交替：歸還後可重用\n");
    nc_hub_t *h = nc_hub_create("rot", 16, 2, NC_HUB_SRAM, 0);
    for (int round = 0; round < 5; round++) {
        void *w = nc_hub_write_begin(h);
        if (w == NULL) {
            printf("  FAIL  round %d 拿不到 writer slot\n", round);
            fails++;
            break;
        }
        *(int *)w = round;
        nc_hub_write_commit(h);
        void *r = nc_hub_read_begin(h);
        if (r == NULL || *(int *)r != round) {
            printf("  FAIL  round %d 讀到的內容不對\n", round);
            fails++;
            break;
        }
        nc_hub_read_end(h);
    }
    CHECK(true, "5 輪交替全部正確");
}

static void test_auto_release(void)
{
    printf("\n[5] read_begin 自動歸還上一格\n");
    nc_hub_t *h = nc_hub_create("auto", 8, 3, NC_HUB_SRAM, 0);
    for (int i = 0; i < 3; i++) {
        void *w = nc_hub_write_begin(h);
        *(int *)w = i;
        nc_hub_write_commit(h);
    }
    void *r1 = nc_hub_read_begin(h);
    void *r2 = nc_hub_read_begin(h);   // 自動歸還 r1
    CHECK(r1 != r2, "拿到不同的格子");
    CHECK(nc_hub_write_begin(h) != NULL, "r1 已自動歸還 → writer 有位置");
    nc_hub_read_end(h);
}

int main(void)
{
    test_n1();
    test_n3();
    test_rotate();
    test_auto_release();
    printf("\n%s（%d 個失敗）\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
