// bus 層核心 —— 只係「轉發 ＋ 檢查」，冇任何硬體知識。
//
// 設計上刻意做到最薄：adapter 填 ops，這裡只負責
//   · 空指標檢查（避免每條線各自漏檢查）
//   · 統一 sign 轉換（read/write 回 bytes，負數 = 錯誤）
//   · 逃生門 `nc_bus_native()`
#include "nc_bus.h"

esp_err_t nc_bus_adopt(nc_bus_t *bus, const char *name, nc_bus_kind_t kind,
                       const nc_bus_ops_t *ops, void *ctx)
{
    if (bus == NULL || name == NULL || ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bus->name = name;
    bus->kind = kind;
    bus->ops = ops;
    bus->ctx = ctx;
    return ESP_OK;
}

int nc_bus_read(nc_bus_t *bus, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (bus == NULL || bus->ops == NULL || bus->ops->read == NULL) {
        return -1;
    }
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return bus->ops->read(bus->ctx, buf, cap, timeout_ms);
}

int nc_bus_write(nc_bus_t *bus, const void *buf, size_t len, uint32_t timeout_ms)
{
    if (bus == NULL || bus->ops == NULL || bus->ops->write == NULL) {
        return -1;
    }
    if (buf == NULL || len == 0) {
        return 0;
    }
    return bus->ops->write(bus->ctx, buf, len, timeout_ms);
}

esp_err_t nc_bus_ioctl(nc_bus_t *bus, uint32_t op, void *arg, size_t len)
{
    if (bus == NULL || bus->ops == NULL || bus->ops->ioctl == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return bus->ops->ioctl(bus->ctx, op, arg, len);
}

void *nc_bus_native(nc_bus_t *bus)
{
    if (bus == NULL || bus->ops == NULL || bus->ops->native == NULL) {
        return NULL;
    }
    return bus->ops->native(bus->ctx);
}

const char *nc_bus_name(const nc_bus_t *bus)
{
    return bus ? bus->name : "?";
}

nc_bus_kind_t nc_bus_kind(const nc_bus_t *bus)
{
    return bus ? bus->kind : NC_BUS_KIND_OTHER;
}
