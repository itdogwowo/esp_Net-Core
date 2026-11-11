// 最小網頁（v0）：/ 首頁、/ota 上傳、/config.json 連結。
// 完整 UI 是獨立任務；這頁只做「打開得到／可以上傳」。
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
esp_err_t nc4_web_register(httpd_handle_t server);
