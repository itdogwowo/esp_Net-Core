// 統一線路 API（bus 層）。
//
// 目的：讓用家（NC4、LED、LCD、測試……）用**同一套動詞**去用一條線，
// 大多數情況之下唔需要知道底下係 UART、CDC、SPI、I80 抑或 socket。
//
// **雙重保障**（使用者 2026-11-10 定案）：
//   1. 統一 API：open / read / write / ioctl —— 90% 的情況用這個就夠。
//   2. 逃生門：`nc_bus_native()` 拿回底層 driver 的原生 handle，
//      需要特殊能力（baud、DMA、面板命令……）時直接呼叫原生 API。
// 兩者都保留，唔會為了「統一」而封印硬體能力。
//
// 這一層**唔知道** NC4、唔知道路由、唔知道任務；它只係「一條線」。
// 線的實作由 adapter 提供（bus_uart、bus_cdc、bus_socket、未來的 bus_spi/bus_i80）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_BUS_KIND_UART = 0,
    NC_BUS_KIND_CDC,
    NC_BUS_KIND_SOCKET,
    NC_BUS_KIND_SPI,
    NC_BUS_KIND_I80,
    NC_BUS_KIND_OTHER,
} nc_bus_kind_t;

// 線的實作（由 adapter 填）。全部可以係 NULL —— 呼叫端要檢查。
typedef struct {
    int  (*read)(void *ctx, void *buf, size_t cap, uint32_t timeout_ms);
    int  (*write)(void *ctx, const void *buf, size_t len, uint32_t timeout_ms);
    esp_err_t (*ioctl)(void *ctx, uint32_t op, void *arg, size_t len);
    void *(*native)(void *ctx);     // 逃生門：原生 handle（uart_port_t* 之類）
} nc_bus_ops_t;

typedef struct nc_bus {
    const char *name;               // "uart1" / "cdc" / "tcp"
    nc_bus_kind_t kind;
    const nc_bus_ops_t *ops;
    void *ctx;                      // adapter 的狀態
} nc_bus_t;

// 由 adapter 呼叫：把一條已開好的線交給 bus 層（唔配置記憶體，只記帳）。
esp_err_t nc_bus_adopt(nc_bus_t *bus, const char *name, nc_bus_kind_t kind,
                       const nc_bus_ops_t *ops, void *ctx);

// 統一 API
int       nc_bus_read(nc_bus_t *bus, void *buf, size_t cap, uint32_t timeout_ms);
int       nc_bus_write(nc_bus_t *bus, const void *buf, size_t len, uint32_t timeout_ms);
esp_err_t nc_bus_ioctl(nc_bus_t *bus, uint32_t op, void *arg, size_t len);

// 逃生門：拿回底層原生 handle（可能係 NULL）
void     *nc_bus_native(nc_bus_t *bus);
const char *nc_bus_name(const nc_bus_t *bus);
nc_bus_kind_t nc_bus_kind(const nc_bus_t *bus);

#ifdef __cplusplus
}
#endif
