#include "nc_file_action.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "ff.h"                 // f_getfree：IDF 的 FATFS 沒有 statvfs
#include "psa/crypto.h"
#include "nc_store.h"
#include "nc_boot.h"            // fs_format：狀態一律經 state manager
#include "nc_log.h"             // log_mode / log_level

static const char *TAG = "nc_file_action";

// ── 指令碼（與 mp_Net-Core slave/schema/*.json 對齊）──
#define FILE_QUERY        0x2005
#define FILE_QUERY_RSP    0x2006
#define FILE_ERROR_RSP    0x2010
#define STATUS_SET        0x1103   // data_json(str_u16len) + persist(u8)
#define STATUS_ACK        0x1104   // ok(u8) + message(str_u16len)

#define PATH_MAX_LOCAL    192

// ───────────────────────── 小工具 ─────────────────────────

// 回覆幀：**每次送出時才要**，所以用 heap 而不是 static。
// 以前是 static uint8_t s_tx[NC4_MAX_FRAME]（8,205 B）—— 全域一份，
// 兩個 decoder 同時回 FILE_READ 就會互相蓋掉對方正在送的幀。
// 這裡的 send 是同步的（lwIP / UART / CDC 都會即刻複製），所以送完就可以 free。
static void send_payload(nc4_ctx_t *ctx, uint16_t cmd, const uint8_t *p, size_t n)
{
    if (ctx == NULL || ctx->send == NULL || n > NC4_MAX_PAYLOAD) {
        return;
    }
    const size_t cap = (size_t)NC4_HDR_LEN + n + NC4_CRC_LEN;
    uint8_t *frame = malloc(cap);
    if (frame == NULL) {
        return;
    }
    const int len = nc4_pack_into(frame, cap, cmd, nc4_reply_addr(ctx), p, n);
    if (len > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)len);
    }
    free(frame);
}

static size_t wr_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    return 4;
}

// str_u16len：2B 長度 + UTF-8
static size_t wr_str(uint8_t *b, const char *s)
{
    const size_t n = strlen(s);
    b[0] = (uint8_t)n;
    b[1] = (uint8_t)(n >> 8);
    memcpy(b + 2, s, n);
    return 2 + n;
}

static size_t rd_str(const uint8_t *p, size_t len, char *out, size_t cap)
{
    if (len < 2) {
        return 0;
    }
    const size_t n = (size_t)p[0] | ((size_t)p[1] << 8);
    if (n + 2 > len || n >= cap) {
        return 0;
    }
    memcpy(out, p + 2, n);
    out[n] = '\0';
    return n + 2;
}

// "/config.json" 或 "config.json" → "<base>/config.json"；拒絕 ".."
// base 由 store 決定：APP 模式 = /data（可寫）、USB 模式 = /ro（唯讀視圖）
static bool map_path(const char *in, char *out, size_t cap)
{
    const char *base = nc_store_fs_base();
    if (base == NULL || in == NULL || strstr(in, "..") != NULL) {
        return false;
    }
    const char *p = in;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    const int n = snprintf(out, cap, "%s/%s", base, p);
    return n > 0 && (size_t)n < cap;
}

static void sha256_of(const char *path, uint8_t out[32], bool *ok)
{
    *ok = false;
    memset(out, 0, 32);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }

    // 2 KB 讀取緩衝放**棧**（decode 任務 6,144 B，這一層夠深）。
    // 以前是 static：兩個 decoder 同時算 SHA-256 會互相蓋掉讀到的資料 → 算出錯的雜湊。
    uint8_t buf[2048];
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool started = (psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS);

    while (started) {
        const size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0 && psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            started = false;
            break;
        }
        if (n < sizeof(buf)) {
            break;
        }
    }
    fclose(f);

    if (started) {
        size_t olen = 0;
        if (psa_hash_finish(&op, out, 32, &olen) != PSA_SUCCESS || olen != 32) {
            memset(out, 0, 32);
        } else {
            *ok = true;
        }
    } else {
        psa_hash_abort(&op);
    }
}

static uint32_t free_bytes(void)
{
    const char *drv = nc_store_fs_drive();
    if (drv == NULL) {
        return 0;
    }
    FATFS *fs = NULL;
    DWORD nclst = 0;
    if (f_getfree(drv, &nclst, &fs) != FR_OK || fs == NULL) {
        return 0;
    }
    // 注意：磁區大小是 fs->ssize，不是 512 —— WL 的 sector 是 4 KB，
    // 寫死 512 會把可用空間少報 8 倍（實測 211 KiB vs 真正的 1688 KiB）。
    const uint32_t cluster = (uint32_t)fs->csize * (uint32_t)fs->ssize;
    return (uint32_t)nclst * cluster;
}

// ── 0x2010 FILE_ERROR_RSP（順序與 schema 相同）──
static void send_error(nc4_ctx_t *ctx, int busy, const char *path)
{
    uint8_t b[7 + 8 + 2 + PATH_MAX_LOCAL];
    memset(b, 0, 7);
    b[6] = busy ? 1 : 0;
    size_t n = 7;
    n += wr_u32(b + n, 0);
    n += wr_u32(b + n, 0);
    n += wr_str(b + n, path ? path : "");
    send_payload(ctx, FILE_ERROR_RSP, b, n);
}

// ── 0x2005 FILE_QUERY ──
static void on_file_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    char req[PATH_MAX_LOCAL] = {0};
    if (rd_str(f->data, f->len, req, sizeof(req)) == 0) {
        send_error(ctx, 0, "");
        return;
    }

    char full[PATH_MAX_LOCAL];
    if (!map_path(req, full, sizeof(full))) {
        ESP_LOGW(TAG, "FILE_QUERY(%s) 讀不到（mode=%s）", req, nc_store_mode_name());
        send_error(ctx, 1, req);
        return;
    }

    struct stat st;
    const bool exists = (stat(full, &st) == 0) && S_ISREG(st.st_mode);
    uint8_t sha[32] = {0};
    uint32_t size = 0;

    if (exists) {
        size = (uint32_t)st.st_size;
        bool ok = false;
        sha256_of(full, sha, &ok);
        if (!ok) {
            ESP_LOGW(TAG, "sha256 失敗：%s", full);
        }
    }

    uint8_t b[1 + 32 + 4 + 2 + PATH_MAX_LOCAL + 4 + 1];
    size_t n = 0;
    b[n++] = exists ? 1 : 0;
    memcpy(b + n, sha, 32);
    n += 32;
    n += wr_u32(b + n, size);
    n += wr_str(b + n, req);
    n += wr_u32(b + n, free_bytes());
    // pending：主機擁有 FAT 時，答案來自唯讀視圖 → 可能過期或半截
    b[n++] = nc_store_app_ready() ? 0 : 1;
    send_payload(ctx, FILE_QUERY_RSP, b, n);

    ESP_LOGI(TAG, "FILE_QUERY %s → exists=%d size=%u（%s）", req,
             (int)exists, (unsigned)size, nc_store_app_ready() ? "app" : "ro");
}

// ── 0x1103 STATUS_SET → 0x1104 STATUS_ACK ──
// 本版只認 System.file_mode（0 = 交還 host、1 = 裝置接管）。
// 之後有完整 config tree 時，這支要搬到 status 元件（GET/SET 放一起）。
static void ack(nc4_ctx_t *ctx, bool ok, const char *msg)
{
    uint8_t b[1 + 2 + 160];
    b[0] = ok ? 1 : 0;
    const size_t n = 1 + wr_str(b + 1, msg ? msg : "");
    send_payload(ctx, STATUS_ACK, b, n);
}

static void on_status_set(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    char json[256] = {0};
    const size_t used = rd_str(f->data, f->len, json, sizeof(json));
    if (used == 0) {
        ack(ctx, false, "bad payload");
        return;
    }
    const uint8_t persist = (f->len > used) ? f->data[used] : 0;
    (void)persist;    // file_mode 是執行期狀態，不落盤

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ack(ctx, false, "bad json");
        return;
    }

    cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "System");

    // ── System.fs_format：只寫 NVS 旗標，下一次開機才真的格式化 ──
    // 支援兩種寫法（跟舊版 STATUS_SET 的點分路徑相容）：
    //   {"System":{"fs_format":1}}      / {"System":{"format":1}}（別名）
    //   {"System.fs_format":1}          / {"System.format":1}
    cJSON *fmt = cJSON_IsObject(sys) ? cJSON_GetObjectItemCaseSensitive(sys, "fs_format") : NULL;
    if (fmt == NULL && cJSON_IsObject(sys)) {
        fmt = cJSON_GetObjectItemCaseSensitive(sys, "format");
    }
    if (fmt == NULL) {
        fmt = cJSON_GetObjectItemCaseSensitive(root, "System.fs_format");
    }
    if (fmt == NULL) {
        fmt = cJSON_GetObjectItemCaseSensitive(root, "System.format");
    }
    if (fmt != NULL) {
        bool on;
        if (cJSON_IsBool(fmt)) {
            on = cJSON_IsTrue(fmt);
        } else if (cJSON_IsNumber(fmt)) {
            on = fmt->valueint != 0;
        } else if (cJSON_IsString(fmt) && fmt->valuestring != NULL &&
                   fmt->valuestring[0] != '\0') {
            const char c = fmt->valuestring[0];
            on = (c == '1' || c == 't' || c == 'T');
        } else {
            cJSON_Delete(root);
            ack(ctx, false, "System.fs_format 要 0/1");
            return;
        }
        cJSON_Delete(root);
        // 狀態一律經 state manager：它決定要不要落盤（這裡一定要，否則重開機就冇）。
        nc_boot_request_format(on);
        char msg[96];
        snprintf(msg, sizeof(msg), "fs_format=%d; %s", on ? 1 : 0,
                 on ? "下次開機抹 data 分區並重建 FAT" : "已取消");
        ESP_LOGI(TAG, "STATUS_SET %s → %s", json, msg);
        ack(ctx, true, msg);
        return;
    }

    // ── System.log_debug_mode：0=正常（INFO、2 KB）｜1=除錯（全部、8 KB）──
    cJSON *ll = cJSON_IsObject(sys) ? cJSON_GetObjectItemCaseSensitive(sys, "log_debug_mode") : NULL;
    if (ll != NULL) {
        if (!cJSON_IsNumber(ll) || (ll->valueint != 0 && ll->valueint != 1)) {
            cJSON_Delete(root);
            ack(ctx, false, "log_debug_mode 要 0 或 1");
            return;
        }
        const int v = ll->valueint;
        cJSON_Delete(root);
        (void)nc_log_set_levels((uint8_t)v);
        char msg[80];
        snprintf(msg, sizeof(msg), "log_debug_mode=%d；即時生效，並記住下次開機", v);
        ESP_LOGI(TAG, "STATUS_SET %s → %s", json, msg);
        ack(ctx, true, msg);
        return;
    }

    cJSON *fm = cJSON_IsObject(sys) ? cJSON_GetObjectItemCaseSensitive(sys, "file_mode") : NULL;

    if (!cJSON_IsNumber(fm)) {
        cJSON_Delete(root);
        ack(ctx, false, "only System.file_mode / System.fs_format is supported");
        return;
    }

    const int mode = fm->valueint;
    cJSON_Delete(root);

    const esp_err_t err = (mode == 1) ? nc_store_claim_app() : nc_store_release_usb();
    char msg[96];
    snprintf(msg, sizeof(msg), "file_mode=%d mode=%s%s", mode, nc_store_mode_name(),
             err == ESP_OK ? "" : " (switch failed)");
    ESP_LOGI(TAG, "STATUS_SET %s → %s", json, msg);
    ack(ctx, err == ESP_OK, msg);
}

void nc_file_action_register(nc4_decoder_t *decoder)
{
    if (decoder == NULL) {
        return;
    }
    psa_crypto_init();                   // 幂等
    nc4_decoder_on_owner(decoder, FILE_QUERY, on_file_query, "file");
    nc4_decoder_on_owner(decoder, STATUS_SET, on_status_set, "file");
    ESP_LOGI(TAG, "已註冊：0x2005 FILE_QUERY / 0x1103 STATUS_SET");
}
