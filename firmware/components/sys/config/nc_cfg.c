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

// ── boot 期間的原始 JSON hash（避免 state sync 重讀一次 13 KB）──
static uint32_t s_raw_all;
static uint32_t s_raw_hw_drv;
static uint16_t s_raw_files;
static bool s_raw_active;

// 表驅動 CRC32（IEEE）：byte-wise bit loop 喺 13 KB 要 ~4.6 ms（實測），
// 換 256-entry 表後 ~0.1 ms；表係第一次用時計（1 KB static，只 boot 用）。
static uint32_t s_crc_tbl[256];
static bool s_crc_tbl_ok;

static void crc_tbl_init(void)
{
    if (s_crc_tbl_ok) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        }
        s_crc_tbl[i] = c;
    }
    s_crc_tbl_ok = true;
}

static uint32_t raw_crc(uint32_t crc, const void *p, size_t n)
{
    if (!s_crc_tbl_ok) {
        crc_tbl_init();
    }
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        crc = (crc >> 8) ^ s_crc_tbl[(crc ^ b[i]) & 0xFFu];
    }
    return crc;
}

void nc_cfg_raw_hash_begin(void)
{
    s_raw_all = 0xFFFFFFFFu;
    s_raw_hw_drv = 0xFFFFFFFFu;
    s_raw_files = 0;
    s_raw_active = true;
}

void nc_cfg_raw_hash_end(void)
{
    s_raw_active = false;
}

uint32_t nc_cfg_raw_hash(void)
{
    return ~s_raw_all;
}

uint32_t nc_cfg_raw_hash_hw_drv(void)
{
    return ~s_raw_hw_drv;
}

uint16_t nc_cfg_raw_hash_files(void)
{
    return s_raw_files;
}

static void raw_hash_feed(const char *layer, const char *buf, size_t n)
{
    if (!s_raw_active || layer == NULL || buf == NULL) {
        return;
    }
    // 名要同 ncportgen 一致：<layer>.json（例如 hardware.json），唔係 layer 名。
    char name[32];
    const int nn = snprintf(name, sizeof(name), "%s.json", layer);
    if (nn <= 0 || (size_t)nn >= sizeof(name)) {
        return;
    }
    const size_t ln = (size_t)nn;
    s_raw_all = raw_crc(s_raw_all, name, ln);
    s_raw_all = raw_crc(s_raw_all, "\0", 1);
    s_raw_all = raw_crc(s_raw_all, buf, n);
    if (strcmp(layer, "hardware") == 0 || strcmp(layer, "driver") == 0) {
        s_raw_hw_drv = raw_crc(s_raw_hw_drv, name, ln);
        s_raw_hw_drv = raw_crc(s_raw_hw_drv, "\0", 1);
        s_raw_hw_drv = raw_crc(s_raw_hw_drv, buf, n);
    }
    s_raw_files++;
}

static bool path_of(const char *layer, char *out, size_t cap, const char *suffix)
{
    if (layer == NULL || out == NULL || cap == 0) {
        return false;
    }
    const int n = snprintf(out, cap, CFG_DIR "/%s.json%s", layer, suffix ? suffix : "");
    return n > 0 && (size_t)n < cap;
}

cfg_file_t *nc_cfg_open_ex(const char *layer, bool *exists)
{
    if (exists != NULL) {
        *exists = false;
    }
    char path[64];
    if (!path_of(layer, path, sizeof(path), NULL)) {
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;    // 缺檔 = 用預設（exists 保持 false）
    }
    if (exists != NULL) {
        *exists = true;   // 檔案在：之後失敗就係「壞檔」而不是「缺檔」
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
    raw_hash_feed(layer, buf, n);

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

cfg_file_t *nc_cfg_open(const char *layer)
{
    return nc_cfg_open_ex(layer, NULL);
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

// link kind 的名字（"cdc" / "usb_jtag" …）。給 log 與 STATUS 用：
// config 檔、`kind_parse()`、這裡三處要一致，否則使用者睇到嘅名同寫嘅名唔同。
const char *nc_cfg_kind_name(nc_link_kind_t kind)
{
    switch (kind) {
    case NC_LINK_CDC:     return "cdc";
    case NC_LINK_UART:    return "uart";
    case NC_LINK_TCP:     return "tcp";
    case NC_LINK_WS:      return "ws";
    case NC_LINK_UDP:     return "udp";
    case NC_LINK_HTTP:    return "http";
    case NC_LINK_USBJTAG: return "usb_jtag";
    default:              return "?";
    }
}

const char *nc_cfg_iface_name(uint8_t iface)
{
    switch (iface) {
    case NC_CFG_IFACE_ETH:  return "eth";
    case NC_CFG_IFACE_WIFI: return "wifi";
    default:            return "any";
    }
}

const char *nc_cfg_service_name(uint8_t service)
{
    switch (service) {
    case NC_CFG_SVC_NC4: return "nc4";
    case NC_CFG_SVC_WEB: return "web";
    default:         return "auto";
    }
}

const char *nc_cfg_mem_name(uint8_t mem)
{
    switch (mem) {
    case NC_CFG_MEM_SRAM:  return "sram";
    case NC_CFG_MEM_PSRAM: return "psram";
    default:           return "auto";
    }
}

const char *nc_cfg_phy_name(uint8_t phy)
{
    switch (phy) {
    case NC_CFG_PHY_IP101:   return "IP101";
    case NC_CFG_PHY_LAN8720: return "LAN8720";
    case NC_CFG_PHY_DP83848: return "DP83848";
    case NC_CFG_PHY_RTL8201: return "RTL8201";
    case NC_CFG_PHY_GENERIC: return "generic";
    default:             return "unknown";
    }
}
