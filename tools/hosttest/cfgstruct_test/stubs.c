// host 測試的 log／err 替身：把 ESP_LOGx 導到 stdout。
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "nc_cfg_types.h"

#include "esp_err.h"

// 假 build-time generated 的 nc_port_usb_kind()（真的那份由 tools/ncportgen.py
// 由 wiring.json 生成，host 測試沒有）。這裡扮一塊「USB.list[0] = usj-fs」的板子：
// port 0 是 USJ，其餘 0xFF（這塊板冇這條 port）。只有這樣
// nc_cfg_load.c 的 kind:"usb" → personality 解析才驗得到。
uint8_t nc_port_usb_kind(uint8_t idx)
{
    return (idx == 0) ? (uint8_t)NC_LINK_USBJTAG : (uint8_t)NC_CFG_USB_PORT_NONE;
}

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
