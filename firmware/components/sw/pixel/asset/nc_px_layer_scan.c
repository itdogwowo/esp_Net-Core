// nc_px_layer_scan.c — map／modes 目錄掃描、登記、排序；registry.json 讀取。
//
// 同 MP 的行為對齊：
//   · 檔名排序（qsort）→ 結果確定，唔靠檔案系統次序
//   · map：id／name 重複 → 跳過（保留第一個）
//   · mode：按 index 排序（冇 index 用 id）；id／name 重複 → 跳過
//   · 壞檔／讀唔到／過大 → rejected++，唔會令整張表失效
#include "nc_px_layer_scan.h"

#include "nc_px_asset.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "px_layer";

#define SCAN_FILE_MAX 8192
#define SCAN_ENTRIES  32
#define SCAN_NAME_LEN 32
#define SCAN_PATH_LEN 192

static nc_px_map_tbl_t  s_maps;
static nc_px_mode_tbl_t s_modes;
static nc_px_show_t     s_show;

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);
    return n >= m && strcmp(s + (n - m), suffix) == 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int cmp_mode_index(const void *a, const void *b)
{
    const nc_px_mode_t *x = (const nc_px_mode_t *)a;
    const nc_px_mode_t *y = (const nc_px_mode_t *)b;
    const int ix = x->id;                  // index 未存：用 id 排序（同 MP 預設）
    const int iy = y->id;
    return (ix < iy) ? -1 : (ix > iy) ? 1 : 0;
}

// 讀檔入 malloc 緩衝；超過 SCAN_FILE_MAX 回 NULL
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    char *buf = (char *)malloc(SCAN_FILE_MAX);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, SCAN_FILE_MAX, f);
    const bool too_big = (got == SCAN_FILE_MAX) && (fgetc(f) != EOF);
    fclose(f);
    if (too_big) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

void nc_px_layer_clear(void)
{
    memset(&s_maps, 0, sizeof(s_maps));
    memset(&s_modes, 0, sizeof(s_modes));
    memset(&s_show, 0, sizeof(s_show));
}

esp_err_t nc_px_map_scan(const char *dir)
{
    if (dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "掃唔到 %s（當作冇 mapping）", dir);
        return ESP_ERR_NOT_FOUND;
    }
    static char names[SCAN_ENTRIES][SCAN_NAME_LEN];
    int n = 0;
    struct dirent *e = NULL;
    while ((e = readdir(d)) != NULL) {
        const char *fn = e->d_name;
        if (fn[0] == '.' || strlen(fn) >= SCAN_NAME_LEN) {
            continue;
        }
        if (ends_with(fn, ".json") && n < SCAN_ENTRIES) {
            memcpy(names[n++], fn, SCAN_NAME_LEN);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_name);

    for (int i = 0; i < n; i++) {
        char path[SCAN_PATH_LEN];
        const int w = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (w < 0 || (size_t)w >= sizeof(path)) {
            s_maps.rejected++;
            continue;
        }
        size_t len = 0;
        char *txt = read_all(path, &len);
        if (txt == NULL) {
            ESP_LOGW(TAG, "%s：讀唔到／過大", path);
            s_maps.rejected++;
            continue;
        }
        // map 結構 2.6 KB、mode 0.6 KB；on_start 由 main task（3–8 KB 棧）呼叫，
        // 放棧會爆。掃描只在開機／換場做，單執行緒 → static 安全。
        static nc_px_map_t m;
        const esp_err_t r = nc_px_map_parse(txt, len, &m);
        free(txt);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "%s：解析失敗（%s）", path, esp_err_to_name(r));
            s_maps.rejected++;
            continue;
        }
        if (m.ngroups == 0 || m.name[0] == '\0') {
            ESP_LOGW(TAG, "%s：冇 groups／name，跳過", path);
            s_maps.rejected++;
            continue;
        }
        bool dup = false;
        for (uint8_t k = 0; k < s_maps.n; k++) {
            if (s_maps.item[k].id == m.id || strcmp(s_maps.item[k].name, m.name) == 0) {
                dup = true;
            }
        }
        if (dup) {
            ESP_LOGW(TAG, "mapping 重複（id=%u name=%s）— 跳過", (unsigned)m.id, m.name);
            s_maps.rejected++;
            continue;
        }
        if (s_maps.n >= NC_PX_MAP_MAX) {
            s_maps.rejected++;
            continue;
        }
        s_maps.item[s_maps.n++] = m;
        ESP_LOGI(TAG, "mapping %u（%s）：%u group(s)", (unsigned)m.id, m.name,
                 (unsigned)m.ngroups);
    }
    return ESP_OK;
}

esp_err_t nc_px_mode_scan(const char *dir)
{
    if (dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "掃唔到 %s（當作冇 mode）", dir);
        return ESP_ERR_NOT_FOUND;
    }
    static char names[SCAN_ENTRIES][SCAN_NAME_LEN];
    int n = 0;
    struct dirent *e = NULL;
    while ((e = readdir(d)) != NULL) {
        const char *fn = e->d_name;
        if (fn[0] == '.' || strlen(fn) >= SCAN_NAME_LEN) {
            continue;
        }
        if (ends_with(fn, ".json") && n < SCAN_ENTRIES) {
            memcpy(names[n++], fn, SCAN_NAME_LEN);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_name);

    for (int i = 0; i < n; i++) {
        char path[SCAN_PATH_LEN];
        const int w = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (w < 0 || (size_t)w >= SCAN_PATH_LEN) {
            s_modes.rejected++;
            continue;
        }
        size_t len = 0;
        char *txt = read_all(path, &len);
        if (txt == NULL) {
            ESP_LOGW(TAG, "%s：讀唔到／過大", path);
            s_modes.rejected++;
            continue;
        }
        static nc_px_mode_t m;
        const esp_err_t r = nc_px_mode_parse(txt, len, &m);
        free(txt);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "%s：解析失敗（%s）", path, esp_err_to_name(r));
            s_modes.rejected++;
            continue;
        }
        if (m.nentry == 0 || m.name[0] == '\0') {
            ESP_LOGW(TAG, "%s：冇有效委派項／name，跳過", path);
            s_modes.rejected++;
            continue;
        }
        bool dup = false;
        for (uint8_t k = 0; k < s_modes.n; k++) {
            if (s_modes.item[k].id == m.id || strcmp(s_modes.item[k].name, m.name) == 0) {
                dup = true;
            }
        }
        if (dup) {
            ESP_LOGW(TAG, "mode 重複（id=%u name=%s）— 跳過", (unsigned)m.id, m.name);
            s_modes.rejected++;
            continue;
        }
        if (s_modes.n >= NC_PX_MODE_MAX) {
            s_modes.rejected++;
            continue;
        }
        s_modes.item[s_modes.n++] = m;
        ESP_LOGI(TAG, "mode %u（%s）：%u 項", (unsigned)m.id, m.name, (unsigned)m.nentry);
    }
    qsort(s_modes.item, s_modes.n, sizeof(s_modes.item[0]), cmp_mode_index);
    return ESP_OK;
}

esp_err_t nc_px_show_load(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = 0;
    char *txt = read_all(path, &len);
    if (txt == NULL) {
        ESP_LOGW(TAG, "%s：讀唔到／過大（當作冇播放清單）", path);
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t r = nc_px_show_parse(txt, len, &s_show);
    free(txt);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "%s：解析失敗（%s）", path, esp_err_to_name(r));
        return r;
    }
    ESP_LOGI(TAG, "registry：auto_play=%d list=%u", s_show.auto_play ? 1 : 0,
             (unsigned)s_show.nlist);
    return ESP_OK;
}

esp_err_t nc_px_layer_load_all(const char *base)
{
    if (base == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nc_px_layer_clear();
    char path[SCAN_PATH_LEN];

    (void)snprintf(path, sizeof(path), "%s/effects", base);
    const esp_err_t ee = nc_px_asset_scan(path);
    if (ee != ESP_OK) {
        ESP_LOGW(TAG, "effects 掃唔到：%s", path);
    }
    (void)snprintf(path, sizeof(path), "%s/map", base);
    if (nc_px_map_scan(path) != ESP_OK) {
        ESP_LOGW(TAG, "map 掃唔到：%s", path);
    }
    (void)snprintf(path, sizeof(path), "%s/modes", base);
    if (nc_px_mode_scan(path) != ESP_OK) {
        ESP_LOGW(TAG, "modes 掃唔到：%s", path);
    }
    (void)snprintf(path, sizeof(path), "%s/registry.json", base);
    if (nc_px_show_load(path) != ESP_OK) {
        ESP_LOGW(TAG, "registry 讀唔到：%s", path);
    }
    ESP_LOGI(TAG, "pixel 樹：effects=%d maps=%u modes=%u list=%u",
             nc_px_asset_count(), (unsigned)s_maps.n, (unsigned)s_modes.n,
             (unsigned)s_show.nlist);
    return (nc_px_asset_count() > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const nc_px_map_tbl_t  *nc_px_map_table(void) { return &s_maps; }
const nc_px_mode_tbl_t *nc_px_mode_table(void) { return &s_modes; }
const nc_px_show_t     *nc_px_show(void) { return &s_show; }

static bool ref_matches(const char *ref, const char *name, uint8_t id)
{
    if (ref == NULL) {
        return false;
    }
    if (strcmp(ref, name) == 0) {
        return true;
    }
    for (const char *p = ref; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return (uint8_t)atoi(ref) == id;
}

const nc_px_map_t *nc_px_map_find(const char *ref)
{
    for (uint8_t i = 0; i < s_maps.n; i++) {
        if (ref_matches(ref, s_maps.item[i].name, s_maps.item[i].id)) {
            return &s_maps.item[i];
        }
    }
    return NULL;
}

const nc_px_mode_t *nc_px_mode_find(const char *ref)
{
    for (uint8_t i = 0; i < s_modes.n; i++) {
        if (ref_matches(ref, s_modes.item[i].name, s_modes.item[i].id)) {
            return &s_modes.item[i];
        }
    }
    return NULL;
}
