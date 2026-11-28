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

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

static const char *TAG = "px_layer";

#define SCAN_NAME_LEN 64
#define SCAN_PATH_LEN 256

static nc_px_map_tbl_t  s_maps;
static nc_px_mode_tbl_t s_modes;
static nc_px_show_t     s_show;

// map 的 group／sel 由檔案決定（可能 500 組 × 幾段），所以用一個 bump pool：
// 開機先數總數、一次過配，解析時只向前推。pool 是 BULK（讀一次、之後由 plan 編譯），
// 唔係熱路徑，所以放 PSRAM；HOT 的只有編譯後的 runs。
static nc_px_map_pool_t s_map_pool;
static nc_px_mode_entry_t *s_mode_pool;
static uint32_t            s_mode_pool_cap;

static void *map_pool_alloc_bytes(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif
    return malloc(n);
}

static void map_pool_free(void)
{
    free(s_map_pool.group);
    free(s_map_pool.sel);
    memset(&s_map_pool, 0, sizeof(s_map_pool));
}

static bool map_pool_alloc(uint32_t ngroups, uint32_t nsels)
{
    map_pool_free();
    if (ngroups == 0) {
        return true;
    }
    if (nsels == 0) {
        nsels = 1;                     // parse 要求非 NULL（0 段都用不到）
    }
    s_map_pool.group = (nc_px_group_t *)map_pool_alloc_bytes(
        (size_t)ngroups * sizeof(nc_px_group_t));
    s_map_pool.sel = (nc_px_group_sel_t *)map_pool_alloc_bytes(
        (size_t)nsels * sizeof(nc_px_group_sel_t));
    if (s_map_pool.group == NULL || s_map_pool.sel == NULL) {
        map_pool_free();
        return false;
    }
    s_map_pool.cap_group = ngroups;
    s_map_pool.cap_sel = nsels;
    return true;
}

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
// 掃目錄的 .json 檔名（動態；唔再固定 32 個）。回傳排序好的陣列，*out_n=數量。
static char (*scan_json_names(const char *dir, uint32_t *out_n))[SCAN_NAME_LEN]
{
    *out_n = 0;
    DIR *d = opendir(dir);
    if (d == NULL) {
        return NULL;
    }
    uint32_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *fn = e->d_name;
        if (fn[0] != '.' && strlen(fn) < SCAN_NAME_LEN && ends_with(fn, ".json")) {
            n++;
        }
    }
    rewinddir(d);
    char (*names)[SCAN_NAME_LEN] = NULL;
    if (n > 0) {
        names = malloc((size_t)n * sizeof(*names));
        if (names == NULL) {
            closedir(d);
            return NULL;
        }
        uint32_t k = 0;
        while ((e = readdir(d)) != NULL) {
            const char *fn = e->d_name;
            if (fn[0] == '.' || strlen(fn) >= SCAN_NAME_LEN || !ends_with(fn, ".json")) {
                continue;
            }
            memcpy(names[k++], fn, SCAN_NAME_LEN);
        }
        qsort(names, n, sizeof(*names), cmp_name);
        *out_n = n;
    }
    closedir(d);
    return names;
}


static int cmp_mode_index(const void *a, const void *b)
{
    const nc_px_mode_t *x = (const nc_px_mode_t *)a;
    const nc_px_mode_t *y = (const nc_px_mode_t *)b;
    const int ix = x->id;                  // index 未存：用 id 排序（同 MP 預設）
    const int iy = y->id;
    return (ix < iy) ? -1 : (ix > iy) ? 1 : 0;
}

// 讀檔入 malloc 緩衝：先問檔案大小，再配剛好（唔再固定 8 KB）。
// 上限 SCAN_FILE_MAX 只係擋壞檔；16×600 的 map／mode 可以幾十 KB。
static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long end = ftell(f);
    if (end <= 0) {
        fclose(f);
        return NULL;
    }
    // 唔再固定 256 KB：由檔案實際大小決定；唯一失敗原因係 malloc 冇記憶體。
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)end);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)end, f);
    fclose(f);
    *len = got;
    return buf;
}

void nc_px_layer_clear(void)
{
    // ★ 效果表都要清：唔清就係「reload 之後仍然用舊效果」——
    //   nc_px_asset_scan() 會把同名效果當重複丟掉（log「撞名：…（保留第一個）」），
    //   結果改咗 effects.json 之後要 reboot 才生效，刪走的效果亦仍然留在 RAM。
    nc_px_asset_clear();
    free(s_maps.item);
    free(s_modes.item);
    memset(&s_maps, 0, sizeof(s_maps));
    memset(&s_modes, 0, sizeof(s_modes));
    nc_px_show_free(&s_show);
    memset(&s_show, 0, sizeof(s_show));
    map_pool_free();
    free(s_mode_pool);
    s_mode_pool = NULL;
    s_mode_pool_cap = 0;
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
    closedir(d);
    uint32_t n = 0;
    char (*names)[SCAN_NAME_LEN] = scan_json_names(dir, &n);
    if (n == 0) {
        free(names);
        return ESP_OK;
    }

    // 表由檔案數決定；上限只剩 RAM。
    free(s_maps.item);
    s_maps.item = (nc_px_map_t *)map_pool_alloc_bytes((size_t)n * sizeof(*s_maps.item));
    s_maps.cap = (s_maps.item != NULL) ? n : 0;
    s_maps.n = 0;
    s_maps.rejected = 0;
    if (s_maps.item == NULL) {
        ESP_LOGW(TAG, "map 表配置失敗（%u 檔）", (unsigned)n);
        free(names);
        return ESP_ERR_NO_MEM;
    }

    // ── pass 1：數容量（group／sel 由檔案決定，唔用固定陣列）──
    uint32_t need_group = 0;
    uint32_t need_sel = 0;
    for (uint32_t i = 0; i < n; i++) {
        char path[SCAN_PATH_LEN];
        const int w = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (w < 0 || (size_t)w >= sizeof(path)) {
            continue;                       // pass 2 會記 rejected
        }
        size_t len = 0;
        char *txt = read_all(path, &len);
        if (txt == NULL) {
            continue;
        }
        uint32_t g = 0;
        uint32_t sl = 0;
        if (nc_px_map_count(txt, len, &g, &sl) == ESP_OK) {
            need_group += g;
            need_sel += sl;
        }
        free(txt);
    }
    if (need_group > 0 && !map_pool_alloc(need_group, need_sel)) {
        ESP_LOGW(TAG, "map pool 配置失敗（groups=%u sels=%u）→ 冇 mapping",
                 (unsigned)need_group, (unsigned)need_sel);
        s_maps.rejected++;
        return ESP_ERR_NO_MEM;
    }

    // ── pass 2：解析＋登記 ──
    for (uint32_t i = 0; i < n; i++) {
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
        // 解析結果 m 只係小 header（groups／sels 指入 s_map_pool），放棧安全。
        nc_px_map_t m;
        const esp_err_t r = nc_px_map_parse(txt, len, &s_map_pool, &m);
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
        for (uint32_t k = 0; k < s_maps.n; k++) {
            if (s_maps.item[k].id == m.id || strcmp(s_maps.item[k].name, m.name) == 0) {
                dup = true;
            }
        }
        if (dup) {
            ESP_LOGW(TAG, "mapping 重複（id=%u name=%s）— 跳過", (unsigned)m.id, m.name);
            s_maps.rejected++;
            continue;
        }
        if (s_maps.n >= s_maps.cap) {
            s_maps.rejected++;
            continue;
        }
        s_maps.item[s_maps.n++] = m;
        ESP_LOGI(TAG, "mapping %u（%s）：%u group(s)", (unsigned)m.id, m.name,
                 (unsigned)m.ngroups);
    }
    free(names);
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
    closedir(d);
    uint32_t n = 0;
    char (*names)[SCAN_NAME_LEN] = scan_json_names(dir, &n);
    if (n == 0) {
        free(names);
        return ESP_OK;
    }

    // 表由檔案數決定；上限只剩 RAM。
    free(s_modes.item);
    s_modes.item = (nc_px_mode_t *)map_pool_alloc_bytes((size_t)n * sizeof(*s_modes.item));
    s_modes.cap = (s_modes.item != NULL) ? n : 0;
    s_modes.n = 0;
    s_modes.rejected = 0;
    if (s_modes.item == NULL) {
        ESP_LOGW(TAG, "mode 表配置失敗（%u 檔）", (unsigned)n);
        free(names);
        return ESP_ERR_NO_MEM;
    }

    // ── pass 1：數 mode 條目（容量由檔案決定）──
    uint32_t need = 0;
    for (uint32_t i = 0; i < n; i++) {
        char path[SCAN_PATH_LEN];
        const int w = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (w < 0 || (size_t)w >= SCAN_PATH_LEN) {
            continue;
        }
        size_t len = 0;
        char *txt = read_all(path, &len);
        if (txt == NULL) {
            continue;
        }
        uint32_t ne = 0;
        if (nc_px_mode_count(txt, len, &ne) == ESP_OK) {
            need += ne;
        }
        free(txt);
    }
    if (need > s_mode_pool_cap) {
        free(s_mode_pool);
        s_mode_pool = (nc_px_mode_entry_t *)map_pool_alloc_bytes(
            (size_t)need * sizeof(nc_px_mode_entry_t));
        if (s_mode_pool == NULL) {
            ESP_LOGW(TAG, "mode pool 配置失敗（%u 條目）", (unsigned)need);
            s_modes.rejected++;
            return ESP_ERR_NO_MEM;
        }
        s_mode_pool_cap = need;
    }
    uint32_t used = 0;

    // ── pass 2：解析＋登記（每個 mode 用 pool 的一段）──
    for (uint32_t i = 0; i < n; i++) {
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
        nc_px_mode_t m;
        const esp_err_t r = nc_px_mode_parse(txt, len, s_mode_pool + used,
                                             s_mode_pool_cap - used, &m);
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
        for (uint32_t k = 0; k < s_modes.n; k++) {
            if (s_modes.item[k].id == m.id || strcmp(s_modes.item[k].name, m.name) == 0) {
                dup = true;
            }
        }
        if (dup) {
            ESP_LOGW(TAG, "mode 重複（id=%u name=%s）— 跳過", (unsigned)m.id, m.name);
            s_modes.rejected++;
            continue;
        }
        if (s_modes.n >= s_modes.cap) {
            s_modes.rejected++;
            continue;
        }
        s_modes.item[s_modes.n++] = m;
        used += m.nentry;
        ESP_LOGI(TAG, "mode %u（%s）：%u 項", (unsigned)m.id, m.name, (unsigned)m.nentry);
    }
    free(names);
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

static bool ref_matches(const char *ref, const char *name, uint16_t id)
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
    return (uint16_t)atoi(ref) == id;
}

const nc_px_map_t *nc_px_map_find(const char *ref)
{
    for (uint32_t i = 0; i < s_maps.n; i++) {
        if (ref_matches(ref, s_maps.item[i].name, s_maps.item[i].id)) {
            return &s_maps.item[i];
        }
    }
    return NULL;
}

const nc_px_mode_t *nc_px_mode_find(const char *ref)
{
    for (uint32_t i = 0; i < s_modes.n; i++) {
        if (ref_matches(ref, s_modes.item[i].name, s_modes.item[i].id)) {
            return &s_modes.item[i];
        }
    }
    return NULL;
}
