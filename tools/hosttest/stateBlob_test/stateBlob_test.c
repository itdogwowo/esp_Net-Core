// stateBlob_test — NCSTATE codec host test（唯一狀態格式：自描述、CRC、向前兼容）。
//
// 覆蓋：
//   1. encode→decode round-trip（uint／sint 邊界、bool、bytes、空 bytes）
//   2. header 錯誤：magic／ver／payload_len／截斷／CRC
//   3. 未知 type → 跳過（向前兼容），其餘 record 照收
//   4. bool 非法值、varint 長度唔一致 → INVALID_SIZE
//   5. encode cap 不足 → 0；size() 與實際一致
#include <stdio.h>
#include <string.h>

#include "nc_state_blob.h"

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

static uint32_t crc32_bytes(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void fix_crc(uint8_t *buf, size_t len)
{
    const size_t payload = len - NC_STATE_BLOB_HDR;
    const uint32_t crc = crc32_bytes(&buf[NC_STATE_BLOB_HDR], payload);
    buf[22] = (uint8_t)(crc & 0xFFu);
    buf[23] = (uint8_t)((crc >> 8) & 0xFFu);
    buf[24] = (uint8_t)((crc >> 16) & 0xFFu);
    buf[25] = (uint8_t)((crc >> 24) & 0xFFu);
}

typedef struct {
    nc_state_rec_t recs[16];
    size_t n;
} collector_t;

static esp_err_t collect(void *ud, const nc_state_rec_t *r)
{
    collector_t *c = (collector_t *)ud;
    if (c->n < 16) {
        c->recs[c->n++] = *r;
    }
    return ESP_OK;
}

static void test_roundtrip(void)
{
    const char *fmt = "ws2812";
    const char *empty = "";
    nc_state_rec_t recs[] = {
        {.key = 0x0002, .type = NC_STATE_T_UINT, .u = 0},
        {.key = 0x0003, .type = NC_STATE_T_UINT, .u = 127},
        {.key = 0x0004, .type = NC_STATE_T_UINT, .u = 128},
        {.key = 0x0005, .type = NC_STATE_T_UINT, .u = 0xFFFFFFFFu},
        {.key = 0x0702, .type = NC_STATE_T_SINT, .i = -1},
        {.key = 0x0703, .type = NC_STATE_T_SINT, .i = 2147483647},
        {.key = 0x0704, .type = NC_STATE_T_SINT, .i = -2147483647 - 1},
        {.key = 0x0C01, .type = NC_STATE_T_BOOL, .u = 1},
        {.key = 0x0C06, .type = NC_STATE_T_BYTES, .bytes = fmt, .bytes_len = 6},
        {.key = 0x0C07, .type = NC_STATE_T_BYTES, .bytes = empty, .bytes_len = 0},
    };
    const size_t nrec = sizeof(recs) / sizeof(recs[0]);
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = 0xDEADBEEFu};
    uint8_t buf[512];
    const size_t need = nc_state_blob_size(recs, nrec);
    CHECK(need > NC_STATE_BLOB_HDR, "size() 應該大過 header（%zu）", need);
    const size_t got = nc_state_blob_encode(buf, sizeof(buf), &meta, recs, nrec);
    CHECK(got == need, "encode size %zu != size() %zu", got, need);

    collector_t c = {0};
    nc_state_info_t info;
    const esp_err_t e = nc_state_blob_decode(buf, got, collect, &c, &info);
    CHECK(e == ESP_OK, "decode 失敗 %d", (int)e);
    CHECK(info.ver == NC_STATE_BLOB_VER, "ver %u", info.ver);
    CHECK(info.src_hash == meta.src_hash, "src_hash %08x", info.src_hash);
    CHECK(info.n_records == nrec, "count %u != %zu", info.n_records, nrec);
    CHECK(info.n_skipped == 0, "skipped %u", info.n_skipped);
    CHECK(c.n == nrec, "callback 收到 %zu 筆（預期 %zu）", c.n, nrec);
    for (size_t i = 0; i < nrec && i < c.n; i++) {
        CHECK(c.recs[i].key == recs[i].key, "rec[%zu] key %u != %u", i, c.recs[i].key, recs[i].key);
        CHECK(c.recs[i].type == recs[i].type, "rec[%zu] type %u != %u", i, c.recs[i].type, recs[i].type);
        if (recs[i].type == NC_STATE_T_UINT || recs[i].type == NC_STATE_T_BOOL) {
            CHECK(c.recs[i].u == recs[i].u, "rec[%zu] u %u != %u", i, c.recs[i].u, recs[i].u);
        } else if (recs[i].type == NC_STATE_T_SINT) {
            CHECK(c.recs[i].i == recs[i].i, "rec[%zu] i %d != %d", i, (int)c.recs[i].i, (int)recs[i].i);
        } else {
            CHECK(c.recs[i].bytes_len == recs[i].bytes_len, "rec[%zu] len %u != %u",
                  i, c.recs[i].bytes_len, recs[i].bytes_len);
            if (recs[i].bytes_len) {
                CHECK(memcmp(c.recs[i].bytes, recs[i].bytes, recs[i].bytes_len) == 0,
                      "rec[%zu] bytes 唔同", i);
            }
        }
    }
}

static void test_empty(void)
{
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = 0};
    uint8_t buf[64];
    const size_t got = nc_state_blob_encode(buf, sizeof(buf), &meta, NULL, 0);
    CHECK(got == NC_STATE_BLOB_HDR, "空 blob 應該等於 header（%zu）", got);
    nc_state_info_t info;
    CHECK(nc_state_blob_decode(buf, got, NULL, NULL, &info) == ESP_OK, "空 blob decode 失敗");
    CHECK(info.n_records == 0, "空 blob count %u", info.n_records);
}

static void test_bad_header(void)
{
    nc_state_rec_t rec = {.key = 1, .type = NC_STATE_T_UINT, .u = 7};
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = 0x12345678u};
    uint8_t buf[64];
    const size_t got = nc_state_blob_encode(buf, sizeof(buf), &meta, &rec, 1);
    CHECK(got > NC_STATE_BLOB_HDR, "前置 encode 失敗");

    uint8_t tmp[64];
    memcpy(tmp, buf, got);
    tmp[0] = 'X';
    CHECK(nc_state_blob_decode(tmp, got, NULL, NULL, NULL) == ESP_ERR_INVALID_STATE, "壞 magic");

    memcpy(tmp, buf, got);
    tmp[8] = 2;   // ver = 2
    CHECK(nc_state_blob_decode(tmp, got, NULL, NULL, NULL) == ESP_ERR_INVALID_STATE, "ver 唔夾");

    memcpy(tmp, buf, got);
    tmp[22] ^= 0xFFu;   // 改 CRC（4 bytes @22）
    CHECK(nc_state_blob_decode(tmp, got, NULL, NULL, NULL) == ESP_FAIL, "CRC 錯");

    memcpy(tmp, buf, got);
    tmp[18] ^= 0x01u;   // 改 payload_len（LE 最低 byte）
    CHECK(nc_state_blob_decode(tmp, got, NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE,
          "payload_len 唔夾");

    CHECK(nc_state_blob_decode(buf, got - 1, NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE, "截斷");
    CHECK(nc_state_blob_decode(buf, 10, NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE, "header 唔夠");
    CHECK(nc_state_blob_decode(NULL, 0, NULL, NULL, NULL) == ESP_ERR_INVALID_ARG, "NULL");
}

static void test_unknown_type_skip(void)
{
    nc_state_rec_t recs[] = {
        {.key = 0x0101, .type = NC_STATE_T_UINT, .u = 11},
        {.key = 0x0102, .type = NC_STATE_T_UINT, .u = 22},
        {.key = 0x0103, .type = NC_STATE_T_UINT, .u = 33},
    };
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = 0};
    uint8_t buf[128];
    const size_t got = nc_state_blob_encode(buf, sizeof(buf), &meta, recs, 3);
    CHECK(got > NC_STATE_BLOB_HDR, "前置 encode 失敗");

    buf[NC_STATE_BLOB_HDR] = 0x7Fu;   // 第一個 record type 改做未知
    fix_crc(buf, got);
    collector_t c = {0};
    nc_state_info_t info;
    const esp_err_t e = nc_state_blob_decode(buf, got, collect, &c, &info);
    CHECK(e == ESP_OK, "未知 type 應該跳過而唔係報錯（%d）", (int)e);
    CHECK(info.n_skipped == 1, "skipped %u（預期 1）", info.n_skipped);
    CHECK(c.n == 2, "callback 收到 %zu 筆（預期 2）", c.n);
    if (c.n == 2) {
        CHECK(c.recs[0].key == 0x0102 && c.recs[0].u == 22, "第一筆應該係 0x0102");
        CHECK(c.recs[1].key == 0x0103 && c.recs[1].u == 33, "第二筆應該係 0x0103");
    }
}

static void test_bool_invalid(void)
{
    nc_state_rec_t rec = {.key = 0x0C01, .type = NC_STATE_T_BOOL, .u = 1};
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = 0};
    uint8_t buf[32];
    const size_t got = nc_state_blob_encode(buf, sizeof(buf), &meta, &rec, 1);
    CHECK(got > NC_STATE_BLOB_HDR, "前置 encode 失敗");
    buf[got - 1] = 2;   // bool 值只准 0/1
    fix_crc(buf, got);
    CHECK(nc_state_blob_decode(buf, got, NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE, "bool 值 2");
}

static void test_cap(void)
{
    nc_state_rec_t rec = {.key = 1, .type = NC_STATE_T_UINT, .u = 123456789};
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = 0};
    uint8_t buf[64];
    const size_t need = nc_state_blob_size(&rec, 1);
    CHECK(nc_state_blob_encode(buf, need - 1, &meta, &rec, 1) == 0, "cap 不足應該回 0");
    CHECK(nc_state_blob_encode(buf, need, &meta, &rec, 1) == need, "cap 剛好應該成功");
    CHECK(nc_state_blob_size(NULL, 1) == 0, "NULL recs 應該回 0");
}

int main(void)
{
    printf("[1] round-trip（uint/sint 邊界、bool、bytes）\n");
    test_roundtrip();
    printf("[2] 空 blob\n");
    test_empty();
    printf("[3] header／CRC／截斷\n");
    test_bad_header();
    printf("[4] 未知 type 跳過（向前兼容）\n");
    test_unknown_type_skip();
    printf("[5] bool 非法值\n");
    test_bool_invalid();
    printf("[6] encode cap／size\n");
    test_cap();
    if (g_fail == 0) {
        printf("stateBlob_test OK\n");
        return 0;
    }
    printf("stateBlob_test FAIL（%d 項）\n", g_fail);
    return 1;
}
