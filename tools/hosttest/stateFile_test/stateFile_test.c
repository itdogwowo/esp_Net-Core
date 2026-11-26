// stateFile_test — NCSTATE 檔案層 host test（hash／存讀／.bak／壞檔／缺檔）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nc_state_file.h"

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

static void write_file(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        (void)fwrite(s, 1, strlen(s), f);
        (void)fclose(f);
    }
}

int main(void)
{
    char dir[] = "/tmp/ncstate_test_XXXXXX";
    if (mkdtemp(dir) == NULL) {
        printf("stateFile_test FAIL：mkdtemp\n");
        return 1;
    }
    char pa[160], pb[160], pst[180], pst_bak[200];
    (void)snprintf(pa, sizeof(pa), "%s/a.json", dir);
    (void)snprintf(pb, sizeof(pb), "%s/b.json", dir);
    (void)snprintf(pst, sizeof(pst), "%s/state.NCSTATE", dir);
    (void)snprintf(pst_bak, sizeof(pst_bak), "%s.bak", pst);

    printf("[1] hash_files：缺檔跳過、改名／改內容會變\n");
    write_file(pa, "{\"a\":1}");
    const char *paths1[] = {pa, pb};
    uint32_t h1 = 0, h2 = 0;
    CHECK(nc_state_file_hash_files(paths1, 2, &h1) == ESP_OK, "hash 失敗");
    CHECK(h1 != 0, "hash 應該非 0");
    write_file(pb, "{\"b\":2}");
    CHECK(nc_state_file_hash_files(paths1, 2, &h2) == ESP_OK, "hash 失敗");
    CHECK(h1 != h2, "加檔內容後 hash 應該變");
    const char *paths2[] = {pb, pa};
    uint32_t h3 = 0;
    CHECK(nc_state_file_hash_files(paths2, 2, &h3) == ESP_OK, "hash 失敗");
    CHECK(h2 != h3, "次序唔同 hash 應該唔同");

    printf("[2] save → peek／load\n");
    const char *fmt = "ws2812";
    nc_state_rec_t recs[] = {
        {.key = 0x0002, .type = NC_STATE_T_UINT, .u = 20},
        {.key = 0x0702, .type = NC_STATE_T_SINT, .i = -1},
        {.key = 0x0C01, .type = NC_STATE_T_BOOL, .u = 1},
        {.key = 0x0C06, .type = NC_STATE_T_BYTES, .bytes = fmt, .bytes_len = 6},
    };
    const nc_state_meta_t meta = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = h3};
    CHECK(nc_state_file_save(pst, &meta, recs, 4) == ESP_OK, "save 失敗");
    nc_state_info_t info;
    CHECK(nc_state_file_peek(pst, &info) == ESP_OK, "peek 失敗");
    CHECK(info.src_hash == h3, "src_hash %08x != %08x", info.src_hash, h3);
    CHECK(info.n_records == 4, "records %u", info.n_records);
    CHECK(nc_state_file_size(pst) > NC_STATE_BLOB_HDR, "size 太細");
    CHECK(nc_state_file_peek(pst_bak, NULL) == ESP_ERR_NOT_FOUND, "第一次 save 唔應該有 .bak");

    printf("[3] 再 save：舊檔變 .bak（last-known-good）\n");
    const nc_state_meta_t meta2 = {.ver = NC_STATE_BLOB_VER, .flags = 0, .src_hash = h3 + 1};
    CHECK(nc_state_file_save(pst, &meta2, recs, 4) == ESP_OK, "第二次 save 失敗");
    nc_state_info_t bi;
    CHECK(nc_state_file_peek(pst_bak, &bi) == ESP_OK, "應該有 .bak");
    CHECK(bi.src_hash == h3, ".bak 應該係舊 hash %08x（實際 %08x）", h3, bi.src_hash);

    printf("[4] 壞檔／截斷／缺檔\n");
    write_file(pst, "garbage-not-ncstate");
    CHECK(nc_state_file_peek(pst, NULL) != ESP_OK, "壞 magic 應該失敗");
    const size_t glen = nc_state_file_size(pst);
    CHECK(glen == strlen("garbage-not-ncstate"), "size 應該係垃圾長度（%zu）", glen);
    CHECK(truncate(pst, (off_t)(NC_STATE_BLOB_HDR - 1)) == 0, "truncate 失敗");
    CHECK(nc_state_file_peek(pst, NULL) == ESP_ERR_INVALID_SIZE, "截斷應該 INVALID_SIZE");
    char missing[180];
    (void)snprintf(missing, sizeof(missing), "%s/nope.NCSTATE", dir);
    CHECK(nc_state_file_peek(missing, NULL) == ESP_ERR_NOT_FOUND, "缺檔應該 NOT_FOUND");
    CHECK(nc_state_file_size(missing) == 0, "缺檔 size 應該 0");

    printf("[5] 無殘留 .tmp\n");
    char ptmp[200];
    (void)snprintf(ptmp, sizeof(ptmp), "%s.tmp", pst);
    CHECK(access(ptmp, F_OK) != 0, ".tmp 應該清走");

    // 清理
    (void)remove(pa);
    (void)remove(pb);
    (void)remove(pst);
    (void)remove(pst_bak);
    (void)rmdir(dir);

    if (g_fail == 0) {
        printf("stateFile_test OK\n");
        return 0;
    }
    printf("stateFile_test FAIL（%d 項）\n", g_fail);
    return 1;
}
