// nc_px_nce.c — `.nce` 值流信箱（雙緩衝、單 writer／單 reader）。
//
// 見 nc_px_nce.h 的契約。這裡只管「搬 0–255 的 bytes」，唔識通道、唔識 write 模式、
// 唔識燈 —— 那些全部係配對層的事。
#include "nc_px_nce.h"
#include "nc_px_layout.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "px.nce";

// 開機自動告訴 layout「.nce 的值數由這個函式查」——雙方唔使互相 include 實作。
static void __attribute__((constructor)) nce_register(void)
{
    nc_px_layout_set_nce_values(nc_px_nce_bytes);
}

static uint8_t *s_buf[2];
static uint32_t s_bytes;
static volatile int s_wr;          // 下一個要寫的 index
static volatile uint32_t s_seq;    // publish 次數（同時係版本號）

// ncm task 每 tick 交一份 → 寫另一塊再 flip，reader 永遠讀到完整的一份。
static inline void mem_barrier(void)
{
#if defined(__XTENSA__) || defined(__riscv)
    __asm__ __volatile__("" ::: "memory");
#endif
}

bool nc_px_nce_init(uint32_t bytes)
{
    if (bytes == 0) {
        return false;
    }
    if (s_bytes == bytes && s_buf[0] != NULL && s_buf[1] != NULL) {
        return true;                       // 冪等
    }
    nc_px_nce_deinit();

    for (int i = 0; i < 2; i++) {
        s_buf[i] = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buf[i] == NULL) {
            s_buf[i] = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_buf[i] == NULL) {
            ESP_LOGE(TAG, "值流信箱要唔到 %u B ×2（PSRAM／internal 都唔夠）", (unsigned)bytes);
            nc_px_nce_deinit();
            return false;
        }
        memset(s_buf[i], 0, bytes);
    }
    s_bytes = bytes;
    s_wr = 0;
    s_seq = 0;
    ESP_LOGI(TAG, "值流信箱 %u B ×2（每 px %u B，即 %u px）",
             (unsigned)bytes, (unsigned)NC_PX_NCE_BPP, (unsigned)(bytes / NC_PX_NCE_BPP));
    return true;
}

void nc_px_nce_deinit(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_buf[i] != NULL) {
            heap_caps_free(s_buf[i]);
            s_buf[i] = NULL;
        }
    }
    s_bytes = 0;
    s_wr = 0;
    s_seq = 0;
}

uint32_t nc_px_nce_bytes(void)
{
    return s_bytes;
}

bool nc_px_nce_publish(const uint8_t *src, uint32_t bytes)
{
    if (src == NULL || bytes == 0 || s_bytes == 0) {
        return false;
    }
    if (bytes > s_bytes) {
        ESP_LOGW(TAG, "publish %u B > 信箱 %u B → 截短（唔會寫爆）",
                 (unsigned)bytes, (unsigned)s_bytes);
        bytes = s_bytes;
    }
    const int dst = s_wr ^ 1;
    memcpy(s_buf[dst], src, bytes);
    mem_barrier();
    s_wr = dst;                            // 換手：reader 之後讀到的一定係完整的一份
    s_seq++;
    return true;
}

bool nc_px_nce_latest(nc_px_nce_frame_t *out)
{
    if (out == NULL || s_bytes == 0 || s_seq == 0) {
        return false;
    }
    const int idx = s_wr;
    mem_barrier();
    out->data = s_buf[idx];
    out->bytes = s_bytes;
    out->seq = s_seq;
    return true;
}

uint32_t nc_px_nce_to_vals(const uint8_t *src, uint32_t bytes, uint16_t *dst, uint32_t n)
{
    if (src == NULL || dst == NULL) {
        return 0;
    }
    uint32_t k = (bytes < n) ? bytes : n;
    for (uint32_t i = 0; i < k; i++) {
        dst[i] = (uint16_t)((uint16_t)src[i] << 4);    // 0–255 → 0–4080（>>4 復原）
    }
    return k;
}
