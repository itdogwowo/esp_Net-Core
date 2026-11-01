#include "nc_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "svc";

typedef struct {
    const nc_service_t *svc;
    nc_res_req_t req[NC_SERVICE_MAX_RES];
    nc_res_view_t view[NC_SERVICE_MAX_RES];
    int nreq;
    int started;          // 1 = start() 成功
    int failed_res;       // >=0：第幾項資源拿不到
} svc_slot_t;

static svc_slot_t s_slots[NC_SERVICE_MAX];
static int s_count;

esp_err_t nc_service_add(const nc_service_t *svc)
{
    if (svc == NULL || svc->name == NULL || svc->declare == NULL || svc->start == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= NC_SERVICE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_slots[i].svc->name, svc->name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 依 level 插入（小的先）；同 level 保持登記順序
    int pos = s_count;
    while (pos > 0 && s_slots[pos - 1].svc->level > svc->level) {
        s_slots[pos] = s_slots[pos - 1];
        pos--;
    }
    memset(&s_slots[pos], 0, sizeof(s_slots[pos]));
    s_slots[pos].svc = svc;
    s_slots[pos].failed_res = -1;
    s_count++;

    ESP_LOGI(TAG, "register [%s] level=%d", svc->name, svc->level);
    return ESP_OK;
}

int nc_service_declare_all(void)
{
    int total = 0;
    for (int i = 0; i < s_count; i++) {
        svc_slot_t *s = &s_slots[i];
        const int n = s->svc->declare(s->req, NC_SERVICE_MAX_RES);
        s->nreq = n < 0 ? 0 : (n > NC_SERVICE_MAX_RES ? NC_SERVICE_MAX_RES : n);
        for (int k = 0; k < s->nreq; k++) {
            const nc_res_req_t *r = &s->req[k];
            const esp_err_t e = nc_plan_declare(&(nc_plan_decl_t){
                .service = s->svc->name, .name = r->name, .level = s->svc->level,
                .cls = r->cls, .bytes = r->bytes, .align = r->align,
            });
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "[%s] 宣告 %s 失敗 %s", s->svc->name, r->name,
                         esp_err_to_name(e));
            } else {
                total++;
            }
        }
    }
    ESP_LOGI(TAG, "宣告完成：%d 個服務、%d 項資源", s_count, total);
    return total;
}

int nc_service_start_all(void)
{
    int started = 0;
    for (int i = 0; i < s_count; i++) {
        svc_slot_t *s = &s_slots[i];
        s->failed_res = -1;
        for (int k = 0; k < s->nreq; k++) {
            s->view[k].ptr = nc_plan_get(s->req[k].name);
            s->view[k].len = s->req[k].bytes;
            if (s->view[k].ptr == NULL) {
                s->failed_res = k;
                ESP_LOGE(TAG, "[%s] 資源 %s（%u B）拿不到 → 這個服務不啟動",
                         s->svc->name, s->req[k].name, (unsigned)s->req[k].bytes);
                break;
            }
        }
        if (s->failed_res >= 0) {
            continue;
        }
        const esp_err_t e = s->svc->start(s->view, s->nreq);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "[%s] start 失敗 %s", s->svc->name, esp_err_to_name(e));
            continue;
        }
        s->started = 1;
        started++;
    }
    ESP_LOGI(TAG, "啟動完成：%d/%d 個服務", started, s_count);
    return started;
}

void nc_service_stop_all(void)
{
    for (int i = s_count - 1; i >= 0; i--) {
        svc_slot_t *s = &s_slots[i];
        if (!s->started) {
            continue;
        }
        if (s->svc->stop != NULL) {
            s->svc->stop();
        }
        s->started = 0;
        // 資源歸還：nc_plan 目前是「開機一次 carve、不釋放」，
        // 所以這裡只清 view；release 等 plan 支援之後再補。
        memset(s->view, 0, sizeof(s->view));
    }
}

size_t nc_service_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    off += snprintf(buf + off, cap - off, "[");
    for (int i = 0; i < s_count && off < cap; i++) {
        const svc_slot_t *s = &s_slots[i];
        off += snprintf(buf + off, cap - off,
                        "%s{\"name\":\"%s\",\"level\":%d,\"started\":%d,\"res\":[",
                        i == 0 ? "" : ",", s->svc->name, s->svc->level, s->started);
        for (int k = 0; k < s->nreq && off < cap; k++) {
            off += snprintf(buf + off, cap - off,
                            "%s{\"name\":\"%s\",\"bytes\":%u,\"ok\":%d}",
                            k == 0 ? "" : ",", s->req[k].name,
                            (unsigned)s->req[k].bytes,
                            s->view[k].ptr != NULL ? 1 : 0);
        }
        off += snprintf(buf + off, cap - off, "]}");
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]");
    }
    return off;
}
