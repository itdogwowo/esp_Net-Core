// nc_pixel_registry.c — 環的註冊表（跨任務）。
//
// 為什麼要獨立：renderer 任務宣告／建立環（plan 資源＋DMA 槽），效果生產者任務
// 只拿一個名字（例如 "i80.prd"）找回同一條環。註冊表放 sw/pixel 是因為兩邊都要用，
// 而且它只是指標陣列（不配置記憶體）。
#include "nc_pixel_core.h"

#include <string.h>

#define PX_RING_MAX 8

static nc_pixel_q_t *s_ring[PX_RING_MAX];
static int s_nring;

int nc_px_ring_register(nc_pixel_q_t *q)
{
    if (q == NULL || nc_px_ring_name(q) == NULL) {
        return -1;
    }
    if (nc_px_ring_find(nc_px_ring_name(q)) != NULL) {
        return -1;                       // 重名
    }
    if (s_nring >= PX_RING_MAX) {
        return -1;
    }
    s_ring[s_nring++] = q;
    return 0;
}

nc_pixel_q_t *nc_px_ring_at(int i)
{
    return (i >= 0 && i < s_nring) ? s_ring[i] : NULL;
}

int nc_px_ring_count(void) { return s_nring; }

nc_pixel_q_t *nc_px_ring_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_nring; i++) {
        const char *n = nc_px_ring_name(s_ring[i]);
        if (n != NULL && strcmp(n, name) == 0) {
            return s_ring[i];
        }
    }
    return NULL;
}

nc_pixel_q_t *nc_px_active_ring(void) { return (s_nring > 0) ? s_ring[0] : NULL; }

bool nc_px_put(const uint8_t *rgbw, uint8_t src, uint16_t gen)
{
    return nc_px_ring_put(nc_px_active_ring(), rgbw, src, gen);
}
