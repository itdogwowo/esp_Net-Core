#include "nc_mem_cfg.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "mem_cfg";

#define DEFAULT_FRAME_BLOCK   512      // 之後由 px × 3 決定，這是啟動時的暫定值
#define DEFAULT_FRAME_COUNT   4
#define DEFAULT_ASSET_BYTES   (64 * 1024)

void nc_mem_limits_default(nc_mem_limits_t *out)
{
    if (out == NULL) {
        return;
    }
    out->frame_block = DEFAULT_FRAME_BLOCK;
    out->frame_count = DEFAULT_FRAME_COUNT;
    out->asset_bytes = DEFAULT_ASSET_BYTES;
}

static size_t clamp_size(size_t v, size_t max, const char *what)
{
    if (v > max) {
        ESP_LOGW(TAG, "%s=%u 超過上限 %u，夾住", what, (unsigned)v, (unsigned)max);
        return max;
    }
    return v;
}

static bool read_num(const cJSON *obj, const char *key, size_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it) || it->valuedouble <= 0) {
        return false;
    }
    *out = (size_t)it->valuedouble;
    return true;
}

esp_err_t nc_mem_limits_from_json(nc_mem_limits_t *lim, const char *json)
{
    if (lim == NULL || json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *buf = cJSON_GetObjectItemCaseSensitive(root, "Buffer");
    if (!cJSON_IsObject(buf)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    size_t v = 0;

    const cJSON *frame = cJSON_GetObjectItemCaseSensitive(buf, "frame");
    if (cJSON_IsObject(frame)) {
        if (read_num(frame, "block", &v)) {
            lim->frame_block = clamp_size(v, NC_MEM_FRAME_BLOCK_MAX, "Buffer.frame.block");
        }
        if (read_num(frame, "count", &v)) {
            lim->frame_count = clamp_size(v, NC_MEM_FRAME_COUNT_MAX, "Buffer.frame.count");
        }
    }

    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(buf, "assets");
    if (cJSON_IsObject(assets) && read_num(assets, "size", &v)) {
        lim->asset_bytes = clamp_size(v, NC_MEM_ASSET_BYTES_MAX, "Buffer.assets.size");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void nc_mem_limits_log(const nc_mem_limits_t *lim)
{
    if (lim == NULL) {
        return;
    }
    ESP_LOGI(TAG, "配置: frame %u x %u B = %u KB, assets %u KB",
             (unsigned)lim->frame_block, (unsigned)lim->frame_count,
             (unsigned)(lim->frame_block * lim->frame_count / 1024),
             (unsigned)(lim->asset_bytes / 1024));
}
