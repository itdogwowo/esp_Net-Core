#include "nc_file_action.h"
#include "nc_action.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"                 // f_getfree：IDF 的 FATFS 沒有 statvfs
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "nc_store.h"
#include "nc_boot.h"            // fs_format：狀態一律經 state manager
#include "nc_log.h"             // log_mode / log_level

static const char *TAG = "nc_file_action";

// ── 指令碼（與 mp_Net-Core slave/schema/*.json 對齊）──
#define FILE_BEGIN        0x2001
#define FILE_CHUNK        0x2002
#define FILE_END          0x2003
#define FILE_ACK          0x2004
#define FILE_QUERY        0x2005
#define FILE_QUERY_RSP    0x2006
#define FILE_READ         0x2007
#define FILE_CONFIRM      0x2008
#define FILE_DELETE       0x2009
#define FILE_UNDO         0x200A
#define FILE_SCAN         0x200B
#define FILE_ERROR_RSP    0x2010
#define STATUS_SET        0x1103   // data_json(str_u16len) + persist(u8)
#define STATUS_ACK        0x1104   // ok(u8) + message(str_u16len)

#define PATH_MAX_LOCAL    192
#define FILE_FULL_MAX     208      // /data/ + path + ".part" 仍夠位
#define FILE_AUX_MAX      (FILE_FULL_MAX + 8)   // <path>.part / <path>.bak + NUL
#define FILE_CHUNK_MAX    4096     // CDC rx.region 4,192；幀 = chunk + 17
#define MANIFEST_MAX_FILES 200
#define MANIFEST_MAX_BYTES (32 * 1024)
#define MANIFEST_MAX_DEPTH 6

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

static uint16_t rd_u16(const uint8_t *b)
{
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t rd_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
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

// 讀：任何模式（app → /data、USB → /ro）；寫：只准 APP。
// SD（/sd）v1 只讀。
static bool resolve_path(const char *req, bool for_write, char *out, size_t cap)
{
    if (req == NULL || req[0] == '\0' || strstr(req, "..") != NULL) {
        return false;
    }
    if (strncmp(req, NC_STORE_SD_MOUNT, 3) == 0 &&
        (req[3] == '\0' || req[3] == '/')) {
        if (for_write || strlen(req) >= cap) {
            return false;
        }
        snprintf(out, cap, "%s", req);
        return true;
    }
    if (for_write && !nc_store_app_ready()) {
        return false;
    }
    return map_path(req, out, cap);
}

static void sha256_of(const char *path, uint8_t out[32], bool *ok)
{
    *ok = false;
    memset(out, 0, 32);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }

    // 2 KB 讀取緩衝：**每次呼叫自己攞**。
    //   static  → 兩個 decoder 同時算 SHA-256 會互相蓋掉讀到的資料（算出錯雜湊）
    //   放棧   → decode 任務只剩 256 B 餘裕（實測 hwm），差一點就爆
    // 所以兩者都唔要，用 heap；用完即還，失敗就當讀不到。
    uint8_t *buf = malloc(2048);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool started = (psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS);

    while (started) {
        const size_t n = fread(buf, 1, 2048, f);
        if (n > 0 && psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            started = false;
            break;
        }
        if (n < 2048) {
            break;
        }
    }
    fclose(f);
    free(buf);

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
static const char *const k_err_names[] = {
    "err_no_space", "err_write_fail", "err_offset_mismatch", "err_id_mismatch",
    "err_sha_mismatch", "err_not_active", "err_busy",
};

static void send_error_kind(nc4_ctx_t *ctx, const char *kind, const char *path,
                            uint32_t failed, uint32_t written)
{
    uint8_t b[7 + 8 + 2 + PATH_MAX_LOCAL];
    memset(b, 0, 7);
    for (size_t i = 0; i < sizeof(k_err_names) / sizeof(k_err_names[0]); i++) {
        if (kind != NULL && strcmp(kind, k_err_names[i]) == 0) {
            b[i] = 1;
            break;
        }
    }
    size_t n = 7;
    n += wr_u32(b + n, failed);
    n += wr_u32(b + n, written);
    n += wr_str(b + n, path ? path : "");
    send_payload(ctx, FILE_ERROR_RSP, b, n);
}

// 舊 query handler 用：busy=1 → err_busy
static void send_error(nc4_ctx_t *ctx, int busy, const char *path)
{
    send_error_kind(ctx, busy ? "err_busy" : NULL, path, 0, 0);
}

// ── 0x2006 FILE_QUERY_RSP（給 query / end / confirm / undo / delete 用）──
static void send_query(nc4_ctx_t *ctx, const char *req, const uint8_t *known_sha)
{
    char full[FILE_FULL_MAX];
    struct stat st;
    bool exists = false;
    bool pending = false;
    uint32_t size = 0;
    uint8_t sha[32] = {0};

    if (resolve_path(req, false, full, sizeof full)) {
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            exists = true;
            size = (uint32_t)st.st_size;
            if (known_sha != NULL) {
                memcpy(sha, known_sha, 32);
            } else {
                bool ok = false;
                sha256_of(full, sha, &ok);
            }
            char bak[FILE_AUX_MAX];
            const int n = snprintf(bak, sizeof bak, "%s.bak", full);
            if (n > 0 && (size_t)n < sizeof bak && stat(bak, &st) == 0) {
                pending = true;
            }
        }
    }

    uint8_t b[1 + 32 + 4 + 2 + PATH_MAX_LOCAL + 4 + 1];
    size_t n = 0;
    b[n++] = exists ? 1 : 0;
    memcpy(b + n, sha, 32);
    n += 32;
    n += wr_u32(b + n, size);
    n += wr_str(b + n, req ? req : "");
    n += wr_u32(b + n, nc_store_app_ready() ? free_bytes() : 0);
    b[n++] = pending ? 1 : 0;
    send_payload(ctx, FILE_QUERY_RSP, b, n);
}

// ───────────────────────── 寫入 session ─────────────────────────
typedef struct {
    bool active;
    uint16_t id;
    char path[PATH_MAX_LOCAL];
    char full[FILE_FULL_MAX];
    char temp[FILE_FULL_MAX];
    uint32_t total;
    uint32_t written;
    uint16_t chunk;
    uint8_t sha_expect[32];
    FILE *f;
    psa_hash_operation_t sha;
    bool sha_active;
    int64_t last_us;          // 閒置 > 30 s 自動 abort，唔好永遠 busy
} file_sess_t;

static file_sess_t s_sess;
static SemaphoreHandle_t s_lock;

static bool lock_acquire(void)
{
    if (s_lock == NULL) {
        return true;                 // init 未跑：唔好令所有檔案操作死鎖
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(4000)) == pdTRUE;
}

static void lock_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void sess_reset(void)
{
    if (s_sess.f != NULL) {
        fclose(s_sess.f);
        s_sess.f = NULL;
    }
    if (s_sess.sha_active) {
        psa_hash_abort(&s_sess.sha);
    }
    memset(&s_sess, 0, sizeof s_sess);
}

static void sess_abort(void)
{
    if (s_sess.temp[0] != '\0') {
        remove(s_sess.temp);
    }
    sess_reset();
}

// ───────── manifest（0x200B FILE_SCAN；格式跟 mp：{path:{"s","h","p"}}）─────────
// 為什麼要有：schema 冇 directory listing；mp 版靠 /manifest.json 一次過交清單。
// C 版照抄呢個模型：scan 一次寫 /data/manifest.json，UI 讀一次就砌到樹。

static void sha_hex(const uint8_t sha[32], char out[65])
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[sha[i] >> 4];
        out[i * 2 + 1] = hex[sha[i] & 0x0F];
    }
    out[64] = '\0';
}

// manifest 路徑放 static：decode 任務棧只有 4 KB，nested 208+224 B 會爆。
// 所有 file handler 都由同一把 s_lock 序列化，所以 static 安全。
static char s_m_path[FILE_FULL_MAX];
static char s_m_tmp[FILE_AUX_MAX + 8];
static char s_end_bak[FILE_AUX_MAX];   // END 用；同一把 s_lock 保護

static bool manifest_paths(void)
{
    const char *base = nc_store_fs_base();
    if (base == NULL) {
        return false;
    }
    const int n = snprintf(s_m_path, sizeof s_m_path, "%s/manifest.json", base);
    if (n <= 0 || (size_t)n >= sizeof s_m_path) {
        return false;
    }
    const int t = snprintf(s_m_tmp, sizeof s_m_tmp, "%s.tmp", s_m_path);
    return t > 0 && (size_t)t < sizeof s_m_tmp;
}

static bool manifest_save(cJSON *root)
{
    if (!manifest_paths()) {
        return false;
    }
    char *js = cJSON_PrintUnformatted(root);
    if (js == NULL) {
        return false;
    }
    const size_t len = strlen(js);
    bool ok = false;
    FILE *f = fopen(s_m_tmp, "wb");
    if (f != NULL) {
        ok = (fwrite(js, 1, len, f) == len);
        fclose(f);
    }
    cJSON_free(js);
    if (ok) {
        remove(s_m_path);             // FATFS rename 唔會覆蓋
        ok = (rename(s_m_tmp, s_m_path) == 0);
    }
    if (!ok) {
        remove(s_m_tmp);
    }
    return ok;
}

static void manifest_scan_dir(const char *full, const char *rel, cJSON *root,
                              int depth, int *count)
{
    if (depth > MANIFEST_MAX_DEPTH || *count >= MANIFEST_MAX_FILES) {
        return;
    }
    DIR *dir = opendir(full);
    if (dir == NULL) {
        return;
    }
    char *child = malloc(FILE_FULL_MAX + 260);
    char *childrel = malloc(PATH_MAX_LOCAL + 260);
    if (child == NULL || childrel == NULL) {
        free(child);
        free(childrel);
        closedir(dir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && *count < MANIFEST_MAX_FILES) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            strcmp(name, "manifest.json") == 0) {
            continue;
        }
        const size_t ln = strlen(name);
        if ((ln >= 4 && strcmp(name + ln - 4, ".bak") == 0) ||
            (ln >= 5 && strcmp(name + ln - 5, ".part") == 0) ||
            (ln >= 4 && strcmp(name + ln - 4, ".tmp") == 0) ||
            (ln >= 7 && strcmp(name + ln - 7, ".esptmp") == 0)) {
            continue;
        }
        const int cn = snprintf(child, FILE_FULL_MAX + 260, "%s/%s", full, name);
        const int rn = snprintf(childrel, PATH_MAX_LOCAL + 260, "%s/%s", rel, name);
        if (cn <= 0 || (size_t)cn >= FILE_FULL_MAX + 260 ||
            rn <= 0 || (size_t)rn >= PATH_MAX_LOCAL + 260) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            manifest_scan_dir(child, childrel, root, depth + 1, count);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        uint8_t sha[32] = {0};
        bool ok = false;
        sha256_of(child, sha, &ok);
        if (!ok) {
            continue;
        }
        // pending = <child>.bak 存在（同 write-through 的 p 對齊）
        bool pending = false;
        const size_t clen = strlen(child);
        if (clen + 4 < FILE_FULL_MAX + 260) {
            memcpy(child + clen, ".bak", 5);      // 連 NUL
            struct stat bst;
            pending = (stat(child, &bst) == 0);
            child[clen] = '\0';
        }
        cJSON *e = cJSON_CreateObject();
        if (e == NULL) {
            break;
        }
        char hx[65];
        sha_hex(sha, hx);
        cJSON_AddNumberToObject(e, "s", (double)st.st_size);
        cJSON_AddStringToObject(e, "h", hx);
        cJSON_AddNumberToObject(e, "p", pending ? 1 : 0);
        cJSON_AddItemToObject(root, childrel, e);
        (*count)++;
    }
    free(child);
    free(childrel);
    closedir(dir);
}

// 0x200B FILE_SCAN（schema：target u8；0=local、1=SD）
// schema 冇回覆 → UI 送完之後 poll FILE_QUERY(/manifest.json)。
static void on_file_scan(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    const uint8_t target = (f->len >= 1) ? f->data[0] : 0;
    if (target != 0) {
        ESP_LOGW(TAG, "FILE_SCAN target=%u：SD 掃描 v1 未支援", (unsigned)target);
        return;
    }
    if (!lock_acquire()) {
        ESP_LOGW(TAG, "FILE_SCAN：busy");
        return;
    }
    if (!nc_store_app_ready()) {
        ESP_LOGW(TAG, "FILE_SCAN：FAT 唔係 app mode");
        lock_release();
        return;
    }
    const char *base = nc_store_fs_base();
    cJSON *root = cJSON_CreateObject();
    if (base == NULL || root == NULL) {
        cJSON_Delete(root);
        lock_release();
        return;
    }
    int count = 0;
    manifest_scan_dir(base, "", root, 0, &count);
    const bool ok = manifest_save(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "FILE_SCAN 完成：%d 個檔，manifest %s", count, ok ? "已寫入" : "寫入失敗");
    lock_release();
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
    // /sd/… 走 SD（裝置端掛載點就是 /sd，唔使再加前綴）。
    // 主機持有那張卡時裝置讀不到 —— 明確回 busy，唔好假裝有答案。
    const bool is_sd = (strncmp(req, NC_STORE_SD_MOUNT "/", sizeof(NC_STORE_SD_MOUNT)) == 0);
    if (is_sd) {
        if (nc_store_sd_on_usb()) {
            ESP_LOGW(TAG, "FILE_QUERY(%s)：SD 由主機持有", req);
            send_error(ctx, 1, req);
            return;
        }
        snprintf(full, sizeof(full), "%s", req);
    } else if (!map_path(req, full, sizeof(full))) {
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
    n += wr_u32(b + n, is_sd ? 0 : free_bytes());
    // pending：主機擁有 FAT 時，答案來自唯讀視圖 → 可能過期或半截
    b[n++] = is_sd ? 0 : (nc_store_app_ready() ? 0 : 1);   // SD 讀的是真值，不是唯讀視圖
    send_payload(ctx, FILE_QUERY_RSP, b, n);

    ESP_LOGI(TAG, "FILE_QUERY %s → exists=%d size=%u（%s）", req,
             (int)exists, (unsigned)size, nc_store_app_ready() ? "app" : "ro");
}

// ── 0x2001 FILE_BEGIN ──
static void on_file_begin(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    char req[PATH_MAX_LOCAL];
    char full[FILE_FULL_MAX];
    char temp[FILE_FULL_MAX];
    uint16_t id = 0;
    uint16_t chunk = 0;
    uint32_t total = 0;

    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", "", 0, 0);
        return;
    }

    if (s_sess.active) {
        // 上一條連線斷咗、冇 END：30 秒後自動放棄，唔好永遠 busy。
        if ((esp_timer_get_time() - s_sess.last_us) > 30000000LL) {
            sess_abort();
        } else {
            send_error_kind(ctx, "err_busy", "", 0, 0);
            goto out;
        }
    }
    memset(req, 0, sizeof req);
    if (f->len < 40 || rd_str(f->data + 40, (size_t)f->len - 40, req, sizeof req) == 0) {
        send_error_kind(ctx, "err_write_fail", "", 0, 0);
        goto out;
    }
    id = rd_u16(f->data);
    total = rd_u32(f->data + 2);
    chunk = rd_u16(f->data + 6);

    if (!nc_store_app_ready()) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        goto out;
    }
    if (!resolve_path(req, true, full, sizeof full)) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    if (chunk == 0 || chunk > FILE_CHUNK_MAX) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    if (total > 0) {
        const uint32_t fb = free_bytes();
        if (fb > 0 && total > fb) {
            send_error_kind(ctx, "err_no_space", req, 0, 0);
            goto out;
        }
    }
    const int tn = snprintf(temp, sizeof temp, "%s.part", full);
    if (tn <= 0 || (size_t)tn >= sizeof temp) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }

    FILE *fp = fopen(temp, "wb");
    if (fp == NULL) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }

    memset(&s_sess, 0, sizeof s_sess);
    if (psa_hash_setup(&s_sess.sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(fp);
        remove(temp);
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    s_sess.active = true;
    s_sess.id = id;
    s_sess.total = total;
    s_sess.chunk = chunk;
    s_sess.f = fp;
    s_sess.sha_active = true;
    s_sess.last_us = esp_timer_get_time();
    memcpy(s_sess.sha_expect, f->data + 8, 32);
    snprintf(s_sess.path, sizeof s_sess.path, "%s", req);
    snprintf(s_sess.full, sizeof s_sess.full, "%s", full);
    snprintf(s_sess.temp, sizeof s_sess.temp, "%s", temp);

    ESP_LOGI(TAG, "FILE_BEGIN %s → %s（%u B，chunk %u）",
             req, temp, (unsigned)total, (unsigned)chunk);
    {
        uint8_t b[6];
        b[0] = (uint8_t)id;
        b[1] = (uint8_t)(id >> 8);
        wr_u32(b + 2, 0);
        send_payload(ctx, FILE_ACK, b, sizeof b);
    }

out:
    lock_release();
}

// ── 0x2002 FILE_CHUNK ──
static void on_file_chunk(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t id = 0;
    uint32_t off = 0;
    const uint8_t *data = NULL;
    size_t n = 0;

    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", "", 0, 0);
        return;
    }

    if (!s_sess.active) {
        send_error_kind(ctx, "err_not_active", "", 0, 0);
        goto out;
    }
    if (f->len < 6) {
        send_error_kind(ctx, "err_write_fail", s_sess.path, 0, s_sess.written);
        goto out;
    }
    id = rd_u16(f->data);
    off = rd_u32(f->data + 2);
    data = f->data + 6;
    n = (size_t)f->len - 6;

    if (id != s_sess.id) {
        send_error_kind(ctx, "err_id_mismatch", s_sess.path, 0, s_sess.written);
        goto out;
    }
    if (off != s_sess.written) {
        send_error_kind(ctx, "err_offset_mismatch", s_sess.path, off, s_sess.written);
        goto out;
    }
    if ((uint64_t)s_sess.written + n > s_sess.total) {
        send_error_kind(ctx, "err_write_fail", s_sess.path, off, s_sess.written);
        goto out;
    }
    if (n > 0) {
        if (fwrite(data, 1, n, s_sess.f) != n ||
            psa_hash_update(&s_sess.sha, data, n) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "FILE_CHUNK 寫入失敗 @%u", (unsigned)off);
            sess_abort();
            send_error_kind(ctx, "err_write_fail", "", off, 0);
            goto out;
        }
        s_sess.written += (uint32_t)n;
    }
    s_sess.last_us = esp_timer_get_time();

    {
        uint8_t b[6];
        b[0] = (uint8_t)id;
        b[1] = (uint8_t)(id >> 8);
        wr_u32(b + 2, off);
        send_payload(ctx, FILE_ACK, b, sizeof b);
    }

out:
    lock_release();
}

// ── 0x2003 FILE_END ──
static void on_file_end(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    // 注意 stack：decode 任務棧只有 4 KB。req 直接借 s_sess.path，.bak 用 static，
    // stacked 只剩 digest 32 B —— 之前 192+216 B 會令 manifest 更新時爆棧（rst=4）。
    uint8_t digest[32] = {0};
    size_t olen = 0;
    bool ok = false;
    uint16_t id = 0;

    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", "", 0, 0);
        return;
    }

    if (!s_sess.active) {
        send_error_kind(ctx, "err_not_active", "", 0, 0);
        goto out;
    }
    const char *req = s_sess.path;
    if (f->len >= 2) {
        id = rd_u16(f->data);
        if (id != s_sess.id) {
            send_error_kind(ctx, "err_id_mismatch", req, 0, s_sess.written);
            goto out;
        }
    }

    if (s_sess.f != NULL) {
        fclose(s_sess.f);
        s_sess.f = NULL;
    }
    if (s_sess.written != s_sess.total) {
        ESP_LOGW(TAG, "FILE_END %s：written=%u total=%u", req,
                 (unsigned)s_sess.written, (unsigned)s_sess.total);
        sess_abort();
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    if (!s_sess.sha_active ||
        psa_hash_finish(&s_sess.sha, digest, sizeof digest, &olen) != PSA_SUCCESS ||
        olen != sizeof digest) {
        sess_abort();
        send_error_kind(ctx, "err_sha_mismatch", req, 0, 0);
        goto out;
    }
    s_sess.sha_active = false;

    if (memcmp(digest, s_sess.sha_expect, 32) != 0) {
        ESP_LOGW(TAG, "FILE_END %s：SHA 不符", req);
        sess_abort();
        send_error_kind(ctx, "err_sha_mismatch", req, 0, 0);
        goto out;
    }

    ok = true;
    const int bn = snprintf(s_end_bak, sizeof s_end_bak, "%s.bak", s_sess.full);
    if (bn <= 0 || (size_t)bn >= sizeof s_end_bak) {
        ok = false;
    }
    struct stat st;
    if (ok && stat(s_sess.full, &st) == 0) {
        remove(s_end_bak);
        if (rename(s_sess.full, s_end_bak) != 0) {
            ok = false;
        }
    }
    if (ok && rename(s_sess.temp, s_sess.full) != 0) {
        // 換唔到新檔：試把 .bak 還原
        if (stat(s_end_bak, &st) == 0) {
            rename(s_end_bak, s_sess.full);
        }
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "FILE_END %s：commit 失敗", req);
        sess_abort();
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }

    ESP_LOGI(TAG, "FILE_END %s ok（%u B）", req, (unsigned)s_sess.total);
    // manifest 只由 0x200B FILE_SCAN 寫；handler 唔做 cJSON/FAT（decode 4KB 棧會爆）。
    send_query(ctx, req, digest);
    memset(&s_sess, 0, sizeof s_sess);   // send_query 之後才清，req 指向 s_sess.path

out:
    lock_release();
}

// ── 0x2007 FILE_READ ──
static void on_file_read(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    char req[PATH_MAX_LOCAL];
    char full[FILE_FULL_MAX];
    uint32_t off = 0;
    uint16_t want = 0;
    uint8_t *out = NULL;
    FILE *fp = NULL;
    size_t n = 0;

    memset(req, 0, sizeof req);
    const size_t used = rd_str(f->data, f->len, req, sizeof req);
    if (used == 0 || f->len < used + 6) {
        send_error_kind(ctx, "err_write_fail", "", 0, 0);
        return;
    }
    off = rd_u32(f->data + used);
    want = rd_u16(f->data + used + 4);
    if (want > FILE_CHUNK_MAX) {
        want = FILE_CHUNK_MAX;
    }

    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        return;
    }
    out = malloc(6 + (size_t)want);
    if (out == NULL) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    out[0] = 0;
    out[1] = 0;
    wr_u32(out + 2, off);

    if (resolve_path(req, false, full, sizeof full)) {
        fp = fopen(full, "rb");
    }
    if (fp != NULL) {
        if (fseek(fp, (long)off, SEEK_SET) == 0) {
            n = fread(out + 6, 1, want, fp);
        }
        fclose(fp);
    } else {
        ESP_LOGW(TAG, "FILE_READ %s：開唔到", req);
    }

    send_payload(ctx, FILE_CHUNK, out, 6 + n);

out:
    free(out);
    lock_release();
}

// ── 0x2009 FILE_DELETE ──
static void on_file_delete(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    // decode 只有 4 KB 棧：這三條路徑改放 static（s_lock 已序列化）。
    static char req[PATH_MAX_LOCAL];
    static char full[FILE_FULL_MAX];
    static char aux[FILE_AUX_MAX];
    struct stat st;

    memset(req, 0, sizeof req);
    if (rd_str(f->data, f->len, req, sizeof req) == 0) {
        send_error_kind(ctx, "err_write_fail", "", 0, 0);
        return;
    }
    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        return;
    }
    if (!resolve_path(req, true, full, sizeof full)) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        goto out;
    }
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    remove(full);
    if (stat(full, &st) == 0) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);   // 刪唔到（FAT／權限）
        goto out;
    }
    snprintf(aux, sizeof aux, "%s.bak", full);
    remove(aux);
    snprintf(aux, sizeof aux, "%s.part", full);
    remove(aux);
    send_query(ctx, req, NULL);

out:
    lock_release();
}

// ── 0x2008 FILE_CONFIRM ──
static void on_file_confirm(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    static char req[PATH_MAX_LOCAL];
    static char full[FILE_FULL_MAX];
    static char bak[FILE_AUX_MAX];

    memset(req, 0, sizeof req);
    if (rd_str(f->data, f->len, req, sizeof req) == 0) {
        send_error_kind(ctx, "err_write_fail", "", 0, 0);
        return;
    }
    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        return;
    }
    if (!resolve_path(req, true, full, sizeof full)) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        goto out;
    }
    snprintf(bak, sizeof bak, "%s.bak", full);
    struct stat st;
    if (stat(bak, &st) == 0 && remove(bak) != 0) {
        send_error_kind(ctx, "err_write_fail", req, 0, 0);
        goto out;
    }
    send_query(ctx, req, NULL);

out:
    lock_release();
}

// ── 0x200A FILE_UNDO ──
static void on_file_undo(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    static char req[PATH_MAX_LOCAL];
    static char full[FILE_FULL_MAX];
    static char bak[FILE_AUX_MAX];
    struct stat st;

    memset(req, 0, sizeof req);
    if (rd_str(f->data, f->len, req, sizeof req) == 0) {
        send_error_kind(ctx, "err_write_fail", "", 0, 0);
        return;
    }
    if (!lock_acquire()) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        return;
    }
    if (!resolve_path(req, true, full, sizeof full)) {
        send_error_kind(ctx, "err_busy", req, 0, 0);
        goto out;
    }
    snprintf(bak, sizeof bak, "%s.bak", full);
    if (stat(bak, &st) == 0) {
        remove(full);
        if (rename(bak, full) != 0) {
            send_error_kind(ctx, "err_write_fail", req, 0, 0);
            goto out;
        }
    }
    send_query(ctx, req, NULL);

out:
    lock_release();
}

// ── 0x1103 STATUS_SET → 0x1104 STATUS_ACK ──
// 本版只認 System.file_mode（0 = 交還 host、1 = 裝置接管）、fs_format、log_debug_mode。
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

// 指令表：靜態宣告（加一條 = 多一行）
static const nc_action_t s_file_actions[] = {
    { FILE_BEGIN,   on_file_begin,   "file" },
    { FILE_CHUNK,   on_file_chunk,   "file" },
    { FILE_END,     on_file_end,     "file" },
    { FILE_QUERY,   on_file_query,   "file" },
    { FILE_READ,    on_file_read,    "file" },
    { FILE_CONFIRM, on_file_confirm, "file" },
    { FILE_DELETE,  on_file_delete,  "file" },
    { FILE_UNDO,    on_file_undo,    "file" },
    { FILE_SCAN,    on_file_scan,    "file" },
    { STATUS_SET,   on_status_set,   "file" },
};

NC_ACTION_TABLE_DEF(nc_file_action_table, "file", s_file_actions);

void nc_file_action_init(void)
{
    psa_crypto_init();                   // 幂等
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}
