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

static void test_generation(void)
{
    printf("\n[6] generation 與統計\n");
    nc_hub_t *h = nc_hub_create("gen", 16, 2, NC_HUB_SRAM, 0);
    CHECK(h != NULL, "建立成功");

    nc_hub_stats_t st;
    nc_hub_stats(h, &st);
    CHECK(st.generation == 0 && st.publishes == 0, "初始 generation／publishes = 0");
    CHECK(st.writer_core == -1 && st.reader_core == -1, "尚未使用時 core = -1");

    void *w = nc_hub_write_begin(h);
    CHECK(w != NULL, "writer 拿得到");
    *(uint32_t *)w = 0xA1;
    CHECK(nc_hub_write_commit(h) == ESP_OK, "commit 成功");
    CHECK(nc_hub_generation(h) == 1, "generation = 1");

    uint32_t gen = 0;
    void *r = nc_hub_read_begin_gen(h, &gen);
    CHECK(r == w && gen == 1, "reader 拿到同一格與對應 generation");
    nc_hub_read_end(h);

    w = nc_hub_write_begin(h);
    CHECK(w != NULL, "歸還後可再寫");
    nc_hub_write_commit(h);
    CHECK(nc_hub_generation(h) == 2, "第二次 publish generation = 2");

    nc_hub_stats(h, &st);
    CHECK(st.publishes == 2 && st.generation == 2, "stats 反映 publish 次數");
    CHECK(st.writer_core == 0 && st.reader_core == 0, "host 上首次使用的 core = 0");
    CHECK(st.core_violations == 0, "同核使用沒有 violation");
}

static void test_destroy(void)
{
    printf("\n[7] destroy 後可重建（registry 上限 8）\n");
    bool ok = true;
    for (int i = 0; i < 12; i++) {
        nc_hub_t *h = nc_hub_create("tmp", 16, 2, NC_HUB_SRAM, 0);
        if (h == NULL) {
            ok = false;
            break;
        }
        if (nc_hub_write_begin(h) == NULL) {
            ok = false;
        }
        nc_hub_destroy(h);
    }
    CHECK(ok, "連續 12 次 create/destroy 都成功");

    static uint8_t region[128];
    nc_hub_t *h = nc_hub_create_in(region, sizeof(region), "ext", 16, 2, 0);
    CHECK(h != NULL, "外部區域建立成功");
    nc_hub_destroy(h);
    region[0] = 1;
    CHECK(region[0] == 1, "destroy 不釋放外部區域");
}

int main(void)
{
    test_n1();
    test_n3();
    test_rotate();
    test_auto_release();
    test_generation();
    test_destroy();
    printf("\n%s（%d 個失敗）\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
