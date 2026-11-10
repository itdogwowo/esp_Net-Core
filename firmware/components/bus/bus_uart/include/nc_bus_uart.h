// UART 線的 bus adapter：把 drv_uart 包成統一線路 API。
//
// 用家可以：
//   nc_bus_read(bus, ...) / nc_bus_write(bus, ...)      ← 統一 API
//   uart_port_t *p = nc_bus_native(bus); uart_set_baudrate(p, ...)  ← 逃生門
#pragma once

#include "nc_bus.h"
#include "nc_uart_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

// 開一條 UART 線（安裝 driver 之後填 bus）。bus 由呼叫端提供（靜態）。
esp_err_t nc_bus_uart_open(nc_bus_t *bus, const char *name,
                           const nc_uart_drv_cfg_t *cfg, uint32_t baud);

#ifdef __cplusplus
}
#endif
