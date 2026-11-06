// host 測試的 log／err 替身：把 ESP_LOGx 導到 stdout。
#include <stdarg.h>
#include <stdio.h>

#include "esp_err.h"

static void vlog(const char *lvl, const char *tag, const char *fmt, va_list ap)
{
    printf("%s[%s] ", lvl, tag ? tag : "?");
    vprintf(fmt, ap);
    printf("\n");
}

void mod_host_log_e(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("E", tag, fmt, ap);
    va_end(ap);
}

void mod_host_log_w(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("W", tag, fmt, ap);
    va_end(ap);
}

void mod_host_log_i(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("I", tag, fmt, ap);
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "OK";
    case ESP_FAIL:
        return "FAIL";
    default:
        return "ERR";
    }
}
