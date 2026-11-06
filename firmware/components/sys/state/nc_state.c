#include "nc_state.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "state";

struct nc_state {
    nc_state_desc_t d;
    bool            dirty;
};

static struct nc_state s_pool[NC_STATE_MAX];
static int s_n;

static struct nc_state *find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_pool[i].d.name, name) == 0) {
            return &s_pool[i];
        }
    }
    return NULL;
}

const char *nc_state_persist_name(int idx)
{
    if (idx < 0 || idx >= s_n) {
        return "?";
    }
    switch (s_pool[idx].d.persist) {
    case NC_ST_PERSIST_NONE:
        return "none";
    case NC_ST_PERSIST_NVS:
        return "nvs";
    case NC_ST_PERSIST_FILE:
        return "file";
    default:
        return "?";
    }
}

nc_state_t *nc_state_add(const nc_state_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL || desc->value == NULL || desc->size == 0) {
        ESP_LOGE(TAG, "登記失敗：desc/name/value/size 不完整");
        return NULL;
    }
    struct nc_state *old = find(desc->name);
    if (old != NULL) {
        return old;   // 重複登記不是錯：回同一份
    }
    if (s_n >= NC_STATE_MAX) {
        ESP_LOGE(TAG, "登記失敗：超過 %d 個狀態（%s）", NC_STATE_MAX, desc->name);
        return NULL;
    }
    if (desc->persist == NC_ST_PERSIST_NVS &&
        (desc->nvs_ns == NULL || desc->nvs_key == NULL)) {
        ESP_LOGE(TAG, "登記失敗：%s 說要落 NVS 但沒有 namespace/key", desc->name);
        return NULL;
    }
    if (desc->persist == NC_ST_PERSIST_FILE && desc->save == NULL) {
        ESP_LOGE(TAG, "登記失敗：%s 說要落檔案但沒有 save()", desc->name);
        return NULL;
    }

    struct nc_state *s = &s_pool[s_n++];
    s->d = *desc;
    s->dirty = false;
    ESP_LOGI(TAG, "登記 %-14s persist=%s size=%u",
             s->d.name, nc_state_persist_name(s_n - 1), (unsigned)s->d.size);
    return s;
}

esp_err_t nc_state_load(void)
{
    esp_err_t first_err = ESP_OK;
    for (int i = 0; i < s_n; i++) {
        struct nc_state *s = &s_pool[i];
        if (s->d.persist != NC_ST_PERSIST_NVS) {
            continue;   // FILE 由各層自己的 loader 負責
        }
        nvs_handle_t h;
        esp_err_t err = nvs_open(s->d.nvs_ns, NVS_READONLY, &h);
        if (err != ESP_OK) {
            // namespace 未建立 = 全新的板子，不是錯誤
            continue;
        }
        size_t len = s->d.size;
        err = nvs_get_blob(h, s->d.nvs_key, s->d.value, &len);
        nvs_close(h);
        if (err == ESP_OK && len == s->d.size) {
            ESP_LOGI(TAG, "讀回 %s（%u B）", s->d.name, (unsigned)len);
        } else if (err != ESP_ERR_NVS_NOT_FOUND && first_err == ESP_OK) {
            // 大小不符也算壞掉：保留呼叫端的預設值
            if (err == ESP_OK) {
                err = ESP_ERR_INVALID_SIZE;
            }
            first_err = err;
            ESP_LOGW(TAG, "讀回 %s 失敗：%s → 保留預設", s->d.name, esp_err_to_name(err));
        }
    }
    return first_err;
}

esp_err_t nc_state_set(nc_state_t *s, const void *v, nc_state_writer_t by)
{
    if (s == NULL || v == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s->d.value, v, s->d.size);
    s->dirty = true;
    // 只做記錄：實際的「准不准寫」由呼叫點的規則決定（見各層 load/save）。
    if (by != s->d.writer) {
        ESP_LOGD(TAG, "%s 由 %d 寫入（宣告為 %d）", s->d.name, (int)by, (int)s->d.writer);
    }
    return ESP_OK;
}

void nc_state_touch(nc_state_t *s)
{
    if (s != NULL) {
        s->dirty = true;
    }
}

esp_err_t nc_state_commit(void)
{
    esp_err_t first_err = ESP_OK;
    for (int i = 0; i < s_n; i++) {
        struct nc_state *s = &s_pool[i];
        if (!s->dirty) {
            continue;
        }
        esp_err_t err = ESP_OK;
        if (s->d.persist == NC_ST_PERSIST_NVS) {
            nvs_handle_t h;
            err = nvs_open(s->d.nvs_ns, NVS_READWRITE, &h);
            if (err == ESP_OK) {
                err = nvs_set_blob(h, s->d.nvs_key, s->d.value, s->d.size);
                if (err == ESP_OK) {
                    err = nvs_commit(h);
                }
                nvs_close(h);
            }
        } else if (s->d.persist == NC_ST_PERSIST_FILE) {
            err = s->d.save(s->d.value, s->d.size);
        }
        if (err == ESP_OK) {
            s->dirty = false;
            ESP_LOGI(TAG, "落盤 %s", s->d.name);
        } else {
            if (first_err == ESP_OK) {
                first_err = err;
            }
            ESP_LOGE(TAG, "落盤 %s 失敗：%s", s->d.name, esp_err_to_name(err));
        }
    }
    return first_err;
}

int nc_state_count(void)
{
    return s_n;
}

const char *nc_state_name(int idx)
{
    return (idx >= 0 && idx < s_n) ? s_pool[idx].d.name : "?";
}

bool nc_state_is_dirty(int idx)
{
    return (idx >= 0 && idx < s_n) ? s_pool[idx].dirty : false;
}
