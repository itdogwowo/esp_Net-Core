// 網絡 OTA：HTTP POST /ota 上傳 .bin，寫入下一個 OTA 分區，成功後重啟。
//
// 安全模型：目前只假設「可信 LAN」。冇 TLS、冇 token；正式對外之前一定要加驗證。
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nc4_ota_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
