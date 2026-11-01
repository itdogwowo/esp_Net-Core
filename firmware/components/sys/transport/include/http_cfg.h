// 用 HTTP 讀寫 /data/config.json（不需要 USB MSC）
//
//   GET  /config.json   → 回檔案內容（text/plain）
//   PUT  /config.json   → 內容必須是合法 JSON；驗證過才寫（先寫 .tmp 再 rename）
//
// 為什麼需要：MSC（把裝置變隨身碟）不是每個型號/版本都能列舉，
// 但設定一定要能讀、能改 —— 走網路是最低限度的替代方案。
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// 掛在既有的 httpd 上（目前是 WS 那個 server，port 8080）
esp_err_t http_cfg_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
