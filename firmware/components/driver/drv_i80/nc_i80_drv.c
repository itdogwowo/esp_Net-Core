// nc_i80_drv.c — I80（LCD_CAM 8080 並口）驅動：GDMA 推 byte 流。
//
// 為什麼是 I80：S3 的 LCD_CAM 把「一個 PCLK 一個 byte」推到 D0..D7 八條資料線；
// 波形由 GDMA 推，CPU 只負責把資料填進 DMA 槽。
// 十六條燈鏈＝兩顆 I80；一個 byte 的 bit0..bit7 就是 D0..D7。
//
// 記憶體：槽按 cfg->slot_bytes × cfg->slots 配置（internal + DMA），不是按任何
// 「最大像素數」的靜態上限 —— 靜態上限在 S3（無 PSRAM）連 link 都過不了。
#include "nc_i80_drv.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#if __has_include("esp_cache.h")
#include "esp_cache.h"
#define NC_HAS_CACHE_MSYNC 1
#else
#define NC_HAS_CACHE_MSYNC 0
#endif
#if __has_include("esp_memory_utils.h")
#include "esp_memory_utils.h"
#define NC_HAS_MEM_UTILS 1
#else
#define NC_HAS_MEM_UTILS 0
#endif
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if SOC_LCD_I80_SUPPORTED

static const char *TAG = "drv_i80";

typedef struct {
    bool     ready;
    uint8_t  slots;
    uint32_t slot_bytes;
    uint8_t *tx[NC_I80_DRV_SLOTS_MAX];
    bool     tx_ext[NC_I80_DRV_SLOTS_MAX];  // 槽在 PSRAM（提交前要 cache msync）
    uint32_t order[NC_I80_DRV_SLOTS_MAX];   // 提交序號（0 = 空閒）
    uint8_t  done[NC_I80_DRV_SLOTS_MAX];
    uint32_t submit_order;                  // task 側單獨寫
    uint32_t done_order;                    // ISR 側單獨寫
    uint32_t submitted;
    uint32_t done_count;
    uint32_t fail;
    uint32_t inflight;
    uint32_t pclk_hz;
    esp_lcd_i80_bus_handle_t  bus;
    esp_lcd_panel_io_handle_t io;
} i80_drv_t;

static i80_drv_t s_i80;

// 完成回呼：esp_lcd 對同一個 panel_io 是 FIFO，但**槽索引不是 FIFO**（槽會回收再重用）。
// 掃「第一個未完成」會在回收重排之後認錯格 → 提早還格 = DMA 還在讀就被覆寫。
// 做法：提交時給每筆一個單調 order，ISR 用 done_order + 1 找回對應的槽。
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    i80_drv_t *d = (i80_drv_t *)ctx;
    const uint32_t want = d->done_order + 1u;
    for (uint32_t i = 0; i < d->slots; i++) {
        if (d->order[i] == want) {
            d->done[i] = 1;
            d->done_order = want;
            break;
        }
    }
    return false;
}

static void i80_free(i80_drv_t *d)
{
    for (uint32_t i = 0; i < NC_I80_DRV_SLOTS_MAX; i++) {
        if (d->tx[i] != NULL) {
            heap_caps_free(d->tx[i]);
            d->tx[i] = NULL;
        }
    }
    d->ready = false;
}

esp_err_t nc_i80_drv_install(const nc_i80_drv_cfg_t *cfg)
{
    if (cfg == NULL || cfg->slot_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // 這個檔是 esp_lcd 版：一定要齊 pins（D0..D、WR、DC）。
    // 想只給 lane_pins（唔要 WR／DC／dummy）就開 CONFIG_NETCORE_PIXEL_I80_DIRECT。
    if (cfg->pins == NULL) {
        ESP_LOGE(TAG, "esp_lcd 版需要 pins[]（DIRECT 版才只需 lane_pins[]）");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i80.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_i80, 0, sizeof(s_i80));
    i80_drv_t *d = &s_i80;
    d->slots = (uint8_t)((cfg->slots < 2) ? 2u : cfg->slots);
    if (d->slots > NC_I80_DRV_SLOTS_MAX) {
        d->slots = NC_I80_DRV_SLOTS_MAX;
    }
    d->slot_bytes = cfg->slot_bytes;
    d->pclk_hz = cfg->pclk_hz;

    const uint8_t width = (cfg->bus_width == 16) ? 16u : 8u;
    for (int i = 0; i < (int)width + 2; i++) {
        if (cfg->pins[i] < 0 || cfg->pins[i] > 48) {
            ESP_LOGE(TAG, "I80 腳位第 %d 個 = %d 不是有效 GPIO", i + 1, cfg->pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (cfg->pins[j] == cfg->pins[i]) {
                ESP_LOGE(TAG, "GPIO%d 重複（第 %d 與第 %d 個）", cfg->pins[i], j + 1, i + 1);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = (gpio_num_t)cfg->pins[width],
        .wr_gpio_num = (gpio_num_t)cfg->pins[width + 1],
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .bus_width = width,
        .max_transfer_bytes = d->slot_bytes,
        .dma_burst_size = 0,      // 0 = 用驅動預設（32）
    };
    for (int i = 0; i < (int)width; i++) {
        bus_cfg.data_gpio_nums[i] = (gpio_num_t)cfg->pins[i];
    }
    if (esp_lcd_new_i80_bus(&bus_cfg, &d->bus) != ESP_OK) {
        ESP_LOGE(TAG, "I80 bus 建立失敗");
        i80_free(d);
        return ESP_FAIL;
    }
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1,
        .pclk_hz = d->pclk_hz,
        .trans_queue_depth = (int)d->slots + 1,
        .on_color_trans_done = on_color_done,
        .user_ctx = d,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = { 0, 0, 0, 0 },
        .flags = { .pclk_active_neg = 0, .pclk_idle_low = 1 },
    };
    if (esp_lcd_new_panel_io_i80(d->bus, &io_cfg, &d->io) != ESP_OK) {
        ESP_LOGE(TAG, "I80 panel IO 建立失敗");
        i80_free(d);
        return ESP_FAIL;
    }
    // 槽放哪裡：小幀（整組 ≤ 48 KB）先試 internal —— internal 不在 cache 管轄，
    // submit 完全唔需要 msync（實測 6×20 的 tx_us 由 76 µs 回到 ~5 µs）；
    // 大幀（16×600 一槽 86.8 KB × 3 槽 = 260 KB）internal 放唔落，才用 PSRAM，
    // cache writeback 交由 esp_lcd 的 tx_color 做（見 nc_i80_drv_submit）。
    const bool prefer_int = ((size_t)d->slot_bytes * d->slots) <= (48u * 1024u);
    for (uint32_t i = 0; i < d->slots; i++) {
        bool ext = false;
        if (prefer_int) {
            d->tx[i] = heap_caps_aligned_calloc(64, d->slot_bytes, 1,
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        }
        if (d->tx[i] == NULL) {
            d->tx[i] = heap_caps_aligned_calloc(64, d->slot_bytes, 1,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA |
                                                MALLOC_CAP_8BIT);
            ext = (d->tx[i] != NULL);
        }
        d->tx_ext[i] = ext;
        if (d->tx[i] == NULL) {
            ESP_LOGE(TAG, "DMA 槽 %u（%u B）配置失敗", (unsigned)i, (unsigned)d->slot_bytes);
            i80_free(d);
            return ESP_ERR_NO_MEM;
        }
    }
    d->ready = true;
    ESP_LOGI(TAG, "I80 就緒：%u-bit、pclk %u Hz、一筆 %u B（%.0f µs）、%u 槽",
             (unsigned)width, (unsigned)d->pclk_hz, (unsigned)d->slot_bytes,
             (double)d->slot_bytes * 1000000.0 / (double)d->pclk_hz, (unsigned)d->slots);
    return ESP_OK;
}

void nc_i80_drv_uninstall(void)
{
    i80_free(&s_i80);
}

bool nc_i80_drv_ready(void)
{
    return s_i80.ready;
}

int nc_i80_drv_slots(void)
{
    return s_i80.ready ? (int)s_i80.slots : 0;
}

uint32_t nc_i80_drv_slot_bytes(void)
{
    return s_i80.slot_bytes;
}

uint8_t *nc_i80_drv_slot_buf(int slot)
{
    if (!s_i80.ready || slot < 0 || slot >= (int)s_i80.slots) {
        return NULL;
    }
    return s_i80.tx[slot];
}

bool nc_i80_drv_submit(int slot)
{
    i80_drv_t *d = &s_i80;
    if (!d->ready || slot < 0 || slot >= (int)d->slots || d->tx[slot] == NULL) {
        return false;
    }
    if (d->order[slot] != 0) {
        return false;                       // 這一格還沒完成，不可以重用
    }
    const uint32_t ord = d->submit_order + 1u;
    d->order[slot] = ord;
    // ⚠ 唔在這裡做 esp_cache_msync：esp_lcd 的 panel_io_i80_tx_color() 已經對
    // cache 管轄的緩衝（PSRAM）自己做 C2M writeback（esp_lcd_panel_io_i80.c
    // 「flush data from cache to the physical memory」）。以前兩邊各做一次，
    // 86.8 KB 的槽就係多一次 cache 掃描。internal 槽唔經 cache，更加唔需要。
    // esp_lcd 的 tx_color 排進佇列就返回，完成時才回呼。
    const esp_err_t e = esp_lcd_panel_io_tx_color(d->io, -1, d->tx[slot], d->slot_bytes);
    if (e != ESP_OK) {
        d->order[slot] = 0;
        d->fail++;
        return false;
    }
    d->submit_order = ord;
    d->submitted++;
    d->inflight++;
    return true;
}

// 面板交易：esp_lcd 版本本身支援 lcd_cmd（cmd phase）——同 direct 版語義一致。
bool nc_i80_drv_submit_cmd(int slot, int lcd_cmd, size_t bytes)
{
    i80_drv_t *d = &s_i80;
    if (!d->ready || slot < 0 || slot >= (int)d->slots || d->tx[slot] == NULL) {
        return false;
    }
    if (d->order[slot] != 0) {
        return false;
    }
    uint32_t len = (bytes == 0) ? d->slot_bytes : (uint32_t)bytes;
    if (len > d->slot_bytes) {
        return false;
    }
    const uint32_t ord = d->submit_order + 1u;
    d->order[slot] = ord;
    const esp_err_t e = esp_lcd_panel_io_tx_color(d->io, lcd_cmd, d->tx[slot], len);
    if (e != ESP_OK) {
        d->order[slot] = 0;
        d->fail++;
        return false;
    }
    d->submit_order = ord;
    d->submitted++;
    d->inflight++;
    return true;
}

bool nc_i80_drv_slot_done(int slot)
{
    i80_drv_t *d = &s_i80;
    if (!d->ready || slot < 0 || slot >= (int)d->slots) {
        return false;
    }
    if (d->order[slot] == 0 || d->done[slot] == 0) {
        return false;
    }
    d->done[slot] = 0;
    d->order[slot] = 0;
    d->done_count++;
    if (d->inflight > 0) {
        d->inflight--;
    }
    return true;
}

uint32_t nc_i80_drv_submitted(void)  { return s_i80.submitted; }
uint32_t nc_i80_drv_done_count(void) { return s_i80.done_count; }
uint32_t nc_i80_drv_fail(void)       { return s_i80.fail; }
uint32_t nc_i80_drv_inflight(void)   { return s_i80.inflight; }
uint32_t nc_i80_drv_pclk(void)       { return s_i80.pclk_hz; }

#else   // !SOC_LCD_I80_SUPPORTED

esp_err_t nc_i80_drv_install(const nc_i80_drv_cfg_t *cfg) { (void)cfg; return ESP_ERR_NOT_SUPPORTED; }
void      nc_i80_drv_uninstall(void) { }
bool      nc_i80_drv_ready(void) { return false; }
int       nc_i80_drv_slots(void) { return 0; }
uint32_t  nc_i80_drv_slot_bytes(void) { return 0; }
uint8_t  *nc_i80_drv_slot_buf(int slot) { (void)slot; return NULL; }
bool      nc_i80_drv_submit(int slot) { (void)slot; return false; }
bool      nc_i80_drv_submit_cmd(int slot, int lcd_cmd, size_t bytes)
{ (void)slot; (void)lcd_cmd; (void)bytes; return false; }
bool      nc_i80_drv_slot_done(int slot) { (void)slot; return false; }
uint32_t  nc_i80_drv_submitted(void) { return 0; }
uint32_t  nc_i80_drv_done_count(void) { return 0; }
uint32_t  nc_i80_drv_fail(void) { return 0; }
uint32_t  nc_i80_drv_inflight(void) { return 0; }
uint32_t  nc_i80_drv_pclk(void) { return 0; }

#endif  // SOC_LCD_I80_SUPPORTED
