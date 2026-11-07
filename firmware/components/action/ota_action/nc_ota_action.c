#include "nc_ota_action.h"
#include "nc_action.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

static const char *TAG = "nc_ota_action";

// ── 指令碼（slave/schema/ota.json）──
#define OTA_BEGIN           0x2201
#define OTA_WRITE           0x2202
#define OTA_END             0x2203
#define OTA_ACK             0x2204
#define OTA_ABORT           0x2205
#define OTA_VERSION_QUERY   0x2206
#define OTA_VERSION_RSP     0x2207
#define OTA_CAPS_QUERY      0x2210
#define OTA_CAPS_RSP        0x2211
#define OTA_PROGRESS_QUERY  0x2218
#define OTA_PROGRESS_RSP    0x2219
#define OTA_STATE_QUERY     0x221A
#define OTA_STATE_RSP       0x221B
#define OTA_ERROR_RSP       0x221C
#define OTA_APPLY           0x2220

// CDC 上的上限；其他 transport 應該各自回報更小的值
// 受 CDC 的 rx.region 限制：幀 = chunk + 17，region 4,192 ⇒ 上限 4,175，取 4,096 留餘裕。
// 正式版應該由 region 推導（見 docs），唔好兩邊各自硬編碼。
#define OTA_MAX_CHUNK       4096

// OTA_ERROR_RSP 的 11 個 bool，順序與 schema 相同
enum {
    E_BEGIN_PARTITION_CONFLICT = 0,
    E_BEGIN_NO_PARTITION,
    E_BEGIN_SIZE_TOO_BIG,
    E_WRITE_OFFSET_MISMATCH,
    E_WRITE_FLASH_FAIL,
    E_WRITE_CHUNK_TOO_BIG,
    E_END_VALIDATE_FAIL,
    E_END_NOT_WRITING,
    E_APPLY_SET_BOOT_FAIL,
    E_NOT_IN_VERIFIED_STATE,
    E_ALREADY_ACTIVE,
    E_COUNT
};

typedef struct {
    bool busy;              // 有 session（writing）
    bool verified;
    esp_ota_handle_t handle;
    const esp_partition_t *target;
    size_t written;
    size_t image_size;
    uint8_t sha_expect[32];
    psa_hash_operation_t sha;
    bool sha_active;
} ota_state_t;

static ota_state_t s_ota;

// ───────────────────────── payload 組裝 ─────────────────────────

typedef struct {
    uint8_t b[256];
    size_t n;
} pb_t;

static void pb_u8(pb_t *p, uint8_t v) { p->b[p->n++] = v; }

static void pb_u16(pb_t *p, uint16_t v)
{
    p->b[p->n++] = (uint8_t)v;
    p->b[p->n++] = (uint8_t)(v >> 8);
}

static void pb_u32(pb_t *p, uint32_t v)
{
    p->b[p->n++] = (uint8_t)v;
    p->b[p->n++] = (uint8_t)(v >> 8);
    p->b[p->n++] = (uint8_t)(v >> 16);
    p->b[p->n++] = (uint8_t)(v >> 24);
}

static void pb_bytes(pb_t *p, const uint8_t *src, size_t len)
{
    memcpy(p->b + p->n, src, len);
    p->n += len;
}

static void pb_str16(pb_t *p, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len > 200) {
        len = 200;
    }
    pb_u16(p, (uint16_t)len);
    pb_bytes(p, (const uint8_t *)s, len);
}

static uint32_t rd_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd_u16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static void send_payload(nc4_ctx_t *ctx, uint16_t cmd, const pb_t *p)
{
    if (ctx == NULL || ctx->send == NULL) {
        return;
    }
    uint8_t frame[NC4_HDR_LEN + 256 + NC4_CRC_LEN];
    int total = nc4_pack_into(frame, sizeof(frame), cmd, nc4_reply_addr(ctx), p->b, p->n);
    if (total > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)total);
    }
}

// ───────────────────────── 回覆 ─────────────────────────

static void reply_error(nc4_ctx_t *ctx, int which, uint32_t failed_offset)
{
    pb_t p = {0};
    for (int i = 0; i < E_COUNT; i++) {
        pb_u8(&p, (i == which) ? 1 : 0);
    }
    pb_u32(&p, failed_offset);
    pb_u32(&p, (uint32_t)s_ota.written);
    pb_str16(&p, s_ota.target ? s_ota.target->label : "");
    ESP_LOGW(TAG, "[%s] OTA 錯誤 #%d（off=%u, written=%u）",
             ctx->source, which, (unsigned)failed_offset, (unsigned)s_ota.written);
    send_payload(ctx, OTA_ERROR_RSP, &p);
}

static void reply_state(nc4_ctx_t *ctx)
{
    pb_t p = {0};
    pb_u8(&p, (!s_ota.busy && !s_ota.verified) ? 1 : 0);   // state_idle
    pb_u8(&p, s_ota.busy ? 1 : 0);                        // state_writing
    pb_u8(&p, s_ota.verified ? 1 : 0);                    // state_verified
    pb_u8(&p, 0);                                         // state_error
    pb_str16(&p, s_ota.target ? s_ota.target->label : "");
    send_payload(ctx, OTA_STATE_RSP, &p);
}

static void reply_ack(nc4_ctx_t *ctx, uint32_t offset)
{
    pb_t p = {0};
    pb_u32(&p, offset);
    pb_u32(&p, (uint32_t)s_ota.written);
    send_payload(ctx, OTA_ACK, &p);
}

// ───────────────────────── 動作 ─────────────────────────

static void on_begin(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_ota.busy) {
        reply_error(ctx, E_BEGIN_PARTITION_CONFLICT, 0);
        return;
    }
    if (f->len < 38) {
        reply_error(ctx, E_BEGIN_SIZE_TOO_BIG, 0);
        return;
    }

    const uint32_t image_size = rd_u32(f->data);
    const uint16_t chunk_size = rd_u16(f->data + 4);
    const uint16_t fw_len = rd_u16(f->data + 38);
    char fw_ver[48] = {0};
    if (fw_len < sizeof(fw_ver) && 40 + fw_len <= f->len) {
        memcpy(fw_ver, f->data + 40, fw_len);
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        reply_error(ctx, E_BEGIN_NO_PARTITION, 0);
        return;
    }
    if (image_size > part->size || chunk_size > OTA_MAX_CHUNK) {
        reply_error(ctx, E_BEGIN_SIZE_TOO_BIG, 0);
        return;
    }

    esp_err_t err = esp_ota_begin(part, image_size, &s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        reply_error(ctx, E_BEGIN_NO_PARTITION, 0);
        return;
    }

    s_ota.busy = true;
    s_ota.verified = false;
    s_ota.target = part;
    s_ota.written = 0;
    s_ota.image_size = image_size;
    memcpy(s_ota.sha_expect, f->data + 6, 32);
    psa_crypto_init();
    s_ota.sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    s_ota.sha_active = (psa_hash_setup(&s_ota.sha, PSA_ALG_SHA_256) == PSA_SUCCESS);

    ESP_LOGW(TAG, "[%s] OTA_BEGIN size=%u chunk=%u fw=%s → %s",
             ctx->source, (unsigned)image_size, chunk_size, fw_ver, part->label);
    reply_ack(ctx, 0);
}

static void on_write(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (!s_ota.busy) {
        reply_error(ctx, E_END_NOT_WRITING, 0);
        return;
    }
    if (f->len < 4) {
        reply_error(ctx, E_WRITE_OFFSET_MISMATCH, 0);
        return;
    }

    const uint32_t offset = rd_u32(f->data);
    const uint8_t *data = f->data + 4;
    const size_t dlen = f->len - 4;

    if (offset != s_ota.written) {
        reply_error(ctx, E_WRITE_OFFSET_MISMATCH, offset);
        return;
    }
    if (dlen > OTA_MAX_CHUNK) {
        reply_error(ctx, E_WRITE_CHUNK_TOO_BIG, offset);
        return;
    }

    esp_err_t err = esp_ota_write(s_ota.handle, data, dlen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        reply_error(ctx, E_WRITE_FLASH_FAIL, offset);
        return;
    }
    if (s_ota.sha_active) {
        psa_hash_update(&s_ota.sha, data, dlen);
    }
    s_ota.written += dlen;
    reply_ack(ctx, offset);
}

static void on_end(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    if (!s_ota.busy) {
        reply_error(ctx, E_END_NOT_WRITING, 0);
        return;
    }

    uint8_t got[32] = {0};
    if (s_ota.sha_active) {
        size_t olen = 0;
        psa_hash_finish(&s_ota.sha, got, sizeof(got), &olen);
        s_ota.sha_active = false;
    }

    if (memcmp(got, s_ota.sha_expect, 32) != 0) {
        ESP_LOGE(TAG, "sha256 不符（收到 %u bytes）", (unsigned)s_ota.written);
        esp_ota_abort(s_ota.handle);
        s_ota.busy = false;
        reply_error(ctx, E_END_VALIDATE_FAIL, (uint32_t)s_ota.written);
        return;
    }

    esp_err_t err = esp_ota_end(s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        s_ota.busy = false;
        reply_error(ctx, E_END_VALIDATE_FAIL, (uint32_t)s_ota.written);
        return;
    }

    s_ota.busy = false;
    s_ota.verified = true;
    ESP_LOGW(TAG, "OTA_END 驗證通過（%u bytes，目標 %s）",
             (unsigned)s_ota.written, s_ota.target->label);
    reply_state(ctx);
}

static void on_abort(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    if (s_ota.busy) {
        esp_ota_abort(s_ota.handle);
        s_ota.busy = false;
    }
    if (s_ota.sha_active) {
        psa_hash_abort(&s_ota.sha);
        s_ota.sha_active = false;
    }
    s_ota.verified = false;
    s_ota.written = 0;
    ESP_LOGW(TAG, "OTA_ABORT");
    reply_state(ctx);
}

static void on_apply(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint8_t set_boot_only = 0;
    uint32_t delay_ms = 0;
    if (f->len >= 5) {
        set_boot_only = f->data[0];
        delay_ms = rd_u32(f->data + 1);
    }

    if (!s_ota.verified || s_ota.target == NULL) {
        reply_error(ctx, E_NOT_IN_VERIFIED_STATE, 0);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(s_ota.target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        reply_error(ctx, E_APPLY_SET_BOOT_FAIL, 0);
        return;
    }

    ESP_LOGW(TAG, "OTA_APPLY：下次開機用 %s（set_boot_only=%u）",
             s_ota.target->label, set_boot_only);

    if (set_boot_only) {
        reply_state(ctx);
        return;
    }
    reply_state(ctx);
    vTaskDelay(pdMS_TO_TICKS(delay_ms + 100));
    esp_restart();
}

// ───────────────────────── 查詢 ─────────────────────────

static void on_version_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *free_slot = esp_ota_get_next_update_partition(NULL);

    pb_t p = {0};
    pb_str16(&p, desc ? desc->version : "?");
    pb_bytes(&p, desc ? (const uint8_t *)desc->app_elf_sha256 : (const uint8_t *)"", 32);
    pb_str16(&p, running ? running->label : "?");
    pb_u32(&p, 0);                                    // running_seq（自訂 metadata，未實作）
    pb_str16(&p, free_slot ? free_slot->label : "?");
    pb_u32(&p, free_slot ? (uint32_t)free_slot->size : 0);
    send_payload(ctx, OTA_VERSION_RSP, &p);
}

static void on_caps_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    pb_t p = {0};
    pb_u16(&p, OTA_MAX_CHUNK);
    pb_u8(&p, 0);   // secure_boot
    pb_u8(&p, 0);   // flash_encrypt
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    pb_u8(&p, 1);
#else
    pb_u8(&p, 0);
#endif
    pb_u8(&p, 0);   // diff_ota_support
    send_payload(ctx, OTA_CAPS_RSP, &p);
}

static void on_progress_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    pb_t p = {0};
    pb_u32(&p, (uint32_t)s_ota.image_size);
    pb_u32(&p, (uint32_t)s_ota.written);
    pb_str16(&p, s_ota.target ? s_ota.target->label : "");
    send_payload(ctx, OTA_PROGRESS_RSP, &p);
}

static void on_state_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    reply_state(ctx);
}

// 指令表：靜態宣告，nc_action 管理器在開機時統計＋一次配置（見 nc_action.h）。
// 加一條指令 = 這裡多一行，總數自動變大，沒有魔術數字。
static const nc_action_t s_ota_actions[] = {
    { OTA_BEGIN,          on_begin,          "ota" },
    { OTA_WRITE,          on_write,          "ota" },
    { OTA_END,            on_end,            "ota" },
    { OTA_ABORT,          on_abort,          "ota" },
    { OTA_APPLY,          on_apply,          "ota" },
    { OTA_VERSION_QUERY,  on_version_query,  "ota" },
    { OTA_CAPS_QUERY,     on_caps_query,     "ota" },
    { OTA_PROGRESS_QUERY, on_progress_query, "ota" },
    { OTA_STATE_QUERY,    on_state_query,    "ota" },
};

NC_ACTION_TABLE_DEF(nc_ota_action_table, "ota", s_ota_actions);

void nc_ota_action_init(void)
{
    memset(&s_ota, 0, sizeof(s_ota));
}
