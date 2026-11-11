#include "nc4_ota_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_http";

static void ota_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }
    if (req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image too large");
        return ESP_FAIL;
    }

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, req->content_len, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "begin: %s", esp_err_to_name(e));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(4096);
    if (buf == NULL) {
        esp_ota_abort(h);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t got = 0;
    while (got < req->content_len) {
        const size_t want = req->content_len - got;
        const int r = httpd_req_recv(req, (char *)buf, want > 4096 ? 4096 : want);
        if (r <= 0) {
            free(buf);
            esp_ota_abort(h);
            ESP_LOGE(TAG, "recv %d（%u/%u）", r, (unsigned)got, (unsigned)req->content_len);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        e = esp_ota_write(h, buf, (size_t)r);
        if (e != ESP_OK) {
            free(buf);
            esp_ota_abort(h);
            ESP_LOGE(TAG, "write: %s", esp_err_to_name(e));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write");
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    free(buf);

    e = esp_ota_end(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "end: %s", esp_err_to_name(e));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end");
        return ESP_FAIL;
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set boot: %s", esp_err_to_name(e));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA 完成 %u B，重啟", (unsigned)got);
    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_sendstr(req, "{\"ok\":1,\"reboot\":1}");
    (void)xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 3, NULL);
    return ESP_OK;
}

esp_err_t nc4_ota_http_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t uri = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post,
    };
    const esp_err_t e = httpd_register_uri_handler(server, &uri);
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "http://<ip>:8080/ota（POST raw .bin；LAN only）");
    }
    return e;
}
