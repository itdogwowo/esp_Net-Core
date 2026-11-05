#include "nc_cfg.h"

#include <unistd.h>

#include "nc_cfg_patch.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "nc_cfg";

#define CFG_DIR   "/data/config"
#define CFG_MAX   (16 * 1024)

struct cfg_file {
    cJSON *root;
};

static bool path_of(const char *layer, char *out, size_t cap, const char *suffix)
{
    if (layer == NULL || out == NULL || cap == 0) {
        return false;
    }
    const int n = snprintf(out, cap, CFG_DIR "/%s.json%s", layer, suffix ? suffix : "");
    return n > 0 && (size_t)n < cap;
}

cfg_file_t *nc_cfg_open(const char *layer)
{
    char path[64];
    if (!path_of(layer, path, sizeof(path), NULL)) {
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;    // 缺檔 = 用預設
    }
    // 只配置「檔案實際大小 + 1」：以前無條件要 16 KB，喺內部 RAM 碎片化時
    // 會無聲失敗（回 NULL → 上層以為「冇 config」而靜靜用預設）。
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long sz = ftell(f);
    if (sz <= 0 || sz >= CFG_MAX) {
        fclose(f);
        ESP_LOGW(TAG, "%s 大小 %ld 不合理（上限 %d）", path, sz, CFG_MAX);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        ESP_LOGW(TAG, "%s 配置 %ld B 失敗（heap 不足）", path, sz);
        return NULL;
    }
    const size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "%s 不是合法 JSON → 用預設", path);
        return NULL;
    }

    cfg_file_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cfg->root = root;
    return cfg;
}

void nc_cfg_close(cfg_file_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cJSON_Delete(cfg->root);
    free(cfg);
}

const cJSON *nc_cfg_root(const cfg_file_t *cfg)
{
    return cfg ? cfg->root : NULL;
}

const char *nc_cfg_str(const cJSON *root, const char *key, const char *def)
{
    if (root == NULL || key == NULL) {
        return def;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        return v->valuestring;
    }
    return def;
}

int nc_cfg_int(const cJSON *root, const char *key, int def)
{
    if (root == NULL || key == NULL) {
        return def;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    return def;
}


bool nc_cfg_set_string(const char *layer, const char *key, const char *value)
{
    char path[64], tmp[72], bak[80];
    if (key == NULL || value == NULL ||
        !path_of(layer, path, sizeof(path), NULL) ||
        !path_of(layer, tmp, sizeof(tmp), ".tmp") ||
        !path_of(layer, bak, sizeof(bak), ".bak")) {
        return false;
    }

    // 先試「只換值」：保留使用者的縮排、鍵順序、_comment 位置
    char *text = NULL;
    size_t text_len = 0;
    bool patched = false;
    bool from_cjson = false;
    FILE *rf = fopen(path, "rb");
    if (rf != NULL) {
        char *src = malloc(CFG_MAX);
        if (src != NULL) {
            const size_t got = fread(src, 1, CFG_MAX - 1, rf);
            src[got] = '\0';
            char *dst = malloc(CFG_MAX);
            if (dst != NULL && nc_cfg_patch_value(src, key, value, dst, CFG_MAX, &text_len)) {
                text = dst;
                patched = true;
            } else {
                free(dst);
            }
            if (!patched) {
                free(src);
            }
        }
        fclose(rf);
    }

    if (!patched) {
        // fallback：沒有這個鍵（或檔案不存在）→ cJSON 建/改後整份重印
        cfg_file_t *cfg = nc_cfg_open(layer);
        cJSON *root = cfg ? cfg->root : cJSON_CreateObject();
        if (root == NULL) {
            return false;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(root, key);
        cJSON_AddStringToObject(root, key, value);
        text = cJSON_Print(root);
        text_len = text ? strlen(text) : 0;
        from_cjson = true;
        if (cfg != NULL) {
            cfg->root = NULL;      // root 由這裡釋放
            nc_cfg_close(cfg);
        }
        cJSON_Delete(root);
    }

    bool ok = false;
    if (text != NULL) {
        FILE *f = fopen(tmp, "wb");
        if (f != NULL) {
            ok = fwrite(text, 1, text_len, f) == text_len;
            if (ok) {
                fsync(fileno(f));   // 舊專案 os.sync() 的教訓：reset 前要落盤
            }
            fclose(f);
        }
        if (from_cjson) {
            cJSON_free(text);
        } else {
            free(text);
        }
    }

    if (ok) {
        remove(bak);
        rename(path, bak);
        if (rename(tmp, path) != 0) {
            rename(bak, path);
            ok = false;
        }
    }
    if (!ok) {
        remove(tmp);
    }

    if (ok) {
        ESP_LOGI(TAG, "%s.%s = \"%s\"（已寫回，%s）", layer, key, value,
                 patched ? "保留原排版" : "重印");
    } else {
        ESP_LOGW(TAG, "%s.%s 寫回失敗（記憶體值仍生效）", layer, key, value);
    }
    return ok;
}

// 找 array 的結尾 ']'（跳過字串內容與巢狀），回傳指向 ']' 的指標。
static const char *find_array_end(const char *lb)
{
    bool instr = false, esc = false;
    int depth = 0;
    for (const char *p = lb; *p != '\0'; p++) {
        const char c = *p;
        if (instr) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                instr = false;
            }
            continue;
        }
        if (c == '"') {
            instr = true;
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
            if (depth == 0) {
                return p;
            }
        }
    }
    return NULL;
}

bool nc_cfg_append_obj(const char *layer, const char *array_key, const char *obj_json)
{
    char path[64], tmp[72], bak[80];
    if (layer == NULL || array_key == NULL || obj_json == NULL ||
        !path_of(layer, path, sizeof(path), NULL) ||
        !path_of(layer, tmp, sizeof(tmp), ".tmp") ||
        !path_of(layer, bak, sizeof(bak), ".bak")) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    char *src = malloc(CFG_MAX);
    if (src == NULL) {
        fclose(f);
        return false;
    }
    const size_t n = fread(src, 1, CFG_MAX - 1, f);
    src[n] = '\0';
    // 檔案比 CFG_MAX-1 大：讀進來的只是前半段，JSON 一定不完整。
    // 以前會照樣在殘缺的字串裡找 key，找到就據此寫出一份壞檔。
    const bool too_big = (n == CFG_MAX - 1) && (fgetc(f) != EOF);
    fclose(f);
    if (too_big) {
        ESP_LOGE(TAG, "%s 超過 %d B 上限 → 不追加（請先縮細個檔）", path, CFG_MAX - 1);
        free(src);
        return false;
    }

    char key[48];
    snprintf(key, sizeof(key), "\"%s\"", array_key);
    // 要找的是「key 後面接 : 再接 [」那一個，不是第一個同名子串 ——
    // 字串值裡出現 "tasks" 也會命中，以前的 strstr 就是這樣中招。
    const char *lb = NULL;
    for (const char *k = strstr(src, key); k != NULL; k = strstr(k + 1, key)) {
        const char *p = k + strlen(key);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p != ':') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '[') {
            lb = p;
            break;
        }
    }
    const char *rb = (lb != NULL) ? find_array_end(lb) : NULL;
    if (rb == NULL) {
        free(src);
        return false;
    }

    // 插在最後一個非空白字元之後（保留 `  ]` 的縮排）
    const char *ins = rb;
    while (ins > lb && isspace((unsigned char)ins[-1])) {
        ins--;
    }
    const bool empty = (ins == lb + 1);

    char *out = malloc(CFG_MAX);
    if (out == NULL) {
        free(src);
        return false;
    }
    size_t off = 0;
    memcpy(out, src, (size_t)(ins - src));
    off = (size_t)(ins - src);
    if (!empty) {
        out[off++] = ',';
    }
    const char *lead = "\n    ";
    memcpy(out + off, lead, 5);
    off += 5;
    const size_t objlen = strlen(obj_json);
    memcpy(out + off, obj_json, objlen);
    off += objlen;
    const size_t restlen = strlen(ins);
    memcpy(out + off, ins, restlen);
    off += restlen;
    out[off] = '\0';
    free(src);

    bool ok = false;
    FILE *w = fopen(tmp, "wb");
    if (w != NULL) {
        ok = fwrite(out, 1, off, w) == off;
        if (ok) {
            fsync(fileno(w));
        }
        fclose(w);
    }
    free(out);

    if (ok) {
        remove(bak);
        rename(path, bak);
        if (rename(tmp, path) != 0) {
            rename(bak, path);
            ok = false;
        }
    }
    if (!ok) {
        remove(tmp);
    }
    if (ok) {
        ESP_LOGI(TAG, "%s.%s += %s", layer, array_key, obj_json);
    } else {
        ESP_LOGW(TAG, "%s.%s 補寫失敗", layer, array_key);
    }
    return ok;
}
