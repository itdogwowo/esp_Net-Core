// nc_px_layer.c — map／modes／registry 三個 JSON → struct（DATA 平面第二～四層）。
//
// 規則（同 nc_px_asset.c 一致）：
//   缺欄位         → 預設（0／false／空字串）
//   唔認識的鍵     → 跳過（向前兼容）
//   mode 的無效項  → **跳過該項**（同 mp pixel_task 的 warn + skip 一致），其餘照載
//   未知 write     → 跳過該項（唔可以靜默用錯通道）
//   名太長         → 截斷（同 asset 層）
#include "nc_px_layer.h"

#include <stdio.h>
#include <string.h>

#include "nc_px_json.h"
#include "nc_px_utf8.h"

// ═══════════════════════ 名字表 ═══════════════════════
static const char *const HW_NAMES[NC_PX_HW_COUNT] = {
    "none", "ws2812", "apa102", "pca9685", "pwm", "uartMotor1",
};

const char *nc_px_hw_name(uint8_t hw)
{
    return (hw < NC_PX_HW_COUNT) ? HW_NAMES[hw] : "?";
}

bool nc_px_hw_parse(const char *name, uint8_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < NC_PX_HW_COUNT; i++) {
        if (strcmp(name, HW_NAMES[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

static const char *const WRITE_NAMES[NC_PX_WRITE_COUNT] = {
    "r", "g", "b", "w", "ww", "rgb", "rgbw", "wwww",
};

const char *nc_px_write_name(uint8_t w)
{
    return (w < NC_PX_WRITE_COUNT) ? WRITE_NAMES[w] : "?";
}

bool nc_px_write_parse(const char *name, uint8_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < NC_PX_WRITE_COUNT; i++) {
        if (strcmp(name, WRITE_NAMES[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

// ═══════════════════════ slice ═══════════════════════
// "0:16" → 0,16,1；"16:" → 16,0(到尾),1；":" → 0,0,1；"::-1" → 0,0,-1
bool nc_px_slice_parse(const char *s, int16_t *start, int16_t *stop, int16_t *step)
{
    if (s == NULL || start == NULL || stop == NULL || step == NULL) {
        return false;
    }
    int parts = 0;
    int v[3] = { 0, 0, 0 };
    bool seen[3] = { false, false, false };
    const char *p = s;
    parts = 1;
    for (;;) {
        bool neg = false;
        if (*p == '-') {
            neg = true;
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            int acc = 0;
            while (*p >= '0' && *p <= '9') {
                acc = acc * 10 + (*p - '0');
                if (acc > 32767) {
                    acc = 32767;
                }
                p++;
            }
            v[parts - 1] = neg ? -acc : acc;
            seen[parts - 1] = true;
        } else if (neg) {
            return false;
        }
        if (*p == ':') {
            p++;
            if (parts >= 3) {
                return false;
            }
            parts++;
            continue;
        }
        break;
    }
    if (*p != '\0') {
        return false;
    }
    *start = seen[0] ? (int16_t)v[0] : 0;
    *stop = seen[1] ? (int16_t)v[1] : 0;
    *step = (parts >= 3 && seen[2]) ? (int16_t)v[2] : 0;
    return true;
}

// ═══════════════════════ 小工具 ═══════════════════════
static void copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    size_t n = 0;
    if (src != NULL) {
        while (src[n] != '\0' && n + 1 < cap) {
            dst[n] = src[n];
            n++;
        }
    }
    n = nc_px_utf8_trim(dst, n);
    dst[n] = '\0';
}

static bool intern_name(nc_px_js_t *j, uint32_t *out_id, const char **out_str)
{
    char *s = nc_px_js_str_dup(j);
    if (s == NULL) {
        return false;
    }
    *out_id = nc_px_name_dict_global_intern(s, strlen(s));
    if (out_str != NULL) {
        *out_str = nc_px_name_dict_global_str(*out_id);
    }
    free(s);
    return *out_id != NC_PX_NAME_DICT_BAD_ID;
}

static const char *skip_bom(const char *json, const char *end)
{
    if ((size_t)(end - json) >= 3 && (uint8_t)json[0] == 0xEF
        && (uint8_t)json[1] == 0xBB && (uint8_t)json[2] == 0xBF) {
        return json + 3;
    }
    return json;
}

// ═══════════════════════ mapping ═══════════════════════
static bool parse_sel(nc_px_js_t *j, nc_px_group_sel_t *sel)
{
    if (!nc_px_js_eat(j, '{')) {
        return false;
    }
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
                uint8_t hw = NC_PX_HW_NONE;
                (void)nc_px_hw_parse(t, &hw);
                sel->hw = hw;
            } else if (strcmp(key, "sel") == 0) {
                char sl[NC_PX_SLICE_MAX];
                if (!nc_px_js_str(j, sl, sizeof(sl))) {
                    return false;
                }
                copy_str(sel->slice, sizeof(sel->slice), sl);
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
    return true;
}

// ── 容量掃描（count pass）的遞歸小幫手：只行 JSON，唔建立 struct ──
static bool count_sel_arr(nc_px_js_t *j, uint32_t *nsels)
{
    if (!nc_px_js_eat(j, '[')) {
        return false;
    }
    if (nc_px_js_eat(j, ']')) {
        return true;
    }
    for (;;) {
        if (!nc_px_js_skip(j)) {
            return false;
        }
        (*nsels)++;
        if (nc_px_js_eat(j, ',')) {
            continue;
        }
        if (nc_px_js_eat(j, ']')) {
            return true;
        }
        return false;
    }
}

static bool count_group_obj(nc_px_js_t *j, uint32_t *ngroups, uint32_t *nsels)
{
    if (!nc_px_js_eat(j, '{')) {
        return false;
    }
    if (!nc_px_js_eat(j, '}')) {
        for (;;) {
            char key[24];
            if (!nc_px_js_key(j, key, sizeof(key))) {
                return false;
            }
            if (strcmp(key, "sel") == 0) {
                if (!count_sel_arr(j, nsels)) {
                    return false;
                }
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
    (*ngroups)++;
    return true;
}

// 內容 hash（FNV-1a）：快取 key 用；同一份 JSON 永遠同一個值。
static uint32_t json_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

// ── 兩步解析：先數容量，再由呼叫端配 pool ──
// 為什麼要兩步：group／sel 數目係檔案決定（可能 500 組 × 幾段），唔可以用固定陣列；
// 而嵌入式唔想 realloc，所以先數一次、一次過配足，再解析。
esp_err_t nc_px_map_count(const char *json, size_t len, uint32_t *ngroups, uint32_t *nsels)
{
    if (json == NULL || ngroups == NULL || nsels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ngroups = 0;
    *nsels = 0;
    const char *const end = json + len;
    const char *const start = skip_bom(json, end);
    nc_px_js_t j;
    nc_px_js_open(&j, start, (size_t)(end - start));
    if (!nc_px_js_eat(&j, '{')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_px_js_eat(&j, '}')) {
        return ESP_OK;                       // 空物件：0 組
    }
    for (;;) {
        char key[24];
        if (!nc_px_js_key(&j, key, sizeof(key))) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(key, "groups") == 0) {
            if (!nc_px_js_eat(&j, '[')) {
                return ESP_ERR_INVALID_ARG;
            }
            if (!nc_px_js_eat(&j, ']')) {
                for (;;) {
                    if (!count_group_obj(&j, ngroups, nsels)) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    // 健全性檢查：擋壞檔／離譜大細（唔係正常截斷）
                    if (*ngroups > NC_PX_GROUP_MAX || *nsels > NC_PX_GROUP_SEL_MAX) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    if (nc_px_js_eat(&j, ',')) {
                        continue;
                    }
                    if (nc_px_js_eat(&j, ']')) {
                        break;
                    }
                    return ESP_ERR_INVALID_ARG;
                }
            }
        } else if (!nc_px_js_skip(&j)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (nc_px_js_eat(&j, ',')) {
            continue;
        }
        if (nc_px_js_eat(&j, '}')) {
            break;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void nc_px_map_pool_reset(nc_px_map_pool_t *pool)
{
    if (pool != NULL) {
        pool->used_group = 0;
        pool->used_sel = 0;
    }
}

// 一個 group：id／name／sel[]；sel 段直接寫入 pool（cap 不足 → NO_MEM，唔截斷）
static esp_err_t parse_group(nc_px_js_t *j, nc_px_map_pool_t *pool, nc_px_group_t *g)
{
    if (!nc_px_js_eat(j, '{')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_px_js_eat(j, '}')) {
        return ESP_OK;
    }
    for (;;) {
        char key[24];
        if (!nc_px_js_key(j, key, sizeof(key))) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(key, "id") == 0) {
            uint32_t v = 0;
            if (nc_px_js_num(j, 65535, &v) == NC_PX_JSN_OK) {
                g->id = (uint16_t)v;
            } else if (!nc_px_js_skip(j)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (strcmp(key, "name") == 0) {
            if (!intern_name(j, &g->name_id, &g->name)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (strcmp(key, "sel") == 0) {
            if (!nc_px_js_eat(j, '[')) {
                return ESP_ERR_INVALID_ARG;
            }
            const uint32_t first = pool->used_sel;
            if (!nc_px_js_eat(j, ']')) {
                for (;;) {
                    if (pool->used_sel >= pool->cap_sel) {
                        return ESP_ERR_NO_MEM;
                    }
                    nc_px_group_sel_t *sel = &pool->sel[pool->used_sel];
                    if (!parse_sel(j, sel)) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    pool->used_sel++;
                    if (nc_px_js_eat(j, ',')) {
                        continue;
                    }
                    if (nc_px_js_eat(j, ']')) {
                        break;
                    }
                    return ESP_ERR_INVALID_ARG;
                }
            }
            g->sel = &pool->sel[first];
            g->nsel = pool->used_sel - first;
        } else if (!nc_px_js_skip(j)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (nc_px_js_eat(j, ',')) {
            continue;
        }
        if (nc_px_js_eat(j, '}')) {
            break;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t nc_px_map_parse(const char *json, size_t len, nc_px_map_pool_t *pool,
                          nc_px_map_t *out)
{
    if (json == NULL || pool == NULL || out == NULL ||
        pool->group == NULL || pool->sel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const uint32_t g0 = pool->used_group;
    const char *const end = json + len;
    const char *const start = skip_bom(json, end);
    nc_px_js_t j;
    nc_px_js_open(&j, start, (size_t)(end - start));
    if (!nc_px_js_eat(&j, '{')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nc_px_js_eat(&j, '}')) {
        for (;;) {
            char key[24];
            if (!nc_px_js_key(&j, key, sizeof(key))) {
                return ESP_ERR_INVALID_ARG;
            }
            if (strcmp(key, "id") == 0) {
                uint32_t v = 0;
                if (nc_px_js_num(&j, 65535, &v) == NC_PX_JSN_OK) {
                    out->id = (uint16_t)v;
                } else if (!nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "name") == 0) {
                if (!intern_name(&j, &out->name_id, &out->name)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "groups") == 0) {
                if (!nc_px_js_eat(&j, '[')) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (!nc_px_js_eat(&j, ']')) {
                    for (;;) {
                        if (pool->used_group >= pool->cap_group) {
                            return ESP_ERR_NO_MEM;
                        }
                        nc_px_group_t *g = &pool->group[pool->used_group];
                        memset(g, 0, sizeof(*g));
                        const esp_err_t r = parse_group(&j, pool, g);
                        if (r != ESP_OK) {
                            return r;
                        }
                        pool->used_group++;
                        if (nc_px_js_eat(&j, ',')) {
                            continue;
                        }
                        if (nc_px_js_eat(&j, ']')) {
                            break;
                        }
                        return ESP_ERR_INVALID_ARG;
                    }
                }
            } else if (!nc_px_js_skip(&j)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (nc_px_js_eat(&j, ',')) {
                continue;
            }
            if (nc_px_js_eat(&j, '}')) {
                break;
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    out->group = (pool->used_group > g0) ? &pool->group[g0] : NULL;
    out->ngroups = pool->used_group - g0;
    out->hash = json_hash(json, len);
    return ESP_OK;
}

// ═══════════════════════ mode ═══════════════════════
static uint32_t s_mode_dropped;
static char s_mode_drop_name[32];

static void (*s_drop_hook)(const char *mode, const char *effect);
void nc_px_layer_set_drop_hook(void (*fn)(const char *mode, const char *effect))
{
    s_drop_hook = fn;
}

uint32_t nc_px_mode_last_dropped(void) { return s_mode_dropped; }
const char *nc_px_mode_last_dropped_name(void) { return s_mode_drop_name; }

static bool parse_entry(nc_px_js_t *j, nc_px_mode_entry_t *e, bool *valid)
{
    *valid = false;
    if (!nc_px_js_eat(j, '{')) {
        return false;
    }
    if (!nc_px_js_eat(j, '}')) {
        for (;;) {
            char key[24];
            if (!nc_px_js_key(j, key, sizeof(key))) {
                return false;
            }
            if (strcmp(key, "group") == 0) {
                char *v = nc_px_js_str_dup(j);
                if (v == NULL) {
                    return false;
                }
                char *dot = strchr(v, '.');
                if (dot != NULL) {
                    *dot = '\0';
                    e->map_id = nc_px_name_dict_global_intern(v, strlen(v));
                    e->group_id = nc_px_name_dict_global_intern(dot + 1, strlen(dot + 1));
                    e->group = nc_px_name_dict_global_str(e->group_id);
                } else {
                    e->group_id = nc_px_name_dict_global_intern(v, strlen(v));
                    e->group = nc_px_name_dict_global_str(e->group_id);
                }
                free(v);
            } else if (strcmp(key, "effect") == 0) {
                if (!intern_name(j, &e->effect_id, &e->effect)) {
                    return false;
                }
            } else if (strcmp(key, "write") == 0) {
                char v[12];
                if (!nc_px_js_str(j, v, sizeof(v))) {
                    return false;
                }
                uint8_t w = NC_PX_WRITE_COUNT;
                e->write = nc_px_write_parse(v, &w) ? w : NC_PX_WRITE_COUNT;
            } else if (strcmp(key, "range") == 0) {
                char v[NC_PX_SLICE_MAX];
                if (!nc_px_js_str(j, v, sizeof(v))) {
                    return false;
                }
                e->has_range = nc_px_slice_parse(v, &e->start, &e->stop, &e->step);
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
    *valid = (e->group_id != NC_PX_NAME_DICT_BAD_ID) &&
             (e->effect_id != NC_PX_NAME_DICT_BAD_ID) &&
             (e->write < NC_PX_WRITE_COUNT);
    return true;
}

static bool parse_play_loop(nc_px_js_t *j, int16_t *out)
{
    nc_px_js_ws(j);
    bool neg = false;
    if (j->p < j->end && *j->p == '-') {
        neg = true;
        j->p++;
    }
    uint32_t v = 0;
    const nc_px_jnum_t r = nc_px_js_num(j, 32767, &v);
    if (r != NC_PX_JSN_OK) {
        return false;
    }
    *out = (int16_t)(neg ? -(int32_t)v : (int32_t)v);
    return true;
}

// white／white_mix：0..100。缺欄位 = 0（舊 JSON 相容）；超範圍（>100 或負數）唔令
// 整份 mode 壞，只係保持 0 —— 白混合係顯示效果，唔應該因為一個數字就打沉個 mode。
// 注意 nc_px_js_num() 對 RANGE 已經行過 cursor，所以 RANGE 之後唔可以再 skip。
static bool parse_white(nc_px_js_t *j, uint8_t *out)
{
    uint32_t v = 0;
    const nc_px_jnum_t r = nc_px_js_num(j, 100, &v);
    if (r == NC_PX_JSN_OK) {
        *out = (uint8_t)v;
        return true;
    }
    if (r == NC_PX_JSN_RANGE) {
        return true;                      // 數字但超範圍（>100／負）→ 保持 0
    }
    return nc_px_js_skip(j);              // 唔係數字（字串／null…）→ 跳過
}

esp_err_t nc_px_mode_count(const char *json, size_t len, uint32_t *nentry)
{
    if (json == NULL || nentry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *nentry = 0;
    const char *const end = json + len;
    const char *const body = skip_bom(json, end);
    nc_px_js_t j;
    nc_px_js_open(&j, body, (size_t)(end - body));
    if (!nc_px_js_eat(&j, '{')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_px_js_eat(&j, '}')) {
        return ESP_OK;
    }
    for (;;) {
        char key[24];
        if (!nc_px_js_key(&j, key, sizeof(key))) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(key, "map") == 0) {
            if (!nc_px_js_eat(&j, '[')) {
                return ESP_ERR_INVALID_ARG;
            }
            if (!nc_px_js_eat(&j, ']')) {
                for (;;) {
                    if (!nc_px_js_skip(&j)) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    (*nentry)++;
                    if (nc_px_js_eat(&j, ',')) {
                        continue;
                    }
                    if (nc_px_js_eat(&j, ']')) {
                        break;
                    }
                    return ESP_ERR_INVALID_ARG;
                }
            }
        } else if (!nc_px_js_skip(&j)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (nc_px_js_eat(&j, ',')) {
            continue;
        }
        if (nc_px_js_eat(&j, '}')) {
            break;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t nc_px_mode_parse(const char *json, size_t len, nc_px_mode_entry_t *pool,
                           uint32_t cap, nc_px_mode_t *out)
{
    s_mode_dropped = 0;
    s_mode_drop_name[0] = '\0';
    if (json == NULL || out == NULL || (pool == NULL && cap > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->name_id = NC_PX_NAME_DICT_BAD_ID;
    out->mapping_id = NC_PX_NAME_DICT_BAD_ID;
    out->map_id = NC_PX_NAME_DICT_BAD_ID;
    out->play_loop = 1;
    const char *const end = json + len;
    const char *const body = skip_bom(json, end);
    nc_px_js_t j;
    nc_px_js_open(&j, body, (size_t)(end - body));
    if (!nc_px_js_eat(&j, '{')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nc_px_js_eat(&j, '}')) {
        for (;;) {
            char key[24];
            if (!nc_px_js_key(&j, key, sizeof(key))) {
                return ESP_ERR_INVALID_ARG;
            }
            if (strcmp(key, "id") == 0) {
                uint32_t v = 0;
                if (nc_px_js_num(&j, 65535, &v) == NC_PX_JSN_OK) {
                    out->id = (uint16_t)v;
                } else if (!nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "name") == 0) {
                if (!intern_name(&j, &out->name_id, &out->name)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "mapping") == 0) {
                if (!intern_name(&j, &out->mapping_id, &out->mapping)) {
                    return ESP_ERR_INVALID_ARG;
                }
                out->map_id = out->mapping_id;
            } else if (strcmp(key, "play_loop") == 0) {
                if (!parse_play_loop(&j, &out->play_loop) && !nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "maxF") == 0) {
                uint32_t v = 0;
                if (nc_px_js_num(&j, 65535, &v) == NC_PX_JSN_OK) {
                    out->maxF = (uint16_t)v;
                } else if (!nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "white") == 0 || strcmp(key, "white_mix") == 0) {
                if (!parse_white(&j, &out->white)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "map") == 0) {
                if (!nc_px_js_eat(&j, '[')) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (!nc_px_js_eat(&j, ']')) {
                    for (;;) {
                        if (out->nentry >= cap) {
                            return ESP_ERR_NO_MEM;
                        }
                        nc_px_mode_entry_t *e = &pool[out->nentry];
                        bool valid = false;
                        memset(e, 0, sizeof(*e));
                        e->group_id = NC_PX_NAME_DICT_BAD_ID;
                        e->effect_id = NC_PX_NAME_DICT_BAD_ID;
                        e->map_id = NC_PX_NAME_DICT_BAD_ID;
                        if (!parse_entry(&j, e, &valid)) {
                            return ESP_ERR_INVALID_ARG;
                        }
                        if (!valid) {
                            // 保留相容（唔令整份 mode 壞），但要**記住**掉了什麼，
                            // 由 paint 大聲報（以前靜靜 drop → jobs 少咗都唔知）。
                            s_mode_dropped++;
                            if (s_mode_drop_name[0] == '\0') {
                                snprintf(s_mode_drop_name, sizeof(s_mode_drop_name), "%s",
                                         e->effect ? e->effect : "?");
                            }
                        } else {
                            out->nentry++;
                        }
                        if (nc_px_js_eat(&j, ',')) {
                            continue;
                        }
                        if (nc_px_js_eat(&j, ']')) {
                            break;
                        }
                        return ESP_ERR_INVALID_ARG;
                    }
                }
            } else if (!nc_px_js_skip(&j)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (nc_px_js_eat(&j, ',')) {
                continue;
            }
            if (nc_px_js_eat(&j, '}')) {
                break;
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    out->entry = pool;                 // 指入呼叫端 pool（nentry = 有效條目數）
    if (out->map_id == NC_PX_NAME_DICT_BAD_ID && out->nentry > 0) {
        out->map_id = out->entry[0].map_id;   // entry 的 "map.group" prefix
    }
    out->hash = json_hash(json, len);
    return ESP_OK;
    if (s_mode_dropped > 0 && s_drop_hook != NULL) {
        s_drop_hook(nc_px_name_dict_global_str(out->name_id), s_mode_drop_name);
    }
}

// ═══════════════════════ registry（show）═══════════════════════
void nc_px_show_free(nc_px_show_t *show)
{
    if (show == NULL) {
        return;
    }
    free((void *)show->list);
    free(show->list_ids);
    memset(show, 0, sizeof(*show));
}

esp_err_t nc_px_show_parse(const char *json, size_t len, nc_px_show_t *out)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const char *const end = json + len;
    const char *const body = skip_bom(json, end);
    nc_px_js_t j;
    nc_px_js_open(&j, body, (size_t)(end - body));
    if (!nc_px_js_eat(&j, '{')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nc_px_js_eat(&j, '}')) {
        for (;;) {
            char key[24];
            if (!nc_px_js_key(&j, key, sizeof(key))) {
                return ESP_ERR_INVALID_ARG;
            }
            if (strcmp(key, "auto_play") == 0) {
                if (!nc_px_js_bool(&j, &out->auto_play) && !nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "list") == 0) {
                if (!nc_px_js_eat(&j, '[')) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (!nc_px_js_eat(&j, ']')) {
                    for (;;) {
                        char *v = nc_px_js_str_dup(&j);
                        if (v != NULL) {
                            if (out->nlist == out->cap) {
                                const uint32_t cap = (out->cap != 0) ? out->cap * 2u : 8u;
                                const char **nl = (const char **)realloc(
                                    out->list, (size_t)cap * sizeof(*nl));
                                uint32_t *id = (uint32_t *)realloc(
                                    out->list_ids, (size_t)cap * sizeof(*id));
                                if (nl == NULL || id == NULL) {
                                    free((void *)nl);
                                    free(id);
                                    free(v);
                                    return ESP_ERR_NO_MEM;
                                }
                                out->list = nl;
                                out->list_ids = id;
                                out->cap = cap;
                            }
                            const uint32_t id =
                                nc_px_name_dict_global_intern(v, strlen(v));
                            out->list_ids[out->nlist] =
                                id;
                            out->list[out->nlist] =
                                nc_px_name_dict_global_str(id);
                            out->nlist++;
                            free(v);
                        } else if (!nc_px_js_skip(&j)) {
                            return ESP_ERR_INVALID_ARG;
                        }
                        if (nc_px_js_eat(&j, ',')) {
                            continue;
                        }
                        if (nc_px_js_eat(&j, ']')) {
                            break;
                        }
                        return ESP_ERR_INVALID_ARG;
                    }
                }
            } else if (!nc_px_js_skip(&j)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (nc_px_js_eat(&j, ',')) {
                continue;
            }
            if (nc_px_js_eat(&j, '}')) {
                break;
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}
