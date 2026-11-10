// UART 迴路驅動器（只有一組實例：測試用的 TX/RX 迴路）。
//
// 這一層只做「一條 UART 匯流排怎樣收、怎樣發」：
//   · 安裝 driver、設參數、設腳位
//   · read：阻塞到第一個 byte 到，之後把環裡現有的一次搬走（唔等填滿）
//   · write：整幀寫入 driver 的 TX 環
//
// **唔知道** NC4、唔知道路由、唔知道 SD 佔用腳位（那些由組裝層 main.c 決定）。
// 對照舊專案 `slave/driver/uart_drv.py`。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;          // UART_NUM_0/1/2
    int tx_pin;
    int rx_pin;
    int rx_ring;       // driver RX 環大小
    int tx_ring;       // driver TX 環大小
} nc_uart_drv_cfg_t;

// 安裝（idempotent：已安裝就回 ESP_OK）。
esp_err_t nc_uart_drv_install(const nc_uart_drv_cfg_t *cfg, uint32_t baud);

// 已安裝？（未安裝時 read 回 0、write 回 -1，呼叫端可以照跑）
bool nc_uart_drv_ready(void);

// 給 nc4_bus_cfg_t.read 用：回 bytes、-1 = 錯誤。
int nc_uart_drv_read(void *arg, uint8_t *buf, size_t cap);

// 給 nc4_link / 測試用：回寫入 bytes、-1 = 未安裝。
int nc_uart_drv_write(const uint8_t *frame, size_t len);

// 給 nc4_bus_cfg_t.send 用（整幀語義）。
esp_err_t nc_uart_drv_send(void *arg, const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif
