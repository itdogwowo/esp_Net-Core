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
    dst[n] = '\0';
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

static bool parse_group(nc_px_js_t *j, nc_px_group_t *g)
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
            if (strcmp(key, "id") == 0) {
                uint32_t v = 0;
                if (nc_px_js_num(j, 255, &v) == NC_PX_JSN_OK) {
                    g->id = (uint8_t)v;
                } else if (!nc_px_js_skip(j)) {
                    return false;
                }
            } else if (strcmp(key, "name") == 0) {
                char nm[NC_PX_LAYER_NAME_MAX];
                if (!nc_px_js_str(j, nm, sizeof(nm))) {
                    return false;
                }
                copy_str(g->name, sizeof(g->name), nm);
            } else if (strcmp(key, "sel") == 0) {
                if (!nc_px_js_eat(j, '[')) {
                    return false;
                }
                if (!nc_px_js_eat(j, ']')) {
                    for (;;) {
                        nc_px_group_sel_t sel;
                        memset(&sel, 0, sizeof(sel));
                        if (!parse_sel(j, &sel)) {
                            return false;
                        }
                        if (g->nsel < NC_PX_GROUP_SEL_MAX) {
                            g->sel[g->nsel++] = sel;
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

esp_err_t nc_px_map_parse(const char *json, size_t len, nc_px_map_t *out)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const char *const end = json + len;
    nc_px_js_t j;
    nc_px_js_open(&j, skip_bom(json, end), (size_t)(end - skip_bom(json, end)));
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
                if (nc_px_js_num(&j, 255, &v) == NC_PX_JSN_OK) {
                    out->id = (uint8_t)v;
                } else if (!nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "name") == 0) {
                char nm[NC_PX_LAYER_NAME_MAX];
                if (!nc_px_js_str(&j, nm, sizeof(nm))) {
                    return ESP_ERR_INVALID_ARG;
                }
                copy_str(out->name, sizeof(out->name), nm);
            } else if (strcmp(key, "groups") == 0) {
                if (!nc_px_js_eat(&j, '[')) {
                    return ESP_ERR_INVALID_ARG;
                }
                if (!nc_px_js_eat(&j, ']')) {
                    for (;;) {
                        nc_px_group_t g;
                        memset(&g, 0, sizeof(g));
                        if (!parse_group(&j, &g)) {
                            return ESP_ERR_INVALID_ARG;
                        }
                        if (out->ngroups < NC_PX_GROUP_MAX) {
                            out->group[out->ngroups++] = g;
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

// ═══════════════════════ mode ═══════════════════════
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
                char v[NC_PX_LAYER_NAME_MAX + 8];
                if (!nc_px_js_str(j, v, sizeof(v))) {
                    return false;
                }
                copy_str(e->group, sizeof(e->group), v);
            } else if (strcmp(key, "effect") == 0) {
                char v[NC_PX_LAYER_NAME_MAX];
                if (!nc_px_js_str(j, v, sizeof(v))) {
                    return false;
                }
                copy_str(e->effect, sizeof(e->effect), v);
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
    *valid = (e->group[0] != '\0') && (e->effect[0] != '\0')
          && (e->write < NC_PX_WRITE_COUNT);
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

esp_err_t nc_px_mode_parse(const char *json, size_t len, nc_px_mode_t *out)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
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
                if (nc_px_js_num(&j, 255, &v) == NC_PX_JSN_OK) {
                    out->id = (uint8_t)v;
                } else if (!nc_px_js_skip(&j)) {
                    return ESP_ERR_INVALID_ARG;
                }
            } else if (strcmp(key, "name") == 0) {
                char v[NC_PX_LAYER_NAME_MAX];
                if (!nc_px_js_str(&j, v, sizeof(v))) {
                    return ESP_ERR_INVALID_ARG;
                }
                copy_str(out->name, sizeof(out->name), v);
            } else if (strcmp(key, "mapping") == 0) {
                char v[NC_PX_LAYER_NAME_MAX];
                if (!nc_px_js_str(&j, v, sizeof(v))) {
                    return ESP_ERR_INVALID_ARG;
                }
                copy_str(out->mapping, sizeof(out->mapping), v);
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
                        nc_px_mode_entry_t e;
                        bool valid = false;
                        memset(&e, 0, sizeof(e));
                        if (!parse_entry(&j, &e, &valid)) {
                            return ESP_ERR_INVALID_ARG;
                        }
                        if (valid && out->nentry < NC_PX_MODE_ENTRY_MAX) {
                            out->entry[out->nentry++] = e;
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

// ═══════════════════════ registry（show）═══════════════════════
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
                        char v[NC_PX_LAYER_NAME_MAX];
                        if (nc_px_js_str(&j, v, sizeof(v))) {
                            if (out->nlist < NC_PX_SHOW_LIST_MAX) {
                                copy_str(out->list[out->nlist++], NC_PX_LAYER_NAME_MAX, v);
                            }
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
