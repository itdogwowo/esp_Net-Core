#include "http_cfg.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "http_cfg";

#define CFG_PATH   "/data/config.json"
#define CFG_TMP    "/data/config.json.tmp"
#define CFG_MAX    (16 * 1024)

static esp_err_t cfg_get(httpd_req_t *req)
{
    FILE *f = fopen(CFG_PATH, "rb");
    if (f == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "config.json not found");
        return ESP_OK;
    }
    char *buf = malloc(CFG_MAX);
    if (buf == NULL) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    const size_t n = fread(buf, 1, CFG_MAX, f);
    fclose(f);

    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return r;
}

static esp_err_t cfg_put(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > CFG_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    body[got] = '\0';

    // ★ 先驗證：壞掉的 config 不寫進去（避免下次開機起不來）
    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        free(body);
        ESP_LOGW(TAG, "拒絕寫入：不是合法 JSON");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON_Delete(json);

    FILE *f = fopen(CFG_TMP, "wb");
    if (f == NULL) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open");
        return ESP_FAIL;
    }
    const size_t w = fwrite(body, 1, got, f);
    fclose(f);
    free(body);

    // FATFS 的 rename 不會覆蓋既有檔 → 先把舊檔改名成 .bak，再換上去
    if (w == got) {
        remove("/data/config.json.bak");
        rename(CFG_PATH, "/data/config.json.bak");
        if (rename(CFG_TMP, CFG_PATH) != 0) {
            rename("/data/config.json.bak", CFG_PATH);   // 回復舊檔
        }
    }
    if (w != got || access(CFG_PATH, F_OK) != 0) {
        remove(CFG_TMP);
        ESP_LOGE(TAG, "寫入失敗（%u/%u）", (unsigned)w, (unsigned)got);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "config.json 已更新（%u B）", (unsigned)w);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":1,\"next_boot\":1}");
}

esp_err_t http_cfg_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t get_uri = {
        .uri = "/config.json", .method = HTTP_GET, .handler = cfg_get,
    };
    const httpd_uri_t put_uri = {
        .uri = "/config.json", .method = HTTP_PUT, .handler = cfg_put,
    };
    esp_err_t err = httpd_register_uri_handler(server, &get_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &put_uri);
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "http://<ip>:8080/config.json（GET/PUT）");
    }
    return err;
}
