// nc_pixel_mock.c — "mock" 後端：什麼硬體都不碰，只計數。
//
// 用途（比「drive=0 沒燈」有用得多）：
//   · 沒有燈板的板子（C3／P4-Mini…）也能把整條路跑起來：
//     生產者 → 環 → 渲染器 → acquire/encode/submit，全部走真的程式碼，
//     只差最後不碰暫存器。所以記憶體帳、節拍、假消費、hold、統計都可以量。
//   · host 測試（tools/hosttest/pixel_test）用它驗「渲染器不依賴驅動」。
//
// 它仍然**真的編碼**（可選）：mock 會跑同一份 codec，把結果寫進自己的私有緩衝，
// 這樣 host 測試可以比對「mock 與 i80 背後的編碼結果完全相同」。
#include <stdio.h>
#include <string.h>

#include "nc_pixel.h"


#define MOCK_SLOTS 3

typedef struct {
    nc_pixel_backend_t be;
    nc_pixel_q_t      *q;
    nc_pixel_codec_t  *codec;
    uint32_t           px;
    uint32_t           seq[MOCK_SLOTS];
    uint32_t           used[MOCK_SLOTS];     // 0 = 空；seq = 已交
    uint32_t           encoded;              // 已編碼的幀
    uint32_t           submitted;            // 已「送出」的幀
    uint32_t           last_seq;
} mock_t;

static mock_t s_mock;

static bool mock_acquire(nc_pixel_backend_t *b, uint32_t post_seq, uint32_t *slot,
                         const uint8_t **payload, uint32_t *px, uint32_t *seq)
{
    mock_t *m = (mock_t *)b->ctx;
    const nc_px_hdr_t *h = NULL;
    uint32_t s = 0;
    const uint8_t *p = nc_px_ring_peek(m->q, &h, &s);
    if (p == NULL || s == post_seq) {
        // 沒有新格，或只是同一格（slave 的 render.py 在停止/暫停時也是「重送中性值」）
        if (p != NULL) {
            nc_px_ring_release(m->q, s);
        }
        return false;
    }
    // 找一格空的
    for (int i = 0; i < MOCK_SLOTS; i++) {
        if (m->seq[i] == 0) {
            *slot = (uint32_t)i;
            *payload = p;
            *px = m->px;
            *seq = s;
            return true;
        }
    }
    // 全部在飛：真的 mock 不會卡住，直接還掉環裡那一格並當成沒拿到（下次再試）
    nc_px_ring_release(m->q, s);
    return false;
}

static bool mock_encode(nc_pixel_backend_t *b, uint32_t slot)
{
    mock_t *m = (mock_t *)b->ctx;
    if (slot >= MOCK_SLOTS) {
        return false;
    }
    // 用真的 codec 走一次：mock 的價值就在「除了暫存器以外全都做了」。
    if (m->codec != NULL) {
        nc_px_codec_frame(m->codec, m->encoded + 1u);
    }
    m->encoded++;
    return true;
}

static bool mock_submit(nc_pixel_backend_t *b, uint32_t slot, uint32_t seq)
{
    mock_t *m = (mock_t *)b->ctx;
    if (slot >= MOCK_SLOTS) {
        return false;
    }
    m->seq[slot] = seq;
    m->last_seq = seq;
    m->submitted++;
    return true;
}

static void mock_reclaim(nc_pixel_backend_t *b)
{
    mock_t *m = (mock_t *)b->ctx;
    // mock 的「DMA」是瞬間完成的：下一次 reclaim 就全部還掉
    for (int i = 0; i < MOCK_SLOTS; i++) {
        if (m->seq[i] != 0) {
            nc_px_ring_release(m->q, m->seq[i]);
            m->seq[i] = 0;
        }
    }
}

static size_t mock_json(nc_pixel_backend_t *b, char *out, size_t cap)
{
    mock_t *m = (mock_t *)b->ctx;
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(out, cap,
                           "{\"be\":\"mock\",\"encoded\":%u,\"submitted\":%u,\"seq\":%u}",
                           (unsigned)m->encoded, (unsigned)m->submitted,
                           (unsigned)m->last_seq);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

nc_pixel_backend_t *nc_px_backend_mock(void)
{
    mock_t *m = &s_mock;
    memset(m, 0, sizeof(*m));
    (void)snprintf(m->be.name, sizeof(m->be.name), "mock");
    m->be.lanes = 1;
    m->be.ctx = m;
    m->be.acquire = mock_acquire;
    m->be.encode = mock_encode;
    m->be.submit = mock_submit;
    m->be.reclaim = mock_reclaim;
    m->be.json = mock_json;
    return &m->be;
}

// 綁環（環從哪來由呼叫端決定：firmware 是 plan 交付的區域，測試是自己切的）。
// 為什麼有這一步：後端只認「環」這個介面，不認「誰建的環」。
void nc_px_backend_mock_bind(nc_pixel_backend_t *b, nc_pixel_q_t *q)
{
    mock_t *m = (mock_t *)b->ctx;
    m->q = q;
    m->px = nc_px_ring_px(q);
}