// 測試用 esp_err.h shim（只給 hosttest 編譯純 C 核心用）。
#pragma once
#include <stdlib.h>
#include <string.h>

typedef enum {
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_INVALID_ARG = 0x102,
    ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_NOT_FOUND = 0x105,
} esp_err_t;
