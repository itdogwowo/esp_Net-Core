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

static inline const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    default: return "ESP_ERR_?";
    }
}
