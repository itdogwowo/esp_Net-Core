// host 測試用的平台 shim（Mac 上 cc 直接編，不需要 IDF）
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "fx_host.h"

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return "ESP_OK";
    case ESP_FAIL:              return "ESP_FAIL";
    case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:  return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:     return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_INVALID_CRC:   return "ESP_ERR_INVALID_CRC";
    default:                    return "ESP_ERR_UNKNOWN";
    }
}

void fx_host_log_e(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "E %s: ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void fx_host_log_w(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "W %s: ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void fx_host_log_i(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "I %s: ", tag);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}

// 與 esp_rom_crc32_le 相同的結果（init=0 時等於 zlib.crc32）
unsigned int esp_rom_crc32_le(unsigned int crc, const void *buf, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned int c = crc ^ 0xFFFFFFFFu;
    for (unsigned int i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

int64_t fx_host_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
