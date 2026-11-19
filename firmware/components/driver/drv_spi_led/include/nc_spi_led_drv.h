// nc_spi_led_drv.h — SPI 時脈式燈串驅動（WS2801／APA102）。
//
// 只管一顆 SPI 的 MOSI/SCLK、時鐘、DMA 槽與送出；唔知道像素格式與顏色次序。
// 一位 = 一個 SPI 時脈；上層把位元打包好（MSB first）放進 slot_buf 再 submit(bits)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_SPI_LED_DRV_SLOTS_MAX 3

typedef struct {
    int      pin_mosi;
    int      pin_sclk;
    int      host;          // SPI2_HOST / SPI3_HOST（-1 = 自動）
    uint32_t clock_hz;
    uint32_t slot_bytes;    // 每槽 buffer 大小（= ceil(bits/8)）
    uint8_t  slots;         // 2..NC_SPI_LED_DRV_SLOTS_MAX
} nc_spi_led_drv_cfg_t;

esp_err_t nc_spi_led_drv_install(const nc_spi_led_drv_cfg_t *cfg);
void      nc_spi_led_drv_uninstall(void);
bool      nc_spi_led_drv_ready(void);

int       nc_spi_led_drv_slots(void);
uint32_t  nc_spi_led_drv_slot_bytes(void);
uint8_t  *nc_spi_led_drv_slot_buf(int slot);

// 同步送出 slot 的前 bits 個位（spi_device_transmit）。回 true = 送完。
bool      nc_spi_led_drv_submit(int slot, uint32_t bits);

uint32_t  nc_spi_led_drv_submitted(void);
uint32_t  nc_spi_led_drv_fail(void);
uint32_t  nc_spi_led_drv_clock(void);

#ifdef __cplusplus
}
#endif
