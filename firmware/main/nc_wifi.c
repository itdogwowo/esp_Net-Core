// Wi-Fi 通道的實作：兩道閘（編譯期 / 執行期），未用時一個 byte 都唔配。
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "nc_cfg.h"
#include "nc_wifi.h"

static const char *TAG = "wifi";

const char *nc_wifi_state_name(nc_wifi_state_t s)
{
    switch (s) {
    case NC_WIFI_OFF_COMPILE:     return "off_compile";
    case NC_WIFI_OFF_CONFIG:      return "off_config";
    case NC_WIFI_OFF_NO_CRED:     return "off_no_cred";
    case NC_WIFI_OFF_UNSUPPORTED: return "off_unsupported";
    case NC_WIFI_ON:              return "on";
    case NC_WIFI_ERROR:           return "error";
    default:                      return "?";
    }
}

// ───────────────────────── 未編入：同一個介面，零成本 ─────────────────────────
#if !CONFIG_NETCORE_WIFI

nc_wifi_state_t nc_wifi_boot(void)
{
    ESP_LOGI(TAG, "Wi-Fi：未編入（CONFIG_NETCORE_WIFI=n）→ 唔初始化、唔佔記憶體");
    return NC_WIFI_OFF_COMPILE;
}

bool nc_wifi_used(void)
{
    return false;
}

size_t nc_wifi_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return (size_t)snprintf(buf, cap,
                            "{\"compiled\":0,\"state\":\"off_compile\","
                            "\"enabled\":0,\"ssid\":\"\",\"ip\":0,\"ip_str\":\"0.0.0.0\","
                            "\"rssi\":0,\"cred\":0,\"scanning\":0}");
}

int nc_wifi_cred_count(void) { return 0; }
int nc_wifi_cred_ssids(char out[][33], int max) { (void)out; (void)max; return 0; }
esp_err_t nc_wifi_cred_add(const char *ssid, const char *pw)
{
    (void)ssid; (void)pw;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t nc_wifi_cred_remove(const char *ssid)
{
    (void)ssid;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t nc_wifi_cred_clear(void) { return ESP_ERR_NOT_SUPPORTED; }
size_t nc_wifi_cred_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return (size_t)snprintf(buf, cap, "{\"count\":0,\"ssids\":[]}");
}

// ───────────────────────── 已編入：兩道閘都在才配置 ─────────────────────────
#else

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#define WIFI_CRED_MAX       NC_WIFI_CRED_MAX
#define WIFI_CRED_MAGIC     0x57494649u   // "WIFI"
#define WIFI_CRED_VER       1
#define WIFI_CRED_NVS_NS    "nccred"
#define WIFI_CRED_NVS_KEY   "creds"
#define WIFI_CRED_MAX_SCAN  32

#define WIFI_BIT_GOT_IP     (1u << 0)
#define WIFI_BIT_FAIL       (1u << 1)

typedef struct {
    char ssid[33];
    char pw[65];
} wifi_cred_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t count;
    wifi_cred_t list[WIFI_CRED_MAX];
} wifi_cred_blob_t;

static nc_wifi_state_t s_state = NC_WIFI_OFF_COMPILE;
static esp_netif_t *s_netif;
static bool s_inited;
static char s_ssid[33];
static bool s_got_ip;
static int s_rssi;
static volatile bool s_connected;
static volatile bool s_scanning;
static EventGroupHandle_t s_evt;
static TaskHandle_t s_task;
static volatile bool s_wake;

static wifi_cred_t s_creds[WIFI_CRED_MAX];
static wifi_ap_record_t s_aps[WIFI_CRED_MAX_SCAN];   // 32 筆很大，唔好放 task stack
static int s_ncreds;
static bool s_cred_loaded;

// ── NVS 憑證 ────────────────────────────────────────────────────────────

static void cred_load_nvs(void)
{
    if (s_cred_loaded) {
        return;
    }
    s_cred_loaded = true;
    s_ncreds = 0;
    nvs_handle_t h;
    if (nvs_open(WIFI_CRED_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    static wifi_cred_blob_t b;      // 1 KB：唔放 task stack（handler stack 只有幾 KB）
    size_t len = sizeof(b);
    const esp_err_t e = nvs_get_blob(h, WIFI_CRED_NVS_KEY, &b, &len);
    nvs_close(h);
    if (e != ESP_OK || len < sizeof(uint32_t) * 2 ||
        b.magic != WIFI_CRED_MAGIC || b.ver != WIFI_CRED_VER) {
        return;
    }
    const int n = (b.count < WIFI_CRED_MAX) ? b.count : WIFI_CRED_MAX;
    for (int i = 0; i < n; i++) {
        b.list[i].ssid[sizeof(b.list[i].ssid) - 1] = '\0';
        b.list[i].pw[sizeof(b.list[i].pw) - 1] = '\0';
        if (b.list[i].ssid[0] != '\0') {
            s_creds[s_ncreds++] = b.list[i];
        }
    }
    ESP_LOGI(TAG, "NVS 憑證：%d 組", s_ncreds);
}

static esp_err_t cred_save_nvs(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(WIFI_CRED_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    static wifi_cred_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = WIFI_CRED_MAGIC;
    b.ver = WIFI_CRED_VER;
    b.count = (uint16_t)s_ncreds;
    for (int i = 0; i < s_ncreds; i++) {
        b.list[i] = s_creds[i];
    }
    e = nvs_set_blob(h, WIFI_CRED_NVS_KEY, &b, sizeof(b));
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

// 記憶體版本：move-to-end；滿了就丟最舊；persist=false 給 config 預設用。
static esp_err_t cred_add_mem(const char *ssid, const char *pw, bool persist)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= sizeof(s_creds[0].ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pw == NULL) {
        pw = "";
    }
    if (strlen(pw) >= sizeof(s_creds[0].pw)) {
        return ESP_ERR_INVALID_ARG;
    }
    int idx = -1;
    for (int i = 0; i < s_ncreds; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) {
            idx = i;
            break;
        }
    }
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.ssid, sizeof(c.ssid), "%s", ssid);
    snprintf(c.pw, sizeof(c.pw), "%s", pw);
    if (idx >= 0) {
        for (int i = idx; i + 1 < s_ncreds; i++) {
            s_creds[i] = s_creds[i + 1];
        }
        s_ncreds--;
    } else if (s_ncreds >= WIFI_CRED_MAX) {
        for (int i = 1; i < s_ncreds; i++) {
            s_creds[i - 1] = s_creds[i];
        }
        s_ncreds--;
    }
    s_creds[s_ncreds++] = c;
    if (persist) {
        return cred_save_nvs();
    }
    return ESP_OK;
}

int nc_wifi_cred_count(void)
{
#if CONFIG_NETCORE_WIFI
    cred_load_nvs();
#endif
    return s_ncreds;
}

int nc_wifi_cred_ssids(char out[][33], int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
#if CONFIG_NETCORE_WIFI
    cred_load_nvs();
#endif
    const int n = (s_ncreds < max) ? s_ncreds : max;
    for (int i = 0; i < n; i++) {
        const size_t l = strnlen(s_creds[i].ssid, 32);
        memcpy(out[i], s_creds[i].ssid, l);
        out[i][l] = '\0';
    }
    return n;
}

esp_err_t nc_wifi_cred_add(const char *ssid, const char *pw)
{
#if CONFIG_NETCORE_WIFI
    cred_load_nvs();
    s_wake = true;
#endif
    return cred_add_mem(ssid, pw, true);
}

esp_err_t nc_wifi_cred_remove(const char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_NETCORE_WIFI
    cred_load_nvs();
#endif
    for (int i = 0; i < s_ncreds; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) {
            for (int k = i; k + 1 < s_ncreds; k++) {
                s_creds[k] = s_creds[k + 1];
            }
            s_ncreds--;
#if CONFIG_NETCORE_WIFI
            s_wake = true;
#endif
            return cred_save_nvs();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nc_wifi_cred_clear(void)
{
    s_ncreds = 0;
#if CONFIG_NETCORE_WIFI
    s_wake = true;
#endif
    return cred_save_nvs();
}

size_t nc_wifi_cred_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
#if CONFIG_NETCORE_WIFI
    cred_load_nvs();
#endif
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(root, "count", s_ncreds);
    cJSON *arr = cJSON_AddArrayToObject(root, "ssids");
    for (int i = 0; i < s_ncreds; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_creds[i].ssid));
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == NULL) {
        return 0;
    }
    const size_t n = strlen(s);
    const size_t c = (n < cap - 1) ? n : cap - 1;
    memcpy(buf, s, c);
    buf[c] = '\0';
    cJSON_free(s);
    return c;
}

// ── 連線 / 掃描 ─────────────────────────────────────────────────────────

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        s_connected = false;
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, WIFI_BIT_FAIL);
        }
        ESP_LOGW(TAG, "Wi-Fi 斷線（manager 會重試）");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_got_ip = true;
        s_connected = true;
        if (s_evt != NULL) {
            xEventGroupSetBits(s_evt, WIFI_BIT_GOT_IP);
        }
        ESP_LOGW(TAG, "Wi-Fi IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static bool try_connect(const char *ssid, const char *pw, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0' || s_evt == NULL) {
        return false;
    }
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", (pw != NULL) ? pw : "");
    wc.sta.threshold.authmode = (wc.sta.password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_evt, WIFI_BIT_GOT_IP | WIFI_BIT_FAIL);
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e == ESP_OK) {
        e = esp_wifi_connect();
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "connect %s → %s", ssid, esp_err_to_name(e));
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_evt, WIFI_BIT_GOT_IP | WIFI_BIT_FAIL, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_BIT_GOT_IP) != 0) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        s_connected = true;
        esp_wifi_sta_get_rssi(&s_rssi);
        return true;
    }
    esp_wifi_disconnect();
    s_connected = false;
    return false;
}

static int scan_aps(wifi_ap_record_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > (uint16_t)max) {
        num = (uint16_t)max;
    }
    if (num == 0 || esp_wifi_scan_get_ap_records(&num, out) != ESP_OK) {
        return 0;
    }
    // RSSI 由強到弱
    for (int i = 0; i < (int)num; i++) {
        for (int k = i + 1; k < (int)num; k++) {
            if (out[k].rssi > out[i].rssi) {
                wifi_ap_record_t t = out[i];
                out[i] = out[k];
                out[k] = t;
            }
        }
    }
    return num;
}

static void wifi_mgr_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_ncreds == 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        s_scanning = true;
        const int n = scan_aps(s_aps, WIFI_CRED_MAX_SCAN);
        s_scanning = false;
        bool tried[WIFI_CRED_MAX] = {false};

        // ① 掃描得到、又有憑證的，按 RSSI 強弱試。
        for (int a = 0; a < n; a++) {
            const char *ssid = (const char *)s_aps[a].ssid;
            if (ssid[0] == '\0') {
                continue;
            }
            for (int c = 0; c < s_ncreds; c++) {
                if (tried[c] || strcmp(s_creds[c].ssid, ssid) != 0) {
                    continue;
                }
                tried[c] = true;
                ESP_LOGI(TAG, "掃描配對：%s（RSSI %d）", ssid, s_aps[a].rssi);
                if (try_connect(ssid, s_creds[c].pw, 15000)) {
                    (void)cred_add_mem(ssid, s_creds[c].pw, true);
                    goto connected;
                }
            }
        }

        // ② 掃描不到的（hidden）／其餘憑證：盲連一次。
        for (int c = 0; c < s_ncreds; c++) {
            if (tried[c]) {
                continue;
            }
            tried[c] = true;
            ESP_LOGI(TAG, "盲連：%s", s_creds[c].ssid);
            if (try_connect(s_creds[c].ssid, s_creds[c].pw, 15000)) {
                (void)cred_add_mem(s_creds[c].ssid, s_creds[c].pw, true);
                goto connected;
            }
        }

        s_wake = false;
        for (int i = 0; i < 30 && !s_wake; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        continue;

connected:
        s_connected = true;
        ESP_LOGW(TAG, "Wi-Fi 已連上：%s（憑證數 %d）", s_ssid, s_ncreds);
        s_wake = false;
    }
}

// 讀 network.json 的 wifi 段。字串一律**複製**出嚟先 close。
static bool cfg_read(char *ssid, size_t cap, char *pw, size_t pwcap,
                     char *mode, size_t modecap, bool *enabled)
{
    cfg_file_t *cfg = nc_cfg_open("network");
    if (cfg == NULL) {
        return false;
    }
    const cJSON *root = nc_cfg_root(cfg);
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(w, "enable");
    *enabled = cJSON_IsNumber(en) ? (en->valueint != 0) : cJSON_IsTrue(en);
    snprintf(ssid, cap, "%s", nc_cfg_str(w, "ssid", ""));
    // 舊專案風格：ssid_pw 為主，password_pw / password 作兼容。
    const char *p = nc_cfg_str(w, "ssid_pw", "");
    if (p[0] == '\0') p = nc_cfg_str(w, "password_pw", "");
    if (p[0] == '\0') p = nc_cfg_str(w, "password", "");
    snprintf(pw, pwcap, "%s", p);
    snprintf(mode, modecap, "%s", nc_cfg_str(w, "mode", "sta"));

    // 舊專案風格：config 內可以放 credentials[] 作初始清單；
    // 連線成功後會寫入 NVS，之後以 NVS 為準（最多 10 組）。
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(w, "credentials");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "ssid");
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "pw");
        if (cJSON_IsString(s) && cJSON_IsString(p)) {
            (void)cred_add_mem(s->valuestring, p->valuestring, false);
        }
    }
    nc_cfg_close(cfg);
    return true;
}

static void wifi_release(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_inited = false;
    }
    if (s_netif != NULL) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_got_ip = false;
    s_connected = false;
}

nc_wifi_state_t nc_wifi_boot(void)
{
    if (s_state == NC_WIFI_ON) {
        return s_state;
    }

    char ssid[33] = {0};
    char pw[65] = {0};
    char mode[8] = {0};
    bool enabled = false;

    cred_load_nvs();            // 先 NVS，cfg_read 再把 config 預設／credentials 疊上
    if (!cfg_read(ssid, sizeof(ssid), pw, sizeof(pw), mode, sizeof(mode), &enabled)) {
        s_state = NC_WIFI_OFF_CONFIG;
        ESP_LOGI(TAG, "Wi-Fi：讀不到 network.json → 唔初始化（0 B）");
        return s_state;
    }
    if (!enabled) {
        s_state = NC_WIFI_OFF_CONFIG;
        ESP_LOGI(TAG, "Wi-Fi：wifi.enable=0 → 唔初始化（0 B 靜態以外、0 B heap）");
        return s_state;
    }
    if (strcmp(mode, "sta") != 0) {
        s_state = NC_WIFI_OFF_UNSUPPORTED;
        ESP_LOGW(TAG, "Wi-Fi：mode=\"%s\" 未實作（目前只支援 sta）→ 唔初始化", mode);
        return s_state;
    }

    if (ssid[0] != '\0') {
        (void)cred_add_mem(ssid, pw, false);   // config 預設先入記憶體，成功才寫 NVS
    }
    if (s_ncreds == 0) {
        s_state = NC_WIFI_OFF_NO_CRED;
        ESP_LOGW(TAG, "Wi-Fi：enable=1 但冇任何 SSID／憑證 → 唔初始化（0 B heap）");
        return s_state;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&ic);
    if (err != ESP_OK) {
        goto fail;
    }
    s_inited = true;

    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL);

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        goto fail;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        goto fail;
    }
    // 測速／低延遲：關 modem sleep。代價係 idle 功耗上升；正式產品可再 config 化。
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if (xTaskCreatePinnedToCore(wifi_mgr_task, "wifi_mgr", 6144, NULL, 3, &s_task, 1) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_state = NC_WIFI_ON;
    ESP_LOGW(TAG, "Wi-Fi 啟動：manager 掃描／配對（憑證 %d 組；驅動約 90–120 KB heap）",
             s_ncreds);
    return s_state;

fail:
    ESP_LOGE(TAG, "Wi-Fi 初始化失敗 %s → 已回收，不留記憶體", esp_err_to_name(err));
    wifi_release();
    s_state = NC_WIFI_ERROR;
    return s_state;
}

bool nc_wifi_used(void)
{
    return s_state == NC_WIFI_ON;
}

size_t nc_wifi_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    char ip[16] = "0.0.0.0";
    if (s_state == NC_WIFI_ON && s_connected) {
        esp_wifi_sta_get_rssi(&s_rssi);
        esp_netif_ip_info_t info;
        if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    return (size_t)snprintf(buf, cap,
                            "{\"compiled\":1,\"state\":\"%s\",\"enabled\":%d,"
                            "\"ssid\":\"%s\",\"ip\":%d,\"ip_str\":\"%s\",\"rssi\":%d,"
                            "\"cred\":%d,\"scanning\":%d,\"connected\":%d}",
                            nc_wifi_state_name(s_state), s_state == NC_WIFI_ON ? 1 : 0,
                            (s_state == NC_WIFI_ON && s_connected) ? s_ssid : "",
                            s_got_ip ? 1 : 0, ip, (s_connected ? (int)s_rssi : 0),
                            s_ncreds, s_scanning ? 1 : 0, s_connected ? 1 : 0);
}

#endif  // CONFIG_NETCORE_WIFI
