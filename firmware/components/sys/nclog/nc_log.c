#include "nc_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// esp_log_set_vprintf 在 esp_log_write.h（esp_log.h 會 include 它）
#include "esp_log_write.h"
#include "nc_state.h"

static const char *TAG = "nclog";

#define NC_LOG_QUEUES 2

// 每條記錄的 header（16 B）：可變長文字緊接在後面。
// 為什麼唔用「固定格」：實測開機 106 行、6,092 B（平均 57 B/行），
// 固定 176 B/格要 18.7 KB，浪費三倍；byte ring 8 KB 就裝得下。
typedef struct __attribute__((packed)) {
    uint16_t len;       // 文字長度
    uint8_t  flags;     // bit0 截斷、bit1 = PAD（環尾補位）
    uint8_t  core;
} nc_log_hdr_t;         // 4 B：時間戳同 seq 都唔存（順序差一行無所謂）

#define NC_LOG_F_TRUNC 0x01u
#define NC_LOG_F_PAD   0x02u

typedef struct {
    portMUX_TYPE      mux;        // **只保護本核**的多個 task（另一核永遠唔會拿）
    uint8_t          *buf;
    uint16_t          cap;        // < 65536，所以 offset 用 uint16
    _Atomic uint16_t  head;       // 生產者：下一個寫入 offset
    _Atomic uint16_t  tail;       // 消費者：下一個讀取 offset
    _Atomic uint32_t  seq;
    uint32_t          dropped;
    uint32_t          peak;       // 歷史最高佔用 bytes（睇餘裕用）
} nc_log_ring_t;

// 只有兩個級別（0=簡、1=詳），存在 NVS boot/log（1 byte）
static uint8_t    s_level = NC_LOG_LEVELS_QUIET;
static nc_state_t *s_st;

// 模式決定 ring 大小（heap 分配：runtime 只需要 2 KB，唔好長期佔 8 KB）
static size_t ring_bytes(int i)
{
    if (s_level == NC_LOG_LEVELS_QUIET) {
        return (i == 0) ? 2048u : 512u;
    }
    return (i == 0) ? NC_LOG_BYTES_0 : NC_LOG_BYTES_1;   // 8192 / 1024
}

static nc_log_ring_t s_ring[NC_LOG_QUEUES];
static vprintf_like_t s_prev;
static nc_log_tx_fn   s_tx;
static TaskHandle_t   s_task;
static volatile bool  s_captured;     // hook 已裝（生產者活躍）
static volatile bool  s_running;      // consumer 已建立、出口已接
static uint32_t       s_sent;         // 成功寫入出口的行數（≠ 主機一定收到）
static uint32_t       s_busy;         // 出口 BUSY 而放棄的次數
static volatile uint8_t s_mask = NC_LOG_LVL_DEFAULT;   // 等級 allow-list
static volatile bool    s_migrating;                   // 換環期間暫停生產者
static uint32_t       s_offline;      // 進入「主機未開 port」狀態的次數
static bool           s_was_offline;
static int            s_core = -1;
static uint8_t        s_rr;            // 合併時兩環輪流的起點

static void ring_migrate(int i, size_t new_bytes);   // 定義在下面（apply_level 會用）

// ───────────────────────── byte ring ─────────────────────────

static inline uint16_t ring_used(const nc_log_ring_t *q, uint16_t head, uint16_t tail)
{
    if (q->cap == 0) {
        return 0;                   // 環未配置（capture 之前問 STATUS）→ 唔可以除零
    }
    return (uint16_t)((head + q->cap - tail) % q->cap);
}

// 生產者：寫一條記錄（呼叫端已拿本核 critical section）
static bool ring_put(nc_log_ring_t *q, const nc_log_hdr_t *h, const char *text)
{
    const uint16_t need = (uint16_t)(sizeof(*h) + h->len);
    uint16_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    const uint16_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    // 記錄必須連續；尾端放唔落就補一個 PAD header 再繞回 0。
    // 注意尾端只剩 1–3 B 時連 header 都放唔落 → 唔寫 PAD，消費者靠「已無 4 B 可讀」認出繞回
    // （ring_at 有對應處理；以前兩邊都冇，會讀過緩衝尾再將垃圾 header 當真）。
    const uint16_t wrap = (uint16_t)((head + need > q->cap) ? (q->cap - head) : 0);
    if ((uint32_t)ring_used(q, head, tail) + need + wrap + 1u > q->cap) {
        return false;                       // 放唔落：丟新行（保已排隊的順序）
    }

    if (wrap >= sizeof(*h)) {
        nc_log_hdr_t pad = { .flags = NC_LOG_F_PAD, .len = 0 };
        memcpy(q->buf + head, &pad, sizeof(pad));
    }
    if (wrap) {
        head = 0;
    }
    memcpy(q->buf + head, h, sizeof(*h));
    memcpy(q->buf + head + sizeof(*h), text, h->len);
    head = (uint16_t)((head + need) % q->cap);
    atomic_store_explicit(&q->head, head, memory_order_release);
    return true;
}

// 消費者：從 offset 讀一條（唔改 tail）。next 回下一條的 offset。
static bool ring_at(nc_log_ring_t *q, uint16_t off, nc_log_hdr_t *h, uint16_t *next)
{
    const uint16_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    for (;;) {
        if (off == head) {
            return false;
        }
        // 環尾剩 < 一個 header（1–3 B）時，ring_put 冇位寫 PAD 標記，會直接由 0 開始寫。
        // 呢種縫隙只能靠消費者自己認：off 之後已經冇 4 B 可讀 → 一定係繞回 0。
        // （以前直接 memcpy，會讀過緩衝尾 1–3 B，用垃圾 header 當真，tail 就會跳去錯位置。）
        if ((uint32_t)off + sizeof(*h) > q->cap) {
            off = 0;
            continue;
        }
        memcpy(h, q->buf + off, sizeof(*h));
        if ((h->flags & NC_LOG_F_PAD) != 0) {
            off = 0;                        // 跳過環尾補位
            continue;
        }
        *next = (uint16_t)((off + sizeof(*h) + h->len) % q->cap);
        return true;
    }
}

// 只抄文字（唔改 tail）
static size_t ring_text_at(const nc_log_ring_t *q, uint16_t off, const nc_log_hdr_t *h,
                           char *out, size_t cap)
{
    const size_t n = (h->len < cap - 1) ? h->len : cap - 1;
    memcpy(out, q->buf + off + sizeof(*h), n);
    out[n] = '\0';
    return n;
}

// ───────────────────────── 生產者（任何核、任何 task） ─────────────────────────

static inline uint8_t level_bit(char c)
{
    switch (c) {
    case 'E': return NC_LOG_LVL_ERROR;
    case 'W': return NC_LOG_LVL_WARN;
    case 'I': return NC_LOG_LVL_INFO;
    case 'D': return NC_LOG_LVL_DEBUG;
    case 'V': return NC_LOG_LVL_VERBOSE;
    default:  return NC_LOG_LVL_INFO;
    }
}

static int log_hook(const char *fmt, va_list ap)
{
    if (!s_captured || fmt == NULL) {
        return (s_prev != NULL) ? s_prev(fmt, ap) : 0;
    }
    // 等級 allow-list：IDF v1 的 format 第一個字元就是等級字母（E/W/I/D/V）
    if ((s_mask & level_bit(fmt[0])) == 0 || s_migrating) {
        return 0;
    }
    // ISR 裡面唔可以用 xTaskNotifyGive（非 FromISR 版本）→ 直接走原本的 console
    if (xPortInIsrContext()) {
        return (s_prev != NULL) ? s_prev(fmt, ap) : 0;
    }

    const int core = (int)xPortGetCoreID();
    char text[NC_LOG_LINE_MAX + 1];
    const int n = vsnprintf(text, sizeof(text), fmt, ap);   // 一定要在 critical 之外
    if (n < 0) {
        return 0;
    }
    nc_log_hdr_t h = {
        .core  = (uint8_t)core,
        .len   = (uint16_t)(((size_t)n >= sizeof(text)) ? sizeof(text) - 1 : (size_t)n),
        .flags = (((size_t)n >= sizeof(text)) ? NC_LOG_F_TRUNC : 0),
    };
    while (h.len > 0 && (text[h.len - 1] == '\n' || text[h.len - 1] == '\r')) {
        text[--h.len] = '\0';               // IDF 的 format 已帶 "\n"，輸出時再補
    }

    nc_log_ring_t *q = &s_ring[core & 1];
    portENTER_CRITICAL(&q->mux);            // 只關本核中斷（另一核唔會拿這個 mux）
    const uint16_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    const uint16_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    const bool was_empty = (head == tail);
    if (ring_put(q, &h, text)) {
        atomic_fetch_add_explicit(&q->seq, 1, memory_order_relaxed);
        const uint32_t u = ring_used(q, atomic_load_explicit(&q->head, memory_order_relaxed),
                                     atomic_load_explicit(&q->tail, memory_order_relaxed));
        if (u > q->peak) {
            q->peak = u;
        }
    } else {
        q->dropped++;
    }
    portEXIT_CRITICAL(&q->mux);

    if (was_empty) {
        const TaskHandle_t t = s_task;
        if (t != NULL) {
            xTaskNotifyGive(t);             // 跨核單向喚醒（每批一次）
        }
    }
    return (int)h.len;
}

// ───────────────────────── 消費者（釘 CDC 核） ─────────────────────────

static nc_log_tx_result_t emit(const char *buf, size_t len)
{
    if (s_tx == NULL) {
        return NC_LOG_TX_OFFLINE;
    }
    // BUSY = 回覆佔用出口／FIFO 滿：讓一讓再試（幾 ms），唔好卡死 consumer
    for (int i = 0; i < 5; i++) {
        const nc_log_tx_result_t r = s_tx(buf, len);
        if (r != NC_LOG_TX_BUSY) {
            return r;
        }
        vTaskDelay(1);
    }
    s_busy++;
    return NC_LOG_TX_BUSY;
}

static void log_task(void *arg)
{
    (void)arg;
    char staging[NC_LOG_BATCH_BYTES + NC_LOG_LINE_MAX + 16];
    for (;;) {
        // 20 ms 是安全網：就算 notify 掉了（例如剛好跨核），最多遲 20 ms
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));

        for (;;) {
            // 合批：由兩個環各自走訪最舊的記錄，**唔 advance**，
            // 確認整批寫入成功才一次過前進 tail（失敗唔會食掉任何行）。
            uint16_t off[NC_LOG_QUEUES];
            for (int c = 0; c < NC_LOG_QUEUES; c++) {
                off[c] = atomic_load_explicit(&s_ring[c].tail, memory_order_relaxed);
            }
            size_t slen = 0;
            int count = 0;
            // 每「批」只服務一個核（輪流）→ 輸出會是 8 行一段，唔會梅花間竹
            int pick = -1;
            for (int k = 0; k < NC_LOG_QUEUES; k++) {
                const int c = (s_rr + k) & 1;
                nc_log_hdr_t probe;
                uint16_t pn;
                if (ring_at(&s_ring[c], off[c], &probe, &pn)) {
                    pick = c;
                    break;
                }
            }
            if (pick < 0) {
                break;
            }
            while (count < NC_LOG_BATCH_LINES) {
                nc_log_hdr_t best;
                uint16_t best_next = 0;
                if (!ring_at(&s_ring[pick], off[pick], &best, &best_next)) {
                    break;
                }
                char text[NC_LOG_LINE_MAX + 1];
                (void)ring_text_at(&s_ring[pick], off[pick], &best, text, sizeof(text));
                const int n = snprintf(staging + slen, sizeof(staging) - slen,
                                       "%c%c %s\n", NC_LOG_PREFIX,
                                       (char)('0' + (best.core & 1)), text);
                if (n <= 0 || slen + (size_t)n > NC_LOG_BATCH_BYTES) {
                    break;                          // 這批滿了：留返下一批
                }
                slen += (size_t)n;
                off[pick] = best_next;
                s_rr = (uint8_t)((pick + 1) & 1);
                count++;
            }
            if (count == 0) {
                break;
            }

            const nc_log_tx_result_t res = emit(staging, slen);
            if (res == NC_LOG_TX_OFFLINE) {
                if (!s_was_offline) {
                    s_was_offline = true;
                    s_offline++;
                }
                // 主機剛剛開 port 時，第一批常常會被吃掉（tty 未 ready）。
                // 退避 100 ms 再試，讓 host 那邊 settle；實測可以救回開機最頭幾行。
                vTaskDelay(pdMS_TO_TICKS(100));
                break;                              // 唔 advance：整批留住
            }
            if (res != NC_LOG_TX_OK) {
                break;                              // BUSY 放棄：一樣唔 advance
            }
            s_was_offline = false;
            for (int c = 0; c < NC_LOG_QUEUES; c++) {   // 成功才放返空間
                atomic_store_explicit(&s_ring[c].tail, off[c], memory_order_release);
            }
            s_sent += (uint32_t)count;
        }
    }
}

// ───────────────────────── 對外 ─────────────────────────

static void rings_reset(void)
{
    for (int i = 0; i < NC_LOG_QUEUES; i++) {
        if (s_ring[i].buf != NULL) {
            free(s_ring[i].buf);                    // 重設時放返舊環
            s_ring[i].buf = NULL;
        }
        const size_t bytes = ring_bytes(i);
        s_ring[i].buf = malloc(bytes);
        s_ring[i].cap = (uint16_t)((s_ring[i].buf != NULL) ? bytes : 0);
        s_ring[i].mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
        atomic_store(&s_ring[i].head, 0);
        atomic_store(&s_ring[i].tail, 0);
        atomic_store(&s_ring[i].seq, 0);
        s_ring[i].dropped = 0;
    }
    s_sent = 0;
    s_busy = 0;
    s_offline = 0;
    s_was_offline = false;
}

// 套用級別：mask ＋ 環大小（必要時遷移，只會喺開機/改設定時發生一次）
static void apply_level(uint8_t levels)
{
    s_level = levels;
    if (levels == NC_LOG_LEVELS_FULL) {
        s_mask = NC_LOG_LVL_ALL;        // 除錯：連 DEBUG/VERBOSE 都收
    } else {
        s_mask = (uint8_t)(NC_LOG_LVL_ERROR | NC_LOG_LVL_WARN | NC_LOG_LVL_INFO);
    }
    ring_migrate(0, ring_bytes(0));
    ring_migrate(1, ring_bytes(1));
}

void nc_log_get_levels(uint8_t *levels)
{
    if (levels != NULL) {
        *levels = s_level;
    }
}

esp_err_t nc_log_set_levels(uint8_t levels)
{
    if (levels > NC_LOG_LEVELS_FULL) {
        levels = NC_LOG_LEVELS_QUIET;
    }
    if (levels != s_level) {
        apply_level(levels);
    }
    if (s_st != NULL) {
        nc_state_touch(s_st);
        (void)nc_state_commit();            // 下次開機最早期就用新級別
    }
    return ESP_OK;
}

static void hook_install(void)
{
    if (!s_captured) {
        s_prev = esp_log_set_vprintf(log_hook);
        s_captured = true;
    }
}

esp_err_t nc_log_capture(void)
{
    if (s_captured || s_st != NULL) {
        return ESP_OK;
    }

    // 設定放 NVS：開機最早期要讀，FAT 還未掛
    s_st = nc_state_add(&(nc_state_desc_t){
        .name    = "log",
        .persist = NC_ST_PERSIST_NVS,
        .writer  = NC_ST_BY_SYSTEM,
        .value   = &s_level,
        .size    = sizeof(s_level),
        .nvs_ns  = "boot",
        .nvs_key = "log",
    });
    (void)nc_state_load();                  // 讀返上次的級別（冇就係 0）
    if (s_level > NC_LOG_LEVELS_FULL) {
        s_level = NC_LOG_LEVELS_QUIET;
    }

    // IDF 全域放到 VERBOSE：真正的過濾由 mask 做
    esp_log_level_set("*", ESP_LOG_VERBOSE);
    s_mask = (s_level == NC_LOG_LEVELS_FULL)
                 ? NC_LOG_LVL_ALL
                 : (uint8_t)(NC_LOG_LVL_ERROR | NC_LOG_LVL_WARN | NC_LOG_LVL_INFO);
    rings_reset();

    // 只有「詳」才喺 app_main 第一行就開始擷取；「簡」等開機完成才裝 hook
    if (s_level == NC_LOG_LEVELS_FULL) {
        hook_install();
        ESP_LOGI(TAG, "log 擷取已開始（lvl=1、%u/%u B 環）",
                 (unsigned)ring_bytes(0), (unsigned)ring_bytes(1));
    }
    return ESP_OK;
}

esp_err_t nc_log_start(int core, nc_log_tx_fn tx)
{
    if (tx == NULL || core < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        s_tx = tx;
        return ESP_OK;
    }
    if (!s_captured) {
        (void)nc_log_capture();     // 呼叫端冇提前擷取：補做（會少掉之前的行）
    }

    hook_install();                         // 「簡」模式：USB 起來之後才開始記
    s_tx = tx;
    s_core = core;

    if (xTaskCreatePinnedToCore(log_task, "nc_log", 3072, NULL, 2, &s_task,
                                (BaseType_t)core) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    ESP_LOGI(TAG, "log 管道啟動：core %d、%u/%u B 環（已排隊 %u 行）",
             core, (unsigned)ring_bytes(0), (unsigned)ring_bytes(1),
             (unsigned)(atomic_load(&s_ring[0].seq) + atomic_load(&s_ring[1].seq)));
    return ESP_OK;
}

// 把一個環搬到新大小：走訪舊記錄、重新寫入新環（放唔落就丟，保留最舊）
static void ring_migrate(int i, size_t new_bytes)
{
    nc_log_ring_t *q = &s_ring[i];
    if (q->buf == NULL || new_bytes == 0 || new_bytes == q->cap) {
        return;
    }
    uint8_t *nb = malloc(new_bytes);
    if (nb == NULL) {
        return;
    }

    nc_log_ring_t t;
    memset(&t, 0, sizeof(t));
    t.buf = nb;
    t.cap = (uint16_t)new_bytes;
    t.mux = q->mux;
    atomic_store(&t.head, 0);
    atomic_store(&t.tail, 0);

    uint16_t off = atomic_load_explicit(&q->tail, memory_order_relaxed);
    const uint16_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    while (off != head) {
        nc_log_hdr_t h;
        if ((uint32_t)off + sizeof(h) > q->cap) {
            off = 0;                    // 尾端 < 4 B 的縫隙（見 ring_at 的說明）
            continue;
        }
        memcpy(&h, q->buf + off, sizeof(h));
        if ((h.flags & NC_LOG_F_PAD) != 0) {
            off = 0;
            continue;
        }
        (void)ring_put(&t, &h, (const char *)(q->buf + off + sizeof(h)));
        off = (uint16_t)((off + sizeof(h) + h.len) % q->cap);
    }

    s_migrating = true;                 // 搬遷期間生產者暫停（< 1 ms）
    free(q->buf);
    q->buf = nb;
    q->cap = (uint16_t)new_bytes;
    atomic_store(&q->head, atomic_load(&t.head));
    atomic_store(&q->tail, 0);
    s_migrating = false;
}

void nc_log_stop(void)
{
    if (!s_captured) {
        return;
    }
    s_captured = false;
    s_running = false;
    (void)esp_log_set_vprintf(s_prev);
    s_prev = NULL;
}

void nc_log_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    const uint16_t h0 = atomic_load(&s_ring[0].head), t0 = atomic_load(&s_ring[0].tail);
    const uint16_t h1 = atomic_load(&s_ring[1].head), t1 = atomic_load(&s_ring[1].tail);
    snprintf(buf, cap,
             "{\"cap\":%d,\"run\":%d,\"core\":%d,\"lvl\":%u,\"mask\":%u,\"buf\":[%u,%u],"
             "\"used\":[%u,%u],\"peak\":[%u,%u],\"produced\":[%u,%u],"
             "\"sent\":%u,\"dropped\":%u,\"busy\":%u,\"offline\":%u}",
             s_captured ? 1 : 0, s_running ? 1 : 0, s_core,
             (unsigned)s_level, (unsigned)s_mask, (unsigned)s_ring[0].cap, (unsigned)s_ring[1].cap,
             (unsigned)ring_used(&s_ring[0], h0, t0),
             (unsigned)ring_used(&s_ring[1], h1, t1),
             (unsigned)s_ring[0].peak, (unsigned)s_ring[1].peak,
             (unsigned)atomic_load(&s_ring[0].seq), (unsigned)atomic_load(&s_ring[1].seq),
             (unsigned)s_sent, (unsigned)(s_ring[0].dropped + s_ring[1].dropped),
             (unsigned)s_busy, (unsigned)s_offline);
}
