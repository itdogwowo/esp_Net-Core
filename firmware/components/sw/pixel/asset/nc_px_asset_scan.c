// nc_px_asset_scan.c — 檔案系統外殼：掃一個目錄，逐個檔交去 nc_px_asset.c。
//
// 解析邏輯全部在 nc_px_asset.c（PC 上可測）；呢一支只做 I/O：
//   · 掃描順序：`effects.json` → `*.json`（檔名排序）→ `*.nce`
//   · `*.json` 冇寫 name → 用檔名（去副檔名）
//   · `*.nce` 只登記一個 kind=NCE 的資產（manifest 由 ncm 自己解析；唔重複讀檔）
//   · 壞檔／讀唔到／過大：跳過 + log（計入 rejected），唔會令整張表失效
//
// 呼叫端（pixel_task）自己決定根路徑：APP 模式 = `nc_store_fs_base()` ＋ `/pixel/effects`。
#include "nc_px_asset.h"
#include "nc_px_utf8.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "px_asset";

#define SCAN_FILE_MAX   16384        // 單一 json 最大（effects.json 現時 ~4 KB）
#define SCAN_ENTRIES    48           // 一個目錄最多收幾個檔
#define SCAN_NAME_LEN   32           // 檔名上限（FAT 8.3 或長檔名都夠）
#define SCAN_PATH_LEN   192



static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);
    return n >= m && strcmp(s + (n - m), suffix) == 0;
}

// "breathing.json" → "breathing"
static void file_stem(const char *name, char *out, size_t cap)
{
    size_t i = 0;
    for (; name[i] != '\0' && name[i] != '.' && i + 1 < cap; i++) {
        out[i] = name[i];
    }
    i = nc_px_utf8_trim(out, i);
    out[i] = '\0';
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

// 讀一個檔 → nc_px_asset_add_json()。hint = 檔名（可選）。
static void load_file(const char *dir, const char *fn, const char *hint, esp_err_t *result)
{
    char path[SCAN_PATH_LEN];
    const int n = snprintf(path, sizeof(path), "%s/%s", dir, fn);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        ESP_LOGW(TAG, "路徑太長：%s/%s", dir, fn);
        nc_px_asset_note_reject();
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "%s：開唔到", path);
        nc_px_asset_note_reject();
        return;
    }
    // 暫存緩衝：掃描係開機／換場才做，用完即刻還（唔佔住 16 KB BSS）
    char *buf = (char *)malloc(SCAN_FILE_MAX);
    if (buf == NULL) {
        fclose(f);
        nc_px_asset_note_reject();
        return;
    }
    const size_t got = fread(buf, 1, SCAN_FILE_MAX, f);
    const bool too_big = (got == SCAN_FILE_MAX) && (fgetc(f) != EOF);
    fclose(f);
    if (too_big) {
        ESP_LOGW(TAG, "%s：大過 %d B，跳過", path, SCAN_FILE_MAX);
        free(buf);
        nc_px_asset_note_reject();
        return;
    }

    const esp_err_t e = nc_px_asset_add_json(buf, got, hint);
    free(buf);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   // 撞名已經有自己的 log
        ESP_LOGW(TAG, "%s：解析失敗（%s）", path, esp_err_to_name(e));
    }
    if (e == ESP_OK) {
        *result = ESP_OK;
    }
}

// *.nce：唔讀內容，只登記名字（manifest 由 ncm 解析；到時 name 可以由 manifest 覆寫）
static void add_nce(const char *fn)
{
    nc_px_asset_t a;
    memset(&a, 0, sizeof(a));
    a.kind = NC_PX_ASSET_NCE;
    file_stem(fn, a.name, sizeof(a.name));
    (void)nc_px_asset_add(&a);
}

esp_err_t nc_px_asset_scan(const char *dir)
{
    if (dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "掃唔到 %s（當作冇效果）", dir);
        return ESP_ERR_NOT_FOUND;
    }

    char batch[SCAN_NAME_LEN] = { 0 };
    static char jsons[SCAN_ENTRIES][SCAN_NAME_LEN];
    static char nces[SCAN_ENTRIES][SCAN_NAME_LEN];
    int nj = 0, nn = 0;

    struct dirent *e = NULL;
    while ((e = readdir(d)) != NULL) {
        const char *fn = e->d_name;
        if (fn[0] == '.' || strlen(fn) >= SCAN_NAME_LEN) {
            continue;                                   // "."／".."／太長
        }
        if (strcmp(fn, "effects.json") == 0) {
            memcpy(batch, fn, sizeof(batch));
        } else if (ends_with(fn, ".json")) {
            if (nj < SCAN_ENTRIES) {
                memcpy(jsons[nj++], fn, sizeof(jsons[0]));
            }
        } else if (ends_with(fn, ".nce")) {
            if (nn < SCAN_ENTRIES) {
                memcpy(nces[nn++], fn, sizeof(nces[0]));
            }
        }
        // 其餘（.md／.txt／相簿）一律唔理
    }
    closedir(d);

    qsort(jsons, (size_t)nj, sizeof(jsons[0]), cmp_name);
    qsort(nces, (size_t)nn, sizeof(nces[0]), cmp_name);

    esp_err_t result = ESP_ERR_NOT_FOUND;

    if (batch[0] != '\0') {                             // 先食大檔（快速改波用）
        load_file(dir, batch, NULL, &result);
    }
    for (int i = 0; i < nj; i++) {                      // 再食一檔一效果
        char hint[SCAN_NAME_LEN];
        file_stem(jsons[i], hint, sizeof(hint));
        load_file(dir, jsons[i], hint, &result);
    }
    for (int i = 0; i < nn; i++) {                      // 最後登記 .nce
        add_nce(nces[i]);
        result = ESP_OK;
    }
    return result;
}
