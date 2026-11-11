// 緩衝區（nc_buf）—— 實作。設計見 docs/03_notes/buffers.md。
#include "nc_buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NC_BUF_HOST_TEST
// 主機測試：用 posix_memalign，冇 ESP-IDF 的 heap／log／空間判斷
#define BUF_LOGI(...) ((void)0)
#define BUF_LOGE(...) ((void)0)
static void *buf_alloc(size_t bytes, size_t align, nc_buf_where_t where)
{
    (void)where;
    void *p = NULL;
    if (posix_memalign(&p, align ? align : NC_BUF_ALIGN, bytes) != 0) {
        return NULL;
    }
    return p;
}
static bool buf_is_ext_ram(const void *p)
{
    (void)p;
    return false;   // host 冇 PSRAM；測試裡面靠「指標落喺邊條 buffer」判空間
}
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char *TAG = "buf";
#define BUF_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define BUF_LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)

static void *buf_alloc(size_t bytes, size_t align, nc_buf_where_t where)
{
    uint32_t caps = MALLOC_CAP_8BIT;
    switch (where) {
    case NC_BUF_DMA:
        caps |= MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
        break;
    case NC_BUF_PSRAM:
        caps |= MALLOC_CAP_SPIRAM;
        break;
    case NC_BUF_SRAM:
    default:
        caps |= MALLOC_CAP_INTERNAL;
        break;
    }
    return heap_caps_aligned_alloc(align ? align : NC_BUF_ALIGN, bytes, caps);
}

static bool buf_is_ext_ram(const void *p)
{
    return esp_ptr_external_ram(p);
}
#endif

#define NC_BUF_SLOTS_MAX 1024

// ── 每格：範圍 ＋ 狀態 ＋ generation ──
// off/len/gen 一律 32-bit：LED 一幀 38,400 B 只是剛剛好，LCD 一幀 750 KB 會爆 uint16。
typedef struct {
    uint32_t off;      // 資料起點（reserve 用）
    uint32_t len;      // 實際用了幾多
    uint32_t gen;      // 這一格最後一次 publish 的序號
    uint8_t  state;    // nc_buf_state_t
} nc_buf_desc_t;

struct nc_buf {
    char     name[NC_BUF_NAME_MAX];
    char     owner[NC_BUF_NAME_MAX];
    uint8_t  where;        // nc_buf_where_t
    uint8_t  flags;
    uint16_t n;
    uint32_t slot_bytes;
    uint8_t *data;
    bool     owns;
    nc_buf_desc_t *desc;

    uint16_t w;            // FIFO 寫位置
    uint16_t r;            // FIFO 讀位置（get_end 才前進）
    int16_t  rd;           // 正在讀的格（-1 = 冇）
    int16_t  wr;           // 正在寫的格（-1 = 冇）

    // STREAM 模式：線性位元組流（必要時壓實），同 parser 環的做法一致
    uint32_t st_off;       // 讀起點
    uint32_t st_end;       // 寫終點（st_end - st_off = pending）

    uint32_t generation;   // 每次 publish（put_end）＋1；換手再＋1
    uint32_t puts, gets, fail_full, fail_big, viol, aborts;
    uint32_t by_s2s, by_s2p, by_p2s, by_p2p;   // 各方向的累計 bytes
    uint32_t by_xcopy;     // 由「另一條 buffer」抄過來的 bytes（本來可能可以零複製）
};

static nc_buf_t *s_reg[NC_BUF_MAX];
static uint32_t s_next_gen = 1;

// ───────────────────────── 內部工具 ─────────────────────────

static inline uint8_t *slot_ptr(nc_buf_t *b, int idx)
{
    return b->data + (size_t)idx * b->slot_bytes;
}

static bool range_has(const void *base, size_t bytes, const void *p)
{
    const uintptr_t a = (uintptr_t)base;
    const uintptr_t z = a + bytes;
    const uintptr_t v = (uintptr_t)p;
    return (v >= a) && (v < z);
}

// 來源指標落在哪條 buffer 裡（冇 = NULL）。
// 這一條就是「工具可以自動判斷」的部分：指標在不在我們自己的緩衝區、哪一條、哪個空間。
static const nc_buf_t *src_buf_of(const void *p)
{
    for (int i = 0; i < NC_BUF_MAX; i++) {
        const nc_buf_t *b = s_reg[i];
        if (b != NULL && range_has(b->data, (size_t)b->slot_bytes * b->n, p)) {
            return b;
        }
    }
    return NULL;
}

static bool src_is_psram(const void *p)
{
    const nc_buf_t *sb = src_buf_of(p);
    return (sb != NULL) ? (sb->where == NC_BUF_PSRAM) : buf_is_ext_ram(p);
}

static void account_copy(nc_buf_t *b, const void *src, uint32_t len)
{
    const nc_buf_t *sb = src_buf_of(src);
    const bool dst_p = (b->where == NC_BUF_PSRAM);
    const bool src_p = (sb != NULL) ? (sb->where == NC_BUF_PSRAM) : src_is_psram(src);
    // 來源係「另一條 buffer」→ 這次 copy 有可能是可以避免的（改用 borrow 或 handover）。
    // 工具唔會自動幫你改，但會計出來，令「隱形 copy」在 mem.buf 現形。
    if (sb != NULL && sb != b) {
        b->by_xcopy += len;
    }
    if (src_p && dst_p) {
        b->by_p2p += len;
    } else if (src_p) {
        b->by_p2s += len;
    } else if (dst_p) {
        b->by_s2p += len;
    } else {
        b->by_s2s += len;
    }
}

static const char *where_name(uint8_t where)
{
    switch (where) {
    case NC_BUF_PSRAM: return "P";
    case NC_BUF_DMA:   return "D";
    default:           return "S";
    }
}

static bool all_free(const nc_buf_t *b)
{
    for (uint16_t i = 0; i < b->n; i++) {
        if (b->desc[i].state != NC_BUF_ST_FREE) {
            return false;
        }
    }
    return true;
}

static void buf_free(nc_buf_t *b)
{
    if (b == NULL) {
        return;
    }
    if (b->owns) {
        free(b->data);
    }
    free(b->desc);
    free(b);
}

// ───────────────────────── 建立 ─────────────────────────

static nc_buf_t *buf_init(const char *name, void *region, size_t region_bytes,
                          nc_buf_where_t where, uint32_t slot_bytes, uint16_t n,
                          uint32_t flags, bool owns)
{
    if (name == NULL || slot_bytes == 0 || n == 0 || n > NC_BUF_SLOTS_MAX) {
        BUF_LOGE("nc_buf_create: 參數唔合法");
        return NULL;
    }
    if ((flags & NC_BUF_F_STREAM) && n != 1) {
        BUF_LOGE("nc_buf_create: STREAM 模式的 n 必須係 1（收到 %u）", (unsigned)n);
        return NULL;
    }
    const size_t bytes = (size_t)slot_bytes * n;
    if (owns) {
        if (region == NULL) {
            return NULL;
        }
    } else {
        if (region == NULL || region_bytes < bytes) {
            BUF_LOGE("%s: 區域唔夠（要 %u B，得 %u B）", name,
                     (unsigned)bytes, (unsigned)region_bytes);
            return NULL;
        }
    }

    int slot = -1;
    for (int i = 0; i < NC_BUF_MAX; i++) {
        if (s_reg[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // registry 滿：一定要喊，唔可以靜默（nc_hub 的舊缺陷）
        BUF_LOGE("nc_buf registry 滿（%d 條）：%s 建立失敗", NC_BUF_MAX, name);
        return NULL;
    }

    nc_buf_t *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    nc_buf_desc_t *d = calloc(n, sizeof(*d));
    if (d == NULL) {
        free(b);
        return NULL;
    }

    snprintf(b->name, sizeof(b->name), "%s", name);
    snprintf(b->owner, sizeof(b->owner), "%s", name);
    b->where = (uint8_t)where;
    b->flags = (uint8_t)(flags & 0xFFu);
    b->n = n;
    b->slot_bytes = slot_bytes;
    b->desc = d;
    b->data = (uint8_t *)region;
    b->owns = owns;
    b->rd = -1;
    b->wr = -1;
    b->generation = 0;

    s_reg[slot] = b;
    BUF_LOGI("buf %-10s %u x %u B（%s，%s，合計 %u B）", b->name, (unsigned)n,
             (unsigned)slot_bytes, where_name(b->where),
             (flags & NC_BUF_F_STREAM) ? "STREAM" : "FIFO", (unsigned)bytes);
    return b;
}

nc_buf_t *nc_buf_create(const char *name, nc_buf_where_t where,
                        uint32_t slot_bytes, uint16_t n, uint32_t flags)
{
    const size_t bytes = (size_t)slot_bytes * n;
    if (name == NULL || bytes == 0) {
        return NULL;
    }
    void *data = buf_alloc(bytes, NC_BUF_ALIGN, where);
    if (data == NULL) {
        BUF_LOGE("%s: 要不到 %u B（%s）", name, (unsigned)bytes, where_name((uint8_t)where));
        return NULL;
    }
    if ((uintptr_t)data % NC_BUF_ALIGN != 0) {
        BUF_LOGE("%s: 配置返來的指標唔係 %d B 對齊", name, NC_BUF_ALIGN);
        free(data);
        return NULL;
    }
    nc_buf_t *b = buf_init(name, data, bytes, where, slot_bytes, n, flags, true);
    if (b == NULL) {
        free(data);
    }
    return b;
}

nc_buf_t *nc_buf_at(const char *name, void *region, size_t region_bytes,
                    nc_buf_where_t where, uint32_t slot_bytes, uint16_t n,
                    uint32_t flags)
{
    return buf_init(name, region, region_bytes, where, slot_bytes, n, flags, false);
}

// ───────────────────────── 寫 ─────────────────────────

void *nc_buf_put_begin(nc_buf_t *b, uint32_t reserve)
{
    if (b == NULL) {
        return NULL;
    }
    if (reserve > b->slot_bytes) {
        b->fail_big++;
        return NULL;
    }

    if (b->flags & NC_BUF_F_STREAM) {
        return NULL;                    // 串流用 nc_buf_write()
    }
    if (b->wr >= 0) {
        return NULL;                    // 同一個 writer 一次只可以揸住一格
    }

    // 只有 FIFO：只可以寫 w 這一格；唔 FREE 就係滿（唔可以跳格）。
    // **唔標 WRITING**：producer 在 put_end 之前死掉，只會令 w 唔前進，
    // 下一輪由同一格重寫；環永遠唔會卡死（nc4_bus 刻意咁做，這裡照跟）。
    // （「隨機／任意空格」模式 2026-11-10 移除：冇真實用戶，而且它要 WRITING 狀態，
    //   多一個狀態就多一個卡死的地方。）
    if (b->desc[b->w].state != NC_BUF_ST_FREE) {
        b->fail_full++;
        return NULL;
    }
    const int idx = b->w;

    nc_buf_desc_t *d = &b->desc[idx];
    d->off = reserve;
    d->len = 0;
    b->wr = (int16_t)idx;
    return slot_ptr(b, idx) + reserve;
}

esp_err_t nc_buf_put_end(nc_buf_t *b, uint32_t len)
{
    if (b == NULL || b->wr < 0) {
        if (b != NULL) {
            b->viol++;
        }
        return ESP_ERR_INVALID_STATE;
    }
    const int idx = b->wr;
    nc_buf_desc_t *d = &b->desc[idx];
    if (d->state != NC_BUF_ST_FREE) {
        b->viol++;
        b->wr = -1;
        return ESP_ERR_INVALID_STATE;
    }
    if (len > b->slot_bytes - d->off) {
        b->fail_big++;
        d->state = NC_BUF_ST_FREE;
        b->wr = -1;
        return ESP_ERR_INVALID_SIZE;
    }
    d->len = len;
    d->gen = ++s_next_gen;
    d->state = NC_BUF_ST_READY;
    b->generation++;
    b->puts++;
    b->wr = -1;
    b->w = (uint16_t)((b->w + 1) % b->n);   // 格模式只有 FIFO，指標一定前進
    return ESP_OK;
}

esp_err_t nc_buf_put(nc_buf_t *b, const void *src, uint32_t len)
{
    if (b == NULL || (src == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > b->slot_bytes) {
        b->fail_big++;
        return ESP_ERR_INVALID_SIZE;
    }
    void *dst = nc_buf_put_begin(b, 0);
    if (dst == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (len > 0) {
        account_copy(b, src, len);
        memcpy(dst, src, len);
    }
    return nc_buf_put_end(b, len);
}

// ───────────────────────── 讀 ─────────────────────────

void *nc_buf_get(nc_buf_t *b, uint32_t *len, uint32_t *off)
{
    if (b == NULL) {
        return NULL;
    }
    if (b->rd >= 0) {
        b->viol++;      // 上一次未 get_end 就再 get：讀者只有一個
        return NULL;
    }

    if (b->desc[b->r].state != NC_BUF_ST_READY) {
        return NULL;
    }
    const int idx = b->r;

    nc_buf_desc_t *d = &b->desc[idx];
    d->state = NC_BUF_ST_READING;
    b->rd = (int16_t)idx;
    b->gets++;
    if (len != NULL) {
        *len = d->len;
    }
    if (off != NULL) {
        *off = d->off;
    }
    return slot_ptr(b, idx) + d->off;
}

esp_err_t nc_buf_get_end(nc_buf_t *b)
{
    if (b == NULL || b->rd < 0) {
        if (b != NULL) {
            b->viol++;
        }
        return ESP_ERR_INVALID_STATE;
    }
    const int idx = b->rd;
    b->desc[idx].state = NC_BUF_ST_FREE;
    b->desc[idx].len = 0;
    b->desc[idx].off = 0;
    b->r = (uint16_t)((b->r + 1) % b->n);   // 格模式只有 FIFO
    b->rd = -1;
    return ESP_OK;
}

esp_err_t nc_buf_put_abort(nc_buf_t *b)
{
    if (b == NULL || b->wr < 0) {
        if (b != NULL) {
            b->viol++;
        }
        return ESP_ERR_INVALID_STATE;
    }
    b->desc[b->wr].state = NC_BUF_ST_FREE;   // 清回 FREE（FIFO 本來就係 FREE）
    b->wr = -1;
    b->aborts++;
    return ESP_OK;
}

// ───────────────────────── 串流（STREAM）─────────────────────────

uint32_t nc_buf_pending(const nc_buf_t *b)
{
    if (b == NULL || !(b->flags & NC_BUF_F_STREAM)) {
        return 0;
    }
    return b->st_end - b->st_off;
}

uint32_t nc_buf_space(const nc_buf_t *b)
{
    if (b == NULL || !(b->flags & NC_BUF_F_STREAM)) {
        return 0;
    }
    // 壓實之後一定有位：容量 - 未讀量
    return b->slot_bytes - (b->st_end - b->st_off);
}

uint32_t nc_buf_write(nc_buf_t *b, const void *src, uint32_t len)
{
    if (b == NULL || src == NULL || len == 0 || !(b->flags & NC_BUF_F_STREAM)) {
        return 0;
    }
    if (b->st_end + len > b->slot_bytes) {
        // 尾部唔夠位：先把未讀的搬到前面（同 nc4_parser_feed 的做法一致）
        const uint32_t used = b->st_end - b->st_off;
        if (b->st_off > 0) {
            memmove(b->data, b->data + b->st_off, used);
            b->st_off = 0;
            b->st_end = used;
        }
        if (len > b->slot_bytes - b->st_end) {
            len = b->slot_bytes - b->st_end;    // 部分寫入（唔覆蓋未讀資料）
            b->fail_full++;
        }
    }
    if (len == 0) {
        return 0;
    }
    account_copy(b, src, len);
    memcpy(b->data + b->st_end, src, len);
    b->st_end += len;
    b->generation++;
    b->puts++;
    return len;
}

const uint8_t *nc_buf_read_ptr(nc_buf_t *b, uint32_t *len)
{
    if (b == NULL || !(b->flags & NC_BUF_F_STREAM)) {
        return NULL;
    }
    const uint32_t used = b->st_end - b->st_off;
    if (used == 0) {
        return NULL;
    }
    if (len != NULL) {
        *len = used;
    }
    b->gets++;
    return b->data + b->st_off;
}

esp_err_t nc_buf_skip(nc_buf_t *b, uint32_t n)
{
    if (b == NULL || !(b->flags & NC_BUF_F_STREAM)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t used = b->st_end - b->st_off;
    if (n > used) {
        n = used;
    }
    b->st_off += n;
    if (b->st_off == b->st_end) {
        b->st_off = 0;
        b->st_end = 0;
    }
    return ESP_OK;
}

nc_buf_t *nc_buf_carve(void *region, size_t bytes, size_t *cursor,
                       const char *name, nc_buf_where_t where,
                       uint32_t slot_bytes, uint16_t n, uint32_t flags)
{
    if (region == NULL || cursor == NULL || slot_bytes == 0 || n == 0) {
        return NULL;
    }
    const size_t align = NC_BUF_ALIGN;
    size_t off = (*cursor + align - 1) & ~(align - 1);
    const size_t need = (size_t)slot_bytes * n;
    if (off > bytes || need > bytes - off) {
        return NULL;        // 唔夠位：唔會部分配置
    }
    nc_buf_t *b = nc_buf_at(name, (uint8_t *)region + off, need, where,
                            slot_bytes, n, flags | NC_BUF_F_RUNTIME);
    if (b == NULL) {
        return NULL;
    }
    *cursor = off + need;
    return b;
}

// ───────────────────────── 換手 ─────────────────────────

esp_err_t nc_buf_handover(nc_buf_t *b, const char *new_owner)
{
    if (b == NULL || new_owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!all_free(b) || b->wr >= 0 || b->rd >= 0 || nc_buf_pending(b) > 0) {
        b->viol++;
        BUF_LOGE("%s: 換手時仲有格未還（fill=%u pending=%u）", b->name,
                 (unsigned)nc_buf_fill(b), (unsigned)nc_buf_pending(b));
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(b->owner, sizeof(b->owner), "%s", new_owner);
    b->generation++;
    b->w = 0;
    b->r = 0;
    b->wr = -1;
    b->rd = -1;
    BUF_LOGI("buf %s 換手 → %s（gen %u）", b->name, new_owner, (unsigned)b->generation);
    return ESP_OK;
}

// ───────────────────────── 查詢／報表 ─────────────────────────

const char *nc_buf_name(const nc_buf_t *b) { return b ? b->name : "?"; }
const char *nc_buf_owner(const nc_buf_t *b) { return b ? b->owner : "?"; }
uint32_t nc_buf_slot_bytes(const nc_buf_t *b) { return b ? b->slot_bytes : 0; }
uint16_t nc_buf_slots(const nc_buf_t *b) { return b ? b->n : 0; }
uint32_t nc_buf_generation(const nc_buf_t *b) { return b ? b->generation : 0; }
uint8_t nc_buf_state_at(const nc_buf_t *b, uint16_t idx)
{
    if (b == NULL || idx >= b->n) {
        return NC_BUF_ST_FREE;
    }
    return b->desc[idx].state;
}
bool nc_buf_writing(const nc_buf_t *b) { return b ? (b->wr >= 0) : false; }
bool nc_buf_reading(const nc_buf_t *b) { return b ? (b->rd >= 0) : false; }

size_t nc_buf_fill(const nc_buf_t *b)
{
    if (b == NULL) {
        return 0;
    }
    size_t n = 0;
    for (uint16_t i = 0; i < b->n; i++) {
        if (b->desc[i].state == NC_BUF_ST_READY) {
            n++;
        }
    }
    return n;
}

void *nc_buf_raw(nc_buf_t *b, uint16_t idx)
{
    if (b == NULL || idx >= b->n) {
        return NULL;
    }
    return slot_ptr(b, idx);
}

nc_buf_t *nc_buf_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < NC_BUF_MAX; i++) {
        if (s_reg[i] != NULL && strcmp(s_reg[i]->name, name) == 0) {
            return s_reg[i];
        }
    }
    return NULL;
}

size_t nc_buf_count(void)
{
    size_t n = 0;
    for (int i = 0; i < NC_BUF_MAX; i++) {
        if (s_reg[i] != NULL) {
            n++;
        }
    }
    return n;
}

size_t nc_buf_total_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < NC_BUF_MAX; i++) {
        if (s_reg[i] != NULL) {
            n += (size_t)s_reg[i]->slot_bytes * s_reg[i]->n;
        }
    }
    return n;
}

// 安全串接：放唔落就整條作廢（回 -1），唔會寫出半條 JSON。
// 直接 `n += snprintf(...)` 的寫法會在截斷時令 n 大過實際寫入量 →
// memcpy 讀到未初始化的尾巴，甚至寫出唔合法的 JSON。
static int ent_cat(char *dst, size_t cap, int base, const char *fmt, ...)
{
    if (base < 0 || (size_t)base + 1 >= cap) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(dst + base, cap - (size_t)base, fmt, ap);
    va_end(ap);
    if (r < 0) {
        return -1;
    }
    if ((size_t)r >= cap - (size_t)base) {
        return -1;
    }
    return base + r;
}

size_t nc_buf_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    size_t off = (size_t)snprintf(out, cap, "{\"count\":%u,\"total\":%u,\"list\":[",
                                  (unsigned)nc_buf_count(),
                                  (unsigned)nc_buf_total_bytes());
    bool first = true;
    int hidden = 0;
    for (int i = 0; i < NC_BUF_MAX && off < cap; i++) {
        const nc_buf_t *b = s_reg[i];
        if (b == NULL) {
            continue;
        }
        // 逐條寫入暫存，再決定放唔放得落：STATUS 的緩衝是有界的（2,600 B），
        // 一半截斷會令整份 JSON 非法（2026-11-10 host 測試撞到）。
        // 零值欄位省略：steady state 下大部分計數都是 0，唔省略的話 12 條 buffer
        // 就把 keys=mem 撐爆（實測約 200 B／條 → 2.4 KB）。
        char ent[320];
        int n = ent_cat(ent, sizeof(ent), 0,
                        // 只留診斷用得著的：bytes = slot × n（可推導）、puts／gets 是虛榮指標，
                        // 13 條 buffer 就會把 keys=mem 撐爆（實測 162 B／條）。
                        "%s{\"name\":\"%s\",\"where\":\"%s\",\"slot\":%u,\"n\":%u,"
                        "\"fill\":%u,\"gen\":%u",
                        first ? "" : ",", b->name, where_name(b->where),
                        (unsigned)b->slot_bytes, (unsigned)b->n,
                        (b->flags & NC_BUF_F_STREAM) ? (unsigned)nc_buf_pending(b)
                                                     : (unsigned)nc_buf_fill(b),
                        (unsigned)b->generation);
        if (n >= 0 && strcmp(b->owner, b->name) != 0) {
            n = ent_cat(ent, sizeof(ent), n, ",\"owner\":\"%s\"", b->owner);
        }
        if (n >= 0 && b->fail_full) {
            n = ent_cat(ent, sizeof(ent), n, ",\"full\":%u", (unsigned)b->fail_full);
        }
        if (n >= 0 && b->fail_big) {
            n = ent_cat(ent, sizeof(ent), n, ",\"big\":%u", (unsigned)b->fail_big);
        }
        if (n >= 0 && b->viol) {
            n = ent_cat(ent, sizeof(ent), n, ",\"viol\":%u", (unsigned)b->viol);
        }
        if (n >= 0 && b->by_s2s) {
            n = ent_cat(ent, sizeof(ent), n, ",\"s2s\":%u", (unsigned)b->by_s2s);
        }
        if (n >= 0 && b->by_s2p) {
            n = ent_cat(ent, sizeof(ent), n, ",\"s2p\":%u", (unsigned)b->by_s2p);
        }
        if (n >= 0 && b->by_p2s) {
            n = ent_cat(ent, sizeof(ent), n, ",\"p2s\":%u", (unsigned)b->by_p2s);
        }
        if (n >= 0 && b->by_p2p) {
            n = ent_cat(ent, sizeof(ent), n, ",\"p2p\":%u", (unsigned)b->by_p2p);
        }
        if (n >= 0 && b->by_xcopy) {
            n = ent_cat(ent, sizeof(ent), n, ",\"xcopy\":%u", (unsigned)b->by_xcopy);
        }
        if (n >= 0 && b->aborts) {
            n = ent_cat(ent, sizeof(ent), n, ",\"abort\":%u", (unsigned)b->aborts);
        }
        if (n >= 0 && (b->flags & NC_BUF_F_STREAM)) {
            n = ent_cat(ent, sizeof(ent), n, ",\"stream\":1,\"space\":%u", (unsigned)nc_buf_space(b));
        }
        if (n >= 0) {
            n = ent_cat(ent, sizeof(ent), n, "}");
        }
        if (n < 0 || (size_t)n + 48 >= cap - off) {
            hidden++;          // 留 48 B 收尾；放唔落就跳過，唔好寫半條
            continue;
        }
        memcpy(out + off, ent, (size_t)n);
        off += (size_t)n;
        first = false;
    }
    if (hidden > 0) {
        off += (size_t)snprintf(out + off, cap - off, "],\"truncated\":%d}", hidden);
    } else {
        off += (size_t)snprintf(out + off, cap - off, "]}");
    }
    return off;
}

esp_err_t nc_buf_release(nc_buf_t *b)
{
    if (b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(b->flags & NC_BUF_F_RUNTIME)) {
        // 開機建立的 buffer 唔准釋放（R2）。這一條唔可以放寬：一放寬就會有碎片。
        b->viol++;
        BUF_LOGE("%s: 開機建立的 buffer 唔可以釋放（只准 RUNTIME）", b->name);
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < NC_BUF_MAX; i++) {
        if (s_reg[i] == b) {
            s_reg[i] = NULL;
            break;
        }
    }
    buf_free(b);
    return ESP_OK;
}

void nc_buf_reset(void)
{
    for (int i = 0; i < NC_BUF_MAX; i++) {
        if (s_reg[i] != NULL) {
            buf_free(s_reg[i]);
            s_reg[i] = NULL;
        }
    }
}
