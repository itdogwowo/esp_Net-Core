// stateStore_test — NCSTATE RAM store／read API host test。
// read／write API 用 test 自己提供的 nc_state_boot_store[_mut]() stub（唔使拉 config／boot）。
#include <stdio.h>
#include <string.h>

#include "nc_state_keys.h"
#include "nc_state_api.h"
#include "nc_state_store.h"

static nc_state_store_t s_stub;

const nc_state_store_t *nc_state_boot_store(void)
{
    return (s_stub.n > 0) ? &s_stub : NULL;
}

nc_state_store_t *nc_state_boot_store_mut(void)
{
    return &s_stub;
}

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
        {.key = NC_KEY_SYS_FRAME_MS, .type = NC_STATE_T_UINT, .u = 20},
        {.key = NC_KEY_I80_WR, .type = NC_STATE_T_SINT, .i = -1},
        {.key = NC_KEY_LED_ENABLE, .type = NC_STATE_T_BOOL, .u = 1},
        {.key = NC_KEY_LED_FMT, .type = NC_STATE_T_BYTES, .bytes = fmt, .bytes_len = 6},
    };
    for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
        CHECK(nc_state_store_put(&s, &rs[i]), "put[%zu] 失敗", i);
    }
    CHECK(s.n == 4, "n=%u（預期 4）", s.n);
    nc_state_rec_t got;
    CHECK(nc_state_store_get(&s, NC_KEY_SYS_FRAME_MS, &got) && got.u == 20, "get frame_ms");
    CHECK(nc_state_store_get(&s, NC_KEY_I80_WR, &got) && got.i == -1, "get wr");
    CHECK(nc_state_store_get(&s, NC_KEY_LED_ENABLE, &got) && got.u == 1, "get enable");
    CHECK(nc_state_store_get(&s, NC_KEY_LED_FMT, &got) && got.bytes_len == 6 &&
          memcmp(got.bytes, fmt, 6) == 0, "get fmt");

    printf("[2] 重複 key 覆蓋（唔會加第二筆）\n");
    nc_state_rec_t r2 = {.key = NC_KEY_SYS_FRAME_MS, .type = NC_STATE_T_UINT, .u = 30};
    CHECK(nc_state_store_put(&s, &r2), "覆蓋 put 失敗");
    CHECK(s.n == 4, "覆蓋後 n=%u（預期 4）", s.n);
    CHECK(nc_state_store_get(&s, NC_KEY_SYS_FRAME_MS, &got) && got.u == 30, "覆蓋後值應該 30");

    printf("[3] from_recs／cmp：一致回 0、改一筆回 1＋first key\n");
    nc_state_store_clear(&s);
    CHECK(nc_state_store_from_recs(&s, rs, 4) == 4, "from_recs 應該 4 筆");
    uint32_t first = 0;
    CHECK(nc_state_store_cmp(&s, rs, 4, &first) == 0, "一致時 cmp 應該 0");
    nc_state_rec_t rs2[4];
    memcpy(rs2, rs, sizeof(rs));
    rs2[2].u = 0;
    CHECK(nc_state_store_cmp(&s, rs2, 4, &first) == 1, "一筆唔同應該 1");
    CHECK(first == NC_KEY_LED_ENABLE, "first_diff=0x%08x", first);

    printf("[4] read API：有值／冇值／型別唔啱／字串 cap\n");
    s_stub = s;   // stub 指向同一份 store
    uint32_t u = 0;
    int32_t iv = 0;
    bool bv = false;
    char buf[16];
    CHECK(nc_state_get_u32(NC_KEY_SYS_FRAME_MS, &u) && u == 20, "read frame_ms");
    CHECK(nc_state_get_i32(NC_KEY_I80_WR, &iv) && iv == -1, "read wr");
    CHECK(nc_state_get_bool(NC_KEY_LED_ENABLE, &bv) && bv, "read enable");
    CHECK(nc_state_get_str(NC_KEY_LED_FMT, buf, sizeof(buf)) &&
          strcmp(buf, "ws2812") == 0, "read fmt");
    CHECK(!nc_state_get_u32(0x7FFF, &u), "唔存在 key 應該 false");
    CHECK(!nc_state_get_u32(NC_KEY_I80_WR, &u), "型別唔啱（sint 當 u32）應該 false");
    CHECK(!nc_state_get_str(NC_KEY_LED_FMT, buf, 4), "cap 唔夠應該 false（唔截斷）");
    nc_state_store_clear(&s_stub);
    CHECK(!nc_state_get_u32(NC_KEY_SYS_FRAME_MS, &u), "store 清空應該 false");

    printf("[5] bytes 太長／store 滿：明示 false\n");
    char big[NC_STATE_STORE_BYTES_MAX + 1];
    memset(big, 'x', sizeof(big));
    nc_state_rec_t rb = {.key = 1, .type = NC_STATE_T_BYTES, .bytes = big,
                         .bytes_len = (uint16_t)sizeof(big)};
    CHECK(!nc_state_store_put(&s, &rb), "bytes 太長應該 false");
    nc_state_store_clear(&s);
    nc_state_rec_t rfill = {.type = NC_STATE_T_UINT, .u = 1};
    for (int i = 0; i < NC_STATE_STORE_MAX; i++) {
        rfill.key = 0x100u + i;
        CHECK(nc_state_store_put(&s, &rfill), "fill[%d] 失敗", i);
    }
    rfill.key = 0xFFFF;
    CHECK(!nc_state_store_put(&s, &rfill), "滿了應該 false");

    printf("[6] 空 store／NULL 安全\n");
    nc_state_store_clear(&s);
    CHECK(s.n == 0, "clear 後 n 應該 0");
    CHECK(nc_state_store_cmp(&s, NULL, 0, NULL) == 0, "空對空應該 0");
    CHECK(nc_state_store_cmp(NULL, rs, 4, NULL) != 0, "NULL store 應該報唔一致");
    nc_state_store_clear(NULL);

    printf("[7] bind：攞到 stable 位址，set 之後同一個位置見到新值\n");
    {
        nc_state_store_clear(&s_stub);
        const char *fmt2 = "ws2812";
        nc_state_rec_t brs[] = {
            {.key = NC_KEY_LED_PX, .type = NC_STATE_T_UINT, .u = 64},
            {.key = NC_KEY_I80_WR, .type = NC_STATE_T_SINT, .i = 14},
            {.key = NC_KEY_LED_FMT, .type = NC_STATE_T_BYTES, .bytes = fmt2, .bytes_len = 6},
        };
        CHECK(nc_state_store_from_recs(&s_stub, brs, 3) == 3, "from_recs 失敗");

        const uint32_t *px = NULL;
        CHECK(nc_state_bind_u32(NC_KEY_LED_PX, &px), "bind px 應該成功");
        CHECK(px != NULL && *px == 64, "px=%u（預期 64）", px ? *px : 0u);

        const int32_t *wr = NULL;
        CHECK(nc_state_bind_i32(NC_KEY_I80_WR, &wr), "bind wr 應該成功");
        CHECK(wr != NULL && *wr == 14, "wr=%d", wr ? (int)*wr : 0);

        CHECK(nc_state_set_u32(NC_KEY_LED_PX, 128) == NC_STATE_SET_OK, "set px");
        const uint32_t *px2 = NULL;
        CHECK(nc_state_bind_u32(NC_KEY_LED_PX, &px2), "re-bind px");
        CHECK(px2 == px, "同一個 key 位址應該 stable");
        CHECK(px != NULL && *px == 128, "同一個 pointer 應該見到新值 128");

        const uint32_t *bad = NULL;
        CHECK(!nc_state_bind_u32(NC_KEY_LED_FMT, &bad), "bind 型別唔對應該 false");
        CHECK(!nc_state_bind_u32(0x00FF00FFu, &bad), "bind 未知 key 應該 false");
        CHECK(!nc_state_bind_u32(NC_KEY_LED_PX, NULL), "out=NULL 應該 false");
    }

    if (g_fail == 0) {
        printf("stateStore_test OK\n");
        return 0;
    }
    printf("stateStore_test FAIL（%d 項）\n", g_fail);
    return 1;
}
