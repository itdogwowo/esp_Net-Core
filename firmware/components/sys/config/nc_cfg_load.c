// JSON → struct 的解析層。
//
// 這裡是「格式」的唯一入口：欄位的名字、型別、範圍、預設都在這幾個函式裡。
// 呼叫端（main / nc_task）只讀 struct，不再自己 cJSON_GetObjectItem。
//
// 解析規則（固定，不逐個欄位自訂）：
//   缺欄位          → 預設          （defaulted++）
//   型別錯／超範圍   → 預設          （invalid++）
//   不認識的鍵      → 忽略          （unknown++，向前兼容）
//   version 不符    → 整層用預設     （bad_version=1）
//   整份壞 JSON     → 整層用預設     （bad_json=1）
//   檔案不存在      → 整層用預設     （missing=1）
// 以上全部**不會寫回用戶的檔**。
#include "nc_cfg.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

static const char *TAG = "cfg";

static nc_system_cfg_t s_sys;
static nc_tasks_cfg_t  s_tasks;
static nc_cfg_stat_t   s_sys_stat;
static nc_cfg_stat_t   s_tasks_stat;
static bool s_loaded;

// ───────────────────────── 小工具 ─────────────────────────

static const cJSON *item(const cJSON *o, const char *key)
{
    return (o != NULL && cJSON_IsObject(o)) ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
}

static int32_t get_int(const cJSON *o, const char *key, int32_t def,
                       int32_t lo, int32_t hi, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    if (v == NULL) {
        st->defaulted++;
        return def;
    }
    if (!cJSON_IsNumber(v)) {
        st->invalid++;
        return def;
    }
    const double d = v->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi)) {
        st->invalid++;
        return def;
    }
    return (int32_t)d;
}

static bool get_bool(const cJSON *o, const char *key, bool def, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    if (v == NULL) {
        st->defaulted++;
        return def;
    }
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v) && (v->valueint == 0 || v->valueint == 1)) {
        return v->valueint != 0;
    }
    st->invalid++;
    return def;
}

static void get_str(const cJSON *o, const char *key, char *out, size_t cap,
                    const char *def, nc_cfg_stat_t *st)
{
    const cJSON *v = item(o, key);
    const char *s = (v != NULL && cJSON_IsString(v) && v->valuestring != NULL)
                        ? v->valuestring : NULL;
    if (s == NULL) {
        st->defaulted++;
        snprintf(out, cap, "%s", def != NULL ? def : "");
        return;
    }
    if (strlen(s) >= cap) {
        st->invalid++;      // 放唔落就唔截斷：截斷會變成另一個名
        snprintf(out, cap, "%s", def != NULL ? def : "");
        return;
    }
    snprintf(out, cap, "%s", s);
}

static void note_unknown(const cJSON *o, const char *const *known, size_t n,
                         nc_cfg_stat_t *st)
{
    if (!cJSON_IsObject(o)) {
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, o) {
        if (it->string == NULL) {
            continue;
        }
        bool ok = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(it->string, known[i]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            st->unknown++;
        }
    }
}

static bool kind_parse(const char *s, nc_link_kind_t *out)
{
    static const char *const names[NC_LINK_KIND_MAX] = {
        [NC_LINK_CDC] = "cdc", [NC_LINK_UART] = "uart", [NC_LINK_TCP] = "tcp",
        [NC_LINK_WS] = "ws",   [NC_LINK_UDP] = "udp",
    };
    if (s == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < NC_LINK_KIND_MAX; i++) {
        if (names[i] != NULL && strcasecmp(s, names[i]) == 0) {
            *out = (nc_link_kind_t)i;
            return true;
        }
    }
    return false;
}

// 讀一個 rx 數值欄位：只有「config 寫了而且合法」才回 true（給 rx_mask 用）。
static bool rx_take(const cJSON *rx, const char *key, int32_t lo, int32_t hi,
                    uint16_t *dst, nc_cfg_stat_t *st)
{
    const cJSON *v = item(rx, key);
    if (v == NULL) {
        st->defaulted++;
        return false;
    }
    if (!cJSON_IsNumber(v)) {
        st->invalid++;
        return false;
    }
    const double d = v->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi)) {
        st->invalid++;
        return false;
    }
    *dst = (uint16_t)d;
    return true;
}

// ───────────────────────── system ─────────────────────────

static void parse_links(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *arr = item(item(root, "nc4"), "links");
    if (arr == NULL) {
        st->defaulted++;        // 整段缺失 → 保留預設的兩條（CDC / UART）
        return;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return;
    }

    out->n_links = 0;           // 有寫 links[] 就以它為準（空陣列 = 全部關閉）
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_links >= NC_CFG_MAX_LINKS) {
            st->invalid++;      // 超出上限：多出來的不建立（fail-visible）
            break;
        }
        nc_link_cfg_t l;
        memset(&l, 0, sizeof(l));

        const cJSON *nm = item(it, "name");
        if (!cJSON_IsString(nm) || nm->valuestring == NULL || nm->valuestring[0] == '\0' ||
            strlen(nm->valuestring) >= NC_CFG_NAME_MAX) {
            st->invalid++;      // 冇名字的線沒有意義
            continue;
        }
        snprintf(l.name, sizeof(l.name), "%s", nm->valuestring);

        const cJSON *kd = item(it, "kind");
        if (kd != NULL && cJSON_IsString(kd) && kd->valuestring != NULL) {
            if (!kind_parse(kd->valuestring, &l.kind)) {
                st->invalid++;
                continue;
            }
        } else {
            nc_link_kind_t k;
            if (kind_parse(l.name, &k)) {
                l.kind = k;     // 冇寫 kind → 由名字推（CDC → cdc）
                st->defaulted++;
            } else {
                st->invalid++;
                continue;
            }
        }

        l.enable = get_bool(it, "enable", true, st);
        l.port = (uint16_t)get_int(it, "port", 0, 0, 65535, st);

        nc_cfg_rx_default(l.kind, &l.rx);
        const cJSON *rx = item(item(it, "cfg"), "rx");
        if (rx == NULL) {
            st->defaulted++;    // 只寫 name/kind → rx 用該 kind 的量測預設
        } else if (!cJSON_IsObject(rx)) {
            st->invalid++;
        } else {
            uint16_t v;
            if (rx_take(rx, "region", 64, NC_CFG_REGION_MAX, &v, st)) {
                l.rx.region = v;
                l.rx_mask |= NC_CFG_RX_REGION;
            }
            if (rx_take(rx, "chunk", 16, NC_CFG_REGION_MAX, &v, st)) {
                l.rx.chunk = v;
                l.rx_mask |= NC_CFG_RX_CHUNK;
            }
            if (rx_take(rx, "budget", 1, 64, &v, st)) {
                l.rx.budget = (uint8_t)v;
                l.rx_mask |= NC_CFG_RX_BUDGET;
            }
            if (rx_take(rx, "slots", 1, 16, &v, st)) {
                l.rx.slots = (uint8_t)v;
                l.rx_mask |= NC_CFG_RX_SLOTS;
            }
            if (rx_take(rx, "slot_size", 16, NC_CFG_REGION_MAX, &v, st)) {
                l.rx.slot_size = v;
                l.rx_mask |= NC_CFG_RX_SLOT_SIZE;
            }
            // chunk 大過一格沒有意義：PUSH 一次只搬一格
            if (l.rx.chunk > l.rx.slot_size) {
                l.rx.chunk = l.rx.slot_size;
            }
        }

        out->links[out->n_links++] = l;
    }
}

static void parse_routes(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    const cJSON *arr = item(item(root, "nc4"), "routes");
    if (arr == NULL) {
        st->defaulted++;
        return;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return;
    }

    out->n_routes = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_routes >= NC_CFG_MAX_ROUTES) {
            st->invalid++;
            break;
        }
        nc_route_cfg_t r;
        memset(&r, 0, sizeof(r));

        const cJSON *in = item(it, "in");
        if (!cJSON_IsString(in) || in->valuestring == NULL || in->valuestring[0] == '\0' ||
            strlen(in->valuestring) >= NC_CFG_NAME_MAX) {
            st->invalid++;
            continue;
        }
        snprintf(r.in, sizeof(r.in), "%s", in->valuestring);

        const cJSON *outs = item(it, "out");
        if (outs == NULL) {
            st->defaulted++;    // 冇 out = 丟棄（合法）
        } else if (!cJSON_IsArray(outs)) {
            st->invalid++;
        } else {
            const cJSON *o = NULL;
            cJSON_ArrayForEach(o, outs) {
                if (r.n_out >= NC_CFG_MAX_OUT) {
                    st->invalid++;   // 超出上限：多的丟掉（會記錄）
                    break;
                }
                if (!cJSON_IsString(o) || o->valuestring == NULL ||
                    strlen(o->valuestring) >= NC_CFG_NAME_MAX) {
                    st->invalid++;
                    continue;
                }
                snprintf(r.out[r.n_out], sizeof(r.out[0]), "%s", o->valuestring);
                r.n_out++;
            }
        }

        out->routes[out->n_routes++] = r;
    }
}

esp_err_t nc_cfg_apply_system(const cJSON *root, nc_system_cfg_t *out, nc_cfg_stat_t *st)
{
    if (out == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = *nc_cfg_defaults_system();
    memset(st, 0, sizeof(*st));

    if (root == NULL) {
        st->missing = 1;
        return ESP_OK;
    }
    if (!cJSON_IsObject(root)) {
        st->bad_json = 1;
        *out = *nc_cfg_defaults_system();
        return ESP_OK;
    }

    const cJSON *ver = item(root, "version");
    if (ver == NULL) {
        st->defaulted++;            // 冇 version → 當 v1（寬容）
    } else if (!cJSON_IsNumber(ver) || ver->valueint != NC_CFG_SCHEMA_VERSION) {
        st->bad_version = 1;        // 唔識的版本 → 整層用預設
        return ESP_OK;
    }

    static const char *const root_keys[] = {
        "version", "_comment", "cID", "frame_interval_ms", "num_pixels", "nc4", "log_debug_mode",
    };
    note_unknown(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]), st);

    get_str(root, "cID", out->cid, sizeof(out->cid), "", st);
    out->frame_interval_ms = (uint16_t)get_int(root, "frame_interval_ms", 20, 1, 1000, st);
    out->log_debug_mode = (uint8_t)get_int(root, "log_debug_mode", 0, 0, 1, st);
    out->num_pixels = (uint16_t)get_int(root, "num_pixels", 0, 0, 65535, st);

    const cJSON *nc4 = item(root, "nc4");
    if (nc4 == NULL) {
        st->defaulted++;            // 冇 nc4 → 保留預設的通道與路由
        return ESP_OK;
    }
    if (!cJSON_IsObject(nc4)) {
        st->invalid++;
        return ESP_OK;
    }
    static const char *const nc4_keys[] = { "links", "routes" };
    note_unknown(nc4, nc4_keys, sizeof(nc4_keys) / sizeof(nc4_keys[0]), st);

    parse_links(root, out, st);
    parse_routes(root, out, st);
    return ESP_OK;
}

// ───────────────────────── tasks ─────────────────────────

esp_err_t nc_cfg_apply_tasks(const cJSON *root, nc_tasks_cfg_t *out, nc_cfg_stat_t *st)
{
    if (out == NULL || st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = *nc_cfg_defaults_tasks();
    memset(st, 0, sizeof(*st));

    if (root == NULL) {
        st->missing = 1;
        return ESP_OK;
    }
    if (!cJSON_IsObject(root)) {
        st->bad_json = 1;
        *out = *nc_cfg_defaults_tasks();
        return ESP_OK;
    }

    const cJSON *ver = item(root, "version");
    if (ver == NULL) {
        st->defaulted++;
    } else if (!cJSON_IsNumber(ver) || ver->valueint != NC_CFG_SCHEMA_VERSION) {
        st->bad_version = 1;
        return ESP_OK;
    }

    static const char *const root_keys[] = { "version", "_comment", "tasks" };
    note_unknown(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]), st);

    const cJSON *arr = item(root, "tasks");
    if (arr == NULL) {
        st->defaulted++;            // 空 = 全部用固件宣告
        return ESP_OK;
    }
    if (!cJSON_IsArray(arr)) {
        st->invalid++;
        return ESP_OK;
    }

    out->n_tasks = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (out->n_tasks >= NC_CFG_MAX_TASKS) {
            st->invalid++;
            break;
        }
        nc_task_cfg_t t;
        memset(&t, 0, sizeof(t));

        const cJSON *nm = item(it, "name");
        if (!cJSON_IsString(nm) || nm->valuestring == NULL || nm->valuestring[0] == '\0' ||
            strlen(nm->valuestring) >= NC_CFG_TASK_NAME_MAX) {
            st->invalid++;
            continue;
        }
        snprintf(t.name, sizeof(t.name), "%s", nm->valuestring);

        // level 要分得出「冇寫」同「寫 0」：冇寫 → 用固件宣告的 level。
        const cJSON *lv = item(it, "level");
        if (lv == NULL) {
            st->defaulted++;
        } else if (cJSON_IsNumber(lv) && lv->valueint >= -1 && lv->valueint <= 7) {
            t.level = (int8_t)lv->valueint;
            t.has_level = true;
        } else {
            st->invalid++;      // 型別錯／超範圍 → 當冇寫
        }

        const cJSON *af = item(it, "affinity");
        if (af == NULL) {
            st->defaulted++;        // 未設定 = (0,0) = 唔跑
        } else if (!cJSON_IsArray(af) || cJSON_GetArraySize(af) != 2) {
            st->invalid++;
        } else {
            for (int k = 0; k < 2; k++) {
                const cJSON *a = cJSON_GetArrayItem(af, k);
                if (cJSON_IsNumber(a) && (a->valueint == 0 || a->valueint == 1)) {
                    t.aff[k] = a->valueint != 0;
                } else {
                    st->invalid++;  // 非 0/1 一律當 0（唔准跑嗰核）
                }
            }
        }

        out->tasks[out->n_tasks++] = t;
    }
    return ESP_OK;
}

// ───────────────────────── 存取 ─────────────────────────

const nc_system_cfg_t *nc_cfg_system(void)
{
    return &s_sys;
}

const nc_tasks_cfg_t *nc_cfg_tasks(void)
{
    return &s_tasks;
}

const nc_cfg_stat_t *nc_cfg_stat_of(const char *layer)
{
    if (layer == NULL) {
        return NULL;
    }
    if (strcmp(layer, "system") == 0) {
        return &s_sys_stat;
    }
    if (strcmp(layer, "tasks") == 0) {
        return &s_tasks_stat;
    }
    return NULL;
}

const nc_link_cfg_t *nc_cfg_link(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_sys.n_links; i++) {
        if (strcmp(s_sys.links[i].name, name) == 0) {
            return &s_sys.links[i];
        }
    }
    return NULL;
}

const nc_route_cfg_t *nc_cfg_route(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_sys.n_routes; i++) {
        if (strcmp(s_sys.routes[i].in, name) == 0) {
            return &s_sys.routes[i];
        }
    }
    return NULL;
}

bool nc_cfg_layer_bad(const char *layer)
{
    const nc_cfg_stat_t *st = nc_cfg_stat_of(layer);
    if (st == NULL) {
        return true;
    }
    return st->missing != 0 || st->bad_json != 0 || st->bad_version != 0;
}

// ───────────────────────── 開機載入 ─────────────────────────

esp_err_t nc_cfg_boot(void)
{
    bool exists = false;

    cfg_file_t *f = nc_cfg_open_ex("system", &exists);
    if (f != NULL) {
        (void)nc_cfg_apply_system(nc_cfg_root(f), &s_sys, &s_sys_stat);
        nc_cfg_close(f);
    } else {
        (void)nc_cfg_apply_system(NULL, &s_sys, &s_sys_stat);
        if (exists) {
            s_sys_stat.missing = 0;
            s_sys_stat.bad_json = 1;
        }
    }

    exists = false;
    f = nc_cfg_open_ex("tasks", &exists);
    if (f != NULL) {
        (void)nc_cfg_apply_tasks(nc_cfg_root(f), &s_tasks, &s_tasks_stat);
        nc_cfg_close(f);
    } else {
        (void)nc_cfg_apply_tasks(NULL, &s_tasks, &s_tasks_stat);
        if (exists) {
            s_tasks_stat.missing = 0;
            s_tasks_stat.bad_json = 1;
        }
    }

    s_loaded = true;

    ESP_LOGI(TAG, "system: links=%u routes=%u%s%s%s（def=%u inv=%u unk=%u）",
             (unsigned)s_sys.n_links, (unsigned)s_sys.n_routes,
             s_sys_stat.missing ? " 缺檔" : "",
             s_sys_stat.bad_json ? " 壞檔" : "",
             s_sys_stat.bad_version ? " 版本不符" : "",
             (unsigned)s_sys_stat.defaulted, (unsigned)s_sys_stat.invalid,
             (unsigned)s_sys_stat.unknown);
    ESP_LOGI(TAG, "tasks: %u 條%s%s（def=%u inv=%u unk=%u）",
             (unsigned)s_tasks.n_tasks,
             s_tasks_stat.missing ? " 缺檔" : "",
             s_tasks_stat.bad_json ? " 壞檔" : "",
             (unsigned)s_tasks_stat.defaulted, (unsigned)s_tasks_stat.invalid,
             (unsigned)s_tasks_stat.unknown);
    return ESP_OK;
}

void nc_cfg_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    snprintf(buf, cap,
             "{\"sys\":{\"miss\":%u,\"bad\":%u,\"ver\":%u,\"def\":%u,\"inv\":%u,\"unk\":%u},"
             "\"tasks\":{\"miss\":%u,\"bad\":%u,\"ver\":%u,\"def\":%u,\"inv\":%u,\"unk\":%u}}",
             (unsigned)s_sys_stat.missing, (unsigned)s_sys_stat.bad_json,
             (unsigned)s_sys_stat.bad_version, (unsigned)s_sys_stat.defaulted,
             (unsigned)s_sys_stat.invalid, (unsigned)s_sys_stat.unknown,
             (unsigned)s_tasks_stat.missing, (unsigned)s_tasks_stat.bad_json,
             (unsigned)s_tasks_stat.bad_version, (unsigned)s_tasks_stat.defaulted,
             (unsigned)s_tasks_stat.invalid, (unsigned)s_tasks_stat.unknown);
}
