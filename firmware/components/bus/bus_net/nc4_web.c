// 靜態 HTTP 服務：只負責 GET/HEAD 檔案，不做 OTA／config／硬件操作。
// 所有業務操作一律經 NC4（WebSocket /nc4 → decoder）。
#include "nc4_web.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "web";
static const char *WWW_ROOT = "/data/www";

static const char FALLBACK[] =
"<!doctype html><meta charset=utf-8><title>esp_Net-Core</title>"
"<style>body{font:14px system-ui;margin:24px;max-width:900px}"
"code{background:#f4f4f4;padding:2px 5px}</style>"
"<h1>esp_Net-Core</h1>"
"<p>HTTP 只提供靜態 UI；所有操作經 NC4。</p>"
"<p>WebSocket endpoint: <code>/nc4</code></p>"
"<p>把 <code>index.html</code> 放到 <code>/data/www/</code> 就會由這裡提供。</p>";

static const char *mime_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0 || strcasecmp(dot, ".mjs") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".woff") == 0) return "font/woff";
    if (strcasecmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcasecmp(dot, ".wasm") == 0) return "application/wasm";
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".md") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".map") == 0) return "application/json";
    return "application/octet-stream";
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URI → 安全相對路徑；拒絕 ..、反斜線、NUL。
static bool safe_rel_path(const char *uri, char *out, size_t cap)
{
    if (uri == NULL || uri[0] != '/') return false;
    size_t o = 0;
    for (size_t i = 0; uri[i] != '\0' && uri[i] != '?'; i++) {
        int c = (unsigned char)uri[i];
        if (c == '%' && uri[i + 1] && uri[i + 2]) {
            const int hi = hexval(uri[i + 1]);
            const int lo = hexval(uri[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (hi << 4) | lo;
            i += 2;
        }
        if (c == '\0' || c == '\\') return false;
        if (o + 1 >= cap) return false;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    if (o == 0 || out[0] != '/') return false;
    if (strstr(out, "..") != NULL) return false;
    return true;
}

static bool build_file_path(const char *uri, char *out, size_t cap)
{
    char rel[384];
    if (!safe_rel_path(uri, rel, sizeof(rel))) return false;
    if (strcmp(rel, "/") == 0) snprintf(rel, sizeof(rel), "/index.html");
    if (rel[0] && rel[strlen(rel) - 1] == '/') {
        const size_t n = strlen(rel);
        if (n + 10 >= sizeof(rel)) return false;
        snprintf(rel + n, sizeof(rel) - n, "index.html");
    }
    const int n = snprintf(out, cap, "%s%s", WWW_ROOT, rel);
    return n > 0 && (size_t)n < cap;
}

static bool parse_range(const char *s, size_t size, size_t *start, size_t *end)
{
    if (s == NULL || strncmp(s, "bytes=", 6) != 0) return false;
    s += 6;
    if (strchr(s, ',') != NULL) return false;       // 只支援單一 range
    char *dash = strchr(s, '-');
    if (dash == NULL) return false;
    *start = 0;
    *end = size ? size - 1 : 0;
    if (dash > s) {
        char *ep = NULL;
        const unsigned long v = strtoul(s, &ep, 10);
        if (ep != dash || v >= size) return false;
        *start = (size_t)v;
    }
    if (dash[1] != '\0') {
        char *ep = NULL;
        const unsigned long v = strtoul(dash + 1, &ep, 10);
        if (*ep != '\0' || v < *start) return false;
        *end = (v >= size) ? (size ? size - 1 : 0) : (size_t)v;
    }
    return *start <= *end;
}

static esp_err_t send_headers(httpd_req_t *req, const char *type, size_t len,
                              const char *etag, bool range, size_t start, size_t end,
                              size_t full)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    if (etag != NULL) httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (range) {
        char cr[96];
        snprintf(cr, sizeof(cr), "bytes %u-%u/%u",
                 (unsigned)start, (unsigned)end, (unsigned)full);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }
    (void)len;
    return ESP_OK;
}

static esp_err_t send_file(httpd_req_t *req, const char *path, struct stat *st)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    const size_t size = (size_t)st->st_size;
    char etag[64];
    snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)size,
             (unsigned long)st->st_mtime);

    char inm[80];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        fclose(f);
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }

    char range_hdr[80];
    size_t start = 0, end = size ? size - 1 : 0;
    bool range = false;
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        if (!parse_range(range_hdr, size, &start, &end)) {
            fclose(f);
            char cr[64];
            snprintf(cr, sizeof(cr), "bytes */%u", (unsigned)size);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            httpd_resp_set_hdr(req, "Content-Range", cr);
            return httpd_resp_send(req, NULL, 0);
        }
        range = true;
    }

    const size_t len = (size > 0) ? (end - start + 1) : 0;
    (void)send_headers(req, mime_of(path), len, etag, range, start, end, size);
    if (len == 0) {
        fclose(f);
        return httpd_resp_send(req, NULL, 0);
    }
    if (fseek(f, (long)start, SEEK_SET) != 0) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek");
        return ESP_OK;
    }

    if (req->method == HTTP_HEAD) {
        fclose(f);
        return httpd_resp_send(req, NULL, (ssize_t)len);
    }

    char buf[2048];
    size_t left = len;
    while (left > 0) {
        const size_t want = left > sizeof(buf) ? sizeof(buf) : left;
        const size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        if (httpd_resp_send_chunk(req, buf, (ssize_t)got) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
        left -= got;
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t web_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_set_hdr(req, "Allow", "GET, HEAD");
        return httpd_resp_send(req, "method not allowed", HTTPD_RESP_USE_STRLEN);
    }

    char path[512];
    if (!build_file_path(req->uri, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }

    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        return send_file(req, path, &st);
    }

    // 根目錄沒有 /data/www/index.html → 內建最小頁
    if (strcmp(path, "/data/www/index.html") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        if (req->method == HTTP_HEAD) {
            return httpd_resp_send(req, NULL, (ssize_t)strlen(FALLBACK));
        }
        return httpd_resp_send(req, FALLBACK, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

esp_err_t nc4_web_register(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;
    static const int methods[] = {
        HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT,
        HTTP_DELETE, HTTP_OPTIONS, HTTP_PATCH,
    };
    esp_err_t e = ESP_OK;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        httpd_uri_t root = { .uri = "/", .method = methods[i], .handler = web_handler };
        httpd_uri_t all  = { .uri = "/*", .method = methods[i], .handler = web_handler };
        e = httpd_register_uri_handler(server, &root);
        if (e == ESP_OK) e = httpd_register_uri_handler(server, &all);
        if (e != ESP_OK) break;
    }
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "static HTTP / 及 /*（GET/HEAD 檔案；其他 method 405；root=%s）", WWW_ROOT);
    } else {
        ESP_LOGE(TAG, "register static HTTP: %s", esp_err_to_name(e));
    }
    return e;
}
