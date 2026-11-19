// nc_spi_led_drv.c — SPI 時脈式燈串驅動（WS2801／APA102）。
//
// 只管 SPI 硬體；一位一個時脈，資料由呼叫端打包成 MSB-first 的 byte 流。
// 送法是同步的（spi_device_transmit 回來就代表送完），所以完成＝submit 回 true。
#include "nc_spi_led_drv.h"

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "drv_spi_led";

typedef struct {
    bool    ready;
    uint8_t slots;
    uint32_t slot_bytes;
    uint8_t *tx[NC_SPI_LED_DRV_SLOTS_MAX];
    uint32_t submitted;
    uint32_t fail;
    uint32_t clock_hz;
    spi_device_handle_t dev;
} spi_led_t;

static spi_led_t s_spi;

static void spi_free(spi_led_t *d)
{
    for (uint32_t i = 0; i < NC_SPI_LED_DRV_SLOTS_MAX; i++) {
        if (d->tx[i] != NULL) {
            heap_caps_free(d->tx[i]);
            d->tx[i] = NULL;
        }
    }
    d->ready = false;
}

esp_err_t nc_spi_led_drv_install(const nc_spi_led_drv_cfg_t *cfg)
{
    if (cfg == NULL || cfg->pin_mosi < 0 || cfg->pin_sclk < 0 || cfg->slot_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_spi.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_spi, 0, sizeof(s_spi));
    spi_led_t *d = &s_spi;
    d->slots = (uint8_t)((cfg->slots < 2) ? 2u : cfg->slots);
    if (d->slots > NC_SPI_LED_DRV_SLOTS_MAX) {
        d->slots = NC_SPI_LED_DRV_SLOTS_MAX;
    }
    d->slot_bytes = cfg->slot_bytes;
    d->clock_hz = (cfg->clock_hz == 0) ? 8000000u : cfg->clock_hz;

    const spi_host_device_t host = (cfg->host < 0) ? SPI2_HOST : (spi_host_device_t)cfg->host;
    spi_bus_config_t bus = {
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = cfg->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)d->slot_bytes,
    };
    if (spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        // 已經初始化過（同一條 bus 有兩個消費者）就沿用 —— 屬於 owner 的責任。
        ESP_LOGW(TAG, "spi_bus_initialize 失敗（可能已初始化），嘗試沿用");
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)d->clock_hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = (int)d->slots,
    };
    if (spi_bus_add_device(host, &dev, &d->dev) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device 失敗");
        return ESP_FAIL;
    }
    for (uint32_t i = 0; i < d->slots; i++) {
        d->tx[i] = heap_caps_calloc(1, d->slot_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (d->tx[i] == NULL) {
            ESP_LOGE(TAG, "SPI 槽 %u（%u B）配置失敗", (unsigned)i, (unsigned)d->slot_bytes);
            spi_free(d);
            return ESP_ERR_NO_MEM;
        }
    }
    d->ready = true;
    ESP_LOGI(TAG, "SPI LED 就緒：clk %u Hz、一槽 %u B、%u 槽",
             (unsigned)d->clock_hz, (unsigned)d->slot_bytes, (unsigned)d->slots);
    return ESP_OK;
}

void nc_spi_led_drv_uninstall(void)
{
    spi_free(&s_spi);
}

bool nc_spi_led_drv_ready(void)
{
    return s_spi.ready;
}

int nc_spi_led_drv_slots(void)
{
    return s_spi.ready ? (int)s_spi.slots : 0;
}

uint32_t nc_spi_led_drv_slot_bytes(void)
{
    return s_spi.slot_bytes;
}

uint8_t *nc_spi_led_drv_slot_buf(int slot)
{
    if (!s_spi.ready || slot < 0 || slot >= (int)s_spi.slots) {
        return NULL;
    }
    return s_spi.tx[slot];
}

bool nc_spi_led_drv_submit(int slot, uint32_t bits)
{
    spi_led_t *d = &s_spi;
    if (!d->ready || slot < 0 || slot >= (int)d->slots || d->tx[slot] == NULL || bits == 0) {
        return false;
    }
    spi_transaction_t t = {
        .length = bits,
        .tx_buffer = d->tx[slot],
    };
    if (spi_device_transmit(d->dev, &t) != ESP_OK) {
        d->fail++;
        return false;
    }
    d->submitted++;
    return true;
}

uint32_t nc_spi_led_drv_submitted(void) { return s_spi.submitted; }
uint32_t nc_spi_led_drv_fail(void)      { return s_spi.fail; }
uint32_t nc_spi_led_drv_clock(void)     { return s_spi.clock_hz; }
