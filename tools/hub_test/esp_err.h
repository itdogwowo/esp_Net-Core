// host 測試用的最小 esp_err shim（只比較相等，數值不重要）
#pragma once
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL               (-1)
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103
