// nc_px_asset.c — DATA 平面：JSON 文字 → `nc_px_asset_t`，同資產表本體。
//
// 刻意**唔碰檔案系統**（掃目錄／讀檔住 `nc_px_asset_scan.c`）：解析係最容易錯的一段，
// 唔想只靠上板撞。呢一支可以原封不動在 PC 上編、用真的 `effects.json` 測。
//
// 解析規則（同 sys/config 的習慣一致，唔逐個欄位自訂）：
//   缺欄位         → 預設（0）
//   型別錯／超範圍  → **整個資產拒絕**（寧願唔載入，好過畫錯）
//   唔認識的鍵     → 跳過（向前兼容：MP 的 step／spacing／speed／reverse 暫時唔用）
//   唔認識的段型別  → 整個資產拒絕
//   名撞           → 保留第一個（dup++）
//   超過上限        → 拒絕（表滿）
// JSON 只喺呢度讀一次，之後全 project（引擎／任務）只行 struct。
#include "nc_px_asset.h"
#include "nc_px_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

static const char *TAG = "px_asset";

// 表本體放邊：板上有 PSRAM 就 PSRAM（效果表係冷資料，熱路徑唔會逐幀讀），
// 冇就 internal；host 就係普通 malloc。
static void *aalloc(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif
    return malloc(n);
}

#define ASSET_CHUNK 8u          // 第一次配置幾個（之後倍增）

nc_px_asset_tbl_t *nc_px_asset_tbl_alloc(uint16_t cap)
{
    nc_px_asset_tbl_t *t = (nc_px_asset_tbl_t *)aalloc(sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    if (cap != 0 && nc_px_asset_tbl_reserve(t, cap) != ESP_OK) {
        free(t);
        return NULL;
    }
    return t;
}

void nc_px_asset_tbl_free(nc_px_asset_tbl_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->owned && t->item != NULL) {
        free(t->item);
    }
    t->item = NULL;
    t->n = 0;
    t->cap = 0;
    t->owned = 0;
    free(t);
}

void nc_px_asset_tbl_use(nc_px_asset_tbl_t *t, nc_px_asset_t *buf, uint16_t cap)
{
    if (t == NULL) {
        return;
    }
    t->n = 0;
    t->cap = cap;
    t->owned = 0;
    t->item = buf;
}

esp_err_t nc_px_asset_tbl_reserve(nc_px_asset_tbl_t *t, uint16_t need)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->cap >= need) {
        return ESP_OK;
    }
    uint32_t cap = (t->cap != 0) ? t->cap : ASSET_CHUNK;
    while (cap < need) {
        cap *= 2u;
    }
    if (cap > 0xFFFFu) {
        cap = 0xFFFFu;
    }
    nc_px_asset_t *it = (nc_px_asset_t *)aalloc((size_t)cap * sizeof(*it));
    if (it == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (t->item != NULL && t->n > 0) {
        memcpy(it, t->item, (size_t)t->n * sizeof(*it));
    }
    if (t->owned && t->item != NULL) {
        free(t->item);
    }
    t->item = it;
    t->cap = (uint16_t)cap;
    t->owned = 1;
    return ESP_OK;
}

static nc_px_asset_tbl_t  s_tbl;
static nc_px_asset_stat_t s_stat;

// ═══════════════════════ 表 ═══════════════════════
void nc_px_asset_clear(void)
{
    s_tbl.n = 0;          // 保留已配置的緩衝：重掃唔使再配一次
    memset(&s_stat, 0, sizeof(s_stat));
}

int nc_px_asset_count(void)
{
    return (int)s_tbl.n;
}

const nc_px_asset_t *nc_px_asset_at(int i)
{
    if (i < 0 || i >= (int)s_tbl.n) {
        return NULL;
    }
    return &s_tbl.item[i];
}

const nc_px_asset_t *nc_px_asset_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < s_tbl.n; i++) {
        if (strcmp(s_tbl.item[i].name, name) == 0) {
            return &s_tbl.item[i];
        }
    }
    return NULL;
}

void nc_px_asset_stats(nc_px_asset_stat_t *out)
{
    if (out != NULL) {
        *out = s_stat;
    }
}

static esp_err_t tbl_add(const nc_px_asset_t *a)
{
    if (a->name[0] == '\0') {
        s_stat.rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_px_asset_find(a->name) != NULL) {
        s_stat.dup++;
        ESP_LOGW(TAG, "撞名：%s（保留第一個）", a->name);
        return ESP_ERR_INVALID_STATE;
    }
    // 冇固定上限：要幾多配幾多（倍增）。配唔到（真係冇記憶體）才拒。
    // 65,535 係 16-bit 欄位的硬邊界（唔係政策上限）：到頂就明示拒絕，唔好靜靜 wrap。
    if (s_tbl.n == 0xFFFFu ||
        nc_px_asset_tbl_reserve(&s_tbl, (uint16_t)(s_tbl.n + 1u)) != ESP_OK) {
        s_stat.rejected++;
        ESP_LOGW(TAG, "無記憶體／表滿（已有 %u 個）：%s 唔載入", (unsigned)s_tbl.n, a->name);
        return ESP_ERR_NO_MEM;
    }
    s_tbl.item[s_tbl.n++] = *a;
    s_stat.loaded++;
    return ESP_OK;
}

esp_err_t nc_px_asset_add(const nc_px_asset_t *a)
{
    if (a == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return tbl_add(a);
}

void nc_px_asset_note_reject(void)
{
    s_stat.rejected++;
}

// ═══════════════════════ 欄位 ═══════════════════════
static bool seg_type_of(const char *name, uint8_t *out)
{
    static const struct { const char *name; uint8_t v; } k[] = {
        { "keep",            NC_PX_SEG_KEEP },
        { "math_now",        NC_PX_SEG_MATH_NOW },
        { "square_wave_now", NC_PX_SEG_SQUARE_WAVE_NOW },
        { "pulse_wave",      NC_PX_SEG_PULSE_WAVE },
        { "pulse",           NC_PX_SEG_PULSE },
        { "starter",         NC_PX_SEG_STARTER },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(name, k[i].name) == 0) {
            *out = k[i].v;
            return true;
        }
    }
    return false;
}

// seg 的數值欄位：範圍錯 → ok=false（但 JSON 已經食完，可以繼續）
static void seg_num(nc_px_js_t *j, uint32_t limit, uint16_t *out, bool *ok)
{
    uint32_t v = 0;
    const nc_px_jnum_t r = nc_px_js_num(j, limit, &v);
    if (r == NC_PX_JSN_OK) {
        *out = (uint16_t)v;
        return;
    }
    if (r == NC_PX_JSN_BAD) {
        (void)nc_px_js_skip(j);
    }
    *ok = false;
}

static bool parse_seg(nc_px_js_t *j, nc_px_seg_t *seg, bool *ok)
{
    if (!nc_px_js_eat(j, '{')) {
        return false;
    }
    bool has_type = false;
    if (!nc_px_js_eat(j, '}')) {
        for (;;) {
            char key[24];
            if (!nc_px_js_key(j, key, sizeof(key))) {
                return false;
            }
            if (strcmp(key, "type") == 0) {
                char t[24];
                if (!nc_px_js_str(j, t, sizeof(t))) {
                    return false;
                }
                has_type = true;
                if (!seg_type_of(t, &seg->type)) {
                    ESP_LOGW(TAG, "未知段型別：%s", t);
                    *ok = false;
                }
            } else if (strcmp(key, "F") == 0 || strcmp(key, "f") == 0) {
                seg_num(j, 65535u, &seg->f, ok);
            } else if (strcmp(key, "l_max") == 0) {
                seg_num(j, 4095u, &seg->l_max, ok);
            } else if (strcmp(key, "l_lim") == 0) {
                seg_num(j, 4095u, &seg->l_lim, ok);
            } else if (strcmp(key, "phi") == 0) {
                seg_num(j, 4095u, &seg->phi, ok);
            } else if (strcmp(key, "end_Time") == 0) {
                seg_num(j, 65535u, &seg->end_f, ok);
            } else if (strcmp(key, "pulse") == 0) {
                seg_num(j, 4095u, &seg->pulse, ok);
            } else if (!nc_px_js_skip(j)) {
                return false;
            }
            if (nc_px_js_eat(j, ',')) {
                continue;
            }
            if (nc_px_js_eat(j, '}')) {
                break;
            }
            return false;
        }
    }
    if (!has_type) {
        *ok = false;                          // 冇 type 的段畫唔到
    }
    return true;
}

static bool parse_program(nc_px_js_t *j, nc_px_asset_t *a, bool *ok)
{
    if (!nc_px_js_eat(j, '[')) {
        return false;
    }
    if (nc_px_js_eat(j, ']')) {
        return true;
    }
    bool warned = false;
    for (;;) {
        nc_px_seg_t seg;
        memset(&seg, 0, sizeof(seg));
        seg.pulse = 2047;                     // MP 的 `seg.get("pulse", 2047)`
        if (!parse_seg(j, &seg, ok)) {
            return false;
        }

        // end_Time 是**絕對幀**，MP 的波表長度 = 最後一段的 end_Time。
        // 唔遞增（含 0）就畫唔出連續波形 → 整個資產拒絕，寧願唔畫。
        const uint16_t prev_end = (a->nseg > 0) ? a->seg[a->nseg - 1].end_f : 0;
        if (seg.end_f <= prev_end) {
            ESP_LOGW(TAG, "%s：end_Time 必須嚴格遞增（%u <= %u）",
                     a->name[0] != '\0' ? a->name : "(未命名)",
                     (unsigned)seg.end_f, (unsigned)prev_end);
            *ok = false;
        }

        if (a->nseg >= NC_PX_SEG_MAX) {
            if (!warned) {
                ESP_LOGW(TAG, "%s：program 超過 %d 段，其餘忽略",
                         a->name[0] != '\0' ? a->name : "(未命名)", NC_PX_SEG_MAX);
                warned = true;
            }
        } else {
            a->seg[a->nseg++] = seg;
        }
        if (nc_px_js_eat(j, ',')) {
            continue;
        }
        if (nc_px_js_eat(j, ']')) {
            return true;
        }
        return false;
    }
}

// 一個 object 的效果欄位；`allow_effects` = 容許 {"effects":[...]}（effects.json）。
static bool parse_object(nc_px_js_t *j, nc_px_asset_t *a, bool *ok, bool *touched,
                         bool allow_effects, bool *batch)
{
    if (!nc_px_js_eat(j, '{')) {
        return false;
    }
    if (nc_px_js_eat(j, '}')) {
        return true;
    }
    for (;;) {
        char key[24];
        if (!nc_px_js_key(j, key, sizeof(key))) {
            return false;
        }
        if (allow_effects && strcmp(key, "effects") == 0) {
            if (!nc_px_js_eat(j, '[')) {
                return false;
            }
            if (!nc_px_js_eat(j, ']')) {
                for (;;) {
                    nc_px_asset_t item;
                    memset(&item, 0, sizeof(item));
                    item.kind = NC_PX_ASSET_PROGRAM;
                    item.speed = 1;
                    bool item_ok = true, item_touched = false, dummy = false;
                    if (!parse_object(j, &item, &item_ok, &item_touched, false, &dummy)) {
                        return false;
                    }
                    if (!item_ok) {
                        s_stat.rejected++;
                    } else if (item_touched) {
                        (void)tbl_add(&item);
                    }
                    if (nc_px_js_eat(j, ',')) {
                        continue;
                    }
                    if (nc_px_js_eat(j, ']')) {
                        break;
                    }
                    return false;
                }
            }
            *batch = true;
        } else if (strcmp(key, "name") == 0) {
            if (!nc_px_js_str(j, a->name, sizeof(a->name))) {
                return false;
            }
            *touched = true;
        } else if (strcmp(key, "program") == 0) {
            *touched = true;
            if (!parse_program(j, a, ok)) {
                return false;
            }
        } else if (strcmp(key, "step") == 0) {
            uint32_t v = 0;
            if (nc_px_js_num(j, 65535u, &v) == NC_PX_JSN_OK) {
                a->step = (uint16_t)v;
            } else if (!nc_px_js_skip(j)) {
                return false;
            }
        } else if (strcmp(key, "spacing") == 0) {
            uint32_t v = 0;
            if (nc_px_js_num(j, 65535u, &v) == NC_PX_JSN_OK) {
                a->spacing = (uint16_t)v;
            } else if (!nc_px_js_skip(j)) {
                return false;
            }
        } else if (strcmp(key, "offset") == 0) {
            uint32_t v = 0;
            if (nc_px_js_num(j, 65535u, &v) == NC_PX_JSN_OK) {
                a->offset = (uint16_t)v;
            } else if (!nc_px_js_skip(j)) {
                return false;
            }
        } else if (strcmp(key, "speed") == 0) {
            uint32_t v = 0;
            if (nc_px_js_num(j, 65535u, &v) == NC_PX_JSN_OK) {
                a->speed = (uint16_t)((v == 0) ? 1 : v);
            } else if (!nc_px_js_skip(j)) {
                return false;
            }
        } else if (strcmp(key, "reverse") == 0) {
            if (!nc_px_js_bool(j, &a->reverse) && !nc_px_js_skip(j)) {
                return false;
            }
        } else if (strcmp(key, "pixel_n") == 0) {
            uint32_t v = 0;
            const nc_px_jnum_t r = nc_px_js_num(j, 65535u, &v);
            if (r == NC_PX_JSN_OK) {
                a->pixel_hint = (uint16_t)v;
            } else {
                if (r == NC_PX_JSN_BAD) {
                    (void)nc_px_js_skip(j);
                }
                *ok = false;
            }
            *touched = true;
        } else if (!nc_px_js_skip(j)) {             // id／version／未知鍵：跳過
            return false;
        }
        if (nc_px_js_eat(j, ',')) {
            continue;
        }
        if (nc_px_js_eat(j, '}')) {
            return true;
        }
        return false;
    }
}

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

esp_err_t nc_px_asset_add_json(const char *json, size_t len, const char *name_hint)
{
    if (json == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = json;
    const char *const end = json + len;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF) {
        p += 3;                               // UTF-8 BOM（Windows 編輯器常見）
    }

    nc_px_js_t j;
    nc_px_js_open(&j, p, (size_t)(end - p));
    nc_px_asset_t a;
    memset(&a, 0, sizeof(a));
    a.kind = NC_PX_ASSET_PROGRAM;
    a.speed = 1;

    bool touched = false, ok = true, batch = false;
    if (!parse_object(&j, &a, &ok, &touched, true, &batch)) {
        s_stat.rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    nc_px_js_ws(&j);
    if (j.p != j.end) {                       // 尾隨垃圾：當壞檔
        s_stat.rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    if (batch) {
        return ESP_OK;                        // 批次：逐個 element 已各自結算
    }
    if (!touched) {                           // 空 object：唔算一個效果
        s_stat.rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    if (!ok) {
        s_stat.rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    if (a.name[0] == '\0' && name_hint != NULL) {
        copy_name(a.name, sizeof(a.name), name_hint);
    }
    return tbl_add(&a);
}

// ═══════════════════════ 狀態 ═══════════════════════
size_t nc_px_asset_json(char *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return 0;
    }
    size_t u = 0;
    int n = snprintf(out, cap, "{\"n\":%u,\"cap\":%u,\"rej\":%u,\"dup\":%u,\"items\":[",
                     (unsigned)s_tbl.n, (unsigned)s_tbl.cap,
                     (unsigned)s_stat.rejected, (unsigned)s_stat.dup);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    u = (size_t)n;
    for (uint16_t i = 0; i < s_tbl.n; i++) {
        n = snprintf(out + u, cap - u, "%s{\"name\":\"%s\",\"kind\":%u,\"seg\":%u}",
                     (i == 0) ? "" : ",",
                     s_tbl.item[i].name, (unsigned)s_tbl.item[i].kind,
                     (unsigned)s_tbl.item[i].nseg);
        if (n < 0 || (size_t)n >= cap - u) {
            return 0;
        }
        u += (size_t)n;
    }
    n = snprintf(out + u, cap - u, "]}");
    if (n < 0 || (size_t)n >= cap - u) {
        return 0;
    }
    return u + (size_t)n;
}
