#include "nc_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "tasks";

typedef struct {
    bool used;
    bool started;
    nc_task_def_t def;
    TaskHandle_t handle;
} nc_task_slot_t;

static nc_task_slot_t s_slots[NC_TASK_MAX];

static int hw_core_requirement(nc_task_hw_t hw)
{
    switch (hw) {
    case NC_TASK_HW_USB:
    case NC_TASK_HW_FLASH:
        // 這兩個周邊的 IDF driver 都綁在 CPU0 的 ISR 上；
        // 沿著 mp 版「LCD 只能 core0」的同一個理由，明確禁止排到 core1。
        return 0;
    case NC_TASK_HW_NONE:
    default:
        return NC_TASK_CORE_ANY;
    }
}

esp_err_t nc_task_register(const nc_task_def_t *def)
{
    if (def == NULL || def->name == NULL || def->fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (def->layer < 0 || def->layer >= NC_TASK_LAYERS) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < NC_TASK_MAX; i++) {
        if (s_slots[i].used && strcmp(s_slots[i].def.name, def->name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 硬體歸屬檢查（與 mp 版 _check_affinity 同語義：不 raise，改寫並警告）
    nc_task_def_t d = *def;
    const int req = hw_core_requirement(d.hw);
    if (req != NC_TASK_CORE_ANY && d.core != req) {
        ESP_LOGW(TAG, "task [%s] 綁核 %d → 強制 %d（硬體歸屬）", d.name, d.core, req);
        d.core = req;
    }

    for (size_t i = 0; i < NC_TASK_MAX; i++) {
        if (!s_slots[i].used) {
            s_slots[i].used = true;
            s_slots[i].def = d;
            ESP_LOGI(TAG, "register [%s] L%d core=%s stack=%u prio=%d",
                     d.name, d.layer,
                     d.core == NC_TASK_CORE_ANY ? "any" : (d.core == 0 ? "0" : "1"),
                     (unsigned)d.stack, (int)d.prio);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t start_slot(nc_task_slot_t *s)
{
    if (s->started) {
        return ESP_OK;
    }
    BaseType_t ok;
    if (s->def.core == NC_TASK_CORE_ANY) {
        ok = xTaskCreate(s->def.fn, s->def.name, s->def.stack, s->def.arg,
                         s->def.prio, &s->handle);
    } else {
        ok = xTaskCreatePinnedToCore(s->def.fn, s->def.name, s->def.stack, s->def.arg,
                                     s->def.prio, &s->handle, s->def.core);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task [%s] 建立失敗（stack=%u）", s->def.name, (unsigned)s->def.stack);
        return ESP_ERR_NO_MEM;
    }
    s->started = true;
    ESP_LOGI(TAG, "start [%s] L%d → core %d",
             s->def.name, s->def.layer, xTaskGetCoreID(s->handle));
    return ESP_OK;
}

esp_err_t nc_tasks_boot_layer(int layer)
{
    if (layer < 0 || layer >= NC_TASK_LAYERS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < NC_TASK_MAX; i++) {
        if (!s_slots[i].used || s_slots[i].def.layer != layer) {
            continue;
        }
        esp_err_t err = start_slot(&s_slots[i]);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

esp_err_t nc_tasks_boot(void)
{
    esp_err_t first_err = ESP_OK;
    for (int layer = 0; layer < NC_TASK_LAYERS; layer++) {
        esp_err_t err = nc_tasks_boot_layer(layer);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

int nc_task_running_core(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < NC_TASK_MAX; i++) {
        if (s_slots[i].used && strcmp(s_slots[i].def.name, name) == 0) {
            if (!s_slots[i].started) {
                return -1;
            }
            return xTaskGetCoreID(s_slots[i].handle);
        }
    }
    return -1;
}

size_t nc_tasks_status_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += snprintf(buf + off, cap - off, "[");
    bool first = true;
    for (size_t i = 0; i < NC_TASK_MAX && off < cap; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        const nc_task_def_t *d = &s_slots[i].def;
        const int core = s_slots[i].started ? xTaskGetCoreID(s_slots[i].handle) : -1;
        off += snprintf(buf + off, cap - off,
                        "%s{\"name\":\"%s\",\"layer\":%d,\"affinity\":%d,"
                        "\"running\":%s,\"core\":%d}",
                        first ? "" : ",", d->name, d->layer, d->core,
                        s_slots[i].started ? "true" : "false", core);
        first = false;
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]");
    }
    return off;
}
