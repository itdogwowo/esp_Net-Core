// NCSTATE 檔案層實作：stdio（ESP-IDF VFS FAT 同 host 都用得）。
#include "nc_state_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "state";

#define NC_STATE_FILE_MAX (64u * 1024u)   // 狀態檔上限（實際 ~3 KB 以內）

// 鏈式 CRC：crc 傳「未做最終 XOR」的狀態（第一次用 0xFFFFFFFF，最後 ~）。
static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

static const char *basename_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return (s != NULL) ? s + 1 : path;
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > NC_STATE_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

esp_err_t nc_state_file_hash_files(const char *const *paths, size_t npaths, uint32_t *out)
{
    if (out == NULL || (npaths > 0 && paths == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t h = 0xFFFFFFFFu;
    for (size_t i = 0; i < npaths; i++) {
        if (paths[i] == NULL) {
            continue;
        }
        size_t len = 0;
        uint8_t *buf = read_file(paths[i], &len);
        if (buf == NULL) {
            continue;   // 缺檔唔算錯（首次開機／未 seed）
        }
        const char *name = basename_of(paths[i]);
        h = crc32_update(h, name, strlen(name));
        const uint8_t sep = 0;   // 分隔（name NUL）
        h = crc32_update(h, &sep, 1);
        h = crc32_update(h, buf, len);
        free(buf);
    }
    *out = ~h;
    return ESP_OK;
}

size_t nc_state_file_size(const char *path)
{
    if (path == NULL) {
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t sz = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0) {
            sz = (size_t)n;
        }
    }
    fclose(f);
    return sz;
}

esp_err_t nc_state_file_load(const char *path, nc_state_rec_cb_t cb, void *ud,
                             nc_state_info_t *info)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (buf == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t e = nc_state_blob_decode(buf, len, cb, ud, info);
    free(buf);
    return e;
}

esp_err_t nc_state_file_peek(const char *path, nc_state_info_t *info)
{
    return nc_state_file_load(path, NULL, NULL, info);
}

esp_err_t nc_state_file_save(const char *path, const nc_state_meta_t *meta,
                             const nc_state_rec_t *recs, size_t nrec)
{
    if (path == NULL || meta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t need = nc_state_blob_size(recs, nrec);
    if (need == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *buf = (uint8_t *)malloc(need);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const size_t n = nc_state_blob_encode(buf, need, meta, recs, nrec);
    if (n != need) {
        free(buf);
        return ESP_FAIL;
    }

    char tmp[160];
    char bak[160];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    (void)snprintf(bak, sizeof(bak), "%s.bak", path);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        free(buf);
        ESP_LOGW(TAG, "NCSTATE 寫入開檔失敗：%s", tmp);
        return ESP_FAIL;
    }
    const size_t w = fwrite(buf, 1, n, f);
    const int flushed = fflush(f);
    const int closed = fclose(f);
    free(buf);
    if (w != n || flushed != 0 || closed != 0) {
        (void)remove(tmp);
        return ESP_FAIL;
    }
    // 舊檔 → .bak（last-known-good）；冇舊檔都唔算錯
    (void)remove(bak);
    (void)rename(path, bak);
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        ESP_LOGW(TAG, "NCSTATE rename 失敗：%s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}
