#include "nc4_web.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "web";

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><title>esp_Net-Core</title>"
"<style>body{font:14px system-ui;margin:24px;max-width:900px}"
"code,pre{background:#f4f4f4;padding:8px}button{padding:6px 12px}"
"input{margin:8px 0}</style>"
"<h1>esp_Net-Core</h1>"
"<p>網絡 OTA（只限可信 LAN，冇登入）</p>"
"<input id=f type=file accept=.bin><button onclick=up()>上傳 .bin</button>"
"<pre id=o>未上傳</pre>"
"<p><a href=/config.json>/config.json</a>（GET/PUT）</p>"
"<pre id=c>loading...</pre>"
"<script>"
"async function up(){let f=document.getElementById('f').files[0];"
"if(!f){alert('選 .bin');return}"
"let o=document.getElementById('o');o.textContent='上傳中 '+f.size+' B...';"
"try{let r=await fetch('/ota',{method:'POST',body:f});"
"o.textContent=await r.text()+'\\n'+(r.ok?'重啟中':'失敗')}"
"catch(e){o.textContent='錯誤 '+e}}"
"fetch('/config.json').then(r=>r.text()).then(t=>{"
"document.getElementById('c').textContent=t.slice(0,4000)}).catch(e=>{"
"document.getElementById('c').textContent='config 讀取失敗'})"
"</script>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

esp_err_t nc4_web_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_get,
    };
    const esp_err_t e = httpd_register_uri_handler(server, &uri);
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "http://<ip>:8080/（v0；完整 UI 獨立任務）");
    }
    return e;
}
