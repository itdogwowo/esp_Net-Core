// nc_i80_drv_direct.c — I80 直接驅動（唔經 esp_lcd）：只 route 真正接燈嘅 lane，
// WR／DC／未接燈嘅 data 線唔再需要真 GPIO（＝ FastLED 在 S3 用 LCD_CAM 的做法）。
//
// 同 nc_i80_drv.c（esp_lcd 版）**同一個介面**（nc_i80_drv.h）；邊個編譯由
// CONFIG_NETCORE_PIXEL_I80_DIRECT 決定（CMakeLists 二選一）。
//
// 為什麼得：LCD_CAM 的 i80 模式由 TRM 講「8/16 條 data + WR + DC」，但那是
// esp_lcd 的 panel IO 封裝要求；直接寫寄存器時，**未用到的 data 線唔 route 就可以**，
// 而且 LCD 的 word 數只由 DMA buffer／EOF 決定（lcd_always_out_en），
// 所以一支 data 線都出得。IDF 內部做法見 components/esp_lcd/i80/esp_lcd_panel_io_i80.c。
//
// 我們借 IDF 的 LL（hal/lcd_ll.h）+ AHB GDMA，不自己擰寄存器位：
//   時鐘：PLL_F160M 160 MHz → 群除頻 2 → LCD_CLK 80 MHz → prescale 32 = 2.5 MHz
//   DMA ：AHB GDMA trigger 5（SOC_GDMA_TRIG_PERIPH_LCD0）
//   完成：LCD_CAM 自己的 level interrupt（ETS_LCD_CAM_INTR_SOURCE），唔用 GDMA EOF
//         （FIFO 仲可以有 ≤16 word 未出，GDMA EOF 唔等於 wire 出完）
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL_I80_DIRECT

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "nc_i80_drv.h"

static const char *TAG = "drv_i80";

#if SOC_LCD_I80_SUPPORTED

#include "esp_cache.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/periph_ctrl.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/lcd_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "soc/clk_tree_defs.h"

#define I80_BUS_ID        0
#define I80_NODE_MAX_BYTES 4095u      // GDMA link node 單筆上限
#define I80_INTR_FLAGS    (ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_SHARED | \
                           ESP_INTR_FLAG_LOWMED)

typedef struct {
    bool                  ready;
    lcd_cam_dev_t        *dev;
    gdma_channel_handle_t chan;
    gdma_link_list_handle_t link;
    intr_handle_t         intr;

    uint8_t  slots;
    uint8_t  bus_width;
    uint8_t  lanes;
    uint32_t slot_bytes;
    uint32_t pclk_hz;

    uint8_t *tx[NC_I80_DRV_SLOTS_MAX];
    bool     tx_ext[NC_I80_DRV_SLOTS_MAX];   // 槽在 PSRAM（submit 前要 cache msync）
    bool     done[NC_I80_DRV_SLOTS_MAX];
    bool     busy[NC_I80_DRV_SLOTS_MAX];

    // 同一時間只跑一筆（64 px = 1.84 ms 線路時間，幀距 20 ms，唔需要管線化）
    volatile int cur;                        // 正在送的槽；-1 = 閒
    int          q[NC_I80_DRV_SLOTS_MAX];    // 等送
    int          q_head;
    int          q_count;

    portMUX_TYPE mux;
    uint32_t submitted;
    uint32_t done_count;
    uint32_t fail;
    uint32_t inflight;
    uint32_t underrun;
} i80_drv_t;

static i80_drv_t s_i80;

// ── 一筆：phase/blank → reset FIFO → mount → start DMA → 4 µs → start LCD ──
static bool start_slot_locked(int slot)
{
    i80_drv_t *d = &s_i80;
    uint8_t *buf = d->tx[slot];
    if (buf == NULL) {
        return false;
    }
    // 只有 data phase：word 數由 DMA buffer 長度（EOF）決定
    lcd_ll_set_phase_cycles(d->dev, 0, 0, 1);
    lcd_ll_set_blank_cycles(d->dev, 1, 1);
    lcd_ll_reset(d->dev);
    lcd_ll_fifo_reset(d->dev);

    const size_t align = gdma_get_buffer_alignment_constraint(d->chan, buf);
    gdma_buffer_mount_config_t mc = {
        .buffer = buf,
        .buffer_alignment = align,
        .length = d->slot_bytes,
        .flags = {
            .mark_eof = true,
            .mark_final = GDMA_FINAL_LINK_TO_NULL,
        },
    };
    if (gdma_link_mount_buffers(d->link, 0, &mc, 1, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "mount DMA buffer 失敗（slot %d）", slot);
        return false;
    }
    gdma_start(d->chan, gdma_link_get_head_addr(d->link));
    // 等 DMA 把第一批資料放入 LCD FIFO（IDF 就係 4 µs；PCLK 唔高時可以更短）
    esp_rom_delay_us(4);
    lcd_ll_start(d->dev);
    esp_intr_enable(d->intr);
    return true;
}

// 由 cur/queue 揀下一筆；caller 要持有 mux
static void kick_locked(void)
{
    i80_drv_t *d = &s_i80;
    while (d->cur < 0 && d->q_count > 0) {
        const int slot = d->q[d->q_head];
        d->q_head = (d->q_head + 1) % d->slots;
        d->q_count--;
        if (start_slot_locked(slot)) {
            d->cur = slot;
            return;
        }
        // 這一筆開唔到：當失敗，唔好卡住之後的槽
        d->busy[slot] = false;
        d->done[slot] = false;
        d->fail++;
        if (d->inflight > 0) {
            d->inflight--;
        }
    }
}

static void IRAM_ATTR i80_isr(void *arg)
{
    i80_drv_t *d = (i80_drv_t *)arg;
    const uint32_t st = lcd_ll_get_interrupt_status(d->dev);

    if (st & LCD_LL_EVENT_TRANS_DONE) {
        lcd_ll_clear_interrupt_status(d->dev, LCD_LL_EVENT_TRANS_DONE);
        esp_intr_disable(d->intr);
        portENTER_CRITICAL_ISR(&d->mux);
        const int slot = d->cur;
        if (slot >= 0) {
            d->done[slot] = true;
            d->cur = -1;
            d->done_count++;
        }
        kick_locked();      // 仲有嘢排隊就即刻開下一筆（會再 enable intr）
        portEXIT_CRITICAL_ISR(&d->mux);
    }
#ifdef LCD_LL_EVENT_UNDERRUN
    if (st & LCD_LL_EVENT_UNDERRUN) {
        lcd_ll_clear_interrupt_status(d->dev, LCD_LL_EVENT_UNDERRUN);
        d->underrun++;
    }
#endif
}

static void free_all(i80_drv_t *d)
{
    for (int i = 0; i < NC_I80_DRV_SLOTS_MAX; i++) {
        if (d->tx[i] != NULL) {
            heap_caps_free(d->tx[i]);
            d->tx[i] = NULL;
        }
    }
    if (d->intr != NULL) {
        esp_intr_free(d->intr);
        d->intr = NULL;
    }
    if (d->link != NULL) {
        gdma_del_link_list(d->link);
        d->link = NULL;
    }
    if (d->chan != NULL) {
        gdma_disconnect(d->chan);
        gdma_del_channel(d->chan);
        d->chan = NULL;
    }
    if (d->dev != NULL) {
        lcd_ll_enable_clock(d->dev, false);
    }
    d->ready = false;
}

esp_err_t nc_i80_drv_install(const nc_i80_drv_cfg_t *cfg)
{
    if (cfg == NULL || cfg->slot_bytes == 0 || cfg->lane_pins == NULL ||
        cfg->lanes == 0 || cfg->lanes > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i80.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_i80, 0, sizeof(s_i80));
    i80_drv_t *d = &s_i80;
    portMUX_INITIALIZE(&d->mux);
    d->cur = -1;
    d->slots = (uint8_t)((cfg->slots < 2) ? 2u : cfg->slots);
    if (d->slots > NC_I80_DRV_SLOTS_MAX) {
        d->slots = NC_I80_DRV_SLOTS_MAX;
    }
    d->slot_bytes = cfg->slot_bytes;
    d->pclk_hz = cfg->pclk_hz;
    d->lanes = cfg->lanes;
    d->bus_width = (cfg->bus_width == 16) ? 16u : 8u;
    if (d->lanes > d->bus_width) {
        ESP_LOGE(TAG, "lanes=%u 大過 bus_width=%u", d->lanes, d->bus_width);
        return ESP_ERR_INVALID_ARG;
    }
    // lane 腳：有效、唔重複
    for (int i = 0; i < (int)d->lanes; i++) {
        const int pin = cfg->lane_pins[i];
        if (pin < 0 || pin > 48) {
            ESP_LOGE(TAG, "lane %d 腳位 = %d 不是有效 GPIO", i, pin);
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (cfg->lane_pins[j] == pin) {
                ESP_LOGE(TAG, "GPIO%d 重複（lane %d 與 %d）", pin, j, i);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    // ① 時鐘：PERIPH_LCD_CAM 的 bus clock → PLL_F160M → 群除頻 2 = 80 MHz → prescale
    bool clock_on = false;
    PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_LCD_CAM_MODULE, rc) {
        if (rc == 0) {
            PERIPH_RCC_ATOMIC() {
                lcd_ll_enable_bus_clock(I80_BUS_ID, true);
                lcd_ll_reset_register(I80_BUS_ID);
            }
        }
        clock_on = true;
    }
    if (!clock_on) {
        ESP_LOGE(TAG, "LCD_CAM 時鐘取用失敗");
        return ESP_FAIL;
    }
    d->dev = LCD_LL_GET_HW(I80_BUS_ID);
    lcd_ll_enable_clock(d->dev, true);
    esp_err_t err = esp_clk_tree_enable_src((soc_module_clk_t)LCD_CLK_SRC_DEFAULT, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clk tree enable 失敗（0x%x）", err);
        free_all(d);
        return err;
    }
    lcd_ll_select_clk_src(I80_BUS_ID, LCD_CLK_SRC_DEFAULT);
    lcd_ll_set_group_clock_coeff(I80_BUS_ID, 2, 0, 0);     // LCD_CLK = 160/2 = 80 MHz

    const uint32_t lcd_clk_hz = 80000000u;                 // S3：PLL_F160M / 2
    uint32_t prescale = (d->pclk_hz != 0) ? (lcd_clk_hz / d->pclk_hz) : 32u;
    if (prescale == 0) {
        prescale = 1;
    }
    if (prescale > 64) {
        ESP_LOGE(TAG, "pclk %u Hz 除唔到（prescale=%u > 64）", d->pclk_hz, prescale);
        free_all(d);
        return ESP_ERR_INVALID_ARG;
    }
    d->pclk_hz = lcd_clk_hz / prescale;                     // 實際 PCLK（2.5 MHz → 32）

    lcd_ll_reset(d->dev);
    lcd_ll_fifo_reset(d->dev);
    lcd_ll_set_pixel_clock_prescale(d->dev, prescale);
    lcd_ll_set_clock_idle_level(d->dev, false);              // pclk_idle_low = 1
    lcd_ll_set_pixel_clock_edge(d->dev, false);              // pclk_active_neg = 0

    // ② i80 模式：只有 data phase、always out、8/16-bit、無 byte swap
    lcd_ll_enable_rgb_mode(d->dev, false);
#ifdef SOC_LCDCAM_I80_HAS_COLOR_CONVERT
    lcd_ll_enable_color_convert(d->dev, false);
#endif
    lcd_ll_set_dma_read_stride(d->dev, d->bus_width);        // 8 ⇒ lcd_2byte_en=0
    lcd_ll_enable_output_always_on(d->dev, true);            // word 數由 DMA 決定
    lcd_ll_set_dc_level(d->dev, false, false, false, false);

    // ③ GPIO：只 route 真正接燈的 lane（呢個就係「零 dummy」）
    for (int i = 0; i < (int)d->lanes; i++) {
        esp_rom_gpio_pad_select_gpio(cfg->lane_pins[i]);
        esp_rom_gpio_connect_out_signal(cfg->lane_pins[i],
                                        (LCD_DATA_OUT0_IDX + i),
                                        false, false);
    }

    // ③b 可選：真熒幕（LCD 面板）訊號。LED lane 模式全部 -1 → 唔會 route。
    //     呢個分支係留給「同一個驅動將來出面板」，唔可以當 LED 專用而刪走。
    //     ⚠ cmd phase／多 device／CS 仲裁未做 → 面板暫時請用 esp_lcd 路徑（唔靜默降級）。
    if (cfg->wr_pin >= 0) {
        esp_rom_gpio_pad_select_gpio(cfg->wr_pin);
        esp_rom_gpio_connect_out_signal(cfg->wr_pin, LCD_PCLK_IDX, false, false);
    }
    if (cfg->dc_pin >= 0) {
        esp_rom_gpio_pad_select_gpio(cfg->dc_pin);
        esp_rom_gpio_connect_out_signal(cfg->dc_pin, LCD_DC_IDX, false, false);
    }
    if (cfg->cs_pin >= 0) {
        const gpio_config_t cs = {
            .pin_bit_mask = 1ull << cfg->cs_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cs);
        gpio_set_level((gpio_num_t)cfg->cs_pin, cfg->cs_active_high ? 0 : 1); // 先 de-assert
    }

    // ④ DMA：AHB GDMA（trigger 5）＋ link list
    gdma_channel_alloc_config_t chan_cfg = {0};
    err = gdma_new_ahb_channel(&chan_cfg, &d->chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GDMA channel 建立失敗（0x%x）", err);
        free_all(d);
        return err;
    }
    gdma_connect(d->chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
    gdma_strategy_config_t strat = { .auto_update_desc = true, .owner_check = true };
    gdma_apply_strategy(d->chan, &strat);
    gdma_transfer_config_t trans = { .max_data_burst_size = 32, .access_ext_mem = true };
    err = gdma_config_transfer(d->chan, &trans);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GDMA config 失敗（0x%x）", err);
        free_all(d);
        return err;
    }
    const uint32_t nodes = (d->slot_bytes + I80_NODE_MAX_BYTES - 1u) / I80_NODE_MAX_BYTES + 1u;
    gdma_link_list_config_t link_cfg = {
        .item_alignment = 4,
        .num_items = nodes,
        .flags = { .check_owner = true },
    };
    err = gdma_new_link_list(&link_cfg, &d->link);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GDMA link list 建立失敗（0x%x）", err);
        free_all(d);
        return err;
    }

    // ⑤ 完成中斷：LCD_CAM 的 level interrupt（status reg + mask 由硬件 filter）
    err = esp_intr_alloc_intrstatus(ETS_LCD_CAM_INTR_SOURCE, I80_INTR_FLAGS,
                                    (uint32_t)lcd_ll_get_interrupt_status_reg(d->dev),
                                    LCD_LL_EVENT_I80, i80_isr, d, &d->intr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "中斷配置失敗（0x%x）", err);
        free_all(d);
        return err;
    }
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(d->dev, LCD_LL_EVENT_I80, true);
    }
    lcd_ll_clear_interrupt_status(d->dev, UINT32_MAX);

    // ⑥ DMA 槽：細幀（整組 ≤48 KB）先試 internal（唔經 cache），大幀才 PSRAM
    const bool prefer_int = ((size_t)d->slot_bytes * d->slots) <= (48u * 1024u);
    for (int i = 0; i < (int)d->slots; i++) {
        if (prefer_int) {
            d->tx[i] = heap_caps_aligned_calloc(64, d->slot_bytes, 1,
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        }
        if (d->tx[i] == NULL) {
            d->tx[i] = heap_caps_aligned_calloc(64, d->slot_bytes, 1,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA |
                                                MALLOC_CAP_8BIT);
            d->tx_ext[i] = (d->tx[i] != NULL);
        }
        if (d->tx[i] == NULL) {
            ESP_LOGE(TAG, "DMA 槽 %d（%u B）配置失敗", i, (unsigned)d->slot_bytes);
            free_all(d);
            return ESP_ERR_NO_MEM;
        }
    }

    d->ready = true;
    ESP_LOGI(TAG, "I80 直驅就緒：%u lane、%u-bit、pclk %u Hz（prescale %u）、"
                  "一筆 %u B（%.0f µs）、%u 槽；只 route lane 腳（無 WR／DC／dummy）",
             (unsigned)d->lanes, (unsigned)d->bus_width, (unsigned)d->pclk_hz,
             (unsigned)prescale, (unsigned)d->slot_bytes,
             (double)d->slot_bytes * 1000000.0 / (double)d->pclk_hz, (unsigned)d->slots);
    return ESP_OK;
}

void nc_i80_drv_uninstall(void)
{
    free_all(&s_i80);
}

bool nc_i80_drv_ready(void)            { return s_i80.ready; }
int    nc_i80_drv_slots(void)          { return s_i80.ready ? (int)s_i80.slots : 0; }
uint32_t nc_i80_drv_slot_bytes(void)   { return s_i80.slot_bytes; }

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
    if (d->busy[slot] || d->done[slot]) {
        return false;                       // 這一格仲未交回，不可以重用
    }
    // PSRAM 槽：DMA 讀之前要把 cache 寫回實體記憶體（internal 槽不會有 cache line）
    if (d->tx_ext[slot] && esp_cache_get_line_size_by_addr(d->tx[slot]) > 0) {
        if (esp_cache_msync(d->tx[slot], d->slot_bytes,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED)
            != ESP_OK) {
            ESP_LOGE(TAG, "cache msync 失敗（slot %d）", slot);
            d->fail++;
            return false;
        }
    }
    portENTER_CRITICAL(&d->mux);
    d->busy[slot] = true;
    d->q[(d->q_head + d->q_count) % d->slots] = slot;
    d->q_count++;
    d->submitted++;
    d->inflight++;
    kick_locked();
    portEXIT_CRITICAL(&d->mux);
    return true;
}

bool nc_i80_drv_slot_done(int slot)
{
    i80_drv_t *d = &s_i80;
    if (!d->ready || slot < 0 || slot >= (int)d->slots) {
        return false;
    }
    bool ok = false;
    portENTER_CRITICAL(&d->mux);
    if (d->done[slot]) {
        d->done[slot] = false;
        d->busy[slot] = false;
        if (d->inflight > 0) {
            d->inflight--;
        }
        ok = true;
    }
    portEXIT_CRITICAL(&d->mux);
    return ok;
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
bool      nc_i80_drv_slot_done(int slot) { (void)slot; return false; }
uint32_t  nc_i80_drv_submitted(void) { return 0; }
uint32_t  nc_i80_drv_done_count(void) { return 0; }
uint32_t  nc_i80_drv_fail(void) { return 0; }
uint32_t  nc_i80_drv_inflight(void) { return 0; }
uint32_t  nc_i80_drv_pclk(void) { return 0; }

#endif  // SOC_LCD_I80_SUPPORTED
#endif  // CONFIG_NETCORE_PIXEL_I80_DIRECT
