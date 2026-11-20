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

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "px_asset";

static nc_px_asset_tbl_t  s_tbl;
static nc_px_asset_stat_t s_stat;

// ═══════════════════════ 表 ═══════════════════════
void nc_px_asset_clear(void)
{
    memset(&s_tbl, 0, sizeof(s_tbl));
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
    for (uint8_t i = 0; i < s_tbl.n; i++) {
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
    if (s_tbl.n >= NC_PX_ASSET_MAX) {
        s_stat.rejected++;
        ESP_LOGW(TAG, "表滿（%d）：%s 唔載入", NC_PX_ASSET_MAX, a->name);
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

// ═══════════════════════ 迷你 JSON 讀取器 ═══════════════════════
// 只支援 schema 用到的子集：object／array／string／整數／true／false／null。
// 冇 DOM、冇 malloc、深度上限 8；全部係「讀咗就前進」，失敗就停。
typedef struct {
    const char *p;
    const char *end;
    int         depth;
} js_t;

typedef enum {
    JSN_OK = 0,
    JSN_BAD,      // 唔係數字（cursor 冇動 → 呼叫者可以 js_skip）
    JSN_RANGE,    // 係數字但超範圍（cursor 已過）
} js_num_t;

static void js_ws(js_t *j)
{
    while (j->p < j->end) {
        const char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->p++;
        } else {
            break;
        }
    }
}

static bool js_eat(js_t *j, char c)
{
    js_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

// 讀一個 JSON string；out 可以係 NULL（只食位）。太長就截斷。
static bool js_str(js_t *j, char *out, size_t cap)
{
    js_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        char ch = *j->p++;
        if (ch == '\\') {
            if (j->p >= j->end) {
                return false;
            }
            const char e = *j->p++;
            switch (e) {
            case '"':  ch = '"';  break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/';  break;
            case 'n':  ch = '\n'; break;
            case 't':  ch = '\t'; break;
            case 'r':  ch = '\r'; break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            case 'u':                       // 只食位：路徑名唔會用到
                for (int k = 0; k < 4; k++) {
                    if (j->p >= j->end) {
                        return false;
                    }
                    j->p++;
                }
                ch = '?';
                break;
            default:
                return false;
            }
        }
        if (out != NULL && n + 1 < cap) {
            out[n++] = ch;
        }
    }
    if (j->p >= j->end) {
        return false;
    }
    j->p++;                                  // 收尾 "
    if (out != NULL) {
        out[n] = '\0';
    }
    return true;
}

// 讀整數（0..limit）。JSN_BAD 唔會前進；JSN_RANGE 已前進（呼叫者可以繼續讀落去）。
static js_num_t js_num(js_t *j, uint32_t limit, uint32_t *out)
{
    js_ws(j);
    const char *const start = j->p;
    bool neg = false;
    if (j->p < j->end && *j->p == '-') {
        neg = true;
        j->p++;
    }
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') {
        j->p = start;
        return JSN_BAD;
    }
    uint64_t v = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        v = v * 10u + (uint64_t)(*j->p - '0');
        if (v > 0xFFFFFFFFull) {
            v = 0xFFFFFFFFull;
        }
        j->p++;
    }
    if (neg || v > limit) {
        return JSN_RANGE;
    }
    *out = (uint32_t)v;
    return JSN_OK;
}

static bool js_skip(js_t *j);

static bool js_skip_nested(js_t *j, char open, char close)
{
    if (++j->depth > 8) {
        return false;
    }
    if (!js_eat(j, open)) {
        return false;
    }
    if (!js_eat(j, close)) {
        for (;;) {
            if (open == '{' && (!js_str(j, NULL, 0) || !js_eat(j, ':'))) {
                return false;
            }
            if (!js_skip(j)) {
                return false;
            }
            if (js_eat(j, ',')) {
                continue;
            }
            if (js_eat(j, close)) {
                break;
            }
            return false;
        }
    }
    j->depth--;
    return true;
}

static bool js_match(js_t *j, const char *lit)
{
    const size_t n = strlen(lit);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n) != 0) {
        return false;
    }
    j->p += n;
    return true;
}

static bool js_skip(js_t *j)
{
    js_ws(j);
    if (j->p >= j->end) {
        return false;
    }
    const char c = *j->p;
    if (c == '"') {
        return js_str(j, NULL, 0);
    }
    if (c == '{') {
        return js_skip_nested(j, '{', '}');
    }
    if (c == '[') {
        return js_skip_nested(j, '[', ']');
    }
    if (js_match(j, "true") || js_match(j, "false") || js_match(j, "null")) {
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        uint32_t v;
        if (js_num(j, 0xFFFFFFFFu, &v) == JSN_BAD) {
            return false;
        }
        // 小數／指數：照跳到 token 尾（我們唔用，但唔想因為佢而整份壞）
        while (j->p < j->end && (strchr("0123456789.eE+-", *j->p) != NULL)) {
            j->p++;
        }
        return true;
    }
    return false;
}

static bool js_key(js_t *j, char *key, size_t cap)
{
    return js_str(j, key, cap) && js_eat(j, ':');
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
static void seg_num(js_t *j, uint32_t limit, uint16_t *out, bool *ok)
{
    uint32_t v = 0;
    const js_num_t r = js_num(j, limit, &v);
    if (r == JSN_OK) {
        *out = (uint16_t)v;
        return;
    }
    if (r == JSN_BAD) {
        (void)js_skip(j);
    }
    *ok = false;
}

static bool parse_seg(js_t *j, nc_px_seg_t *seg, bool *ok)
{
    if (!js_eat(j, '{')) {
        return false;
    }
    bool has_type = false;
    if (!js_eat(j, '}')) {
        for (;;) {
            char key[24];
            if (!js_key(j, key, sizeof(key))) {
                return false;
            }
            if (strcmp(key, "type") == 0) {
                char t[24];
                if (!js_str(j, t, sizeof(t))) {
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
            } else if (!js_skip(j)) {
                return false;
            }
            if (js_eat(j, ',')) {
                continue;
            }
            if (js_eat(j, '}')) {
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

static bool parse_program(js_t *j, nc_px_asset_t *a, bool *ok)
{
    if (!js_eat(j, '[')) {
        return false;
    }
    if (js_eat(j, ']')) {
        return true;
    }
    bool warned = false;
    for (;;) {
        nc_px_seg_t seg;
        memset(&seg, 0, sizeof(seg));
        if (!parse_seg(j, &seg, ok)) {
            return false;
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
        if (js_eat(j, ',')) {
            continue;
        }
        if (js_eat(j, ']')) {
            return true;
        }
        return false;
    }
}

// 一個 object 的效果欄位；`allow_effects` = 容許 {"effects":[...]}（effects.json）。
static bool parse_object(js_t *j, nc_px_asset_t *a, bool *ok, bool *touched,
                         bool allow_effects, bool *batch)
{
    if (!js_eat(j, '{')) {
        return false;
    }
    if (js_eat(j, '}')) {
        return true;
    }
    for (;;) {
        char key[24];
        if (!js_key(j, key, sizeof(key))) {
            return false;
        }
        if (allow_effects && strcmp(key, "effects") == 0) {
            if (!js_eat(j, '[')) {
                return false;
            }
            if (!js_eat(j, ']')) {
                for (;;) {
                    nc_px_asset_t item;
                    memset(&item, 0, sizeof(item));
                    item.kind = NC_PX_ASSET_PROGRAM;
                    bool item_ok = true, item_touched = false, dummy = false;
                    if (!parse_object(j, &item, &item_ok, &item_touched, false, &dummy)) {
                        return false;
                    }
                    if (!item_ok) {
                        s_stat.rejected++;
                    } else if (item_touched) {
                        (void)tbl_add(&item);
                    }
                    if (js_eat(j, ',')) {
                        continue;
                    }
                    if (js_eat(j, ']')) {
                        break;
                    }
                    return false;
                }
            }
            *batch = true;
        } else if (strcmp(key, "name") == 0) {
            if (!js_str(j, a->name, sizeof(a->name))) {
                return false;
            }
            *touched = true;
        } else if (strcmp(key, "program") == 0) {
            *touched = true;
            if (!parse_program(j, a, ok)) {
                return false;
            }
        } else if (strcmp(key, "pixel_n") == 0) {
            uint32_t v = 0;
            const js_num_t r = js_num(j, 65535u, &v);
            if (r == JSN_OK) {
                a->pixel_hint = (uint16_t)v;
            } else {
                if (r == JSN_BAD) {
                    (void)js_skip(j);
                }
                *ok = false;
            }
            *touched = true;
        } else if (!js_skip(j)) {             // id／version／step／reverse… 暫時唔用
            return false;
        }
        if (js_eat(j, ',')) {
            continue;
        }
        if (js_eat(j, '}')) {
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

    js_t j = { p, end, 0 };
    nc_px_asset_t a;
    memset(&a, 0, sizeof(a));
    a.kind = NC_PX_ASSET_PROGRAM;

    bool touched = false, ok = true, batch = false;
    if (!parse_object(&j, &a, &ok, &touched, true, &batch)) {
        s_stat.rejected++;
        return ESP_ERR_INVALID_ARG;
    }
    js_ws(&j);
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
    int n = snprintf(out, cap, "{\"n\":%u,\"max\":%u,\"rej\":%u,\"dup\":%u,\"items\":[",
                     (unsigned)s_tbl.n, (unsigned)NC_PX_ASSET_MAX,
                     (unsigned)s_stat.rejected, (unsigned)s_stat.dup);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    u = (size_t)n;
    for (uint8_t i = 0; i < s_tbl.n; i++) {
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
