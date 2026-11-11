// UART 迴路驅動器 —— 實作。設計說明見 include/nc_uart_drv.h。
#include "nc_uart_drv.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart_drv";

static bool s_ready;
static nc_uart_drv_cfg_t s_cfg;

esp_err_t nc_uart_drv_install(const nc_uart_drv_cfg_t *cfg, uint32_t baud)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_OK;
    }
    const uart_config_t uc = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->port, cfg->rx_ring, cfg->tx_ring,
                                            0, NULL, 0),
                        TAG, "uart driver");
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->port, &uc), TAG, "uart param");
    ESP_RETURN_ON_ERROR(uart_set_pin(cfg->port, cfg->tx_pin, cfg->rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pins");
    if (cfg->en_pin >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << cfg->en_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&io);
        gpio_set_level(cfg->en_pin, 0);      // idle = receive
    }
    s_cfg = *cfg;
    s_cfg.baud = baud;
    s_ready = true;
    ESP_LOGW(TAG, "UART 已啟動：port=%d TX=%d RX=%d baud=%u（ring %d/%d%s）",
             cfg->port, cfg->tx_pin, cfg->rx_pin, (unsigned)baud,
             cfg->rx_ring, cfg->tx_ring,
             (cfg->en_pin >= 0) ? "，RS485 DE" : "");
    return ESP_OK;
}

bool nc_uart_drv_ready(void)
{
    return s_ready;
}

int nc_uart_drv_port(void)
{
    return s_ready ? s_cfg.port : -1;
}

uint32_t nc_uart_drv_baud(void)
{
    return s_ready ? s_cfg.baud : 0;
}

int nc_uart_drv_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    if (!s_ready) {
        vTaskDelay(1);
        return 0;       // 尚未安裝（driver 由 bus 所屬核的 owner_init 安裝）
    }
    if (buf == NULL || cap == 0) {
        return 0;
    }

    // 阻塞等「第一個 byte」（ISR 同核、wake 本地，portMAX_DELAY 安全），
    // 然後把環裡已經有的全部搬走（最多 cap），立即返回。
    //
    // 為什麼唔用 `uart_read_bytes(port, buf, cap, portMAX_DELAY)`：
    // IDF 的實作會**一直等到收滿 cap 才返回**。cap = slot_size = 1 KB 的年代，
    // 一幀 18 B 的指令要等 1 KB 到齊才被讀走 —— 等於 UART 的延遲沒有上限。
    // 實測：送 1 幀（77 B）之後 producer 一直卡在 read，rx 永遠是 0。
    const int n = uart_read_bytes(s_cfg.port, buf, 1, portMAX_DELAY);
    if (n <= 0) {
        return (n < 0) ? -1 : 0;
    }
    size_t avail = 0;
    if (uart_get_buffered_data_len(s_cfg.port, &avail) == ESP_OK &&
        avail > 0 && cap > 1) {
        const size_t want = (avail < cap - 1) ? avail : (cap - 1);
        const int more = uart_read_bytes(s_cfg.port, buf + 1, want, 0);
        if (more > 0) {
            return n + more;
        }
    }
    return n;
}

int nc_uart_drv_write(const uint8_t *frame, size_t len)
{
    if (!s_ready) {
        return -1;      // 未安裝（例如 GPIO7 已經被 SD 的 CLK 佔用）
    }
    if (frame == NULL || len == 0) {
        return 0;
    }
    if (s_cfg.en_pin >= 0) {
        // 舊專案 _Rs485Uart：拉高 DE → settle → 寫 → 等 TX 真正排空 → 等最後一個 byte
        // 離開 shift register → 放低 DE。0 ms 實測會吃掉 start bit，預設 1 ms。
        gpio_set_level(s_cfg.en_pin, 1);
        esp_rom_delay_us((uint32_t)s_cfg.en_settle_ms * 1000u);
        const int n = uart_write_bytes(s_cfg.port, (const char *)frame, len);
        if (n > 0) {
            const uint32_t baud = (s_cfg.baud > 0) ? s_cfg.baud : 115200;
            const uint32_t ms = ((uint32_t)n * 10u * 1000u) / baud + 100u;
            (void)uart_wait_tx_done(s_cfg.port, pdMS_TO_TICKS(ms));
            esp_rom_delay_us((10u * 1000000u) / baud + 20u);
        }
        gpio_set_level(s_cfg.en_pin, 0);
        return n;
    }
    // 阻塞到「整幀都排入環」為止（1 KB 環、921600 bps ⇒ 4 KB 最壞 ~44 ms）。
    return uart_write_bytes(s_cfg.port, (const char *)frame, len);
}

esp_err_t nc_uart_drv_send(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    return (nc_uart_drv_write(frame, len) == (int)len) ? ESP_OK : ESP_FAIL;
}
