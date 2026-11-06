#include "nc_boot.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "nc_state.h"

static const char *TAG = "boot";

_Static_assert(sizeof(nc_boot_record_t) == 8, "boot record 必須固定 8 B（NVS blob 大小要穩定）");

static nc_boot_record_t s_rec;
static nc_state_t *s_state;
static bool s_fw_changed;

// 用 app_elf_sha256 的前 4 bytes。不用 date/time：esp_app_desc.c 只在 configure
// 時生成，連續 rebuild 兩次 date/time 可能一樣（實測），會永遠偵測不到換版。
static uint32_t fw_hash_now(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const uint8_t *p = app->app_elf_sha256;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void nc_boot_init(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.version = NC_BOOT_SCHEMA_VERSION;

    s_state = nc_state_add(&(nc_state_desc_t){
        .name      = "boot",
        .persist   = NC_ST_PERSIST_NVS,
        .writer    = NC_ST_BY_SYSTEM,
        .value     = &s_rec,
        .size      = sizeof(s_rec),
        .nvs_ns    = "boot",
        .nvs_key   = "record",
    });
    (void)nc_state_load();

    if (s_rec.version != NC_BOOT_SCHEMA_VERSION) {
        // 未知格式（舊版、或 NVS 被寫壞）→ 當一塊全新的板子，但保留 fw_hash 的比較
        ESP_LOGW(TAG, "boot 記錄版本 %u ≠ %u → 當第一次",
                 (unsigned)s_rec.version, (unsigned)NC_BOOT_SCHEMA_VERSION);
        memset(&s_rec, 0, sizeof(s_rec));
        s_rec.version = NC_BOOT_SCHEMA_VERSION;
    }

    // DEGRADED 講的是「**上一次**開機的 config 有問題」。
    // 每次開機都要先清掉，等本次 boot 真的讀到壞檔才由 main 重新標記；
    // 否則一次壞檔就會令 STATUS 永遠顯示 degraded，變成沒人相信的指標。
    if ((s_rec.flags & NC_BOOT_DEGRADED) != 0) {
        s_rec.flags &= (uint16_t)~NC_BOOT_DEGRADED;
        nc_state_touch(s_state);
    }

    const uint32_t now = fw_hash_now();
    const uint32_t old = s_rec.fw_hash;
    s_fw_changed = (old != now);
    if (s_fw_changed) {
        // 換韌體 → 回到生命週期起點：這次開機格式化，然後重建底座。
        // 不另寫「刪 config」的邏輯 —— 格式化之後它自然不存在。
        s_rec.fw_hash = now;
        s_rec.flags |= NC_BOOT_FORMAT_PENDING;
        nc_state_touch(s_state);
        ESP_LOGW(TAG, "韌體 %08" PRIx32 " → %08" PRIx32 "：data 分區重建後重新建立底座",
                 old, now);
    }

    ESP_LOGI(TAG, "provisioned=%d fmtq=%d degraded=%d fw_new=%d",
             nc_boot_provisioned(), nc_boot_format_pending(),
             nc_boot_degraded(), s_fw_changed);
}

bool nc_boot_provisioned(void)
{
    return (s_rec.flags & NC_BOOT_PROVISIONED) != 0;
}

bool nc_boot_format_pending(void)
{
    return (s_rec.flags & NC_BOOT_FORMAT_PENDING) != 0;
}

bool nc_boot_degraded(void)
{
    return (s_rec.flags & NC_BOOT_DEGRADED) != 0;
}

bool nc_boot_fw_changed(void)
{
    return s_fw_changed;
}

void nc_boot_request_format(bool on)
{
    const uint16_t before = s_rec.flags;
    if (on) {
        s_rec.flags |= NC_BOOT_FORMAT_PENDING;
    } else {
        s_rec.flags &= (uint16_t)~NC_BOOT_FORMAT_PENDING;
    }
    if (s_rec.flags == before) {
        return;
    }
    nc_state_touch(s_state);
    const esp_err_t err = nc_state_commit();   // 重開機之前一定要落盤
    ESP_LOGW(TAG, "fs_format=%d（%s）→ %s", on ? 1 : 0,
             on ? "下次開機抹 data 分區並重建 FAT" : "已取消",
             esp_err_to_name(err));
}

void nc_boot_mark_provisioned(void)
{
    s_rec.flags |= NC_BOOT_PROVISIONED;
    s_rec.flags &= (uint16_t)~NC_BOOT_FORMAT_PENDING;   // 重建完成，請求消費掉
    nc_state_touch(s_state);
}

void nc_boot_mark_degraded(void)
{
    if ((s_rec.flags & NC_BOOT_DEGRADED) != 0) {
        return;
    }
    s_rec.flags |= NC_BOOT_DEGRADED;
    nc_state_touch(s_state);
}

esp_err_t nc_boot_commit(void)
{
    return nc_state_commit();
}

void nc_boot_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    snprintf(buf, cap, "{\"prov\":%d,\"fmtq\":%d,\"degr\":%d,\"fw_new\":%d}",
             nc_boot_provisioned() ? 1 : 0, nc_boot_format_pending() ? 1 : 0,
             nc_boot_degraded() ? 1 : 0, s_fw_changed ? 1 : 0);
}
