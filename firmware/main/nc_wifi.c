// Wi-Fi 通道的實作：兩道閘（編譯期 / 執行期），未用時一個 byte 都唔配。
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

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
                            "\"enabled\":0,\"ssid\":\"\",\"ip\":0,\"rssi\":0}");
}

// ───────────────────────── 已編入：兩道閘都在才配置 ─────────────────────────
#else

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static nc_wifi_state_t s_state = NC_WIFI_OFF_COMPILE;
static esp_netif_t *s_netif;
static bool s_inited;
static char s_ssid[33];
static bool s_got_ip;
static int s_rssi;   // IDF 的 esp_wifi_sta_get_rssi 要 int*，不是 int8_t*

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_got_ip = false;
        ESP_LOGW(TAG, "Wi-Fi 斷線 → 重試（之後由 failure policy 接管）");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_got_ip = true;
        ESP_LOGW(TAG, "Wi-Fi IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

// 任何一步失敗就把已經要到的東西還回去 —— 「唔用就唔好長期霸佔」。
static void wifi_release(void)
{
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
}

// 讀 network.json 的 wifi 段。字串一律**複製**出嚟先 close，
// 唔可以借 cJSON 的指標（router_from_cfg 就是踩過這個）。
static bool cfg_read(char *ssid, size_t cap, char *pw, size_t pwcap,
                     char *mode, size_t modecap, bool *enabled)
{
    cfg_file_t *cfg = nc_cfg_open("network");
    if (cfg == NULL) {
        return false;
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(nc_cfg_root(cfg), "wifi");
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(w, "enable");
    *enabled = cJSON_IsNumber(en) ? (en->valueint != 0) : cJSON_IsTrue(en);
    snprintf(ssid, cap, "%s", nc_cfg_str(w, "ssid", ""));
    snprintf(pw, pwcap, "%s", nc_cfg_str(w, "ssid_pw", ""));
    snprintf(mode, modecap, "%s", nc_cfg_str(w, "mode", "sta"));
    nc_cfg_close(cfg);
    return true;
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
    if (ssid[0] == '\0') {
        s_state = NC_WIFI_OFF_NO_CRED;
        ESP_LOGW(TAG, "Wi-Fi：enable=1 但 ssid 空白 → 唔初始化（0 B heap）");
        return s_state;
    }
    if (strcmp(mode, "sta") != 0) {
        s_state = NC_WIFI_OFF_UNSUPPORTED;
        ESP_LOGW(TAG, "Wi-Fi：mode=\"%s\" 未實作（目前只支援 sta）→ 唔初始化", mode);
        return s_state;
    }

    // ── 到這裡才開始要記憶體 ──
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
    s_inited = true;                       // 之後的失敗要 deinit

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL);

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pw);
    wc.sta.threshold.authmode = (pw[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        goto fail;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wc)) != ESP_OK) {
        goto fail;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        goto fail;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_state = NC_WIFI_ON;
    ESP_LOGW(TAG, "Wi-Fi 啟動：SSID=\"%s\"（驅動執行期約 90–120 KB heap）", s_ssid);
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
    if (s_state == NC_WIFI_ON) {
        esp_wifi_sta_get_rssi(&s_rssi);
    }
    return (size_t)snprintf(buf, cap,
                            "{\"compiled\":1,\"state\":\"%s\",\"enabled\":%d,"
                            "\"ssid\":\"%s\",\"ip\":%d,\"rssi\":%d}",
                            nc_wifi_state_name(s_state), s_state == NC_WIFI_ON ? 1 : 0,
                            s_state == NC_WIFI_ON ? s_ssid : "", s_got_ip ? 1 : 0,
                            (int)s_rssi);
}

#endif  // CONFIG_NETCORE_WIFI
