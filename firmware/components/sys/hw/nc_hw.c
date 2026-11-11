#include "nc_hw.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/queue.h"

static const char *TAG = "nc_hw";

typedef struct {
    bool          used;
    char          name[NC_HW_NAME_MAX];
    QueueHandle_t q;
    _Atomic uint32_t seq;
    uint16_t      len[2];
    uint8_t       buf[2][NC_HW_SNAP_MAX];
} hw_res_t;

typedef struct {
    int      pin;
    char     owner[NC_HW_NAME_MAX];
    char     label[NC_HW_NAME_MAX];
    uint32_t flags;
} hw_claim_t;

static hw_res_t   s_res[NC_HW_MAX];
static hw_claim_t s_claim[NC_HW_CLAIM_MAX];
static int        s_nclaims;
static bool       s_ok = true;
static bool       s_inited;

static hw_res_t *find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < NC_HW_MAX; i++) {
        if (s_res[i].used && strcmp(s_res[i].name, name) == 0) {
            return &s_res[i];
        }
    }
    return NULL;
}

esp_err_t nc_hw_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    memset(s_res, 0, sizeof(s_res));
    memset(s_claim, 0, sizeof(s_claim));
    s_nclaims = 0;
    s_ok = true;
    s_inited = true;
    return ESP_OK;
}

void nc_hw_reset(void)
{
    for (int i = 0; i < NC_HW_MAX; i++) {
        if (s_res[i].used && s_res[i].q != NULL) {
            vQueueDelete(s_res[i].q);
        }
    }
    memset(s_res, 0, sizeof(s_res));
    memset(s_claim, 0, sizeof(s_claim));
    s_nclaims = 0;
    s_ok = true;
    s_inited = false;
}

esp_err_t nc_hw_register(const char *name)
{
    if (!s_inited) {
        (void)nc_hw_init();
    }
    if (name == NULL || name[0] == '\0' || strlen(name) >= NC_HW_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find(name) != NULL) {
        ESP_LOGE(TAG, "resource 重名：%s", name);
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < NC_HW_MAX; i++) {
        if (s_res[i].used) {
            continue;
        }
        QueueHandle_t q = xQueueCreate(NC_HW_CMD_MAX, sizeof(nc_hw_cmd_t));
        if (q == NULL) {
            ESP_LOGE(TAG, "resource %s：queue 建立失敗", name);
            return ESP_ERR_NO_MEM;
        }
        s_res[i].used = true;
        snprintf(s_res[i].name, sizeof(s_res[i].name), "%s", name);
        s_res[i].q = q;
        atomic_store(&s_res[i].seq, 0);
        s_res[i].len[0] = 0;
        s_res[i].len[1] = 0;
        ESP_LOGI(TAG, "resource 登記：%s", name);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "resource registry 滿（%d）→ %s 登記失敗", NC_HW_MAX, name);
    return ESP_ERR_NO_MEM;
}

bool nc_hw_recv(const char *name, nc_hw_cmd_t *cmd, uint32_t timeout_ms)
{
    hw_res_t *r = find(name);
    if (r == NULL || cmd == NULL) {
        return false;
    }
    return xQueueReceive(r->q, cmd, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

esp_err_t nc_hw_send(const char *name, const nc_hw_cmd_t *cmd)
{
    hw_res_t *r = find(name);
    if (r == NULL || cmd == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xQueueSend(r->q, cmd, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nc_hw_publish(const char *name, const void *data, size_t len)
{
    hw_res_t *r = find(name);
    if (r == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > NC_HW_SNAP_MAX) {
        ESP_LOGE(TAG, "snapshot 太大（%u > %u）：%s", (unsigned)len,
                 (unsigned)NC_HW_SNAP_MAX, name);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t s = atomic_load_explicit(&r->seq, memory_order_relaxed);
    atomic_store_explicit(&r->seq, s + 1, memory_order_release);   // odd = writing
    // 兩個 buffer 輪替：s 每次 +2；第一個 publish 寫 idx 1，第二個寫 0。
    const int idx = (int)(((s >> 1) + 1u) & 1u);
    memcpy(r->buf[idx], data, len);
    r->len[idx] = (uint16_t)len;
    atomic_store_explicit(&r->seq, s + 2, memory_order_release);   // even = stable
    return ESP_OK;
}

size_t nc_hw_snapshot(const char *name, void *out, size_t cap)
{
    hw_res_t *r = find(name);
    if (r == NULL || out == NULL || cap == 0) {
        return 0;
    }
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t s1 = atomic_load_explicit(&r->seq, memory_order_acquire);
        if (s1 & 1u) {
            continue;
        }
        const int idx = (int)((s1 >> 1) & 1u);
        const size_t n = r->len[idx];
        const size_t c = (n < cap) ? n : cap;
        memcpy(out, r->buf[idx], c);
        const uint32_t s2 = atomic_load_explicit(&r->seq, memory_order_acquire);
        if (s1 == s2) {
            return c;
        }
    }
    return 0;
}

bool nc_hw_reserved(int pin)
{
#if CONFIG_IDF_TARGET_ESP32S3
    // N16R8：26–32 flash、33–37 octal PSRAM；19/20 USB；43/44 console；0/3/45/46 strapping。
    if (pin >= 26 && pin <= 37) {
        return true;
    }
    if (pin == 19 || pin == 20 || pin == 43 || pin == 44) {
        return true;
    }
    if (pin == 0 || pin == 3 || pin == 45 || pin == 46) {
        return true;
    }
#endif
    return pin < 0;
}

esp_err_t nc_hw_claim(int pin, const char *owner, const char *label, uint32_t flags)
{
    if (!s_inited) {
        (void)nc_hw_init();
    }
    if (pin < 0 || owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_hw_reserved(pin)) {
        ESP_LOGE(TAG, "claim pin %d（%s）→ 保留腳位，拒絕", pin, owner);
        s_ok = false;
        return ESP_ERR_INVALID_STATE;
    }
    if (flags == 0) {
        flags = NC_HW_CLAIM_WRITE;
    }
    for (int i = 0; i < s_nclaims; i++) {
        if (s_claim[i].pin != pin) {
            continue;
        }
        if ((s_claim[i].flags & NC_HW_CLAIM_WRITE) && (flags & NC_HW_CLAIM_WRITE) &&
            strcmp(s_claim[i].owner, owner) != 0) {
            ESP_LOGE(TAG, "claim pin %d 衝突：%s 已寫，%s 再寫",
                     pin, s_claim[i].owner, owner);
            s_ok = false;
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_nclaims >= NC_HW_CLAIM_MAX) {
        ESP_LOGE(TAG, "claim 表滿（%d）", NC_HW_CLAIM_MAX);
        s_ok = false;
        return ESP_ERR_NO_MEM;
    }
    hw_claim_t *c = &s_claim[s_nclaims++];
    c->pin = pin;
    snprintf(c->owner, sizeof(c->owner), "%s", owner);
    snprintf(c->label, sizeof(c->label), "%s", (label != NULL) ? label : "");
    c->flags = flags;
    return ESP_OK;
}

bool nc_hw_claim_ok(void)
{
    return s_ok;
}

bool nc_hw_pin_claimed_by_other(int pin, const char *owner)
{
    for (int i = 0; i < s_nclaims; i++) {
        if (s_claim[i].pin != pin) {
            continue;
        }
        if (owner == NULL || strcmp(s_claim[i].owner, owner) != 0) {
            return true;
        }
    }
    return false;
}

size_t nc_hw_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += (size_t)snprintf(out + off, (off < cap) ? cap - off : 0,
                            "{\"resources\":{");
    bool first = true;
    for (int i = 0; i < NC_HW_MAX; i++) {
        if (!s_res[i].used) {
            continue;
        }
        off += (size_t)snprintf(out + off, (off < cap) ? cap - off : 0,
                                "%s\"%s\":%u", first ? "" : ",",
                                s_res[i].name,
                                (unsigned)uxQueueMessagesWaiting(s_res[i].q));
        first = false;
    }
    off += (size_t)snprintf(out + off, (off < cap) ? cap - off : 0,
                            "},\"claims\":[");
    for (int i = 0; i < s_nclaims; i++) {
        off += (size_t)snprintf(out + off, (off < cap) ? cap - off : 0,
                                "%s{\"pin\":%d,\"owner\":\"%s\",\"label\":\"%s\","
                                "\"flags\":%u}",
                                (i == 0) ? "" : ",",
                                s_claim[i].pin, s_claim[i].owner, s_claim[i].label,
                                (unsigned)s_claim[i].flags);
    }
    off += (size_t)snprintf(out + off, (off < cap) ? cap - off : 0, "]}");
    return (off < cap) ? off : cap;
}
