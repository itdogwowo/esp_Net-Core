// nc_buf 的 host 測試：格狀態機、FIFO／隨機、off/len、S/P 記帳、換手、registry。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_buf.h"

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

static void test_basic(void)
{
    printf("\n[1] 建立與帳面\n");
    nc_buf_t *b = nc_buf_create("basic", NC_BUF_SRAM, 256, 4, 0);
    CHECK(b != NULL, "建立成功");
    CHECK(nc_buf_slot_bytes(b) == 256, "slot_bytes = 256");
    CHECK(nc_buf_slots(b) == 4, "n = 4");
    CHECK(nc_buf_fill(b) == 0, "一開始 fill = 0");
    CHECK(strcmp(nc_buf_name(b), "basic") == 0, "name");
    CHECK(strcmp(nc_buf_owner(b), "basic") == 0, "owner 預設 = name");
    CHECK(nc_buf_find("basic") == b, "find 找得到");
    CHECK(nc_buf_count() == 1, "count = 1");
    CHECK(nc_buf_total_bytes() == 1024, "total = 1024");
}

static void test_random_mode(void)
{
    printf("\n[2] 預設 FIFO：四格 + 滿了回 NULL\n");
    nc_buf_t *b = nc_buf_create("rand", NC_BUF_SRAM, 16, 4, 0);
    uint8_t v[16];
    for (int i = 0; i < 4; i++) {
        memset(v, i + 1, sizeof(v));
        CHECK(nc_buf_put(b, v, sizeof(v)) == ESP_OK, "put 成功（四格之內）");
    }
    CHECK(nc_buf_fill(b) == 4, "fill = 4");
    CHECK(nc_buf_put(b, v, sizeof(v)) == ESP_ERR_NO_MEM, "第五次 put → NO_MEM（不阻塞、不覆蓋）");

    for (int i = 0; i < 4; i++) {
        uint32_t len = 0;
        const uint8_t *p = nc_buf_get(b, &len, NULL);
        CHECK(p != NULL && len == 16, "get 拿到完整一格");
        CHECK(p != NULL && p[0] == (uint8_t)(i + 1), "內容依寫入次序");
        CHECK(nc_buf_get_end(b) == ESP_OK, "get_end 還格");
    }
    CHECK(nc_buf_get(b, NULL, NULL) == NULL, "讀完 → NULL");
}

static void test_fifo_mode(void)
{
    printf("\n[3] FIFO 模式：嚴格次序、唔可以跳格\n");
    nc_buf_t *b = nc_buf_create("fifo", NC_BUF_SRAM, 32, 2, NC_BUF_F_FIFO);
    uint8_t v[32] = {0};
    v[0] = 7;
    CHECK(nc_buf_put(b, v, 8) == ESP_OK, "寫第 1 格");
    v[0] = 9;
    CHECK(nc_buf_put(b, v, 8) == ESP_OK, "寫第 2 格");
    CHECK(nc_buf_put(b, v, 8) == ESP_ERR_NO_MEM, "第 3 格滿 → NO_MEM");

    uint32_t len = 0;
    const uint8_t *p = nc_buf_get(b, &len, NULL);
    CHECK(p != NULL && p[0] == 7, "先讀到第 1 格（FIFO）");
    CHECK(nc_buf_get(b, NULL, NULL) == NULL, "未還格前唔可以再 get");
    CHECK(nc_buf_get_end(b) == ESP_OK, "還格");
    p = nc_buf_get(b, &len, NULL);
    CHECK(p != NULL && p[0] == 9, "再讀到第 2 格");
    nc_buf_get_end(b);
    CHECK(nc_buf_fill(b) == 0, "清空");
    CHECK(nc_buf_put(b, v, 8) == ESP_OK, "歸還後可以再寫");
}

static void test_reserve_and_len(void)
{
    printf("\n[4] reserve（自己的表頭）與 len（實際用量）\n");
    nc_buf_t *b = nc_buf_create("hdr", NC_BUF_SRAM, 128, 2, NC_BUF_F_FIFO);
    uint8_t *base = nc_buf_put_begin(b, 16);
    CHECK(base != NULL, "put_begin(reserve=16) 成功");
    uint32_t *hdr = (uint32_t *)(base - 16);
    hdr[0] = 0xDEADBEEF;
    memset(base, 0x5A, 100);
    CHECK(nc_buf_put_end(b, 100) == ESP_OK, "put_end(100)");

    uint32_t len = 0, off = 0;
    const uint8_t *p = nc_buf_get(b, &len, &off);
    CHECK(p != NULL, "get 拿到");
    CHECK(off == 16, "off = 16（表頭在前）");
    CHECK(len == 100, "len = 100（不是整格 128）");
    CHECK(p != NULL && p[0] == 0x5A && p[99] == 0x5A, "資料完整");
    CHECK(*(uint32_t *)(p - 16) == 0xDEADBEEF, "表頭沒有被工具蓋掉");
    nc_buf_get_end(b);

    CHECK(nc_buf_put_begin(b, 200) == NULL, "reserve > 格容量 → NULL");
    void *w = nc_buf_put_begin(b, 0);
    CHECK(w != NULL, "重新拿到一格");
    CHECK(nc_buf_put_end(b, 129) == ESP_ERR_INVALID_SIZE, "len > cap → INVALID_SIZE");
}

static void test_32bit_len(void)
{
    printf("\n[5] len / cap 是 32-bit（uint16 會爆）\n");
    const uint32_t big = 100000;   // > 65535
    nc_buf_t *b = nc_buf_create("big", NC_BUF_SRAM, big, 1, 0);
    CHECK(b != NULL, "建立 100,000 B 的格");
    uint8_t *w = nc_buf_put_begin(b, 0);
    CHECK(w != NULL, "拿到");
    memset(w, 0x11, big);
    CHECK(nc_buf_put_end(b, big) == ESP_OK, "put_end(100000) 唔會截斷");
    uint32_t len = 0;
    CHECK(nc_buf_get(b, &len, NULL) != NULL && len == big, "get 回 100,000");
    nc_buf_get_end(b);
}

static void test_space_accounting(void)
{
    printf("\n[6] S2S / S2P / P2S / P2P 記帳\n");
    nc_buf_t *s1 = nc_buf_create("s1", NC_BUF_SRAM, 64, 1, 0);
    nc_buf_t *s2 = nc_buf_create("s2", NC_BUF_SRAM, 64, 1, 0);
    nc_buf_t *p1 = nc_buf_create("p1", NC_BUF_PSRAM, 64, 1, 0);
    nc_buf_t *p2 = nc_buf_create("p2", NC_BUF_PSRAM, 64, 1, 0);
    uint8_t v[64];
    memset(v, 0x33, sizeof(v));

    CHECK(nc_buf_put(s2, v, 64) == ESP_OK, "S → S");
    CHECK(nc_buf_put(p1, v, 64) == ESP_OK, "S → P");
    void *pr = nc_buf_raw(p1, 0);
    CHECK(nc_buf_put(s1, pr, 64) == ESP_OK, "P → S");
    CHECK(nc_buf_put(p2, pr, 64) == ESP_OK, "P → P");

    char js[4096];
    const size_t n = nc_buf_json(js, sizeof(js));
    CHECK(n > 0, "JSON 出得來");
    CHECK(strstr(js, "\"s2s\":64") != NULL, "s2s 記到 64");
    CHECK(strstr(js, "\"s2p\":64") != NULL, "s2p 記到 64");
    CHECK(strstr(js, "\"p2s\":64") != NULL, "p2s 記到 64");
    CHECK(strstr(js, "\"p2p\":64") != NULL, "p2p 記到 64");
    // 由「另一條 buffer」抄過來 → xcopy 現形（s1 ← p1 64、p2 ← p1 64）
    CHECK(strstr(js, "\"xcopy\":64") != NULL, "xcopy 記到 64（跨 buffer copy 被看見）");
}

static void test_handover(void)
{
    printf("\n[7] 換手：記憶體不動、只換 owner 與 generation\n");
    nc_buf_t *b = nc_buf_create("stage", NC_BUF_PSRAM, 128, 2, NC_BUF_F_FIFO);
    void *before = nc_buf_raw(b, 0);
    const uint32_t gen0 = nc_buf_generation(b);
    CHECK(nc_buf_handover(b, "fx") == ESP_OK, "全部空 → 換手成功");
    CHECK(nc_buf_generation(b) == gen0 + 1, "generation +1");
    CHECK(strcmp(nc_buf_owner(b), "fx") == 0, "owner = fx");
    CHECK(nc_buf_raw(b, 0) == before, "記憶體位置一模一樣");

    uint8_t v[8] = {0};
    CHECK(nc_buf_put(b, v, 8) == ESP_OK, "新 owner 可以寫");
    CHECK(nc_buf_handover(b, "ota") == ESP_ERR_INVALID_STATE, "有格未還 → 換手失敗");
    nc_buf_get(b, NULL, NULL);
    nc_buf_get_end(b);
    CHECK(nc_buf_handover(b, "ota") == ESP_OK, "還清之後可以再換手");
}

static void test_registry_full(void)
{
    printf("\n[8] registry 滿：一定要回 NULL（唔可以靜默）\n");
    nc_buf_reset();
    int made = 0;
    for (int i = 0; i < NC_BUF_MAX + 4; i++) {
        char n[16];
        snprintf(n, sizeof(n), "x%d", i);
        if (nc_buf_create(n, NC_BUF_SRAM, 64, 1, 0) != NULL) {
            made++;
        }
    }
    CHECK(made == NC_BUF_MAX, "剛好建立 NC_BUF_MAX 條");
    CHECK(nc_buf_create("over", NC_BUF_SRAM, 64, 1, 0) == NULL, "再多一條 → NULL");
    CHECK(nc_buf_count() == (size_t)NC_BUF_MAX, "count 仍然是上限");
    nc_buf_reset();
    CHECK(nc_buf_count() == 0, "reset 之後清空");
}

static void test_json_truncation(void)
{
    printf("\n[9] JSON 截斷仍然合法（STATUS 的緩衝是有界的）\n");
    nc_buf_create("alpha", NC_BUF_SRAM, 64, 2, 0);
    nc_buf_create("bravo", NC_BUF_SRAM, 64, 2, 0);
    nc_buf_create("charlie", NC_BUF_PSRAM, 64, 2, 0);
    char small[200];
    const size_t n = nc_buf_json(small, sizeof(small));
    CHECK(n > 0 && n < sizeof(small), "寫得入小緩衝");
    CHECK(n > 0 && small[n - 1] == '}', "有收尾，唔會半條");
    CHECK(strstr(small, "\"truncated\"") != NULL, "有 truncated 標記");
    CHECK(strstr(small, "\"count\":") != NULL, "仍然有 count");
}

static void test_stream(void)
{
    printf("\n[10] STREAM：連續位元組、部分寫／部分讀、壓實\n");
    nc_buf_t *b = nc_buf_create("st", NC_BUF_SRAM, 1024, 1, NC_BUF_F_STREAM);
    CHECK(b != NULL, "建立 STREAM 緩衝區");
    CHECK(nc_buf_pending(b) == 0, "一開始冇資料");
    CHECK(nc_buf_space(b) == 1024, "space = 1024");

    uint8_t v[600];
    memset(v, 0x7E, sizeof(v));
    CHECK(nc_buf_write(b, v, 600) == 600, "寫 600");
    CHECK(nc_buf_pending(b) == 600, "pending = 600");
    uint32_t len = 0;
    const uint8_t *p = nc_buf_read_ptr(b, &len);
    CHECK(p != NULL && len == 600 && p[0] == 0x7E, "讀到連續 600、內容對");
    CHECK(nc_buf_skip(b, 100) == ESP_OK, "消費 100（重同步可以 skip(1)）");
    CHECK(nc_buf_pending(b) == 500, "pending = 500");
    CHECK(nc_buf_space(b) == 524, "space = 1024 - 500");

    CHECK(nc_buf_write(b, v, 400) == 400, "尾部唔夠 → 壓實後仍然寫得入 400");
    CHECK(nc_buf_pending(b) == 900, "pending = 900");
    p = nc_buf_read_ptr(b, &len);
    CHECK(p != NULL && len == 900 && p[0] == 0x7E, "壓實後讀到的仍然係一段連續");

    CHECK(nc_buf_write(b, v, 600) == 124, "只剩 124 空間 → 部分寫入 124");
    CHECK(nc_buf_pending(b) == 1024, "滿了");
    CHECK(nc_buf_write(b, v, 1) == 0, "滿了再寫 → 0（唔覆蓋未讀）");

    CHECK(nc_buf_put_begin(b, 0) == NULL, "STREAM 唔可以用格 API（put_begin）");
    CHECK(nc_buf_get(b, NULL, NULL) == NULL, "STREAM 唔可以用格 API（get）");
    CHECK(nc_buf_handover(b, "x") == ESP_ERR_INVALID_STATE, "仲有未讀資料 → 唔准換手");

    nc_buf_skip(b, 1024);
    CHECK(nc_buf_pending(b) == 0 && nc_buf_space(b) == 1024, "跳完歸零");
    nc_buf_reset();
}

static void test_abort(void)
{
    printf("\n[11] 中途放棄唔會卡死環（nc4_bus 的教訓）\n");
    nc_buf_t *f = nc_buf_create("noStuck", NC_BUF_SRAM, 64, 2, NC_BUF_F_FIFO);
    uint8_t *w = nc_buf_put_begin(f, 0);
    CHECK(w != NULL, "拿得到格");
    CHECK(nc_buf_put_begin(f, 0) == NULL, "同一個 writer 一次只可以揸住一格");
    CHECK(nc_buf_handover(f, "x") == ESP_ERR_INVALID_STATE, "揸住格時唔准換手");
    CHECK(nc_buf_put_abort(f) == ESP_OK, "可以放棄");
    CHECK(nc_buf_put_begin(f, 0) != NULL, "放棄之後同一格再拿得到（唔會卡死）");
    CHECK(nc_buf_put_end(f, 8) == ESP_OK, "正常寫完仍然可以");
    nc_buf_get(f, NULL, NULL);
    nc_buf_get_end(f);
    CHECK(nc_buf_handover(f, "y") == ESP_OK, "清空之後可以換手");

    nc_buf_t *r = nc_buf_create("one", NC_BUF_SRAM, 64, 1, 0);
    CHECK(nc_buf_put_begin(r, 0) != NULL, "n=1 FIFO 拿一格");
    CHECK(nc_buf_put_abort(r) == ESP_OK, "放棄");
    CHECK(nc_buf_put_begin(r, 0) != NULL, "放棄後同一格回到 FREE");
    nc_buf_reset();
}

static void test_carve(void)
{
    printf("\n[12] carve：由 arena 切出多條 buffer\n");
    enum { ARENA = 4096 };
    void *region = NULL;
    if (posix_memalign(&region, 64, ARENA) != 0 || region == NULL) {
        CHECK(false, "配置 arena");
        return;
    }
    size_t cursor = 0;

    nc_buf_t *a = nc_buf_carve(region, ARENA, &cursor, "c.a", NC_BUF_PSRAM, 256, 4, 0);
    CHECK(a != NULL, "切第一條（256×4）");
    CHECK(cursor == 1024, "cursor 前進到 1024");
    nc_buf_t *b = nc_buf_carve(region, ARENA, &cursor, "c.b", NC_BUF_PSRAM, 100, 2, NC_BUF_F_STREAM == 0 ? 0 : 0);
    CHECK(b != NULL, "切第二條（100×2）");
    CHECK(nc_buf_raw(a, 0) != nc_buf_raw(b, 0), "兩條唔會重疊");

    const size_t before = cursor;
    CHECK(nc_buf_carve(region, ARENA, &cursor, "c.big", NC_BUF_PSRAM, 4096, 1, 0) == NULL,
          "唔夠位 → NULL（唔會部分配置）");
    CHECK(cursor == before, "失敗時 cursor 唔會亂跳");

    CHECK(nc_buf_release(a) == ESP_OK, "runtime buffer 可以 release");
    CHECK(nc_buf_release(b) == ESP_OK, "同上");
    free(region);
    nc_buf_reset();
    CHECK(nc_buf_count() == 0, "清空");
}

static void test_ring(void)
{
    printf("\n-- RING：真環形、read_segments 兩段 --\n");
    nc_buf_t *b = nc_buf_create("r.ring", NC_BUF_SRAM, 64, 1,
                                NC_BUF_F_RUNTIME | NC_BUF_F_STREAM | NC_BUF_F_RING);
    CHECK(b != NULL, "建立 RING 64 B");
    uint8_t src[64];
    for (int i = 0; i < 64; i++) {
        src[i] = (uint8_t)i;
    }
    CHECK(nc_buf_write(b, src, 40) == 40, "寫 40");
    CHECK(nc_buf_pending(b) == 40 && nc_buf_space(b) == 24, "pending 40 / space 24");

    const uint8_t *p0 = NULL, *p1 = NULL;
    uint32_t n0 = 0, n1 = 0;
    CHECK(nc_buf_read_segments(b, &p0, &n0, &p1, &n1) == 40, "讀 40");
    CHECK(n0 == 40 && n1 == 0, "一段");
    CHECK(p0 != NULL && p0[0] == 0 && p0[39] == 39, "內容正確");

    const uint8_t *rp = nc_buf_read_ptr(b, NULL);
    CHECK(rp == p0, "read_ptr 回第一段");
    CHECK(nc_buf_skip(b, 32) == ESP_OK, "消費 32");
    CHECK(nc_buf_pending(b) == 8, "剩 8");

    CHECK(nc_buf_write(b, src, 40) == 40, "再寫 40（跨環）");
    CHECK(nc_buf_pending(b) == 48, "pending 48");
    CHECK(nc_buf_read_segments(b, &p0, &n0, &p1, &n1) == 48, "回 48");
    CHECK(n0 == 32 && n1 == 16, "兩段 32 + 16");
    CHECK(p0 == nc_buf_raw(b, 0) + 32 && p1 == nc_buf_raw(b, 0), "第二段由頭開始");
    CHECK(nc_buf_skip(b, 48) == ESP_OK, "全部消費");
    CHECK(nc_buf_pending(b) == 0 && nc_buf_space(b) == 64, "歸零");

    CHECK(nc_buf_write(b, src, 40) == 40, "歸零後再寫");
    CHECK(nc_buf_skip(b, 40) == ESP_OK, "再清空");
    CHECK(nc_buf_release(b) == ESP_OK, "release RING");

    // 線性 STREAM 的 read_segments 仍然只有一段。
    nc_buf_t *l = nc_buf_create("r.lin", NC_BUF_SRAM, 64, 1,
                                NC_BUF_F_RUNTIME | NC_BUF_F_STREAM);
    CHECK(l != NULL, "建立線性 STREAM 64 B");
    CHECK(nc_buf_write(l, src, 10) == 10, "寫 10");
    CHECK(nc_buf_read_segments(l, &p0, &n0, &p1, &n1) == 10, "回 10");
    CHECK(n0 == 10 && n1 == 0 && p1 == NULL, "線性永遠一段");
    CHECK(nc_buf_release(l) == ESP_OK, "release 線性");
}

int main(void)
{
    test_basic();
    test_random_mode();
    test_fifo_mode();
    test_reserve_and_len();
    test_32bit_len();
    test_space_accounting();
    test_handover();
    test_registry_full();
    test_json_truncation();
    test_stream();
    test_ring();
    test_abort();
    test_carve();

    printf("\n%s\n", fails == 0 ? "ALL PASS" : "有 FAIL");
    return fails == 0 ? 0 : 1;
}
