#include "nc_bus_uart.h"

#include "driver/uart.h"

typedef struct {
    nc_uart_drv_cfg_t cfg;
    bool opened;
} uart_line_t;

static uart_line_t s_line;      // 本板只有一條 UART 測試線

static int line_read(void *ctx, void *buf, size_t cap, uint32_t timeout_ms)
{
    (void)timeout_ms;           // drv 的語義：等第一個 byte（阻塞），唔用逾時
    return nc_uart_drv_read(ctx, (uint8_t *)buf, cap);
}

static int line_write(void *ctx, const void *buf, size_t len, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;           // drv 會阻塞到整幀排入環
    return nc_uart_drv_write((const uint8_t *)buf, len);
}

static esp_err_t line_ioctl(void *ctx, uint32_t op, void *arg, size_t len)
{
    (void)ctx;
    // 只開放最小的一組；其餘一律用逃生門（nc_bus_native）。
    if (op == 1 && arg != NULL && len == sizeof(uint32_t)) {
        // op 1 = SET_BAUD
        return uart_set_baudrate(s_line.cfg.port, *(const uint32_t *)arg);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static void *line_native(void *ctx)
{
    (void)ctx;
    return nc_uart_drv_ready() ? &s_line.cfg.port : NULL;   // uart_port_t*
}

static const nc_bus_ops_t s_ops = {
    .read = line_read,
    .write = line_write,
    .ioctl = line_ioctl,
    .native = line_native,
};

esp_err_t nc_bus_uart_open(nc_bus_t *bus, const char *name,
                           const nc_uart_drv_cfg_t *cfg, uint32_t baud)
{
    if (bus == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t e = nc_uart_drv_install(cfg, baud);
    if (e != ESP_OK) {
        return e;
    }
    s_line.cfg = *cfg;
    s_line.opened = true;
    return nc_bus_adopt(bus, name ? name : "uart", NC_BUS_KIND_UART, &s_ops, NULL);
}
