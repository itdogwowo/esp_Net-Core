// 最小 esp_err.h stub：只為 host 測試而存在（真 IDF 由元件提供）
#pragma once

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
