// stateStore_test — NCSTATE RAM store host test（set/get/對拍/容量/bytes）。
#include <stdio.h>
#include <string.h>

#include "nc_state_store.h"

static int g_fail;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            g_fail++;                                                           \
        }                                                                       \
    } while (0)

int main(void)
{
    nc_state_store_t s;

    printf("[1] put／get：uint／sint／bool／bytes\n");
    nc_state_store_clear(&s);
    const char *fmt = "ws2812";
    nc_state_rec_t rs[] = {
        {.key = 0x0002, .type = NC_STATE_T_UINT, .u = 20},
        {.key = 0x0702, .type = NC_STATE_T_SINT, .i = -1},
        {.key = 0x0C01, .type = NC_STATE_T_BOOL, .u = 1},
        {.key = 0x0C06, .type = NC_STATE_T_BYTES, .bytes = fmt, .bytes_len = 6},
    };
    for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
        CHECK(nc_state_store_put(&s, &rs[i]), "put[%zu] 失敗", i);
    }
    CHECK(s.n == 4, "n=%u（預期 4）", s.n);
    nc_state_rec_t got;
    CHECK(nc_state_store_get(&s, 0x0002, &got) && got.u == 20, "get 0x0002");
    CHECK(nc_state_store_get(&s, 0x0702, &got) && got.i == -1, "get 0x0702");
    CHECK(nc_state_store_get(&s, 0x0C01, &got) && got.u == 1, "get 0x0C01");
    CHECK(nc_state_store_get(&s, 0x0C06, &got) && got.bytes_len == 6 &&
          memcmp(got.bytes, fmt, 6) == 0, "get 0x0C06 bytes");
    CHECK(!nc_state_store_get(&s, 0x9999, &got), "唔存在應該 false");

    printf("[2] 重複 key 覆蓋（唔會加第二筆）\n");
    nc_state_rec_t r2 = {.key = 0x0002, .type = NC_STATE_T_UINT, .u = 30};
    CHECK(nc_state_store_put(&s, &r2), "覆蓋 put 失敗");
    CHECK(s.n == 4, "覆蓋後 n=%u（預期 4）", s.n);
    CHECK(nc_state_store_get(&s, 0x0002, &got) && got.u == 30, "覆蓋後值應該 30");

    printf("[3] from_recs／cmp：一致回 0、改一筆回 1＋first key\n");
    nc_state_store_clear(&s);
    CHECK(nc_state_store_from_recs(&s, rs, 4) == 4, "from_recs 應該 4 筆");
    uint16_t first = 0;
    CHECK(nc_state_store_cmp(&s, rs, 4, &first) == 0, "一致時 cmp 應該 0");
    nc_state_rec_t rs2[4];
    memcpy(rs2, rs, sizeof(rs));
    rs2[2].u = 0;   // bool 1 → 0
    CHECK(nc_state_store_cmp(&s, rs2, 4, &first) == 1, "一筆唔同應該 1");
    CHECK(first == 0x0C01, "first_diff=0x%04x（預期 0x0C01）", first);
    rs2[3].bytes_len = 5;
    CHECK(nc_state_store_cmp(&s, rs2, 4, &first) >= 2, "兩筆唔同應該 ≥2");

    printf("[4] bytes 太長／store 滿：明示 false\n");
    char big[NC_STATE_STORE_BYTES_MAX + 1];
    memset(big, 'x', sizeof(big));
    nc_state_rec_t rb = {.key = 1, .type = NC_STATE_T_BYTES, .bytes = big,
                         .bytes_len = (uint16_t)sizeof(big)};
    CHECK(!nc_state_store_put(&s, &rb), "bytes 太長應該 false");
    nc_state_store_clear(&s);
    nc_state_rec_t rfill = {.type = NC_STATE_T_UINT, .u = 1};
    for (int i = 0; i < NC_STATE_STORE_MAX; i++) {
        rfill.key = (uint16_t)(0x100 + i);
        CHECK(nc_state_store_put(&s, &rfill), "fill[%d] 失敗", i);
    }
    rfill.key = 0xFFFF;
    CHECK(!nc_state_store_put(&s, &rfill), "滿了應該 false");

    printf("[5] 空 store／NULL 安全\n");
    nc_state_store_clear(&s);
    CHECK(s.n == 0, "clear 後 n 應該 0");
    CHECK(nc_state_store_cmp(&s, NULL, 0, NULL) == 0, "空對空應該 0");
    CHECK(nc_state_store_cmp(NULL, rs, 4, NULL) != 0, "NULL store 應該報唔一致");
    nc_state_store_clear(NULL);   // 唔應該 crash

    if (g_fail == 0) {
        printf("stateStore_test OK\n");
        return 0;
    }
    printf("stateStore_test FAIL（%d 項）\n", g_fail);
    return 1;
}
