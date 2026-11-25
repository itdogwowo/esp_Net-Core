// nc_render_task.c — 公用輸出層 render_task（推送／渲染；LED 現在、LCD 將來）。
//
// 2026-11-25 四層四任務：
//   pixel_effect_task（效果值）→ pixel_pair_task（抖動＋配對）→
//   pixel_encode_task（環 → 編碼 → 後端的 DMA 槽；獨立 component）→
//   **render_task（本檔；DMA 槽 → 硬件；環／lane／節拍的主人）**。
//
// render 唔屬 pixel 系列：其他子系統（LCD 等）都會用同一個輸出層。
//
// 為什麼畫波同推送要分兩個任務：
//   · 畫波係 CPU 重活（9,600 px 合成 ~6.4 ms），推送係 submit＋等 DMA；
//   · 分開之後推送唔會俾畫波阻住，DMA 完成可以即刻還格（P6 陷阱 1：
//     一定要 DMA 完成才可以 release 環格，提前還 = torn frame）；
//   · wave → push 只傳「格索引」（8 格隊列），唔 copy 任何像素。
//
// 一個 render 任務可以有多道 lane；每道 lane = 一個後端 + 一條環。
// 環由 render_task 宣告（declare）同建立，生產者（pair／wave／.nce／主機）按名字找回。
//
// 檔案分工：
//   sw/pixel                    環、編碼器、效果引擎（純 C，host 測試同一份）
//   tasks/render_task           這個檔＋後端轉接（i80／spi／direct／mock）
//   driver/drv_i80、drv_spi_led 硬件（PCLK／DMA／完成）
#include "nc_render.h"
#include "nc_fps_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nc_cfg.h"
#include "nc_prio.h"
#include "nc_task.h"
#include "sdkconfig.h"

#if CONFIG_NETCORE_PIXEL

static const char *TAG = "px_out";       // 後端／環／畫波／推送

// ═══════════════════════ 後端註冊表 ═══════════════════════
static nc_pixel_backend_t *s_be[NC_PX_MAX_LANES];
static int s_nbe;

esp_err_t nc_px_backend_register(nc_pixel_backend_t *be)
{
    if (be == NULL || be->name[0] == '\0' || be->acquire == NULL || be->encode == NULL ||
        be->submit == NULL || be->reclaim == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nc_px_backend_find(be->name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_nbe >= NC_PX_MAX_LANES) {
        return ESP_ERR_NO_MEM;
    }
    s_be[s_nbe++] = be;
    return ESP_OK;
}

esp_err_t nc_px_backend_unregister(const char *name)
{
    for (int i = 0; i < s_nbe; i++) {
        if (strcmp(s_be[i]->name, name) == 0) {
            s_be[i] = s_be[--s_nbe];
            s_be[s_nbe] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

nc_pixel_backend_t *nc_px_backend_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_nbe; i++) {
        if (strcmp(s_be[i]->name, name) == 0) {
            return s_be[i];
        }
    }
    return NULL;
}

int nc_px_backend_count(void) { return s_nbe; }

// ═══════════════════════ 設定 ═══════════════════════
// 預設 = 「這塊板有 I80 就用 I80，否則退到 mock」。
// 環名帶 prd = 內建效果生產者（pixel 任務）餵這一格。
typedef struct {
    char     ring[NC_PX_NAME_MAX + 1];
    char     be[NC_PX_BACKEND_MAX];
    uint32_t px;
    uint32_t slots;
} px_plan_t;

static px_plan_t s_plan[NC_PX_MAX_LANES];
static int s_nplan;
static nc_pixel_q_t s_rings[NC_PX_MAX_LANES];

static nc_pixel_cfg_t s_cfg = {
    // 後備節拍：實際以 system.json 的 frame_interval_ms 為準（見 px_start）。
    .fps = CONFIG_NETCORE_PIXEL_FPS,
#if CONFIG_NETCORE_PIXEL_MOCK_ONLY
    .nlane = 1,
    .lane = {
        { .be = "mock", .ring = "mock.prd" },   // 量 CPU：唔受線路時間限制
    },
#else
    .nlane = 2,
    .lane = {
        { .be = "i80", .ring = "i80.prd" },
        { .be = "mock", .ring = "mock.prd" },   // 冇 I80 的組態：效果管線仍然有環可餵
    },
#endif
};

static bool s_cfg_explicit;   // nc_render_set_cfg() 明確給過 → driver.json 唔可以蓋

void nc_render_set_cfg(const nc_pixel_cfg_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
        s_cfg_explicit = true;
        s_nplan = 0;                 // 設定改了，plan 重算
    }
}

const nc_pixel_cfg_t *nc_render_cfg(void)
{
    return &s_cfg;
}

// ═══════════════════════ 燈鏈來源：driver.json 先行、Kconfig 後備 ═══════════════════════
// 「硬件先、driver 後」：腳位在 wiring.json，這裡只拿 ncportgen 併好的 nc_led_cfg_t。
// 三種狀態，語意唔可以混：
//   present=0                → 完全冇宣告（未遷移的板子）→ 用 CONFIG_NETCORE_PIXEL_*
//   present=1, enable=0      → 明確「唔用燈」→ 只留 mock lane，**唔會**退回 Kconfig
//   present=1, enable=1      → backend／lanes／px／format／timing／pins 全部由 JSON 決定
// project 的 sdkconfig 覆寫了 pixel（外接燈具）→ ncportgen 已經讓位，這裡拿到 present=0。
static const nc_led_cfg_t *led_cfg(void)
{
    static bool done;
    static const nc_led_cfg_t *c;
    if (!done) {
        done = true;
        c = nc_cfg_defaults_led();          // NULL = 冇宣告
        if (c != NULL) {
            ESP_LOGI(TAG, "燈鏈由 driver.json 決定：enable=%d backend=%u lanes=%u px=%u fmt=%s",
                     c->enable ? 1 : 0, (unsigned)c->backend, (unsigned)c->lanes,
                     (unsigned)c->px, c->fmt);
        }
    }
    return c;
}

// 「真的要用燈」= 有宣告而且 enable。
static const nc_led_cfg_t *led_active(void)
{
    const nc_led_cfg_t *c = led_cfg();
    return (c != NULL && c->enable) ? c : NULL;
}

#if !CONFIG_NETCORE_PIXEL_MOCK_ONLY
// MOCK_ONLY 的映像只用 mock lane，唔會叫後端名 → 呢個 function 會冇人用。
static const char *led_backend_name(uint8_t backend)
{
    switch (backend) {
    case NC_LED_BACKEND_I80:    return "i80";
    case NC_LED_BACKEND_SPI:    return "spi";
    case NC_LED_BACKEND_RMT:    return "rmt";
    case NC_LED_BACKEND_PARLIO: return "parlio";
    case NC_LED_BACKEND_DIRECT: return "direct";
    case NC_LED_BACKEND_MOCK:   return "mock";
    default:                    return NULL;
    }
}
#endif

// ═══════════════════════ 後端：誰存在、要幾大 ═══════════════════════
// 一個後端的「一格幾顆」由它自己回答，而且**不碰硬體**：
//   · i80／spi 只在 Kconfig 開了、而且板上有腳位時才算存在；
//   · direct／mock 永遠存在 —— 沒有燈板也能把整條路跑起來。
static uint32_t stage_px(const char *be)
{
    if (be == NULL) {
        return 0;
    }
    // driver.json 宣告了燈鏈 → 一格幾顆是它的 px（per lane），唔再讀 Kconfig。
    // present=1 而 enable=0 都照用 px：那時只剩 mock lane，環的大小仍然要一致。
    const nc_led_cfg_t *decl = led_cfg();
    if (decl != NULL && decl->px > 0) {
        return (uint32_t)decl->px;
    }
    const uint32_t px = (CONFIG_NETCORE_PIXEL_LEDS > 0) ? (uint32_t)CONFIG_NETCORE_PIXEL_LEDS
                                                        : 0u;
    if (px == 0) {
        return 0;
    }
#if CONFIG_NETCORE_PIXEL_I80
    if (strcmp(be, "i80") == 0) {
        return px;
    }
#endif
#if CONFIG_NETCORE_PIXEL_SPI
    if (strcmp(be, "spi") == 0) {
        return px;
    }
#endif
    if (strcmp(be, "direct") == 0 || strcmp(be, "mock") == 0) {
        return px;
    }
    return 0;
}

static int plan_lanes(void)
{
    if (s_nplan > 0) {
        return s_nplan;
    }
    // driver.json 有 LED 段 → 預設的 lane 由它決定（backend + "<backend>.prd" 環）。
    // 只改「預設」：nc_render_set_cfg() 已經明確給過的唔會被蓋走。
    static bool json_applied;
    const nc_led_cfg_t *decl = led_cfg();        // 有宣告（唔理 enable）
    const nc_led_cfg_t *led = led_active();      // 有宣告而且要用
    if (!json_applied && !s_cfg_explicit && decl != NULL) {
        json_applied = true;
#if CONFIG_NETCORE_PIXEL_MOCK_ONLY
        // 量 CPU 的映像（Show2400 之類）刻意唔點燈：mock-only 永遠贏。
        ESP_LOGW(TAG, "Kconfig 是 MOCK_ONLY → 忽略 driver.json 的燈鏈（量測模式）");
        (void)led;
#else
        memset(s_cfg.lane, 0, sizeof(s_cfg.lane));
        s_cfg.nlane = 1;
        const char *be = (led != NULL) ? led_backend_name(led->backend) : NULL;
        if (be == NULL) {
            // present=1 而 enable=0（或者 backend 未設定）= 明確唔用燈：
            // 只留 mock，唔會退回去讀 Kconfig 亂開 i80。
            be = "mock";
            if (led != NULL) {
                ESP_LOGE(TAG, "driver.json 的 backend=%u 冇對應名稱 → 只留 mock lane",
                         (unsigned)led->backend);
            }
        }
        (void)snprintf(s_cfg.lane[0].be, sizeof(s_cfg.lane[0].be), "%s", be);
        (void)snprintf(s_cfg.lane[0].ring, sizeof(s_cfg.lane[0].ring), "%s.prd", be);
#endif
    }
    for (int i = 0; i < s_cfg.nlane && i < NC_PX_MAX_LANES; i++) {
        const uint32_t px = stage_px(s_cfg.lane[i].be);
        if (px == 0) {
            continue;
        }
        px_plan_t *pl = &s_plan[s_nplan++];
        (void)snprintf(pl->be, sizeof(pl->be), "%s", s_cfg.lane[i].be);
        // 環名：lane cfg 寫了就用它（例如 "i80.prd" 表示「這條環由效果生產者餵」），
        // 否則 "<後端>.<第幾道>"。
        if (s_cfg.lane[i].ring[0] != '\0') {
            (void)snprintf(pl->ring, sizeof(pl->ring), "%s", s_cfg.lane[i].ring);
        } else {
            (void)snprintf(pl->ring, sizeof(pl->ring), "%s.%d", s_cfg.lane[i].be, i);
        }
        pl->px = px;
        // 3 格：DMA 送第 N 格時，生產者可以填 N+1、N+2（P6 的三緩衝）。
        pl->slots = (uint32_t)CONFIG_NETCORE_PIXEL_RING_SLOTS;
    }
    return s_nplan;
}

// codec 設定：格式與時序是「板子的物理事實」。
// driver.json 有 LED 段 → 由它給（timing 省略 = 用格式本身的預設波形）；
// 否則跟 Kconfig（未遷移的板子）。
static void codec_cfg_of(nc_px_codec_cfg_t *cc, uint32_t px, const char *be)
{
    memset(cc, 0, sizeof(*cc));
    const nc_led_cfg_t *led = led_active();
    nc_px_format_t fmt = NC_PX_FMT_WS2812W;
    if (led != NULL) {
        if (!nc_px_format_parse(led->fmt, &fmt)) {
            ESP_LOGE(TAG, "driver.json 的 format=%s 唔認得 → 用 ws2812w", led->fmt);
            fmt = NC_PX_FMT_WS2812W;
        }
    } else {
        (void)nc_px_format_parse(CONFIG_NETCORE_PIXEL_FORMAT, &fmt);
    }
    if (strcmp(be, "direct") == 0) {
        fmt = NC_PX_FMT_WS2812;      // direct 只是把波形放著，用小一點的格式
    }
    cc->fmt = fmt;
    nc_px_wave_default(fmt, &cc->wave);
    if (led != NULL) {
        // 0 = 該格式的預設值（見 nc_px_wave_default）；寫了就以 JSON 為準。
        if (led->clk_hz > 0) {
            cc->wave.clk_hz = led->clk_hz;
        }
        if (led->bit_cells > 0) {
            cc->wave.bit_cells = led->bit_cells;
        }
        if (led->t0h > 0) {
            cc->wave.t0h = led->t0h;
        }
        if (led->t1h > 0) {
            cc->wave.t1h = led->t1h;
        }
    } else {
        cc->wave.clk_hz = CONFIG_NETCORE_PIXEL_PCLK_HZ;
        cc->wave.bit_cells = CONFIG_NETCORE_PIXEL_BIT_CELLS;
        cc->wave.t0h = CONFIG_NETCORE_PIXEL_T0H;
        cc->wave.t1h = CONFIG_NETCORE_PIXEL_T1H;
    }
    cc->bits = (uint8_t)CONFIG_NETCORE_PIXEL_DITHER_BITS;
    cc->dither_mode = (uint8_t)CONFIG_NETCORE_PIXEL_DITHER;
    cc->zone_px = (uint16_t)CONFIG_NETCORE_PIXEL_ZONE_PX;
    cc->px = px;
    cc->bright = 255;
}

#if CONFIG_NETCORE_PIXEL_I80
// 舊 sdkconfig 未有新符號時 fallback 8-bit（唔強制要 --fresh 才 build 得到）。
#ifndef CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH
#define CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH 8
#endif

// I80 的腳位字串：D0..D(width-1)、WR、DC（width=8 → 10 個；16 → 18 個）。
static bool parse_i80_pins(int out[18], int width)
{
    if (width != 8 && width != 16) {
        return false;
    }
    const char *s = CONFIG_NETCORE_PIXEL_I80_PINS;
    for (int i = 0; i < width + 2; i++) {
        char *end = NULL;
        const long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 48) {
            ESP_LOGE(TAG, "I80 腳位字串第 %d 個唔合法（剩返 \"%s\"）", i + 1, s);
            return false;
        }
        out[i] = (int)v;
        s = end;
        while (*s == ' ' || *s == ',') {
            s++;
        }
    }
    return true;
}

// I80 的腳位來源：**hardware 層**（driver.json 只寫 bus index）。
//   · 有 driver.json 的 LED 段 → nc_cfg_hardware()->i80[led->bus]（wiring.json 的預設，
//     runtime 可以由 /data/config/hardware.json 覆蓋 —— 唔使重 build）
//   · 冇 → Kconfig 字串（未遷移的板子，行為不變）
// 回 false = 這道 i80 lane 唔應該起。
static bool i80_src(int out[18], int *width_out, int *lanes_out)
{
    const nc_led_cfg_t *led = led_active();
    if (led != NULL) {
        if (led->backend != NC_LED_BACKEND_I80) {
            return false;
        }
        const nc_hardware_cfg_t *hw = nc_cfg_hardware();
        if (hw == NULL || led->bus < 0 || led->bus >= (int)hw->n_i80) {
            ESP_LOGE(TAG, "LED.bus=%d 指唔到 hardware 的 I80（這塊板有 %d 條）→ 這道 lane 停用",
                     (int)led->bus, (hw != NULL) ? (int)hw->n_i80 : 0);
            return false;
        }
        const nc_i80_cfg_t *b = &hw->i80[led->bus];
        if (!b->enable) {
            ESP_LOGE(TAG, "hardware 的 I80[%d] 是 enable=false → 這道 lane 停用", (int)led->bus);
            return false;
        }
        const int w = (b->bus_width == 16) ? 16 : 8;
        if (b->n_data < w || b->wr < 0 || b->dc < 0) {
            ESP_LOGE(TAG, "I80[%d] 腳位不完整（n_data=%u wr=%d dc=%d）→ 這道 lane 停用",
                     (int)led->bus, (unsigned)b->n_data, (int)b->wr, (int)b->dc);
            return false;
        }
        for (int i = 0; i < w; i++) {
            out[i] = b->data[i];
        }
        out[w] = b->wr;
        out[w + 1] = b->dc;
        int lanes = (int)led->lanes;
        if (lanes < 1) {
            lanes = 1;
        }
        if (lanes > w) {
            lanes = w;
        }
        *width_out = w;
        *lanes_out = lanes;
        return true;
    }
    const int w = (CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH == 16) ? 16 : 8;
    if (!parse_i80_pins(out, w)) {
        return false;
    }
    *width_out = w;
    *lanes_out = CONFIG_NETCORE_PIXEL_I80_LANES;
    return true;
}
#endif  // CONFIG_NETCORE_PIXEL_I80

static esp_err_t setup_backends(void)
{
    const int n = plan_lanes();
    for (int i = 0; i < n; i++) {
        const px_plan_t *pl = &s_plan[i];
        nc_px_codec_cfg_t cc;
        codec_cfg_of(&cc, pl->px, pl->be);
#if CONFIG_NETCORE_PIXEL_I80
        if (strcmp(pl->be, "i80") == 0) {
            static int s_i80_pins[18];
            int width = 8;
            int lanes = 1;
            if (!i80_src(s_i80_pins, &width, &lanes)) {
                ESP_LOGE(TAG, "I80 腳位設定壞了 → 這道 lane 停用");
                continue;
            }
            nc_px_i80_cfg_t ic = {
                .pins = s_i80_pins,
                .lanes = (uint8_t)lanes,
                .bus_width = (uint8_t)width,
                .pclk_hz = cc.wave.clk_hz,
                .px = pl->px,
                .fmt = cc.fmt,
                .wave = cc.wave,
                .dither_bits = cc.bits,
                .dither_mode = cc.dither_mode,
                .zone_px = (uint8_t)cc.zone_px,
                .slots = (uint8_t)pl->slots,
            };
            nc_pixel_backend_t *be = nc_px_backend_i80(&ic);
            if (be == NULL) {
                ESP_LOGE(TAG, "i80 後端建立失敗（腳位被佔用或沒開）→ 這道 lane 停用");
            } else if (nc_px_backend_register(be) != ESP_OK) {
                ESP_LOGE(TAG, "i80 後端登記失敗 → 這道 lane 停用");
            }
            continue;
        }
#endif
#if CONFIG_NETCORE_PIXEL_SPI
        if (strcmp(pl->be, "spi") == 0) {
            int mosi = CONFIG_NETCORE_PIXEL_SPI_MOSI;
            int sclk = CONFIG_NETCORE_PIXEL_SPI_SCLK;
            uint32_t clk_hz = CONFIG_NETCORE_PIXEL_SPI_HZ;
            const nc_led_cfg_t *led = led_active();
            if (led != NULL) {
                // 腳位由 hardware 層的 SPI[] 來（同 I80 一樣，runtime 可覆蓋）。
                const nc_hardware_cfg_t *hw = nc_cfg_hardware();
                if (led->backend != NC_LED_BACKEND_SPI) {
                    continue;
                }
                if (hw == NULL || led->bus < 0 || led->bus >= (int)hw->n_spi) {
                    ESP_LOGE(TAG, "LED.bus=%d 指唔到 hardware 的 SPI（有 %d 條）→ 停用",
                             (int)led->bus, (hw != NULL) ? (int)hw->n_spi : 0);
                    continue;
                }
                const nc_spi_cfg_t *b = &hw->spi[led->bus];
                if (!b->enable || b->mosi < 0 || b->sck < 0) {
                    ESP_LOGE(TAG, "hardware 的 SPI[%d] 未接（mosi=%d sck=%d）→ 停用",
                             (int)led->bus, (int)b->mosi, (int)b->sck);
                    continue;
                }
                mosi = b->mosi;
                sclk = b->sck;
                if (led->clk_hz > 0) {
                    clk_hz = led->clk_hz;
                } else if (b->baud > 0) {
                    clk_hz = b->baud;
                }
            }
            nc_px_spi_cfg_t sc = {
                .pin_mosi = mosi,
                .pin_sclk = sclk,
                .host = -1,
                .clock_hz = clk_hz,
                .px = pl->px,
                .fmt = cc.fmt,
                .dither_bits = cc.bits,
                .dither_mode = cc.dither_mode,
                .zone_px = (uint8_t)cc.zone_px,
                .bright = 255,
                .slots = (uint8_t)pl->slots,
            };
            nc_pixel_backend_t *be = nc_px_backend_spi(&sc);
            if (be == NULL) {
                ESP_LOGE(TAG, "spi 後端建立失敗 → 這道 lane 停用");
            } else if (nc_px_backend_register(be) != ESP_OK) {
                ESP_LOGE(TAG, "spi 後端登記失敗 → 這道 lane 停用");
            }
            continue;
        }
#endif
        if (strcmp(pl->be, "direct") == 0) {
            nc_px_direct_bind(&cc);
            (void)nc_px_backend_register(nc_px_backend_direct());
            continue;
        }
        if (strcmp(pl->be, "mock") == 0) {
            (void)nc_px_backend_register(nc_px_backend_mock());
            continue;
        }
        // 走到這裡 = 呢個後端冇 firmware 支援（driver.json 寫了 rmt／parlio 但未實作，
        // 或者 Kconfig 冇開 i80／spi）。寧願大聲講，唔好靜靜退到 mock。
        ESP_LOGE(TAG, "後端 \"%s\" 未有 firmware 支援 → 這道 lane 停用"
                 "（driver.json 的 backend 要對得上實際 driver）", pl->be);
    }
    return (s_nbe > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ═══════════════════════ 執行期狀態 ═══════════════════════
typedef struct {
    nc_pixel_backend_t *be;
    nc_pixel_q_t       *q;
    uint32_t            last_seq;
} px_lane_t;

// wave → push 的「已編碼、等提交」隊列。每項只有索引，唔搬任何像素資料。
typedef struct {
    uint8_t  lane;
    uint32_t slot;
    uint32_t seq;
} px_ready_t;

static struct {
    bool         started;        // px_start 完成（wave 見到 false 就等）
    uint32_t     period_us;      // 由 system.json frame_interval_ms 換算
    uint16_t     fps;            // 1000 / frame_interval_ms（顯示用）
    px_lane_t    lane[NC_PX_MAX_LANES];
    int          nlane;
    nc_px_stat_t st;
    uint32_t     mark_frames;
    int64_t      mark_us;
    SemaphoreHandle_t tick;      // 節拍：esp_timer 週期性給（唔 busy-wait）
    esp_timer_handle_t timer;
    QueueHandle_t ready;         // wave → push（已編碼、等提交）；冇 queue 就唔畫波
} s_px;
static nc_fps_clock_t s_clock;

// esp_timer 回呼（esp_timer 任務 context，不是 ISR）→ 給一次節拍。
static void render_tick(void *arg)
{
    (void)arg;
    if (s_px.tick != NULL) {
        xSemaphoreGive(s_px.tick);
    }
}

void nc_render_get_stats(nc_px_stat_t *out)
{
    if (out != NULL) {
        *out = s_px.st;
    }
}

static esp_err_t bind_lanes(void)
{
    s_px.nlane = 0;
    for (int i = 0; i < s_nplan; i++) {
        nc_pixel_backend_t *be = nc_px_backend_find(s_plan[i].be);
        nc_pixel_q_t *q = nc_px_ring_find(s_plan[i].ring);
        if (be == NULL || q == NULL) {
            ESP_LOGW(TAG, "lane %d 停用：後端 %s %s、環 %s %s", i, s_plan[i].be,
                     (be != NULL) ? "有" : "**起唔到**", s_plan[i].ring,
                     (q != NULL) ? "有登記" : "**未登記**");
            continue;
        }
        be->ring = q;                // 後端要知道讀哪條環（i80／spi 的 acquire 用）
        // mock 後端有自己的 q 指標（它不經 be->ring）；唔 bind 就永遠 acquire 失敗。
        if (strcmp(be->name, "mock") == 0) {
            nc_px_backend_mock_bind(be, q);
        }
        s_px.lane[s_px.nlane].be = be;
        s_px.lane[s_px.nlane].q = q;
        s_px.lane[s_px.nlane].last_seq = 0;
        s_px.nlane++;
    }
    return (s_px.nlane > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ── push 段：還格（DMA 已完成）＋ submit。 ──
// 為什麼 reclaim 在 push 而唔在 wave：reclaim 會 release 環格，只有 DMA 真正
// 完成才准還（P6 陷阱 1）。放喺 push 就同 submit 同一個 context，順序明確。
static void reclaim_all(void)
{
    for (int i = 0; i < s_px.nlane; i++) {
        px_lane_t *ln = &s_px.lane[i];
        if (ln->be != NULL) {
            ln->be->reclaim(ln->be);
        }
    }
}

// 一道 lane 畫波：acquire（環最新一格）→ encode（寫後端的 DMA 槽）→ 入隊。
// 唔 submit、唔 reclaim：呢兩件事係 render_task 的工作。
static void lane_encode(int idx)
{
    px_lane_t *ln = &s_px.lane[idx];
    if (ln->be == NULL || ln->q == NULL) {
        return;
    }
    // 先確認 queue 有位才 acquire：encode 之後 queue 滿就唔可以 release 環格
    // （後端的 DMA 槽仍然記住這個 seq，release 會令槽永遠洩漏 → 管線停頓）。
    // wave 是唯一生產者、push 只會清空 queue，所以 check 之後一定有位。
    if (uxQueueSpacesAvailable(s_px.ready) == 0) {
        s_px.st.starved++;
        return;
    }

    uint32_t slot = 0;
    const uint8_t *payload = NULL;
    uint32_t px = 0;
    uint32_t seq = 0;
    if (!ln->be->acquire(ln->be, ln->last_seq, &slot, &payload, &px, &seq)) {
        s_px.st.starved++;             // 沒有新格／全部在飛 → 下一輪再試
        return;
    }
    s_px.st.fresh++;
    if (ln->last_seq != 0 && seq > ln->last_seq + 1u) {
        s_px.st.skipped += seq - ln->last_seq - 1u;   // 假消費：中間的格不補播
    }
    ln->last_seq = seq;

    const int64_t t0 = esp_timer_get_time();
    if (!ln->be->encode(ln->be, slot)) {
        s_px.st.fail++;
        if (payload != NULL) {
            nc_px_ring_release(ln->q, seq);   // 編不了就立刻還格（不可以洩漏）
        }
        return;
    }
    s_px.st.enc_us = (uint32_t)(esp_timer_get_time() - t0);

    const px_ready_t r = { .lane = (uint8_t)idx, .slot = slot, .seq = seq };
    if (xQueueSend(s_px.ready, &r, 0) != pdTRUE) {
        // 理論上唔會（queue 位先 check）；真係撞到就直接 submit，唔可以 release
        // 環格之外仲留住後端的 DMA 槽（會永久洩漏）。明示計數，唔靜默丟。
        if (!ln->be->submit(ln->be, slot, seq)) {
            nc_px_ring_release(ln->q, seq);
            s_px.st.fail++;
        } else {
            s_px.st.frames++;
        }
    }
}

// 一道 lane 推送：submit 已編碼的格；失敗要還環格（後端 submit 失敗會自己清槽）。
static void lane_submit(const px_ready_t *r)
{
    if (r->lane >= (uint8_t)s_px.nlane) {
        return;
    }
    px_lane_t *ln = &s_px.lane[r->lane];
    if (ln->be == NULL || ln->q == NULL) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    if (!ln->be->submit(ln->be, r->slot, r->seq)) {
        nc_px_ring_release(ln->q, r->seq);
        s_px.st.fail++;
        return;
    }
    s_px.st.tx_us = (uint32_t)(esp_timer_get_time() - t0);
    s_px.st.frames++;
}

const char *nc_render_first_ring(void)
{
    if (s_px.nlane <= 0 || s_px.lane[0].q == NULL) {
        return NULL;
    }
    return nc_px_ring_name(s_px.lane[0].q);
}

// ── FPS 穩定度：逐輪 dt 的統計（0xF081）──
typedef struct {
    uint32_t n;
    uint64_t sum;          // Σ dt
    uint64_t sumsq;        // Σ dt²
    uint32_t min_us;
    uint32_t max_us;
    uint32_t bad;          // |dt - target| > target/5
    uint32_t hist[64];     // 0..62 ms，63 = ≥63 ms
} fps_jit_t;

static fps_jit_t s_jit;

static void jit_record(uint32_t dt, uint32_t target_us)
{
    s_jit.n++;
    s_jit.sum += dt;
    s_jit.sumsq += (uint64_t)dt * dt;
    if (s_jit.min_us == 0 || dt < s_jit.min_us) {
        s_jit.min_us = dt;
    }
    if (dt > s_jit.max_us) {
        s_jit.max_us = dt;
    }
    if (target_us > 0) {
        const uint32_t diff = (dt > target_us) ? (dt - target_us) : (target_us - dt);
        if (diff > target_us / 5u) {
            s_jit.bad++;
        }
    }
    uint32_t b = dt / 1000u;
    if (b > 63u) {
        b = 63u;
    }
    s_jit.hist[b]++;
}

void nc_render_fps_reset(void)
{
    memset(&s_jit, 0, sizeof(s_jit));
}

size_t nc_render_fps_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const uint32_t n = s_jit.n;
    const uint32_t avg = (n > 0) ? (uint32_t)(s_jit.sum / n) : 0;
    uint32_t sd = 0;
    if (n > 0) {
        const uint64_t mean = s_jit.sum / n;
        const uint64_t var = (s_jit.sumsq / n) - mean * mean;
        sd = (uint32_t)sqrt((double)((var > 0) ? var : 0));
    }
    int k = snprintf(out, cap,
                     "{\"target_us\":%u,\"n\":%u,\"avg_us\":%u,\"sd_us\":%u,"
                     "\"min_us\":%u,\"max_us\":%u,\"bad\":%u,"
                     "\"starved\":%lu,\"late\":%lu,\"frames\":%lu,"
                     "\"slack_us\":%lld,\"catchup\":%lu,\"overrun\":%lu,"
                     "\"sum_us\":%llu,\"hist\":[",
                     (unsigned)s_px.period_us, (unsigned)n, (unsigned)avg, (unsigned)sd,
                     (unsigned)s_jit.min_us, (unsigned)s_jit.max_us, (unsigned)s_jit.bad,
                     (unsigned long)s_px.st.starved, (unsigned long)s_px.st.late,
                     (unsigned long)s_px.st.frames,
                     (long long)s_clock.slack_us, (unsigned long)s_clock.catchup_n,
                     (unsigned long)s_clock.overrun_n,
                     (unsigned long long)s_jit.sum);
    if (k < 0 || (size_t)k >= cap) {
        return 0;
    }
    for (int i = 0; i < 64; i++) {
        const int w = snprintf(out + k, cap - (size_t)k, "%s%u", (i > 0) ? "," : "",
                               (unsigned)s_jit.hist[i]);
        if (w < 0 || (size_t)(k + w) >= cap) {
            return 0;
        }
        k += w;
    }
    const int w = snprintf(out + k, cap - (size_t)k, "]}");
    return (w > 0 && (size_t)(k + w) < cap) ? (size_t)(k + w) : 0;
}

// ── 畫波：由 pixel_encode_task（pixel_encode_task component）呼叫。 ──
// render_task 係公用的輸出層（LED 現在、LCD 將來），唔屬 pixel 系列；
// 所以畫波任務只拿這三個 API，環／lane／後端／節拍仍然由 render 擁有。
bool nc_render_encode_ready(void)
{
    return s_px.started && s_px.ready != NULL;
}

bool nc_render_encode_wait(uint32_t timeout_ms)
{
    if (!nc_render_encode_ready()) {
        return false;
    }
    // 節拍由**週期 timer** 給；時間緩衝只做統計／吸收，唔改週期。
    if (s_px.tick != NULL) {
        (void)xSemaphoreTake(s_px.tick, pdMS_TO_TICKS(timeout_ms));
    }
    return true;
}

void nc_render_encode_tick(void)
{
    if (!nc_render_encode_ready()) {
        return;
    }
    for (int i = 0; i < s_px.nlane; i++) {
        lane_encode(i);
    }

    static int64_t s_prev;          // 畫波任務的上一輪時間（首次呼叫才初始化）
    const int64_t after = esp_timer_get_time();
    if (s_prev == 0) {
        s_prev = after;
        s_px.mark_us = after;
        s_px.mark_frames = 0;
        return;
    }
    nc_fps_clock_done(&s_clock, after);
    const uint32_t dt = (uint32_t)(after - s_prev);
    s_px.st.frame_us = dt;
    jit_record(dt, s_px.period_us);
    if (dt > s_px.st.frame_max_us) {
        s_px.st.frame_max_us = dt;
    }
    s_prev = after;

    const int64_t span = after - s_px.mark_us;
    if (span >= 1000000) {
        const uint32_t n = s_px.st.frames - s_px.mark_frames;
        s_px.st.fps_x10 = (uint32_t)(((int64_t)n * 10 * 1000000) / span);
        s_px.mark_us = after;
        s_px.mark_frames = s_px.st.frames;
        uint32_t drop = 0;
        for (int i = 0; i < nc_px_ring_count(); i++) {
            nc_px_q_stat_t qs;
            nc_px_ring_stats(nc_px_ring_at(i), &qs);
            drop += qs.drops;
        }
        s_px.st.drop = drop;
    }

    // 節拍由 timer 控制；如果這一輪做超過一個 period，記一次 late。
    if (s_px.period_us > 0) {
        const uint32_t spent = (uint32_t)(esp_timer_get_time() - after);
        if (spent > s_px.period_us) {
            s_px.st.late++;
        }
    }
}

// ── 推送 loop（render_task）：先還完成的 DMA 格，再 submit 畫好的格。
static void px_push_loop(void *arg)
{
    (void)arg;
    while (!nc_task_exit_requested()) {
        if (!s_px.started || s_px.ready == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            nc_task_beat();
            continue;
        }
        reclaim_all();                 // DMA 完成 → release 環格（唔可以提前）
        px_ready_t r;
        if (xQueueReceive(s_px.ready, &r, pdMS_TO_TICKS(10)) == pdTRUE) {
            lane_submit(&r);
        }
        nc_task_beat();
    }
}

size_t nc_render_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap,
                     "{\"started\":%d,\"fps\":%u,\"fps_x10\":%lu,\"frames\":%lu"
                     ",\"fresh\":%lu,\"starved\":%lu,\"late\":%lu"
                     ",\"fail\":%lu,\"skipped\":%lu,\"drop\":%lu,\"frame_us\":%lu"
                     ",\"frame_max_us\":%lu,\"enc_us\":%lu,\"tx_us\":%lu"
                     ",\"lanes\":[",
                     s_px.started ? 1 : 0, (unsigned)s_px.fps, (unsigned long)s_px.st.fps_x10,
                     (unsigned long)s_px.st.frames, (unsigned long)s_px.st.fresh,
                     (unsigned long)s_px.st.starved, (unsigned long)s_px.st.late,
                     (unsigned long)s_px.st.fail, (unsigned long)s_px.st.skipped,
                     (unsigned long)s_px.st.drop, (unsigned long)s_px.st.frame_us,
                     (unsigned long)s_px.st.frame_max_us, (unsigned long)s_px.st.enc_us,
                     (unsigned long)s_px.st.tx_us);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    for (int i = 0; i < s_px.nlane; i++) {
        const int k = snprintf(out + n, cap - (size_t)n,
                               "%s{\"be\":\"%s\",\"lanes\":%u,\"width\":%u,\"ring\":\"%s\""
                               ",\"px\":%lu,\"seq\":%lu,\"fill\":%lu}",
                               (i > 0) ? "," : "", s_px.lane[i].be->name,
                               (unsigned)s_px.lane[i].be->lanes,
                               (unsigned)s_px.lane[i].be->width,
                               nc_px_ring_name(s_px.lane[i].q),
                               (unsigned long)nc_px_ring_px(s_px.lane[i].q),
                               (unsigned long)s_px.lane[i].last_seq,
                               (unsigned long)nc_px_ring_fill(s_px.lane[i].q));
        if (k < 0 || (size_t)k >= cap - (size_t)n) {
            return (size_t)n;
        }
        n += k;
    }
    const int k = snprintf(out + n, cap - (size_t)n, "]}");
    if (k < 0 || (size_t)k >= cap - (size_t)n) {
        return (size_t)n;
    }
    n += k;
    return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

// ═══════════════════════ 任務 ═══════════════════════
static int px_declare(nc_task_res_t *out, int max)
{
    const int n = plan_lanes();
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        out[k++] = (nc_task_res_t){
            .name = s_plan[i].ring,
            .bytes = nc_px_ring_bytes(s_plan[i].px, s_plan[i].slots),
            // 環由 CPU 寫、CPU 讀；DMA 讀的是後端自己的 internal 槽，不是環。
            // 大畫面（9,600 px × 4 × 3 = 115 KB）internal 放唔落 → build-time 政策選 PSRAM。
#if CONFIG_NETCORE_PIXEL_RING_PSRAM
            .cls = NC_PLAN_PSRAM,
#else
            .cls = NC_PLAN_INTERNAL,
#endif
            .align = 64,
            .slot_bytes = nc_px_slot_bytes(s_plan[i].px),
            .n = (uint16_t)s_plan[i].slots,
        };
    }
    return k;
}

static esp_err_t px_start(const nc_task_res_view_t *res, int nres)
{
    const int n = plan_lanes();
    if (n == 0) {
        ESP_LOGE(TAG, "沒有可用後端 → 渲染器不啟動");
        return ESP_ERR_NOT_FOUND;
    }
    if (nres < n) {
        ESP_LOGE(TAG, "只拿到 %d 項資源（要 %d）→ 渲染器不啟動", nres, n);
        return ESP_FAIL;
    }
    for (int i = 0; i < n; i++) {
        if (res[i].ptr == NULL) {
            ESP_LOGE(TAG, "[%s] 沒有拿到環", s_plan[i].ring);
            return ESP_FAIL;
        }
        nc_pixel_q_t *q = nc_px_ring_init(&s_rings[i], &(nc_px_ring_cfg_t){
            .name = s_plan[i].ring,
            .region = res[i].ptr,
            .region_bytes = (uint32_t)res[i].len,
            .px = s_plan[i].px,
            .slots = s_plan[i].slots, });
        if (q == NULL || nc_px_ring_register(q) != 0) {
            return ESP_FAIL;
        }
    }
    if (setup_backends() != ESP_OK) {
        return ESP_FAIL;
    }
    const esp_err_t e = bind_lanes();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "一個後端都綁不到環 → 渲染器不啟動");
        return e;
    }
    // 渲染節拍由 **設定檔** 控制：system.json 的 frame_interval_ms（預設 20 ms = 50 fps）。
    // 呢個係用戶可見、可改的節奏；Kconfig 的 FPS 只在設定拿不到時做後備。
    uint32_t interval_ms = 20;
    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys != NULL && sys->frame_interval_ms > 0) {
        interval_ms = sys->frame_interval_ms;
    }
    s_px.period_us = interval_ms * 1000u;
    s_px.fps = (uint16_t)(1000u / interval_ms);
    memset(&s_px.st, 0, sizeof(s_px.st));
    for (int i = 0; i < s_px.nlane; i++) {
        // 開機時生產者也許已經先寫了一幀：從最新那一格開始（不補播舊幀）
        const nc_px_hdr_t *h = NULL;
        uint32_t seq = 0;
        if (nc_px_ring_peek(s_px.lane[i].q, &h, &seq) != NULL) {
            s_px.lane[i].last_seq = seq;
            nc_px_ring_release(s_px.lane[i].q, seq);
        }
    }
    // 節拍：esp_timer **週期性**，完全按 system.json 的 frame_interval_ms。
    // tick 同 ready queue 係 wave 任務都要用的共享物件：只建立一次，之後重用。
    if (s_px.tick == NULL) {
        s_px.tick = xSemaphoreCreateBinary();
    }
    if (s_px.ready == NULL) {
        s_px.ready = xQueueCreate(NC_PX_MAX_LANES * 2, sizeof(px_ready_t));
    }
    if (s_px.timer != NULL) {
        (void)esp_timer_stop(s_px.timer);
        (void)esp_timer_delete(s_px.timer);
        s_px.timer = NULL;
    }
    const esp_timer_create_args_t targs = {
        .callback = render_tick,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pixel_tick",
    };
    if (s_px.tick == NULL || s_px.ready == NULL ||
        esp_timer_create(&targs, &s_px.timer) != ESP_OK ||
        esp_timer_start_periodic(s_px.timer, s_px.period_us) != ESP_OK) {
        ESP_LOGE(TAG, "節拍 timer／queue 建立失敗（%u µs）", (unsigned)s_px.period_us);
        return ESP_FAIL;
    }
    xQueueReset(s_px.ready);           // 重啟時清走上一輪未送的格索引
    nc_fps_clock_init(&s_clock, esp_timer_get_time(), (int64_t)s_px.period_us,
                      (int64_t)CONFIG_NETCORE_PIXEL_SLACK_MS * 1000);
    s_px.started = true;
    ESP_LOGI(TAG, "輸出層就緒：%d 道 lane、%u fps（frame_interval_ms=%u）；lane0 = %s（環 %s）",
             s_px.nlane, (unsigned)s_px.fps, (unsigned)interval_ms, s_px.lane[0].be->name,
             nc_px_ring_name(s_px.lane[0].q));
    return ESP_OK;
}

static void px_stop(void)
{
    s_px.started = false;
    // 唔 delete tick／ready：pixel_encode_task 係另一個 component 的任務，可能仍然等緊；
    // destroy 會 use-after-free。停止時只停 timer，物件留到重開機（一次性小 leak）。
    if (s_px.timer != NULL) {
        (void)esp_timer_stop(s_px.timer);
        (void)esp_timer_delete(s_px.timer);
        s_px.timer = NULL;
    }
}

static const nc_task_loop_t s_push_loops[] = {
    {
        // 推送：DMA 完成 → 還格；畫好的格 → submit。唔使等畫波節拍。
        .name = "push",
        .fn = px_push_loop,
        .stack = 3072,                // 只 call submit／reclaim，唔使畫波咁大
        .prio = NC_PRIO_OTHERS_MAX,
        .core = CONFIG_NETCORE_PIXEL_CORE,
        .layer = 5,
        .period_ms = 0,
    },
};

// 第四層：公用輸出／渲染（LED 現在、LCD 將來；唔屬 pixel 系列）。
// 環、lane、後端、節拍都係它擁有（order 10 先 on_start，pixel_encode_task 同
// pixel_pair_task 之後才找得到環）。真正的 submit 在 push loop。
static const nc_task_t s_task = {
    .name = "render",
    .kind = NC_TASK_NORMAL,
    .level = 4,
    .order = 10,
    .declare = px_declare,
    .on_start = px_start,
    .on_stop = px_stop,
    .loops = s_push_loops,
    .nloops = 1,
};

NC_TASK_AUTO_REG(render, &s_task);

const nc_task_t *nc_render_task(void)
{
    return &s_task;
}

#else   // !CONFIG_NETCORE_PIXEL

// 這塊板沒開 pixel：main.c 不會登記，呼叫端只會拿到 NULL。
const nc_task_t *nc_render_task(void)
{
    return NULL;
}

void nc_render_get_stats(nc_px_stat_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

size_t nc_render_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    const int n = snprintf(out, cap, "{\"started\":0,\"err\":\"disabled\"}");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

#endif  // CONFIG_NETCORE_PIXEL
