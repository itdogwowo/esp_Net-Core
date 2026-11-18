// nc_pixel_q — 生產者與渲染器之間的環（SPSC，無鎖、不等待、不喚醒）。
//
// 為什麼不是 nc_buf：兩邊要的東西不同語義。
//   · 生產者寫 **canonical R,G,B,W**，而且要「滿了就丟」；
//   · 渲染器吃的是**自己後端的物理格式**（I80 時序單元／SPI 位元流／12-bit 抖動），
//     同一格可能被兩條以上 lane 各抄一份。
//   · nc_buf 的 FIFO 是單寫單讀、一種 payload 語義；這裡是「單寫多讀 + 假消費」，
//     所以自己一條很短的環（假消費、gen 換手），不硬套。
//
// 佈局（每格，64 B 對齊）：
//   slot_base(i) = region + i * slot_bytes
//   +0  nc_px_hdr_t（24 B）    +64  payload（R,G,B,W）
//
// 執行緒（契約）：
//   · writer：只有一個。只寫 slot 內容與 hdr->seq；不碰其它欄位。
//   · reader：只有一個。peek() 只讀；release() 只把「舊 seq 的 state」標回 FREE。
//   · 沒有任何一方 reset 對方的游標（對照 buffers.md §12.6）。
//   · state 只用 atomic 做「READY 與否」的單向旗標；payload 的可見性由
//     hdr->seq 的 release/acquire 保證。
#include "nc_pixel_core.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "nc_px_q";

enum {
    SLOT_FREE = 0,
    SLOT_FILLING,
    SLOT_READY,
};

// （nc_pixel_q_t 本身在 nc_pixel_core.h：它是「純 Data」，firmware 與測試共用同一個大小。）

static uint8_t *slot_base(const nc_pixel_q_t *q, uint32_t i)
{
    return q->region + (size_t)i * q->slot_bytes;
}

static nc_px_hdr_t *slot_hdr(const nc_pixel_q_t *q, uint32_t i)
{
    return (nc_px_hdr_t *)slot_base(q, i);
}

// hdr->seq 是發布旗標：reader 用 acquire 讀、writer 用 release 寫。
// 為什麼要有這個小轉型：`&p->field` 會先掉到 non-atomic 的 lvalue，標準不保證那條路徑；
// 明確抓成 `_Atomic uint32_t *` 才符合 C11 §7.17（MSVC 更會直接報 C7707）。
#define hseq(h) ((_Atomic uint32_t *)&(h)->seq)

static uint8_t *slot_payload(const nc_pixel_q_t *q, uint32_t i)
{
    return slot_base(q, i) + nc_px_payload_off();
}

nc_pixel_q_t *nc_px_ring_init(nc_pixel_q_t *q, const nc_px_ring_cfg_t *cfg)
{
    if (q == NULL || cfg == NULL || cfg->region == NULL || cfg->name == NULL) {
        return NULL;
    }
    if (cfg->px == 0 || cfg->px > NC_PX_MAX_LEDS) {
        ESP_LOGE(TAG, "[%s] px=%u 超出 1..%u", cfg->name, (unsigned)cfg->px,
                 (unsigned)NC_PX_MAX_LEDS);
        return NULL;
    }
    if (cfg->slots < 2 || cfg->slots > NC_PX_MAX_SLOTS) {
        // 1 格 = 生產者每幀都要等渲染器 = 生產者被渲染器拖住（P6 的設計理由）。
        ESP_LOGE(TAG, "[%s] slots=%u 必須 2..%u", cfg->name, (unsigned)cfg->slots,
                 (unsigned)NC_PX_MAX_SLOTS);
        return NULL;
    }
    const uint32_t need = nc_px_ring_bytes(cfg->px, cfg->slots);
    if (cfg->region_bytes < need) {
        ESP_LOGE(TAG, "[%s] 區域 %u B 不夠（要 %u B）", cfg->name, (unsigned)cfg->region_bytes,
                 (unsigned)need);
        return NULL;
    }
    memset(q, 0, sizeof(*q));
    (void)snprintf(q->name, sizeof(q->name), "%s", cfg->name);
    q->region = cfg->region;
    q->region_bytes = cfg->region_bytes;
    q->px = cfg->px;
    q->slots = cfg->slots;
    q->slot_bytes = nc_px_slot_bytes(cfg->px);
    for (uint32_t i = 0; i < cfg->slots; i++) {
        atomic_store(&q->state[i], SLOT_FREE);
        memset(slot_hdr(q, i), 0, NC_PX_HDR_BYTES);
    }
    atomic_store(&q->wr, 0);
    atomic_store(&q->seq, 0);
    atomic_store(&q->busy, 0);
    return q;
}

void nc_px_ring_deinit(nc_pixel_q_t *q)
{
    if (q != NULL) {
        q->region = NULL;
    }
}

uint8_t *nc_px_ring_region(const nc_pixel_q_t *q) { return (q != NULL) ? q->region : NULL; }
uint32_t nc_px_ring_px(const nc_pixel_q_t *q) { return (q != NULL) ? q->px : 0; }
uint32_t nc_px_ring_slots(const nc_pixel_q_t *q) { return (q != NULL) ? q->slots : 0; }
uint32_t nc_px_ring_slot_bytes(const nc_pixel_q_t *q) { return (q != NULL) ? q->slot_bytes : 0; }
const char *nc_px_ring_name(const nc_pixel_q_t *q) { return (q != NULL) ? q->name : NULL; }

uint32_t nc_px_ring_fill(const nc_pixel_q_t *q)
{
    uint32_t n = 0;
    if (q != NULL) {
        for (uint32_t i = 0; i < q->slots; i++) {
            const uint8_t st = atomic_load(&q->state[i]);
            if (st == SLOT_READY || st == SLOT_FILLING) {
                n++;                       // WRITING 中的格同樣是「唔可用」
            }
        }
    }
    return n;
}

// ── 生產者 ──

uint8_t *nc_px_ring_begin(nc_pixel_q_t *q)
{
    if (q == NULL) {
        return NULL;
    }
    if (atomic_exchange(&q->busy, 1) != 0) {
        q->overruns++;
        return NULL;
    }
    const uint32_t i = atomic_load(&q->wr) % q->slots;
    if (atomic_load(&q->state[i]) != SLOT_FREE) {
        atomic_store(&q->busy, 0);
        return NULL;                       // 滿了：不覆蓋
    }
    // 表頭先清一次：seq 是最後的發布旗標（0 = 未提交），其餘欄位由 put_ts 填。
    memset(slot_hdr(q, i), 0, NC_PX_HDR_BYTES);
    atomic_store(&q->state[i], SLOT_FILLING);
    return slot_payload(q, i);
}

void nc_px_ring_commit(nc_pixel_q_t *q)
{
    if (q == NULL || atomic_load(&q->busy) == 0) {
        return;
    }
    const uint32_t i = atomic_load(&q->wr) % q->slots;
    nc_px_hdr_t *h = slot_hdr(q, i);
    const uint32_t next = atomic_load(&q->seq) + 1u;
    q->puts++;
    q->drops = h->drops;                // 生產者自報的丟幀數
    atomic_store_explicit(hseq(h), next, memory_order_release);
    atomic_store(&q->seq, next);
    atomic_store(&q->state[i], SLOT_READY);
    atomic_store(&q->wr, (i + 1u) % q->slots);
    atomic_store(&q->busy, 0);
}

void nc_px_ring_abort(nc_pixel_q_t *q)
{
    if (q == NULL || atomic_load(&q->busy) == 0) {
        return;
    }
    const uint32_t i = atomic_load(&q->wr) % q->slots;
    memset(slot_hdr(q, i), 0, NC_PX_HDR_BYTES);   // 清掉 seq（發布旗標）`n    atomic_store(&q->state[i], SLOT_FREE);  // 游標不動：下一幀仍然寫同一格
    atomic_store(&q->busy, 0);
}

bool nc_px_ring_put_ts(nc_pixel_q_t *q, const uint8_t *rgbw, uint8_t src, uint16_t gen,
                       uint32_t ts_us)
{
    if (q == NULL || rgbw == NULL) {
        return false;
    }
    uint8_t *dst = nc_px_ring_begin(q);
    if (dst == NULL) {
        q->drops++;
        return false;
    }
    memcpy(dst, rgbw, (size_t)q->px * NC_PX_BPP);
    nc_px_hdr_t *h = slot_hdr(q, atomic_load(&q->wr) % q->slots);
    h->ts_us = ts_us;
    h->gen = gen;
    h->src = src;
    h->drops = q->drops;
    nc_px_ring_commit(q);
    return true;
}

bool nc_px_ring_put(nc_pixel_q_t *q, const uint8_t *rgbw, uint8_t src, uint16_t gen)
{
    if (q == NULL) {
        return false;
    }
    // 不帶時戳：用 seq 造一個單調遞增值（100 ms 一格），保證「比上一格新」。
    return nc_px_ring_put_ts(q, rgbw, src, gen, (atomic_load(&q->seq) + 1u) * 100000u);
}

// ── 渲染器 ──

const uint8_t *nc_px_ring_peek(nc_pixel_q_t *q, const nc_px_hdr_t **hdr, uint32_t *out_seq)
{
    if (q == NULL) {
        return NULL;
    }
    const uint32_t newest = atomic_load(&q->seq);
    if (newest == 0) {
        q->stale++;
        return NULL;
    }
    // 被假消費（沒有交出去就變舊）的格只有 state 標記，直接還給 writer。
    // 只還「不是最新那格」：最新那格要留給 take 的人。
    for (uint32_t k = 1; k < q->slots; k++) {
        const uint32_t j = (newest - 1u - k) % q->slots;
        if (atomic_load(&q->state[j]) == SLOT_READY) {
            atomic_store(&q->state[j], SLOT_FREE);
        }
    }
    const uint32_t i = (newest - 1u) % q->slots;
    if (atomic_load(&q->state[i]) != SLOT_READY) {
        q->stale++;
        return NULL;
    }
    const nc_px_hdr_t *h = slot_hdr(q, i);
    if (atomic_load_explicit((const _Atomic uint32_t *)hseq((nc_px_hdr_t *)h), memory_order_acquire) != newest) {
        q->stale++;
        return NULL;
    }
    if (hdr != NULL) {
        *hdr = h;
    }
    if (out_seq != NULL) {
        *out_seq = newest;
    }
    return slot_payload(q, i);
}

void nc_px_ring_release(nc_pixel_q_t *q, uint32_t seq)
{
    if (q == NULL || seq == 0) {
        return;
    }
    const uint32_t i = (seq - 1u) % q->slots;
    if (atomic_load(&q->state[i]) == SLOT_READY) {
        const uint32_t s = atomic_load((const _Atomic uint32_t *)hseq(slot_hdr(q, i)));
        // 只有「還是這一格」時才還：writer 可能已經往下寫新的一格了。
        if (s == seq) {
            atomic_store(&q->state[i], SLOT_FREE);
        }
    }
}

void nc_px_ring_stats(const nc_pixel_q_t *q, nc_px_q_stat_t *out)
{
    if (q == NULL || out == NULL) {
        return;
    }
    out->puts = q->puts;
    out->drops = q->drops;
    out->overruns = q->overruns;
    out->stale = q->stale;
}
