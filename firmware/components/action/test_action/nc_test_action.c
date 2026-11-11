// 測試指令（dev bench / hub bench / tx bench / bus stats / net tune / bench 域）。
// 由 main.c 搬出：正式映像唔編譯（NC_ENABLE_TESTS=OFF 時 component 唔存在）。
#include "sdkconfig.h"
#include "nc_test_action.h"
#include "nc_action.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_cache.h"
#include "soc/soc_caps.h"
#if CONFIG_NETCORE_WIFI
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#endif
#include "esp_heap_caps.h"
#if SOC_LCD_I80_SUPPORTED && !CONFIG_IDF_TARGET_ESP32P4
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include <fcntl.h>

// ESP-NOW 要有**真實 radio**：P4 的 Wi-Fi 在外掛協處理器（esp_hosted），
// host 側冇 esp_now 符號。所以用 SOC_WIFI_SUPPORTED，唔可以只看 CONFIG_NETCORE_WIFI。
#if CONFIG_NETCORE_WIFI && SOC_WIFI_SUPPORTED
#define NC_HAVE_ESPNOW 1
#else
#define NC_HAVE_ESPNOW 0
#endif

#include "nc4_bus.h"
#include "nc4.h"
#include "nc4_link.h"
#include "nc_hw.h"
#include "nc_hw_gpio.h"
#include "nc_pwm_drv.h"
#include "nc_uart_drv.h"
#include "nc_buf.h"
#include "nc_hub.h"

static const char *TAG = "test_action";

#define NC4_REPLY_JSON_MAX 2600
#define NC4_CMD_DEV_BENCH  0xF001
#define NC4_CMD_DEV_RSP    0xF002
#define NC4_CMD_HUB_BENCH  0xF003
#define NC4_CMD_HUB_RSP    0xF004
#define NC4_CMD_NET_TUNE   0xF00A
#define NC4_CMD_BUS_STATS  0xF008
#define NC4_CMD_BUS_RSP    0xF009
#define NC4_CMD_TX_BENCH   0xF007
#define NC4_CMD_BENCH_READY   0x1811
#define NC4_CMD_BENCH_DATA    0x1812
#define NC4_CMD_BENCH_RESULT  0x1813
#define NC4_CMD_BENCH_REPORT  0x1814

static nc_test_action_hooks_t s_hooks;

// 回覆幀由 **decoder** 持有（PSRAM），唔再放任務棧。
// 之前這裡是 `uint8_t frame[2,615]` 的 by-value struct：每個測試指令的回覆
// 都要在 decode 任務的 4 KB 棧上佔 2.6 KB。實測代價：
//   decode0 的 HWM 由 2,580 → 364（單一條 0xF00C），洪水時只剩 124 B。
// 7.19 已經把 main.c 的回覆幀搬到 decoder，這裡是漏掉的一處。
typedef struct {
    nc4_ctx_t *ctx;
    uint16_t cmd;
    uint8_t *frame;      // → nc4_decoder_reply_buf()，不在棧上
} nc4_reply_t;

static uint32_t s_bench_ok;

// 回傳可寫的 JSON 區（容量 NC4_REPLY_JSON_MAX + 1，已清空）
static char *reply_open(nc4_reply_t *r, nc4_ctx_t *ctx, uint16_t cmd)
{
    r->ctx = ctx;
    r->cmd = cmd;
    r->frame = nc4_decoder_reply_buf(ctx->decoder);
    if (r->frame == NULL) {
        return NULL;      // decoder 建立時已配置好，正常唔會發生
    }
    char *json = (char *)r->frame + NC4_HDR_LEN + 2;
    json[0] = '\0';
    return json;
}

static void reply_send(nc4_reply_t *r, size_t len)
{
    if (r->ctx == NULL || r->ctx->send == NULL || r->frame == NULL) {
        return;
    }
    if (len > NC4_REPLY_JSON_MAX) {
        // 與 main.c 同一條規則：超限回合法錯誤物件，唔好靜默截斷（見 tx-path-design §7）。
        const size_t need = len;
        char *json = (char *)r->frame + NC4_HDR_LEN + 2;
        len = (size_t)snprintf(json, NC4_REPLY_JSON_MAX + 1,
                               "{\"err\":\"reply_too_big\",\"need\":%u,\"cap\":%u}",
                               (unsigned)need, (unsigned)NC4_REPLY_JSON_MAX);
        ESP_LOGW(TAG, "[%s] 回覆超限（cmd 0x%04X）：需要 %u B > 上限 %u → 回錯誤物件",
                 r->ctx->source, (unsigned)r->cmd, (unsigned)need,
                 (unsigned)NC4_REPLY_JSON_MAX);
    }
    uint8_t *data = r->frame + NC4_HDR_LEN;   // DATA 區：2B 長度前綴 + JSON
    data[0] = (uint8_t)len;
    data[1] = (uint8_t)(len >> 8);
    // 用 NC4_REPLY_FRAME_MAX，唔可以用 sizeof(r->frame)：frame 由陣列改成指標之後
    // sizeof 只剩 4/8 B → pack_into 直接回 -1 → 回覆靜靜消失（實測：0xF00C/0xF00E 全部冇回）。
    const int total = nc4_pack_into(r->frame, NC4_REPLY_FRAME_MAX, r->cmd,
                                    nc4_reply_addr(r->ctx), data, 2 + len);
    if (total > 0) {
        r->ctx->send(r->ctx->send_arg, r->frame, (size_t)total);
    }
}

static void reply_json(nc4_ctx_t *ctx, uint16_t cmd, const char *json)
{
    if (ctx->send == NULL) {
        return;
    }
    nc4_reply_t r;
    char *dst = reply_open(&r, ctx, cmd);
    if (dst == NULL) {
        return;
    }
    size_t n = strlen(json);
    if (n > NC4_REPLY_JSON_MAX) {
        reply_send(&r, n);      // 由 reply_send 換成合法錯誤物件（唔截斷、唔越界）
        return;
    }
    memcpy(dst, json, n);
    reply_send(&r, n);
}

static void jappend(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    if (*off >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *off += (size_t)n;
        if (*off >= cap) {
            *off = cap - 1;
        }
    }
}

typedef size_t (*nc_json_fn)(char *buf, size_t cap);

static void jchain(char *buf, size_t cap, size_t *off, nc_json_fn fn)
{
    if (*off >= cap) {
        return;
    }
    *off += fn(buf + *off, cap - *off);
    if (*off >= cap) {
        *off = cap - 1;
    }
}


static void bench_report(nc4_ctx_t *ctx)
{
    uint8_t p[4] = {
        (uint8_t)s_bench_ok, (uint8_t)(s_bench_ok >> 8),
        (uint8_t)(s_bench_ok >> 16), (uint8_t)(s_bench_ok >> 24),
    };
    if (ctx->send != NULL) {
        uint8_t frame[NC4_HDR_LEN + 4 + NC4_CRC_LEN];
        int total = nc4_pack_into(frame, sizeof(frame), NC4_CMD_BENCH_REPORT,
                                  nc4_reply_addr(ctx), p, sizeof(p));
        if (total > 0) {
            ctx->send(ctx->send_arg, frame, (size_t)total);
        }
    }
}

static void on_bench_ready(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    s_bench_ok = 0;
    bench_report(ctx);
}

static void on_bench_data(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)ctx;
    (void)f;
    s_bench_ok++;   // CRC 已在 parser 驗過；到這裡就是「收到一包合法資料」
}

static void on_bench_result(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    bench_report(ctx);
    s_bench_ok = 0;
}

// ── 0xF001 專屬測試：解碼 / 組包（純 CPU）──
static void on_dev_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t size = 8192, iters = 100;
    uint8_t mode = 0;
    if (f->len >= 4) {
        size = (uint16_t)(f->data[0] | (f->data[1] << 8));
        iters = (uint16_t)(f->data[2] | (f->data[3] << 8));
    }
    if (f->len >= 5) {
        mode = f->data[4];
    }
    if (size < 16) {
        size = 16;
    }
    if (size > NC4_MAX_PAYLOAD) {
        size = NC4_MAX_PAYLOAD;
    }
    if (iters == 0) {
        iters = 1;
    }
    // 舊工具可能還在送 mode 2/3/4；明確報錯，不要靜默當成 decode 回一份假數字
    if (mode > 1) {
        reply_json(ctx, NC4_CMD_DEV_RSP, "{\"err\":\"mode\"}");
        return;
    }

    const size_t total = NC4_HDR_LEN + (size_t)size + NC4_CRC_LEN;
    uint8_t *frame = malloc(total);
    uint8_t *payload = malloc(size);
    nc4_decoder_t *dec = nc4_decoder_create();
    nc4_parser_t *parser = NULL;
    if (frame == NULL || payload == NULL || dec == NULL) {
        free(frame);
        free(payload);
        nc4_decoder_destroy(dec);
        reply_json(ctx, NC4_CMD_DEV_RSP, "{\"err\":\"oom\"}");
        return;
    }
    memset(payload, 0xA5, size);

    int64_t t0 = 0, dt = 0;
    char json[192];

    if (mode == 1) {
        nc4_pack_into(frame, total, 0x7FFE, NC4_ADDR_BROADCAST, payload, size);
        t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < iters; i++) {
            nc4_pack_into(frame, total, 0x7FFE, NC4_ADDR_BROADCAST, payload, size);
        }
    } else {
        parser = nc4_parser_create(dec, NULL, NULL, "DEV", NC4_MAX_FRAME);
        if (parser == NULL) {
            free(payload);
            free(frame);
            nc4_decoder_destroy(dec);
            reply_json(ctx, NC4_CMD_DEV_RSP, "{\"err\":\"parser\"}");
            return;
        }
        nc4_pack_into(frame, total, 0x7FFE, NC4_ADDR_BROADCAST, payload, size);
        nc4_parser_feed(parser, frame, total);
        t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < iters; i++) {
            nc4_parser_feed(parser, frame, total);
        }
    }
    dt = esp_timer_get_time() - t0;

    const double mbps = (dt > 0) ? ((double)total * (double)iters / (double)dt) : 0.0;
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"size\":%u,\"iters\":%u,"
             "\"us_per_frame\":%.1f,\"mb_s\":%.2f}",
             mode == 1 ? "pack" : "decode",
             (unsigned)size, (unsigned)iters, (double)dt / (double)iters, mbps);
    reply_json(ctx, NC4_CMD_DEV_RSP, json);

    nc4_parser_destroy(parser);
    nc4_decoder_destroy(dec);
    free(payload);
    free(frame);
}

// ── 0xF003 雙核 hub 交換量測 ──
// 結束原因（回報用）：看出「read=0」到底是哪一邊沒動
#define HUB_EXIT_NONE    0
#define HUB_EXIT_DONE    1
#define HUB_EXIT_EXPIRED 2
#define HUB_EXIT_NO_START 3

typedef struct {
    nc_hub_t *hub;
    uint32_t iters;
    volatile bool start;        // 閘門：handler 記完 t0 才放行
    volatile int64_t deadline_us;   // 硬性截止：任何等待都不准超過這個時間
    volatile bool writer_done;
    volatile uint32_t torn;
    volatile uint32_t read_count;
    // 診斷計數：read=0 時要能分辨「沒排到」「沒空位」「根本沒跑」
    volatile uint32_t w_commit;     // writer 成功 commit 的次數
    volatile uint32_t w_full;       // write_begin 拿不到空位的次數
    volatile uint32_t r_null;       // read_begin 拿不到 READY 的次數
    volatile uint8_t w_exit;
    volatile uint8_t r_exit;
} hub_bench_t;

// 先自旋一小段（量真實速度），太久才讓一次（餵 watchdog、避免餓死 IDLE）
static void hub_bench_backoff(uint32_t *spins)
{
    if (++(*spins) < 400) {
        return;
    }
    *spins = 0;
    vTaskDelay(1);
}

static bool hub_bench_expired(const hub_bench_t *b)
{
    return esp_timer_get_time() > b->deadline_us;
}

static void hub_bench_writer(void *arg)
{
    hub_bench_t *b = arg;
    while (!b->start) {
        if (hub_bench_expired(b)) {
            b->w_exit = HUB_EXIT_NO_START;   // 這條就是 P4 版 read=0 的指紋
            b->writer_done = true;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(1);          // 不用 taskYIELD：要讓 IDLE / watchdog 跑得到
    }
    uint32_t spins = 0;
    for (uint32_t i = 1; i <= b->iters; i++) {
        uint8_t *p = NULL;
        while ((p = nc_hub_write_begin(b->hub)) == NULL) {
            b->w_full++;
            if (hub_bench_expired(b)) {
                b->w_exit = HUB_EXIT_EXPIRED;
                b->writer_done = true;
                vTaskDelete(NULL);
                return;
            }
            hub_bench_backoff(&spins);
        }
        memcpy(p, &i, sizeof(i));
        memset(p + sizeof(i), (uint8_t)i, nc_hub_slot_size(b->hub) - sizeof(i));
        nc_hub_write_commit(b->hub);
        b->w_commit++;
    }
    b->w_exit = HUB_EXIT_DONE;
    b->writer_done = true;
    vTaskDelete(NULL);
}

static void hub_bench_reader(void *arg)
{
    hub_bench_t *b = arg;
    while (!b->start) {
        if (hub_bench_expired(b)) {
            b->r_exit = HUB_EXIT_NO_START;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(1);
    }
    uint32_t spins = 0;
    for (;;) {
        uint8_t *p = nc_hub_read_begin(b->hub);
        if (p == NULL) {
            b->r_null++;
            if (b->writer_done && !nc_hub_dirty(b->hub)) {
                b->r_exit = HUB_EXIT_DONE;
                break;
            }
            if (hub_bench_expired(b)) {
                b->r_exit = HUB_EXIT_EXPIRED;
                break;
            }
            hub_bench_backoff(&spins);
            continue;
        }
        uint32_t v = 0;
        memcpy(&v, p, sizeof(v));
        const size_t sz = nc_hub_slot_size(b->hub);
        for (size_t k = sizeof(v); k < sz; k++) {
            if (p[k] != (uint8_t)v) {
                b->torn++;
                break;
            }
        }
        nc_hub_read_end(b->hub);
        b->read_count++;
    }
    vTaskDelete(NULL);
}

static void on_hub_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint32_t iters = 20000;
    if (f->len >= 4) {
        iters = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (iters < 1000) {
        iters = 1000;
    }
    if (iters > 200000) {
        iters = 200000;
    }

    // hub 建一次就好：每次重配會漏 1 KB，而且 registry 只有 8 格
    static nc_hub_t *s_bench_hub;
    if (s_bench_hub == NULL) {
        s_bench_hub = nc_hub_create("bench", 64, 16, NC_HUB_SRAM, 32);
    }
    if (s_bench_hub == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"hub\"}");
        return;
    }
    // 上一輪若在逾時處收手，slot 可能停在 READY：先清乾淨再量
    while (nc_hub_read_begin(s_bench_hub) != NULL) {
        nc_hub_read_end(s_bench_hub);
    }

    static hub_bench_t b;
    memset((void *)&b, 0, sizeof(b));
    b.iters = iters;
    b.hub = s_bench_hub;
    // mode bit0 = 舊版順序（先開 task 才寫 deadline）：拿來重現 read=0 的競爭
    const uint8_t mode = (f->len >= 5) ? f->data[4] : 0;

    // ★ 期限必須在「開 task 之前」就寫好：
    //   新 task 可能立刻在另一個核心跑起來（P4 就是這樣），
    //   若此時 deadline_us 還是 0，它會判定「已逾時」直接結束，
    //   於是 writer 一格都沒寫、reader 空轉 3 秒 → read=0。
    if (!(mode & 1)) {
        b.deadline_us = esp_timer_get_time() + 3 * 1000 * 1000;
    }

    TaskHandle_t wr = NULL, rd = NULL;
    xTaskCreatePinnedToCore(hub_bench_writer, "hub_w", 4096, &b, 1, &wr, 1);
    xTaskCreatePinnedToCore(hub_bench_reader, "hub_r", 4096, &b, 1, &rd, 0);
    if (wr == NULL || rd == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"task\"}");
        return;
    }
    if (mode & 1) {
        b.deadline_us = esp_timer_get_time() + 3 * 1000 * 1000;   // 重現用：太晚
    }

    const int64_t t0 = esp_timer_get_time();
    b.start = true;                 // 放行，開始計時
    while (b.read_count < iters) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (esp_timer_get_time() - t0 > 3 * 1000 * 1000) {
            break;
        }
        if (b.writer_done && !nc_hub_dirty(s_bench_hub) &&
            b.r_exit != HUB_EXIT_NONE) {
            break;                  // 兩邊都收手了，不用等滿 3 秒
        }
    }
    const int64_t dt = esp_timer_get_time() - t0;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"iters\":%u,\"read\":%u,\"us\":%lld,\"per_sec\":%.0f,\"torn\":%u,"
             "\"w\":%u,\"w_full\":%u,\"r_null\":%u,\"exit\":[%u,%u],\"late\":%u}",
             (unsigned)iters, (unsigned)b.read_count, (long long)dt,
             dt > 0 ? (double)b.read_count * 1e6 / (double)dt : 0.0,
             (unsigned)b.torn, (unsigned)b.w_commit,
             (unsigned)b.w_full, (unsigned)b.r_null,
             (unsigned)b.w_exit, (unsigned)b.r_exit, (unsigned)(mode & 1));
    ESP_LOGW(TAG, "HUB_BENCH %s", json);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF00B / 0xF00C：UART 迴路自測（IO7↔IO8 短接）──
// 唔喺 handler 裡面等回覆：handler 揸住 decoder mutex，等 decode1 處理就係自己等自己。
#define NC4_CMD_UART_LOOP  0xF00B
#define NC4_CMD_LOOP_STAT  0xF00C
#define NC4_CMD_LOOP_PING  0xF0FE
#define UART_LOOP_PORT     UART_NUM_1

static volatile uint32_t s_loop_rx;

static void on_loop_ping(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)ctx;
    (void)f;
    __atomic_add_fetch(&s_loop_rx, 1, __ATOMIC_RELAXED);
}

static void on_uart_loop(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t frames = 50, size = 32;
    if (f->len >= 2) frames = (uint16_t)(f->data[0] | (f->data[1] << 8));
    if (f->len >= 4) size = (uint16_t)(f->data[2] | (f->data[3] << 8));
    if (size < 16) size = 16;
    if (size > 256) size = 256;
    if (frames == 0) frames = 1;

    // 這兩個陣列以前在 decode0 的 4 KB 棧上：525 B ＋ uart_write_bytes/wait_tx_done
    // 的呼叫深度直接把棧打爆（實測：每次 0xF00B 都撞 stack canary → 整塊板重啟）。
    // handler 由 decoder lock 序列化，所以 static 是安全的。
    static uint8_t payload[256];
    static uint8_t frame[NC4_HDR_LEN + 256 + NC4_CRC_LEN];
    memset(payload, 0xA5, sizeof(payload));
    const int total = nc4_pack_into(frame, sizeof(frame), NC4_CMD_LOOP_PING,
                                    NC4_ADDR_BROADCAST, payload, size);
    if (total <= 0) { reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"pack\"}"); return; }
    __atomic_store_n(&s_loop_rx, 0, __ATOMIC_RELAXED);
    for (uint16_t i = 0; i < frames; i++) {
        uart_write_bytes(UART_LOOP_PORT, (const char *)frame, total);
    }
    uart_wait_tx_done(UART_LOOP_PORT, pdMS_TO_TICKS(1000));
    char json[96];
    snprintf(json, sizeof(json), "{\"sent\":%u,\"frame\":%d}", frames, total);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF00E UART_STREAM：由**獨立 task** 灌 UART（唔揸 decoder 鎖）──
//
// 為什麼要獨立 task：handler 執行時揸住 decoder 的鎖，而 decode0/decode1 共用
// 同一個 decoder —— 寫入期間 decode1 根本冇辦法 dispatch，插槽一滿 producer 就停，
// UART driver 的 RX ring 跟著溢位，看起來像「UART 掉幀」。
// 實測（0xF00B 從 handler 寫 200 幀 × 77 B）：rx 只有 62/200，還等滿 20 秒 timeout。
// 真實的 UART 對端不會揸住我們的鎖，所以量吞吐一定要用這支。
#define NC4_CMD_UART_STREAM 0xF00E
#define UART_STREAM_PORT    UART_NUM_1

static volatile uint32_t s_stream_sent;

static void uart_stream_task(void *arg)
{
    const uint32_t v = (uint32_t)(uintptr_t)arg;
    const uint16_t frames = (uint16_t)(v & 0xFFFF);
    const uint16_t size = (uint16_t)((v >> 16) & 0xFFFF);
    static uint8_t payload[256];
    static uint8_t frame[NC4_HDR_LEN + 256 + NC4_CRC_LEN];
    memset(payload, 0x5A, size);
    const int total = nc4_pack_into(frame, sizeof(frame), NC4_CMD_LOOP_PING,
                                    NC4_ADDR_BROADCAST, payload, size);
    if (total <= 0) {
        __atomic_store_n(&s_stream_sent, 0xFFFFFFFFu, __ATOMIC_RELAXED);
        vTaskDelete(NULL);
        return;
    }
    __atomic_store_n(&s_loop_rx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_stream_sent, 0, __ATOMIC_RELAXED);
    for (uint16_t i = 0; i < frames; i++) {
        const int w = uart_write_bytes(UART_STREAM_PORT, (const char *)frame, total);
        if (w != total) {
            break;
        }
        __atomic_add_fetch(&s_stream_sent, 1, __ATOMIC_RELAXED);
    }
    uart_wait_tx_done(UART_STREAM_PORT, pdMS_TO_TICKS(2000));
    vTaskDelete(NULL);
}

static void on_uart_stream(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t frames = 200, size = 64;
    if (f->len >= 2) frames = (uint16_t)(f->data[0] | (f->data[1] << 8));
    if (f->len >= 4) size = (uint16_t)(f->data[2] | (f->data[3] << 8));
    if (size < 16) size = 16;
    if (size > 256) size = 256;
    if (frames == 0) frames = 1;
    const uint32_t arg = ((uint32_t)size << 16) | frames;
    if (xTaskCreatePinnedToCore(uart_stream_task, "u_stream", 2560,
                                (void *)(uintptr_t)arg, 3, NULL, 1) != pdPASS) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"task\"}");
        return;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"started\":1,\"frames\":%u,\"size\":%u}",
             frames, size);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF00F UART_INJECT：把任意 NC4 幀寫入 UART（IO7→IO8 迴路 → decode1 執行）──
//
// 用途：**驗證雙核並行**。注入一條「慢指令」（例如 0xF001 DEV_BENCH）之後，
// core1 的 decode 會被那個 handler 佔住；同時量 core0 的 CDC RTT，就知道兩個核
// 是不是真的各跑各的（每核一個 decoder 之前，shared lock 會讓 core0 一起等）。
#define NC4_CMD_UART_INJECT 0xF00F
#define UART_INJECT_PORT    UART_NUM_1

static void on_uart_inject(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    // 一次寫整段；UART TX 環 1 KB，超過會阻塞到排空（測試用，可接受）
    const int w = uart_write_bytes(UART_INJECT_PORT, (const char *)f->data, f->len);
    char json[64];
    snprintf(json, sizeof(json), "{\"wrote\":%d}", w);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_loop_stat(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char json[96];
    snprintf(json, sizeof(json), "{\"rx\":%u,\"sent\":%u}",
             (unsigned)__atomic_load_n(&s_loop_rx, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_stream_sent, __ATOMIC_RELAXED));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF008 匯流排統計（證明 copy 有沒有真的消失）──
static void on_bus_stats(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len >= 1 && f->data[0] != 0) {
        nc4_bus_stats_reset();
        s_hooks.reset_rx();
    }
    nc4_reply_t r;
    char *json = reply_open(&r, ctx, NC4_CMD_BUS_RSP);
    const size_t cap = NC4_REPLY_JSON_MAX + 1;
    size_t off = 0;
    jappend(json, cap, &off, "{\"bus\":");
    jchain(json, cap, &off, nc4_bus_stats_json);
    jappend(json, cap, &off,
            ",\"resync\":%u,\"rx\":{\"f\":%u,\"hub_full\":%u,"
            "\"sel\":%u,\"sel_to\":%u,\"us\":%llu}}",
            (unsigned)s_hooks.rx_counter(0), (unsigned)s_hooks.rx_counter(1), (unsigned)s_hooks.rx_counter(2),
            (unsigned)s_hooks.rx_counter(3), (unsigned)s_hooks.rx_counter(4),
            (unsigned long long)s_hooks.rx_fill_us());
    reply_send(&r, off);
}


// 0xF00A：改 rx_split / rx_core / frame_align（寫 NVS，下次開機生效）
static void on_net_tune(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 2) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) != ESP_OK) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nvs open\"}");
        return;
    }
    // rx_split / rx_core 已刪：核心歸屬只有 router 一個來源。
    const uint8_t frame = (f->len >= 3) ? (f->data[2] ? 1 : 0) : (s_hooks.frame_align() ? 1 : 0);
    const esp_err_t e1 = nvs_set_u8(h, "frame_align", frame);
    const esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    char json[128];
    if (e1 != ESP_OK || e2 != ESP_OK) {
        snprintf(json, sizeof(json),
                 "{\"err\":\"nvs write\",\"set\":\"%s\",\"commit\":\"%s\"}",
                 esp_err_to_name(e1), esp_err_to_name(e2));
        reply_json(ctx, NC4_CMD_HUB_RSP, json);
        return;
    }
    snprintf(json, sizeof(json),
             "{\"frame_align\":%u,\"next_boot\":1}", (unsigned)frame);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF007 NC4 上傳（打包 + 送出）量測 ──
// frames(u32) size(u16) mode(u8) → 0xF004
//   mode 0 = 同核：同一個 task 逐幀 pack，pack 完立刻 send（單核）
//   mode 1 = 分核：core1 的 packer 打包進插槽，decode task（core0）只負責 send
#define NC4_CMD_TX_BENCH 0xF007

typedef struct {
    nc_hub_t *hub;
    uint32_t frames;
    uint32_t packed;
    size_t size;
    size_t total;
    volatile bool done;
    volatile uint8_t exit;        // 1=完成 2=逾時
    volatile int64_t deadline_us;
} tx_bench_t;

// 8 KB 填充內容由 PSRAM 出（第一次量測才配，之後重用）。
// 以前係 static uint8_t payload[8192]：一個測試指令用唔到，卻長期佔住 SRAM。
static uint8_t *s_tx_payload;

static void tx_bench_packer(void *arg)
{
    tx_bench_t *b = arg;
    uint8_t *payload = s_tx_payload;
    if (payload == NULL) {                 // 呼叫端已擋；這裡再保險一次
        b->exit = 3;
        b->done = true;
        vTaskDelete(NULL);
        return;
    }
    uint32_t spins = 0;
    for (uint32_t i = 0; i < b->frames; i++) {
        uint8_t *p = nc_hub_write_begin(b->hub);
        while (p == NULL) {
            if (esp_timer_get_time() > b->deadline_us) {
                b->exit = 2;
                b->done = true;
                vTaskDelete(NULL);
                return;
            }
            hub_bench_backoff(&spins);
            p = nc_hub_write_begin(b->hub);
        }
        nc4_pack_into(p, b->total, NC4_CMD_BENCH_DATA, NC4_ADDR_BROADCAST, payload, b->size);
        nc_hub_write_commit(b->hub);
        b->packed++;
    }
    b->exit = 1;
    b->done = true;
    vTaskDelete(NULL);
}

static void on_tx_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint32_t frames = 2000;
    uint16_t size = 8192;
    uint8_t mode = 0;
    if (f->len >= 4) {
        frames = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                 ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (f->len >= 6) {
        size = (uint16_t)(f->data[4] | (f->data[5] << 8));
    }
    if (f->len >= 7) {
        mode = f->data[6];
    }
    if (frames < 10) {
        frames = 10;
    }
    if (frames > 200000) {
        frames = 200000;
    }
    if (size < 16) {
        size = 16;
    }
    if (size > NC4_MAX_PAYLOAD) {
        size = NC4_MAX_PAYLOAD;
    }
    if (ctx->send == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"no send\"}");
        return;
    }
    const size_t total = NC4_HDR_LEN + (size_t)size + NC4_CRC_LEN;

    uint32_t sent = 0, packed = 0;
    int64_t dt = 0;

    if (mode == 0) {
        uint8_t *buf = malloc(total);
        uint8_t *payload = malloc(size);
        if (buf == NULL || payload == NULL) {
            free(buf);
            free(payload);
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"oom\"}");
            return;
        }
        memset(payload, 0xA5, size);
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < frames; i++) {
            nc4_pack_into(buf, total, NC4_CMD_BENCH_DATA, NC4_ADDR_BROADCAST, payload, size);
            if (ctx->send(ctx->send_arg, buf, total) < 0) {
                break;
            }
            sent++;
        }
        dt = esp_timer_get_time() - t0;
        packed = sent;
        free(buf);
        free(payload);
    } else {
        static nc_hub_t *s_tx_hub;
        if (s_tx_hub == NULL) {
            s_tx_hub = nc_hub_create("txbench", NC4_MAX_FRAME, 8, NC_HUB_SRAM, 32);
        }
        if (s_tx_hub == NULL) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"hub\"}");
            return;
        }
        while (nc_hub_read_begin(s_tx_hub) != NULL) {
            nc_hub_read_end(s_tx_hub);      // 上一輪的殘格
        }

        static tx_bench_t b;
        memset((void *)&b, 0, sizeof(b));
        b.hub = s_tx_hub;
        b.frames = frames;
        b.size = size;
        b.total = total;
        b.deadline_us = esp_timer_get_time() + 10 * 1000 * 1000;
        if (s_tx_payload == NULL) {
            s_tx_payload = heap_caps_malloc(NC4_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
            if (s_tx_payload == NULL) {
                s_tx_payload = malloc(NC4_MAX_PAYLOAD);   // 冇 PSRAM 就照舊
            }
            if (s_tx_payload != NULL) {
                memset(s_tx_payload, 0xA5, NC4_MAX_PAYLOAD);
            }
        }
        if (s_tx_payload == NULL) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"oom\"}");
            return;
        }
        TaskHandle_t pk = NULL;
        xTaskCreatePinnedToCore(tx_bench_packer, "tx_pack", 4096, &b, 2, &pk, 1);
        if (pk == NULL) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"task\"}");
            return;
        }
        uint32_t spins = 0;
        const int64_t t0 = esp_timer_get_time();
        for (;;) {
            uint8_t *p = nc_hub_read_begin(s_tx_hub);
            if (p != NULL) {
                int r = ctx->send(ctx->send_arg, p, total);
                nc_hub_read_end(s_tx_hub);
                if (r < 0) {
                    break;
                }
                sent++;
                continue;
            }
            if (b.done && !nc_hub_dirty(s_tx_hub)) {
                break;
            }
            if (esp_timer_get_time() > b.deadline_us) {
                break;
            }
            hub_bench_backoff(&spins);
        }
        dt = esp_timer_get_time() - t0;
        packed = b.packed;
    }

    char json[224];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"frames\":%u,\"sent\":%u,\"packed\":%u,\"size\":%u,"
             "\"us\":%lld,\"mb_s\":%.2f}",
             mode ? "split" : "same", (unsigned)frames, (unsigned)sent, (unsigned)packed,
             (unsigned)size, (long long)dt,
             dt > 0 ? (double)sent * (double)total / (double)dt : 0.0);
    ESP_LOGW(TAG, "TX_BENCH %s", json);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF01A BIG_REPLY：驗「回覆超限不再靜默截斷」（tx-path-design §7 的 6.1）──
//
// payload = u16 n（預設 3000，1..4096）。回一條長度 n 的字串：
//   n ≤ 2,600 → 正常（但內容係 'x'，不是 JSON —— 這支只為驗超限那條路）
//   n > 2,600 → 應該收到 {"err":"reply_too_big","need":n,"cap":2600}
// 以前會回半截 JSON，主機 parse 失敗之餘，完全唔知係「截斷」。
#define NC4_CMD_BIG_REPLY  0xF01A

static void on_big_reply(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t n = 3000;
    if (f->len >= 2) {
        n = (uint16_t)(f->data[0] | (f->data[1] << 8));
    }
    if (n < 1) {
        n = 1;
    }
    if (n > 4096) {
        n = 4096;
    }
    static char s_big[4097];
    memset(s_big, 'x', n);
    s_big[n] = '\0';
    reply_json(ctx, NC4_CMD_HUB_RSP, s_big);
}

// ── 0xF01B FWD：把一格整幀交給指定 link（驗 nc4_link_send ＋ UART TX）──
//
// payload = [u16 name_len][name][NC4 幀 bytes]
// 回 {"link":…,"len":…,"err":…,"frames":…,"bytes":…,"drops":…}
// 用途：唔使改 config 也可以量「轉寄一幀要幾久、UART 收唔收得到」
//（配合 0xF00C 看 UART 迴路的 rx 計數）。
#define NC4_CMD_LINK_FWD   0xF01B

static void on_link_fwd(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 4) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const uint16_t nlen = (uint16_t)(f->data[0] | (f->data[1] << 8));
    if (nlen == 0 || nlen >= NC4_LINK_NAME_MAX || 2 + nlen >= f->len) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    char name[NC4_LINK_NAME_MAX];
    for (uint16_t i = 0; i < nlen; i++) {
        const char c = (char)f->data[2 + i];
        // 只收安全字元：避免 reply 的 JSON 被名字弄壞（測試指令也唔例外）
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"name\"}");
            return;
        }
        name[i] = c;
    }
    name[nlen] = '\0';

    const uint8_t *frame = f->data + 2 + nlen;
    const size_t flen = (size_t)f->len - 2 - nlen;
    const esp_err_t e = nc4_link_send(name, frame, flen);
    uint32_t frames = 0, bytes = 0, drops = 0;
    nc4_link_stats(name, &frames, &bytes, &drops);
    char json[192];
    snprintf(json, sizeof(json),
             "{\"link\":\"%s\",\"len\":%u,\"err\":\"%s\","
             "\"frames\":%u,\"bytes\":%u,\"drops\":%u}",
             name, (unsigned)flen, esp_err_to_name(e),
             (unsigned)frames, (unsigned)bytes, (unsigned)drops);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF01C MEM_BENCH：internal / PSRAM 的實際頻寬（2026-11-09）──
//
// payload = [u16 psram_kb][u16 iters]（預設 256 KB、4 次）
// 回 {"int":{kb,memset_mbps,read_mbps,copy_mbps},
//     "psram":{kb,memset_mbps,read_mbps,copy_mbps},
//     "msync_us":N}
// 用途：回答「PSRAM 夠不夠做 framebuffer／音效」——用實測取代估計。
#define NC4_CMD_MEM_BENCH  0xF01C
#define MEM_BENCH_INT_KB   32

static uint32_t bench_mbps(size_t bytes, int iters, int64_t us)
{
    if (us <= 0) {
        return 0;
    }
    return (uint32_t)((double)bytes * (double)iters / (double)us);
}

static void on_mem_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t kb = 256;
    uint16_t iters = 4;
    if (f->len >= 2) {
        kb = (uint16_t)(f->data[0] | (f->data[1] << 8));
    }
    if (f->len >= 4) {
        iters = (uint16_t)(f->data[2] | (f->data[3] << 8));
    }
    if (kb < 16) kb = 16;
    if (kb > 2048) kb = 2048;
    if (iters < 1) iters = 1;
    if (iters > 20) iters = 20;

    const size_t in_bytes = (MEM_BENCH_INT_KB * 1024u) & ~(size_t)63;
    const size_t ps_bytes = ((size_t)kb * 1024u) & ~(size_t)63;

    uint8_t *is = heap_caps_malloc(in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *id = heap_caps_malloc(in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *ps = heap_caps_malloc(ps_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pd = heap_caps_malloc(ps_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (is == NULL || id == NULL || ps == NULL || pd == NULL) {
        free(is); free(id); free(ps); free(pd);
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem\"}");
        return;
    }

    // internal：memset / read / copy
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) memset(is, 0xA5, in_bytes);
    const uint32_t i_memset = bench_mbps(in_bytes, iters, esp_timer_get_time() - t0);

    volatile uint32_t sum = 0;
    const volatile uint32_t *ip = (const volatile uint32_t *)is;
    t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) {
        for (size_t i = 0; i < in_bytes / 4; i++) sum += ip[i];
    }
    const uint32_t i_read = bench_mbps(in_bytes, iters, esp_timer_get_time() - t0);

    t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) memcpy(id, is, in_bytes);
    const uint32_t i_copy = bench_mbps(in_bytes, iters, esp_timer_get_time() - t0);

    // PSRAM：同樣三項
    t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) memset(ps, 0x5A, ps_bytes);
    const uint32_t p_memset = bench_mbps(ps_bytes, iters, esp_timer_get_time() - t0);

    const volatile uint32_t *pp = (const volatile uint32_t *)ps;
    t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) {
        for (size_t i = 0; i < ps_bytes / 4; i++) sum += pp[i];
    }
    const uint32_t p_read = bench_mbps(ps_bytes, iters, esp_timer_get_time() - t0);

    t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) memcpy(pd, ps, ps_bytes);
    const uint32_t p_copy = bench_mbps(ps_bytes, iters, esp_timer_get_time() - t0);

    // cache writeback 的成本（I80 driver 每次 tx_color 都會做）。
    // 先 memset 把 cache 弄髒，再量一次 flush；否則 read-only 之後量到的是 no-op。
    memset(ps, 0x33, ps_bytes);
    int64_t msync_us = -1;
    t0 = esp_timer_get_time();
    const esp_err_t me = esp_cache_msync(ps, ps_bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (me == ESP_OK) {
        msync_us = esp_timer_get_time() - t0;
    }

    char json[420];
    snprintf(json, sizeof(json),
             "{\"int\":{\"kb\":%u,\"memset_mbps\":%u,\"read_mbps\":%u,\"copy_mbps\":%u},"
             "\"psram\":{\"kb\":%u,\"memset_mbps\":%u,\"read_mbps\":%u,\"copy_mbps\":%u},"
             "\"msync_us\":%lld,\"msync_err\":\"%s\",\"sum\":%u}",
             (unsigned)(in_bytes / 1024), (unsigned)i_memset, (unsigned)i_read, (unsigned)i_copy,
             (unsigned)(ps_bytes / 1024), (unsigned)p_memset, (unsigned)p_read, (unsigned)p_copy,
             (long long)msync_us, esp_err_to_name(me), (unsigned)sum);

    free(is);
    free(id);
    free(ps);
    free(pd);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF01D TILE_BENCH：整幀 streaming vs 分塊（cache-hot）──
//
// payload = [u16 total_kb][u16 tile_kb][u16 iters]（預設 40 / 8 / 8）
// 回 {"total_kb":..,"tile_kb":..,"full_mbps":..,"tile_mbps":..,
//     "full_us":..,"tile_us":..,"ratio":..}
// 用途：證明「先寫完整幀再讀」vs「每塊寫完立即讀」在 PSRAM 的差距；
// 這是 surface/audio 要不要分塊、要不要內部 staging 的實測依據。
#define NC4_CMD_TILE_BENCH  0xF01D

static void on_tile_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t total_kb = 40;
    uint16_t tile_kb = 8;
    uint16_t iters = 8;
    if (f->len >= 2) total_kb = (uint16_t)(f->data[0] | (f->data[1] << 8));
    if (f->len >= 4) tile_kb = (uint16_t)(f->data[2] | (f->data[3] << 8));
    if (f->len >= 6) iters = (uint16_t)(f->data[4] | (f->data[5] << 8));
    if (total_kb < 16) total_kb = 16;
    if (total_kb > 1024) total_kb = 1024;
    if (tile_kb < 1) tile_kb = 1;
    if (tile_kb > 64) tile_kb = 64;
    if (tile_kb > total_kb) tile_kb = total_kb;
    if (iters < 1) iters = 1;
    if (iters > 32) iters = 32;

    const size_t total = (size_t)total_kb * 1024u;
    const size_t tile = (size_t)tile_kb * 1024u;
    uint8_t *src = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *dst = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (src == NULL || dst == NULL) {
        free(src); free(dst);
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem\"}");
        return;
    }

    // 整幀：先寫完整塊，再 copy 完整塊（第二個 pass 已經 cache miss）
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) {
        memset(src, 0xA5, total);
        memcpy(dst, src, total);
    }
    const int64_t full_us = (esp_timer_get_time() - t0) / iters;

    // 分塊：每塊寫完立即 copy（同一塊仍在 cache）
    t0 = esp_timer_get_time();
    for (int k = 0; k < iters; k++) {
        for (size_t off = 0; off < total; off += tile) {
            const size_t n = (off + tile <= total) ? tile : (total - off);
            memset(src + off, 0x5A, n);
            memcpy(dst + off, src + off, n);
        }
    }
    const int64_t tile_us = (esp_timer_get_time() - t0) / iters;

    const uint32_t full_mbps = full_us > 0 ? (uint32_t)((double)total / (double)full_us) : 0;
    const uint32_t tile_mbps = tile_us > 0 ? (uint32_t)((double)total / (double)tile_us) : 0;
    const uint32_t ratio = tile_us > 0 ? (uint32_t)((double)full_us / (double)tile_us * 10) : 0;

    char json[320];
    snprintf(json, sizeof(json),
             "{\"total_kb\":%u,\"tile_kb\":%u,\"iters\":%u,"
             "\"full_us\":%lld,\"tile_us\":%lld,"
             "\"full_mbps\":%u,\"tile_mbps\":%u,\"ratio_x10\":%u}",
             (unsigned)total_kb, (unsigned)tile_kb, (unsigned)iters,
             (long long)full_us, (long long)tile_us,
             (unsigned)full_mbps, (unsigned)tile_mbps, (unsigned)ratio);

    free(src);
    free(dst);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF01E STAGE_BENCH：SRAM↔PSRAM 的方向、copy 與 freshness 成本 ──
// payload = [u32 bytes][u16 iters][u16 mode]；bit0：0=fresh（含 sync）、1=hot；bit1=另一核持續 PSRAM copy。
#define NC4_CMD_STAGE_BENCH  0xF01E

typedef struct {
    uint32_t copy_us;
    uint32_t total_us;
    uint32_t max_us;
    uint32_t mbps;
} stage_result_t;

static void stage_sync(void *p, size_t bytes, int flags, esp_err_t *err)
{
    const esp_err_t e = esp_cache_msync(p, bytes, flags);
    if (*err == ESP_OK && e != ESP_OK) {
        *err = e;
    }
}

static stage_result_t stage_run(void *dst, const void *src, size_t bytes, int iters,
                                bool fresh, bool src_ext, bool dst_ext,
                                esp_err_t *sync_err)
{
    int64_t copy_total = 0;
    int64_t total = 0;
    uint32_t max_us = 0;
    for (int k = 0; k < iters; k++) {
        const int64_t begin = esp_timer_get_time();
        if (fresh && src_ext) {
            stage_sync((void *)src, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C, sync_err);
        }
        const int64_t t0 = esp_timer_get_time();
        memcpy(dst, src, bytes);
        const int64_t t1 = esp_timer_get_time();
        if (fresh && dst_ext) {
            stage_sync(dst, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M, sync_err);
        }
        const int64_t t2 = esp_timer_get_time();
        copy_total += t1 - t0;
        total += t2 - begin;
        if ((uint32_t)(t2 - begin) > max_us) {
            max_us = (uint32_t)(t2 - begin);
        }
    }
    return (stage_result_t){
        .copy_us = (uint32_t)(copy_total / iters),
        .total_us = (uint32_t)(total / iters),
        .max_us = max_us,
        .mbps = bench_mbps(bytes, iters, total),
    };
}

typedef struct {
    volatile bool stop;
    volatile bool started;
    TaskHandle_t done;
    uint8_t *src;
    uint8_t *dst;
} stage_competitor_t;

static void stage_competitor_task(void *arg)
{
    stage_competitor_t *c = arg;
    c->started = true;
    uint32_t rounds = 0;
    while (!c->stop) {
        memcpy(c->dst, c->src, 64 * 1024);
        if ((++rounds & 0xFF) == 0) {
            vTaskDelay(1);
        }
    }
    c->done = NULL;
    vTaskDelete(NULL);
}

static esp_err_t stage_competitor_begin(stage_competitor_t *c, uint8_t *src, uint8_t *dst)
{
    memset(c, 0, sizeof(*c));
    c->src = src;
    c->dst = dst;
    const int other_core = (int)xPortGetCoreID() ^ 1;
    const BaseType_t ok = xTaskCreatePinnedToCore(stage_competitor_task, "stage_competitor",
                                                  3072, c, uxTaskPriorityGet(NULL),
                                                  &c->done, other_core);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    while (!c->started) {
        vTaskDelay(1);
    }
    return ESP_OK;
}

static void stage_competitor_end(stage_competitor_t *c)
{
    c->stop = true;
    while (c->done != NULL) {
        vTaskDelay(1);
    }
}

static void on_stage_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint32_t bytes = 64 * 1024;
    uint16_t iters = 8;
    uint16_t mode = 0;
    if (f->len >= 4) {
        bytes = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (f->len >= 6) {
        iters = (uint16_t)(f->data[4] | (f->data[5] << 8));
    }
    if (f->len >= 8) {
        mode = (uint16_t)(f->data[6] | (f->data[7] << 8));
    }
    if (bytes < 64) bytes = 64;
    if (bytes > 128 * 1024) bytes = 128 * 1024;
    bytes = (bytes + 63) & ~(uint32_t)63;
    if (iters < 1) iters = 1;
    if (iters > 32) iters = 32;

    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *is = heap_caps_aligned_alloc(64, bytes, internal_caps);
    uint8_t *id = heap_caps_aligned_alloc(64, bytes, internal_caps);
    uint8_t *ps = heap_caps_aligned_alloc(64, bytes, psram_caps);
    uint8_t *pd = heap_caps_aligned_alloc(64, bytes, psram_caps);
    if (is == NULL || id == NULL || ps == NULL || pd == NULL) {
        free(is); free(id); free(ps); free(pd);
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem\"}");
        return;
    }

    memset(is, 0x11, bytes);
    memset(id, 0x22, bytes);
    memset(ps, 0x33, bytes);
    memset(pd, 0x44, bytes);
    esp_err_t sync_err = ESP_OK;
    stage_sync(ps, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M, &sync_err);
    stage_sync(pd, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M, &sync_err);

    const bool fresh = ((mode & 1) == 0);
    const bool contend = ((mode & 2) != 0);
    uint8_t *cs = NULL;
    uint8_t *cd = NULL;
    stage_competitor_t competitor;
    if (contend) {
        cs = heap_caps_aligned_alloc(64, 64 * 1024, psram_caps);
        cd = heap_caps_aligned_alloc(64, 64 * 1024, psram_caps);
        if (cs == NULL || cd == NULL) {
            free(is); free(id); free(ps); free(pd); free(cs); free(cd);
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem_competitor\"}");
            return;
        }
        memset(cs, 0x55, 64 * 1024);
        if (stage_competitor_begin(&competitor, cs, cd) != ESP_OK) {
            free(is); free(id); free(ps); free(pd); free(cs); free(cd);
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem_task\"}");
            return;
        }
    }

    const stage_result_t ss = stage_run(id, is, bytes, iters, fresh, false, false, &sync_err);
    const stage_result_t sp = stage_run(pd, is, bytes, iters, fresh, false, true, &sync_err);
    const stage_result_t ps2i = stage_run(id, ps, bytes, iters, fresh, true, false, &sync_err);
    const stage_result_t pp = stage_run(pd, ps, bytes, iters, fresh, true, true, &sync_err);
    if (contend) {
        stage_competitor_end(&competitor);
        free(cs);
        free(cd);
    }

    char json[700];
    snprintf(json, sizeof(json),
             "{\"bytes\":%u,\"iters\":%u,\"mode\":\"%s\",\"contend\":%u,\"sync_err\":\"%s\","
             "\"s2s\":{\"copy_us\":%u,\"total_us\":%u,\"max_us\":%u,\"mbps\":%u},"
             "\"s2p\":{\"copy_us\":%u,\"total_us\":%u,\"max_us\":%u,\"mbps\":%u},"
             "\"p2s\":{\"copy_us\":%u,\"total_us\":%u,\"max_us\":%u,\"mbps\":%u},"
             "\"p2p\":{\"copy_us\":%u,\"total_us\":%u,\"max_us\":%u,\"mbps\":%u}}",
             (unsigned)bytes, (unsigned)iters, fresh ? "fresh" : "hot",
             contend ? 1 : 0, esp_err_to_name(sync_err),
             (unsigned)ss.copy_us, (unsigned)ss.total_us, (unsigned)ss.max_us, (unsigned)ss.mbps,
             (unsigned)sp.copy_us, (unsigned)sp.total_us, (unsigned)sp.max_us, (unsigned)sp.mbps,
             (unsigned)ps2i.copy_us, (unsigned)ps2i.total_us, (unsigned)ps2i.max_us, (unsigned)ps2i.mbps,
             (unsigned)pp.copy_us, (unsigned)pp.total_us, (unsigned)pp.max_us, (unsigned)pp.mbps);
    free(is); free(id); free(ps); free(pd);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF01F HUB_STRESS：雙核壓力測試 ──
// payload = [u32 iters]；依序跑 N=2 / N=3 / N=16 / N=16+慢讀者。
// 每個情境都檢查：torn、generation 對不對、序號是否連續、slot 是否洩漏、跨核違規。
#define NC4_CMD_HUB_STRESS  0xF01F
#define HUB_STRESS_ITERS_MAX 300000u

typedef struct {
    nc_hub_t *hub;
    uint32_t iters;
    volatile bool start;
    volatile bool writer_done;
    volatile uint32_t read_count;
    uint32_t torn;
    uint32_t seq_err;
    uint32_t gap;
    uint32_t dup;
    uint32_t w_full;
    uint32_t r_null;
    uint32_t w_exit;
    uint32_t r_exit;
    uint32_t yield_every;
    uint32_t prev_seq;      // 診斷：上一筆讀到的 seq
    uint32_t bad_prev;      // 診斷：第一次 gap/dup 的 (上一筆, 本筆)
    uint32_t bad_seq;
    int64_t deadline_us;
} hub_stress_t;

static void hub_stress_writer(void *arg)
{
    hub_stress_t *s = arg;
    while (!s->start) {
        if (esp_timer_get_time() > s->deadline_us) {
            s->w_exit = 2;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(1);
    }
    const size_t sz = nc_hub_slot_size(s->hub);
    uint32_t seq = 1;
    uint32_t spins = 0;
    while (seq <= s->iters) {
        uint8_t *p = nc_hub_write_begin(s->hub);
        if (p == NULL) {
            s->w_full++;
            if (esp_timer_get_time() > s->deadline_us) {
                s->w_exit = 2;
                break;
            }
            // 不可純 taskYIELD：同核 idle（prio 0）永遠輪不到 → TWDT。
            if ((++spins & 0x3F) == 0) {
                vTaskDelay(1);
            } else {
                taskYIELD();
            }
            continue;
        }
        spins = 0;
        memcpy(p, &seq, sizeof(seq));
        memset(p + sizeof(seq), (uint8_t)seq, sz - sizeof(seq));
        nc_hub_write_commit(s->hub);
        seq++;
    }
    s->writer_done = true;
    if (s->w_exit == 0) {
        s->w_exit = 1;
    }
    vTaskDelete(NULL);
}

static void hub_stress_reader(void *arg)
{
    hub_stress_t *s = arg;
    while (!s->start) {
        if (esp_timer_get_time() > s->deadline_us) {
            s->r_exit = 2;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(1);
    }
    const size_t sz = nc_hub_slot_size(s->hub);
    uint32_t expect = 1;
    uint32_t spins = 0;
    while (s->read_count < s->iters) {
        uint32_t gen = 0;
        uint8_t *p = nc_hub_read_begin_gen(s->hub, &gen);
        if (p == NULL) {
            s->r_null++;
            if (s->writer_done && !nc_hub_dirty(s->hub)) {
                s->r_exit = 1;
                break;
            }
            if (esp_timer_get_time() > s->deadline_us) {
                s->r_exit = 2;
                break;
            }
            if ((++spins & 0x0F) == 0) {
                vTaskDelay(1);
            } else {
                taskYIELD();
            }
            continue;
        }
        spins = 0;
        uint32_t seq = 0;
        memcpy(&seq, p, sizeof(seq));
        const uint8_t want = (uint8_t)seq;
        for (size_t k = sizeof(seq); k < sz; k++) {
            if (p[k] != want) {
                s->torn++;
                break;
            }
        }
        if (gen != seq) {
            s->seq_err++;
        }
        if (seq != expect && s->bad_prev == 0) {
            s->bad_prev = s->prev_seq;
            s->bad_seq = seq;
        }
        s->prev_seq = seq;
        if (seq == expect) {
            expect++;
        } else if (seq > expect) {
            s->gap++;
            expect = seq + 1;
        } else {
            s->dup++;
        }
        nc_hub_read_end(s->hub);
        s->read_count++;
        if (s->yield_every != 0 && (s->read_count % s->yield_every) == 0) {
            vTaskDelay(1);
        }
    }
    if (s->r_exit == 0) {
        s->r_exit = 1;
    }
    vTaskDelete(NULL);
}

// 跑完後每個 slot 都應回到 IDLE；拿不滿 N 格就代表有 slot 卡住。
static int hub_stress_leak(nc_hub_t *hub)
{
    const size_t n = nc_hub_depth(hub);
    size_t got = 0;
    while (got < n) {
        uint8_t *p = nc_hub_write_begin(hub);
        if (p == NULL) {
            break;
        }
        memset(p, 0xEE, nc_hub_slot_size(hub));
        nc_hub_write_commit(hub);
        got++;
    }
    while (nc_hub_read_begin(hub) != NULL) {
        nc_hub_read_end(hub);
    }
    return (int)(n - got);
}

static size_t hs_append(char *buf, size_t cap, size_t off, const char *fmt, ...)
{
    if (off + 1 >= cap) {
        return off;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return off;
    }
    const size_t next = off + (size_t)n;
    return next < cap ? next : cap - 1;   // 截斷時也要保持合法 C 字串
}

static bool hub_stress_case(hub_stress_t *s, uint32_t n, uint32_t iters,
                            uint32_t yield_every, char *json, size_t cap,
                            size_t *off, bool *first)
{
    memset(s, 0, sizeof(*s));
    s->hub = nc_hub_create("stress", 64, n, NC_HUB_SRAM, 32);
    if (s->hub == NULL) {
        *off = hs_append(json, cap, *off, "%s{\"n\":%u,\"err\":\"hub\"}",
                         *first ? "" : ",", (unsigned)n);
        *first = false;
        return false;
    }
    s->iters = iters;
    s->yield_every = yield_every;
    s->deadline_us = esp_timer_get_time() + 4 * 1000 * 1000;

    TaskHandle_t wr = NULL;
    TaskHandle_t rd = NULL;
    xTaskCreatePinnedToCore(hub_stress_writer, "st_w", 4096, s, 1, &wr, 1);
    xTaskCreatePinnedToCore(hub_stress_reader, "st_r", 4096, s, 1, &rd, 0);
    if (wr == NULL || rd == NULL) {
        *off = hs_append(json, cap, *off, "%s{\"n\":%u,\"err\":\"task\"}",
                         *first ? "" : ",", (unsigned)n);
        *first = false;
        nc_hub_destroy(s->hub);
        return false;
    }

    const int64_t t0 = esp_timer_get_time();
    s->start = true;
    while (s->read_count < iters && s->r_exit == 0) {
        vTaskDelay(1);
        if (esp_timer_get_time() - t0 > 5 * 1000 * 1000) {
            break;
        }
    }
    const int64_t us = esp_timer_get_time() - t0;

    // 必須等兩個 task 真正收手才能離開：s 在 stack 上，先返回會 use-after-free。
    while (s->w_exit == 0 || s->r_exit == 0) {
        vTaskDelay(1);
    }

    nc_hub_stats_t st;
    nc_hub_stats(s->hub, &st);
    const int leak = hub_stress_leak(s->hub);
    const uint32_t per_sec = us > 0
                                 ? (uint32_t)((uint64_t)s->read_count * 1000000u / (uint64_t)us)
                                 : 0;

    *off = hs_append(json, cap, *off,
                     "%s{\"n\":%u,\"ops\":%u,\"per_sec\":%u,\"us\":%lld,"
                     "\"torn\":%u,\"seq_err\":%u,\"gap\":%u,\"dup\":%u,"
                     "\"w_full\":%u,\"r_null\":%u,\"leak\":%d,\"viol\":%u,"
                     "\"bad\":[%u,%u],\"exit\":[%u,%u]}",
                     *first ? "" : ",", (unsigned)n, (unsigned)s->read_count,
                     (unsigned)per_sec, (long long)us,
                     (unsigned)s->torn, (unsigned)s->seq_err,
                     (unsigned)s->gap, (unsigned)s->dup,
                     (unsigned)s->w_full, (unsigned)s->r_null, leak,
                     (unsigned)st.core_violations,
                     (unsigned)s->bad_prev, (unsigned)s->bad_seq,
                     (unsigned)s->w_exit, (unsigned)s->r_exit);
    *first = false;
    nc_hub_destroy(s->hub);
    return true;
}

// 放 .bss，不放 decode 任務的 stack（1.8 KB 區域曾在板上造成溢位重啟）。
// 每核一份，避免兩核同時進來時共用同一塊。
static char s_hub_stress_json[2][1200];

static void on_hub_stress(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    static volatile bool busy;
    if (busy) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    busy = true;

    uint32_t iters = 50000;
    if (f->len >= 4) {
        iters = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (iters < 1000) {
        iters = 1000;
    }
    if (iters > HUB_STRESS_ITERS_MAX) {
        iters = HUB_STRESS_ITERS_MAX;
    }

    char *json = s_hub_stress_json[xPortGetCoreID() & 1];
    const size_t cap = sizeof(s_hub_stress_json[0]);
    size_t off = 0;
    bool first = true;
    off = hs_append(json, cap, off, "{\"iters\":%u,\"cases\":[", (unsigned)iters);

    hub_stress_t s;
    const uint32_t slow_iters = (iters / 4 < 1000) ? 1000 : iters / 4;
    bool ok = true;
    ok = ok && hub_stress_case(&s, 2, iters, 0, json, cap, &off, &first);
    ok = ok && hub_stress_case(&s, 3, iters, 0, json, cap, &off, &first);
    ok = ok && hub_stress_case(&s, 16, iters, 0, json, cap, &off, &first);
    ok = ok && hub_stress_case(&s, 16, slow_iters, 32, json, cap, &off, &first);
    (void)ok;

    off = hs_append(json, cap, off, "]}");
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
    busy = false;
}

// ── 0xF020 HUB_DUPLEX：兩個常駐測試任務，雙向同時交換 ──
// payload = [u32 duration_ms]；core1 跑 A（tx=ab, rx=ba），core0 跑 B（tx=ba, rx=ab）。
// 兩個任務同時送／收，跑滿時間才停；停止後對帳 sent/recv、torn、順序、洩漏。
#define NC4_CMD_HUB_DUPLEX  0xF020

typedef struct {
    nc_hub_t *tx;
    nc_hub_t *rx;
    volatile bool start;
    volatile bool stop;
    volatile bool done;
    uint32_t sent;
    uint32_t recv;
    uint32_t full;
    uint32_t empty;
    uint32_t torn;
    uint32_t seq_err;
    uint32_t expect;
} duplex_ep_t;

static void hub_duplex_task(void *arg)
{
    duplex_ep_t *e = arg;
    while (!e->start) {
        vTaskDelay(1);
    }
    const size_t txs = nc_hub_slot_size(e->tx);
    const size_t rxs = nc_hub_slot_size(e->rx);
    uint32_t seq = 1;
    uint32_t spins = 0;
    while (!e->stop) {
        uint8_t *p = nc_hub_write_begin(e->tx);
        if (p != NULL) {
            memcpy(p, &seq, sizeof(seq));
            memset(p + sizeof(seq), (uint8_t)seq, txs - sizeof(seq));
            nc_hub_write_commit(e->tx);
            e->sent++;
            seq++;
        } else {
            e->full++;
        }

        uint8_t *q = nc_hub_read_begin(e->rx);
        if (q != NULL) {
            uint32_t v = 0;
            memcpy(&v, q, sizeof(v));
            const uint8_t want = (uint8_t)v;
            for (size_t k = sizeof(v); k < rxs; k++) {
                if (q[k] != want) {
                    e->torn++;
                    break;
                }
            }
            if (v != e->expect) {
                e->seq_err++;
            }
            e->expect = v + 1;
            nc_hub_read_end(e->rx);
            e->recv++;
        } else {
            e->empty++;
        }

        // 每 4096 圈讓出 1 tick：避免同核 idle 餓死 → TWDT。
        if ((++spins & 0xFFF) == 0) {
            vTaskDelay(1);
        }
    }
    e->done = true;
    vTaskDelete(NULL);
}

static char s_duplex_json[2][1200];
static char s_sweep_json[2][1400];
static volatile bool s_duplex_busy;

typedef struct {
    uint32_t a_sent, a_recv, a_full, a_empty, a_torn, a_seq_err;
    uint32_t b_sent, b_recv, b_full, b_empty, b_torn, b_seq_err;
    uint32_t drain_ab, drain_ba;
    int leak_ab, leak_ba;
    uint32_t us;
} duplex_result_t;

static bool duplex_run(uint32_t slot, uint32_t n, uint32_t ms, duplex_result_t *out)
{
    if (slot < 8) slot = 8;
    if (slot > 16384) slot = 16384;
    if (n < 2) n = 2;
    if (n > 16) n = 16;

    nc_hub_t *ab = nc_hub_create("d_ab", slot, n, NC_HUB_SRAM, 64);
    nc_hub_t *ba = nc_hub_create("d_ba", slot, n, NC_HUB_SRAM, 64);
    if (ab == NULL || ba == NULL) {
        if (ab != NULL) nc_hub_destroy(ab);
        if (ba != NULL) nc_hub_destroy(ba);
        return false;
    }

    duplex_ep_t a = { .tx = ab, .rx = ba, .expect = 1 };
    duplex_ep_t b = { .tx = ba, .rx = ab, .expect = 1 };

    TaskHandle_t ta = NULL;
    TaskHandle_t tb = NULL;
    xTaskCreatePinnedToCore(hub_duplex_task, "dpx_a", 4096, &a, 1, &ta, 1);
    xTaskCreatePinnedToCore(hub_duplex_task, "dpx_b", 4096, &b, 1, &tb, 0);
    if (ta == NULL || tb == NULL) {
        a.start = b.start = true;
        a.stop = b.stop = true;
        while (!a.done || !b.done) {
            vTaskDelay(1);
        }
        nc_hub_destroy(ab);
        nc_hub_destroy(ba);
        return false;
    }

    const int64_t t0 = esp_timer_get_time();
    a.start = b.start = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    a.stop = b.stop = true;
    const int64_t us = esp_timer_get_time() - t0;
    while (!a.done || !b.done) {
        vTaskDelay(1);
    }

    uint32_t drain_ab = 0;
    uint32_t drain_ba = 0;
    while (nc_hub_read_begin(ab) != NULL) {
        nc_hub_read_end(ab);
        drain_ab++;
    }
    while (nc_hub_read_begin(ba) != NULL) {
        nc_hub_read_end(ba);
        drain_ba++;
    }

    out->leak_ab = hub_stress_leak(ab);
    out->leak_ba = hub_stress_leak(ba);
    out->us = (uint32_t)us;
    out->a_sent = a.sent;
    out->a_recv = a.recv;
    out->a_full = a.full;
    out->a_empty = a.empty;
    out->a_torn = a.torn;
    out->a_seq_err = a.seq_err;
    out->b_sent = b.sent;
    out->b_recv = b.recv;
    out->b_full = b.full;
    out->b_empty = b.empty;
    out->b_torn = b.torn;
    out->b_seq_err = b.seq_err;
    out->drain_ab = drain_ab;
    out->drain_ba = drain_ba;

    nc_hub_destroy(ab);
    nc_hub_destroy(ba);
    return true;
}

static void on_hub_duplex(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_duplex_busy) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    s_duplex_busy = true;

    uint32_t ms = 3000;
    if (f->len >= 4) {
        ms = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
             ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (ms < 500) {
        ms = 500;
    }
    if (ms > 10000) {
        ms = 10000;
    }

    duplex_result_t r;
    if (!duplex_run(64, 8, ms, &r)) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"hub\"}");
        s_duplex_busy = false;
        return;
    }

    char *json = s_duplex_json[xPortGetCoreID() & 1];
    const size_t cap = sizeof(s_duplex_json[0]);
    const uint32_t a2b_ps = r.us ? (uint32_t)((uint64_t)r.b_recv * 1000000u / r.us) : 0;
    const uint32_t b2a_ps = r.us ? (uint32_t)((uint64_t)r.a_recv * 1000000u / r.us) : 0;

    size_t off = 0;
    off = hs_append(json, cap, off,
                    "{\"ms\":%u,\"slot\":64,\"n\":8,\"a2b\":{"
                    "\"sent\":%u,\"recv\":%u,\"per_sec\":%u,\"full\":%u,\"empty\":%u,"
                    "\"torn\":%u,\"seq_err\":%u,\"drain\":%u},"
                    "\"b2a\":{\"sent\":%u,\"recv\":%u,\"per_sec\":%u,\"full\":%u,\"empty\":%u,"
                    "\"torn\":%u,\"seq_err\":%u,\"drain\":%u},"
                    "\"leak\":[%d,%d],\"us\":%u}",
                    (unsigned)ms,
                    (unsigned)r.a_sent, (unsigned)r.b_recv, (unsigned)a2b_ps,
                    (unsigned)r.a_full, (unsigned)r.b_empty,
                    (unsigned)r.b_torn, (unsigned)r.b_seq_err, (unsigned)r.drain_ab,
                    (unsigned)r.b_sent, (unsigned)r.a_recv, (unsigned)b2a_ps,
                    (unsigned)r.b_full, (unsigned)r.a_empty,
                    (unsigned)r.a_torn, (unsigned)r.a_seq_err, (unsigned)r.drain_ba,
                    r.leak_ab, r.leak_ba, (unsigned)r.us);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
    s_duplex_busy = false;
}

// ── 0xF021 HUB_DUPLEX_SWEEP：尺寸／深度梯度（每個 case 重新建環）──
#define NC4_CMD_HUB_DUPLEX_SWEEP  0xF021

static const struct {
    uint16_t slot;
    uint16_t n;
} k_sweep_cases[] = {
    { 16, 8 },   { 64, 8 },  { 256, 8 },  { 1024, 8 }, { 4096, 8 },
    { 16384, 2 }, { 8192, 6 }, { 4096, 12 }, { 32768, 1 },
    { 64, 2 },   { 64, 3 },  { 64, 16 },
};

static void on_hub_sweep(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_duplex_busy) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    s_duplex_busy = true;

    uint32_t ms = 800;
    if (f->len >= 4) {
        ms = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
             ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (ms < 300) {
        ms = 300;
    }
    if (ms > 2000) {
        ms = 2000;
    }

    char *json = s_sweep_json[xPortGetCoreID() & 1];
    const size_t cap = sizeof(s_sweep_json[0]);
    size_t off = 0;
    bool first = true;
    off = hs_append(json, cap, off, "{\"ms\":%u,\"cases\":[", (unsigned)ms);

    const size_t ncases = sizeof(k_sweep_cases) / sizeof(k_sweep_cases[0]);
    for (size_t i = 0; i < ncases; i++) {
        const uint32_t slot = k_sweep_cases[i].slot;
        const uint32_t n = k_sweep_cases[i].n;
        duplex_result_t r;
        if (!duplex_run(slot, n, ms, &r)) {
            off = hs_append(json, cap, off, "%s{\"slot\":%u,\"n\":%u,\"err\":\"hub\"}",
                            first ? "" : ",", (unsigned)slot, (unsigned)n);
            first = false;
            continue;
        }
        const uint32_t a_ps = r.us ? (uint32_t)((uint64_t)r.b_recv * 1000000u / r.us) : 0;
        const uint32_t b_ps = r.us ? (uint32_t)((uint64_t)r.a_recv * 1000000u / r.us) : 0;
        off = hs_append(json, cap, off,
                        "%s{\"slot\":%u,\"n\":%u,\"a\":%u,\"b\":%u,"
                        "\"kb\":[%u,%u],\"torn\":[%u,%u],\"order\":[%u,%u],"
                        "\"leak\":[%d,%d]}",
                        first ? "" : ",", (unsigned)slot, (unsigned)n,
                        (unsigned)a_ps, (unsigned)b_ps,
                        (unsigned)((uint64_t)a_ps * slot / 1024u),
                        (unsigned)((uint64_t)b_ps * slot / 1024u),
                        (unsigned)r.b_torn, (unsigned)r.a_torn,
                        (unsigned)r.b_seq_err, (unsigned)r.a_seq_err,
                        r.leak_ab, r.leak_ba);
        first = false;
    }

    off = hs_append(json, cap, off, "]}");
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
    s_duplex_busy = false;
}

// ── 0xF022 HUB_DUPLEX4：4 任務、純方向（tx 只送、rx 只收）──
// 每方向兩個任務，兩個方向同時跑；優先嘗試吃最多的內部 SRAM（16 KB × 3 × 2 環 = 96 KB）。
#define NC4_CMD_HUB_DUPLEX4  0xF022

typedef struct {
    nc_hub_t *hub;
    volatile bool *start;
    volatile bool *stop;
    volatile bool *done;
    bool is_tx;
    uint32_t sent, full, seq;
    uint32_t recv, empty, torn, seq_err, expect;
} d4_ep_t;

static void d4_tx_task(void *arg)
{
    d4_ep_t *e = arg;
    while (!*e->start) {
        vTaskDelay(1);
    }
    const size_t sz = nc_hub_slot_size(e->hub);
    uint32_t seq = 1;
    uint32_t spins = 0;
    while (!*e->stop) {
        uint8_t *p = nc_hub_write_begin(e->hub);
        if (p != NULL) {
            memcpy(p, &seq, sizeof(seq));
            memset(p + sizeof(seq), (uint8_t)seq, sz - sizeof(seq));
            nc_hub_write_commit(e->hub);
            e->sent++;
            seq++;
        } else {
            e->full++;
            taskYIELD();          // 讓同核的 rx 先跑，不要燒完整個 tick
        }
        if ((++spins & 0x1FF) == 0) {
            vTaskDelay(1);        // 10 s 長跑時確保 idle 定期執行（TWDT）
        }
    }
    *e->done = true;
    vTaskDelete(NULL);
}

static void d4_rx_task(void *arg)
{
    d4_ep_t *e = arg;
    while (!*e->start) {
        vTaskDelay(1);
    }
    const size_t sz = nc_hub_slot_size(e->hub);
    uint32_t expect = 1;
    uint32_t spins = 0;
    while (!*e->stop) {
        uint8_t *q = nc_hub_read_begin(e->hub);
        if (q != NULL) {
            uint32_t v = 0;
            memcpy(&v, q, sizeof(v));
            const uint32_t want32 = (uint32_t)(uint8_t)v * 0x01010101u;
            // q 是 64-byte 對齊、k 是 4 的倍數 → 直接 32-bit load，避免 memcpy 呼叫。
            for (size_t k = sizeof(v); k + sizeof(uint32_t) <= sz; k += sizeof(uint32_t)) {
                if (*(const uint32_t *)(const void *)(q + k) != want32) {
                    e->torn++;
                    break;
                }
            }
            if (v != expect) {
                e->seq_err++;
            }
            expect = v + 1;
            nc_hub_read_end(e->hub);
            e->recv++;
        } else {
            e->empty++;
            taskYIELD();
        }
        if ((++spins & 0x1FF) == 0) {
            vTaskDelay(1);        // 10 s 長跑時確保 idle 定期執行（TWDT）
        }
    }
    e->expect = expect;
    *e->done = true;
    vTaskDelete(NULL);
}

static char s_d4_json[2][1400];

static void on_hub_duplex4(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_duplex_busy) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    s_duplex_busy = true;

    uint32_t ms = 3000;
    if (f->len >= 4) {
        ms = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
             ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (ms < 500) {
        ms = 500;
    }
    if (ms > 15000) {
        ms = 15000;
    }

    // 由大到小嘗試；任何一組失敗就往下退，回報實際用到的組合。
    static const struct {
        uint32_t slot;
        uint32_t n;
    } tries[] = {
        { 32768, 2 }, { 32768, 1 }, { 16384, 3 }, { 16384, 2 },
        { 8192, 6 },  { 8192, 4 },  { 4096, 12 }, { 4096, 8 },
    };

    nc_hub_t *ab = NULL;
    nc_hub_t *ba = NULL;
    uint32_t slot = 0;
    uint32_t n = 0;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        ab = nc_hub_create("d4_ab", tries[i].slot, tries[i].n, NC_HUB_SRAM, 64);
        if (ab == NULL) {
            continue;
        }
        ba = nc_hub_create("d4_ba", tries[i].slot, tries[i].n, NC_HUB_SRAM, 64);
        if (ba == NULL) {
            nc_hub_destroy(ab);
            ab = NULL;
            continue;
        }
        slot = tries[i].slot;
        n = tries[i].n;
        break;
    }
    if (ab == NULL || ba == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem\"}");
        s_duplex_busy = false;
        return;
    }

    volatile bool start = false;
    volatile bool stop = false;
    volatile bool done0 = false;
    volatile bool done1 = false;
    volatile bool done2 = false;
    volatile bool done3 = false;
    d4_ep_t atx = { .hub = ab, .start = &start, .stop = &stop, .done = &done0, .is_tx = true };
    d4_ep_t brx = { .hub = ab, .start = &start, .stop = &stop, .done = &done1 };
    d4_ep_t btx = { .hub = ba, .start = &start, .stop = &stop, .done = &done2, .is_tx = true };
    d4_ep_t arx = { .hub = ba, .start = &start, .stop = &stop, .done = &done3 };

    TaskHandle_t h0 = NULL, h1 = NULL, h2 = NULL, h3 = NULL;
    xTaskCreatePinnedToCore(d4_tx_task, "d4_atx", 4096, &atx, 1, &h0, 1);
    xTaskCreatePinnedToCore(d4_rx_task, "d4_brx", 4096, &brx, 1, &h1, 0);
    xTaskCreatePinnedToCore(d4_tx_task, "d4_btx", 4096, &btx, 1, &h2, 0);
    xTaskCreatePinnedToCore(d4_rx_task, "d4_arx", 4096, &arx, 1, &h3, 1);
    if (h0 == NULL || h1 == NULL || h2 == NULL || h3 == NULL) {
        start = true;
        stop = true;
        while (!done0 || !done1 || !done2 || !done3) {
            vTaskDelay(1);
        }
        nc_hub_destroy(ab);
        nc_hub_destroy(ba);
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"task\"}");
        s_duplex_busy = false;
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    start = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    stop = true;
    const int64_t us = esp_timer_get_time() - t0;
    while (!done0 || !done1 || !done2 || !done3) {
        vTaskDelay(1);
    }

    uint32_t drain_ab = 0;
    uint32_t drain_ba = 0;
    while (nc_hub_read_begin(ab) != NULL) {
        nc_hub_read_end(ab);
        drain_ab++;
    }
    while (nc_hub_read_begin(ba) != NULL) {
        nc_hub_read_end(ba);
        drain_ba++;
    }
    const int leak_ab = hub_stress_leak(ab);
    const int leak_ba = hub_stress_leak(ba);

    const uint32_t a_ps = us > 0 ? (uint32_t)((uint64_t)brx.recv * 1000000u / (uint64_t)us) : 0;
    const uint32_t b_ps = us > 0 ? (uint32_t)((uint64_t)arx.recv * 1000000u / (uint64_t)us) : 0;

    char *json = s_d4_json[xPortGetCoreID() & 1];
    const size_t cap = sizeof(s_d4_json[0]);
    size_t off = 0;
    off = hs_append(json, cap, off,
                    "{\"ms\":%u,\"slot\":%u,\"n\":%u,\"sram\":%u,"
                    "\"a2b\":{\"sent\":%u,\"recv\":%u,\"per_sec\":%u,\"kb\":%u,"
                    "\"full\":%u,\"empty\":%u,\"torn\":%u,\"seq_err\":%u,\"drain\":%u},"
                    "\"b2a\":{\"sent\":%u,\"recv\":%u,\"per_sec\":%u,\"kb\":%u,"
                    "\"full\":%u,\"empty\":%u,\"torn\":%u,\"seq_err\":%u,\"drain\":%u},"
                    "\"leak\":[%d,%d],\"us\":%u}",
                    (unsigned)ms, (unsigned)slot, (unsigned)n,
                    (unsigned)(slot * n * 2),
                    (unsigned)atx.sent, (unsigned)brx.recv, (unsigned)a_ps,
                    (unsigned)((uint64_t)a_ps * slot / 1024u),
                    (unsigned)atx.full, (unsigned)brx.empty,
                    (unsigned)brx.torn, (unsigned)brx.seq_err, (unsigned)drain_ab,
                    (unsigned)btx.sent, (unsigned)arx.recv, (unsigned)b_ps,
                    (unsigned)((uint64_t)b_ps * slot / 1024u),
                    (unsigned)btx.full, (unsigned)arx.empty,
                    (unsigned)arx.torn, (unsigned)arx.seq_err, (unsigned)drain_ba,
                    leak_ab, leak_ba, (unsigned)us);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);

    nc_hub_destroy(ab);
    nc_hub_destroy(ba);
    s_duplex_busy = false;
}

// ── 0xF023 HUB_SINGLE：單核（同一任務）交換，量 API 本身成本 ──
#define NC4_CMD_HUB_SINGLE  0xF023

static char s_single_json[2][1000];

static void on_hub_single(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_duplex_busy) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    s_duplex_busy = true;

    uint32_t iters = 20000;
    if (f->len >= 4) {
        iters = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (iters < 1000) {
        iters = 1000;
    }
    if (iters > 100000) {
        iters = 100000;
    }

    // mode bit0=1：純 copy；bit1=1：純位址交換（完全不搬資料，只寫/讀 4 B 序號）。
    uint16_t mode = 0;
    if (f->len >= 6) {
        mode = (uint16_t)(f->data[4] | (f->data[5] << 8));
    }

    static const uint16_t sizes[] = { 16, 64, 256, 1024, 4096, 16384, 32768 };
    char *json = s_single_json[xPortGetCoreID() & 1];
    const size_t cap = sizeof(s_single_json[0]);
    size_t off = 0;
    bool first = true;
    off = hs_append(json, cap, off, "{\"iters\":%u,\"mode\":\"%s\",\"cases\":[",
                    (unsigned)iters,
                    (mode & 2) ? "pointer" : ((mode & 1) ? "copy" : "fill+verify"));

    for (size_t c = 0; c < sizeof(sizes) / sizeof(sizes[0]); c++) {
        const uint32_t sz = sizes[c];
        nc_hub_t *h = nc_hub_create("sgl", sz, 2, NC_HUB_SRAM, 64);
        if (h == NULL) {
            off = hs_append(json, cap, off, "%s{\"slot\":%u,\"err\":\"hub\"}",
                            first ? "" : ",", (unsigned)sz);
            first = false;
            continue;
        }
        uint8_t *src = NULL;
        uint8_t *dst = NULL;
        if ((mode & 1) != 0 && (mode & 2) == 0) {
            const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
            src = heap_caps_aligned_alloc(64, sz, caps);
            dst = heap_caps_aligned_alloc(64, sz, caps);
            if (src == NULL || dst == NULL) {
                free(src);
                free(dst);
                off = hs_append(json, cap, off, "%s{\"slot\":%u,\"err\":\"nomem\"}",
                                first ? "" : ",", (unsigned)sz);
                first = false;
                nc_hub_destroy(h);
                continue;
            }
            memset(src, 0xA5, sz);
        }

        uint32_t torn = 0;
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t i = 1; i <= iters; i++) {
            uint8_t *p = nc_hub_write_begin(h);
            if (p == NULL) {
                torn++;
                continue;
            }
            if ((mode & 2) != 0) {
                memcpy(p, &i, sizeof(i));      // 只寫序號，不搬 payload
            } else if ((mode & 1) != 0) {
                memcpy(p, src, sz);
            } else {
                memcpy(p, &i, sizeof(i));
                memset(p + sizeof(i), (uint8_t)i, sz - sizeof(i));
            }
            nc_hub_write_commit(h);

            uint32_t gen = 0;
            uint8_t *q = nc_hub_read_begin_gen(h, &gen);
            if (q == NULL) {
                torn++;
                continue;
            }
            if ((mode & 2) != 0) {
                uint32_t v = 0;
                memcpy(&v, q, sizeof(v));
                if (v != i) {
                    torn++;
                }
            } else if ((mode & 1) != 0) {
                memcpy(dst, q, sz);
            } else {
                uint32_t v = 0;
                memcpy(&v, q, sizeof(v));
                const uint32_t w32 = (uint32_t)(uint8_t)v * 0x01010101u;
                for (size_t k = sizeof(v); k + sizeof(uint32_t) <= sz; k += sizeof(uint32_t)) {
                    if (*(const uint32_t *)(const void *)(q + k) != w32) {
                        torn++;
                        break;
                    }
                }
                if (gen != v) {
                    torn++;
                }
            }
            nc_hub_read_end(h);
            if ((i & 0x1FF) == 0) {
                vTaskDelay(1);   // 單核長跑也要餵 idle（TWDT）
            }
        }
        free(src);
        free(dst);
        const int64_t us = esp_timer_get_time() - t0;
        const uint32_t ps = us > 0 ? (uint32_t)((uint64_t)iters * 1000000u / (uint64_t)us) : 0;
        const uint32_t mbs = us > 0 ? (uint32_t)((uint64_t)iters * sz / (uint64_t)us) : 0;
        off = hs_append(json, cap, off,
                        "%s{\"slot\":%u,\"per_sec\":%u,\"mbps\":%u,\"us\":%u,\"torn\":%u}",
                        first ? "" : ",", (unsigned)sz, (unsigned)ps, (unsigned)mbs,
                        (unsigned)us, (unsigned)torn);
        first = false;
        nc_hub_destroy(h);
    }

    off = hs_append(json, cap, off, "]}");
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
    s_duplex_busy = false;
}

// ── 0xF024 HUB_MESH：N 個任務 all-to-all，每對一條單向 hub ──
// payload = [u32 ms][u16 nodes][u16 slot]；nodes 2–6（6 → 30 條鏈）。
#define NC4_CMD_HUB_MESH  0xF024
#define MESH_MAX_NODES    6

typedef struct {
    nc_hub_t *out[MESH_MAX_NODES];
    nc_hub_t *in[MESH_MAX_NODES];
    uint32_t seq[MESH_MAX_NODES];
    uint32_t expect[MESH_MAX_NODES];
    uint32_t sent, recv, full, empty, torn, seq_err;
    uint32_t idx, nodes;
    volatile bool *start;
    volatile bool *stop;
    volatile bool *done;
} mesh_node_t;

static mesh_node_t s_mesh[MESH_MAX_NODES];
static nc_hub_t *s_mesh_hub[MESH_MAX_NODES][MESH_MAX_NODES];
static volatile bool s_mesh_done[MESH_MAX_NODES];
static char s_mesh_json[2][1000];

static void mesh_task(void *arg)
{
    mesh_node_t *e = arg;
    while (!*e->start) {
        vTaskDelay(1);
    }
    size_t sz = 0;
    for (uint32_t j = 0; j < e->nodes; j++) {
        if (j != e->idx) {
            sz = nc_hub_slot_size(e->out[j]);
            break;
        }
    }
    uint32_t spins = 0;
    while (!*e->stop) {
        for (uint32_t j = 0; j < e->nodes; j++) {
            if (j == e->idx) {
                continue;
            }
            uint8_t *p = nc_hub_write_begin(e->out[j]);
            if (p != NULL) {
                const uint32_t v = e->seq[j]++;
                memcpy(p, &v, sizeof(v));
                memset(p + sizeof(v), (uint8_t)v, sz - sizeof(v));
                nc_hub_write_commit(e->out[j]);
                e->sent++;
            } else {
                e->full++;
            }

            uint8_t *q = nc_hub_read_begin(e->in[j]);
            if (q != NULL) {
                uint32_t v = 0;
                memcpy(&v, q, sizeof(v));
                const uint32_t w32 = (uint32_t)(uint8_t)v * 0x01010101u;
                for (size_t k = sizeof(v); k + sizeof(uint32_t) <= sz; k += sizeof(uint32_t)) {
                    if (*(const uint32_t *)(const void *)(q + k) != w32) {
                        e->torn++;
                        break;
                    }
                }
                if (v != e->expect[j]) {
                    e->seq_err++;
                }
                e->expect[j] = v + 1;
                nc_hub_read_end(e->in[j]);
                e->recv++;
            } else {
                e->empty++;
            }
        }
        // 6 節點 = 每核 3 個 prio-1 任務；太晚讓出會餓死 idle → TWDT。
        if ((++spins & 0x3F) == 0) {
            vTaskDelay(1);
        }
    }
    *e->done = true;
    vTaskDelete(NULL);
}

static void on_hub_mesh(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_duplex_busy) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    s_duplex_busy = true;

    uint32_t ms = 2000;
    uint32_t nodes = 4;
    uint32_t slot = 256;
    if (f->len >= 4) {
        ms = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
             ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    if (f->len >= 6) {
        nodes = (uint16_t)(f->data[4] | (f->data[5] << 8));
    }
    if (f->len >= 8) {
        slot = (uint16_t)(f->data[6] | (f->data[7] << 8));
    }
    if (ms < 500) ms = 500;
    if (ms > 15000) ms = 15000;
    if (nodes < 2) nodes = 2;
    if (nodes > MESH_MAX_NODES) nodes = MESH_MAX_NODES;
    if (slot < 64) slot = 64;
    if (slot > 2048) slot = 2048;

    memset(s_mesh_hub, 0, sizeof(s_mesh_hub));
    bool ok = true;
    for (uint32_t i = 0; i < nodes && ok; i++) {
        for (uint32_t j = i + 1; j < nodes; j++) {
            s_mesh_hub[i][j] = nc_hub_create("mesh", slot, 4, NC_HUB_SRAM, 64);
            s_mesh_hub[j][i] = nc_hub_create("mesh", slot, 4, NC_HUB_SRAM, 64);
            if (s_mesh_hub[i][j] == NULL || s_mesh_hub[j][i] == NULL) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        for (uint32_t i = 0; i < MESH_MAX_NODES; i++) {
            for (uint32_t j = 0; j < MESH_MAX_NODES; j++) {
                if (s_mesh_hub[i][j] != NULL) {
                    nc_hub_destroy(s_mesh_hub[i][j]);
                    s_mesh_hub[i][j] = NULL;
                }
            }
        }
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem\"}");
        s_duplex_busy = false;
        return;
    }

    volatile bool start = false;
    volatile bool stop = false;
    TaskHandle_t th[MESH_MAX_NODES] = { 0 };
    for (uint32_t i = 0; i < nodes; i++) {
        memset(&s_mesh[i], 0, sizeof(s_mesh[i]));
        s_mesh[i].idx = i;
        s_mesh[i].nodes = nodes;
        s_mesh[i].start = &start;
        s_mesh[i].stop = &stop;
        s_mesh[i].done = &s_mesh_done[i];
        s_mesh_done[i] = false;
        for (uint32_t j = 0; j < nodes; j++) {
            if (j == i) {
                continue;
            }
            s_mesh[i].out[j] = s_mesh_hub[i][j];
            s_mesh[i].in[j] = s_mesh_hub[j][i];
            s_mesh[i].seq[j] = 1;
            s_mesh[i].expect[j] = 1;
        }
        // prio 0（與 idle 同級）：多任務時仍能靠 time slicing 保證 idle 執行，
        // 避免 6 節點長跑時 TWDT 餓死。
        xTaskCreatePinnedToCore(mesh_task, "mesh", 4096, &s_mesh[i], 0, &th[i], (int)(i & 1));
        if (th[i] == NULL) {
            s_mesh_done[i] = true;   // 建立失敗：不要讓 handler 等不到 done
        }
    }

    const int64_t t0 = esp_timer_get_time();
    start = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    stop = true;
    const int64_t us = esp_timer_get_time() - t0;
    for (uint32_t i = 0; i < nodes; i++) {
        while (!s_mesh_done[i]) {
            vTaskDelay(1);
        }
    }

    uint32_t sent = 0, recv = 0, full = 0, empty = 0, torn = 0, order = 0;
    for (uint32_t i = 0; i < nodes; i++) {
        sent += s_mesh[i].sent;
        recv += s_mesh[i].recv;
        full += s_mesh[i].full;
        empty += s_mesh[i].empty;
        torn += s_mesh[i].torn;
        order += s_mesh[i].seq_err;
    }
    int leak = 0;
    for (uint32_t i = 0; i < nodes; i++) {
        for (uint32_t j = 0; j < nodes; j++) {
            if (i == j || s_mesh_hub[i][j] == NULL) {
                continue;
            }
            while (nc_hub_read_begin(s_mesh_hub[i][j]) != NULL) {
                nc_hub_read_end(s_mesh_hub[i][j]);
            }
            leak += hub_stress_leak(s_mesh_hub[i][j]);
        }
    }

    const uint32_t ps = us > 0 ? (uint32_t)((uint64_t)recv * 1000000u / (uint64_t)us) : 0;
    const uint32_t mbs = us > 0 ? (uint32_t)((uint64_t)recv * slot / (uint64_t)us) : 0;

    char *json = s_mesh_json[xPortGetCoreID() & 1];
    const size_t cap = sizeof(s_mesh_json[0]);
    size_t off = 0;
    off = hs_append(json, cap, off,
                    "{\"ms\":%u,\"nodes\":%u,\"slot\":%u,\"links\":%u,"
                    "\"sent\":%u,\"recv\":%u,\"per_sec\":%u,\"mbps\":%u,"
                    "\"full\":%u,\"empty\":%u,\"torn\":%u,\"order\":%u,"
                    "\"leak\":%d,\"us\":%u}",
                    (unsigned)ms, (unsigned)nodes, (unsigned)slot,
                    (unsigned)(nodes * (nodes - 1)),
                    (unsigned)sent, (unsigned)recv, (unsigned)ps, (unsigned)mbs,
                    (unsigned)full, (unsigned)empty, (unsigned)torn, (unsigned)order,
                    leak, (unsigned)us);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);

    for (uint32_t i = 0; i < MESH_MAX_NODES; i++) {
        for (uint32_t j = 0; j < MESH_MAX_NODES; j++) {
            if (s_mesh_hub[i][j] != NULL) {
                nc_hub_destroy(s_mesh_hub[i][j]);
                s_mesh_hub[i][j] = NULL;
            }
        }
    }
    s_duplex_busy = false;
}

// ── 0xF025–0xF027：執行期 GPIO 設定／讀寫（兩板接線測試用）──
//
// 為什麼要這幾支：板子接好之後 GPIO0/EN 可能被拉住，重燒要拔線；
// 有了這幾支就可以經 USB 直接配置／讀寫任何 GPIO，唔使改 hardware.json 再 reboot。
// payload：
//   CFG  [pin:u8][mode:u8 0=out 1=in][pull:u8 0=none 1=up 2=down][level:u8]
//   SET  [pin:u8][level:u8]
//   GET  [n:u8][pin:u8 ...]  → {"levels":[...]}
#define NC4_CMD_GPIO_CFG  0xF025
#define NC4_CMD_GPIO_SET  0xF026
#define NC4_CMD_GPIO_GET  0xF027

static bool gpio_pin_ok(int pin)
{
    // 19/20 = USB D-/D+；唔好碰。其餘 0..48 留給測試。
    return pin >= 0 && pin <= 48 && pin != 19 && pin != 20;
}

static void on_gpio_cfg(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 4) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int pin = f->data[0];
    const int mode = f->data[1];
    const int pull = f->data[2];
    const int level = f->data[3];
    if (!gpio_pin_ok(pin) || mode > 1 || pull > 2) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"pin\"}");
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT,
        .pull_up_en = (pull == 1) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == 2) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t e = gpio_config(&io);
    if (e == ESP_OK && mode == 0) {
        gpio_set_level(pin, level ? 1 : 0);
    }
    char json[96];
    snprintf(json, sizeof(json),
             "{\"pin\":%d,\"mode\":\"%s\",\"pull\":%d,\"level\":%d,\"err\":\"%s\"}",
             pin, mode ? "in" : "out", pull, gpio_get_level(pin), esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_gpio_set(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 2) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int pin = f->data[0];
    const int level = f->data[1];
    if (!gpio_pin_ok(pin)) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"pin\"}");
        return;
    }
    gpio_set_level(pin, level ? 1 : 0);
    char json[64];
    snprintf(json, sizeof(json), "{\"pin\":%d,\"level\":%d}", pin, gpio_get_level(pin));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_gpio_get(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int n = f->data[0];
    if (n < 1 || n > 16 || (int)f->len < 1 + n) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    char json[256];
    size_t off = 0;
    off += snprintf(json + off, sizeof(json) - off, "{\"n\":%d,\"levels\":[", n);
    for (int i = 0; i < n; i++) {
        const int pin = f->data[1 + i];
        if (!gpio_pin_ok(pin)) {
            off += snprintf(json + off, sizeof(json) - off, "%s-1", i ? "," : "");
        } else {
            off += snprintf(json + off, sizeof(json) - off, "%s%d", i ? "," : "",
                            gpio_get_level(pin));
        }
    }
    snprintf(json + off, sizeof(json) - off, "]}");
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

// ── 0xF035–0xF037：經 nc_hw owner 的 GPIO（不是直接 gpio_set_level）──
//
// 0xF035  [op][pin][mode][pull][level]  → 入 "gpio" command queue，回 queued
// 0xF036  無                            → 讀 "gpio" snapshot
// 0xF037  無                            → resource/claim 表
#define NC4_CMD_HW_GPIO  0xF03E
#define NC4_CMD_HW_SNAP  0xF036
#define NC4_CMD_HW_INFO  0xF037

static void on_hw_gpio(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 2) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    nc_hw_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = f->data[0];
    c.pin = f->data[1];
    if (c.op == NC_HW_GPIO_CFG) {
        if (f->len < 5) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
            return;
        }
        c.arg = f->data[2];
        c.data[0] = f->data[3];
        c.data[1] = f->data[4];
    } else if (c.op == NC_HW_GPIO_SET) {
        if (f->len < 3) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
            return;
        }
        c.arg = f->data[2];
    } else {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"op\"}");
        return;
    }
    const esp_err_t e = nc_hw_send(NC_HW_GPIO_NAME, &c);
    char j[128];
    snprintf(j, sizeof(j), "{\"op\":%u,\"pin\":%u,\"queued\":%d,\"err\":\"%s\"}",
             c.op, c.pin, e == ESP_OK ? 1 : 0, esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

static void on_hw_snap(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    nc_hw_gpio_snap_t s;
    const size_t n = nc_hw_snapshot(NC_HW_GPIO_NAME, &s, sizeof(s));
    char j[256];
    if (n != sizeof(s)) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"no_snapshot\"}");
        return;
    }
    snprintf(j, sizeof(j), "{\"levels\":%llu,\"low\":%lu,\"high\":%lu}",
             (unsigned long long)s.levels,
             (unsigned long)(s.levels & 0xFFFFFFFFu),
             (unsigned long)(s.levels >> 32));
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

static void on_hw_info(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char j[768];
    (void)nc_hw_json(j, sizeof(j));
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

// nc_wifi 在 main 元件；test_action 不 reverse-depend main，所以這裡只宣告用到的介面。
// 未編入 WiFi 時 nc_wifi.c 仍有同名 stub（回 not_supported），所以不用 #if。
int       nc_wifi_cred_count(void);
esp_err_t nc_wifi_cred_add(const char *ssid, const char *pw);
esp_err_t nc_wifi_cred_remove(const char *ssid);
esp_err_t nc_wifi_cred_clear(void);
size_t    nc_wifi_cred_json(char *buf, size_t cap);

#define NC4_CMD_PWM_STAY  0xF03C
#define NC4_CMD_PWM_ZERO  0xF03D

static void pwm_report(nc4_ctx_t *ctx, int idx, esp_err_t e)
{
    char j[128];
    snprintf(j, sizeof(j), "{\"idx\":%d,\"duty\":%u,\"err\":\"%s\"}",
             idx, (unsigned)nc_pwm_drv_get(idx), esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

static void on_pwm_stay(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int idx = f->data[0];
    pwm_report(ctx, idx, nc_pwm_drv_stay(idx));
}

static void on_pwm_zero(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int idx = f->data[0];
    pwm_report(ctx, idx, nc_pwm_drv_zero(idx));
}

#define NC4_CMD_UART_INFO 0xF03B

static void on_uart_info(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char j[160];
    snprintf(j, sizeof(j), "{\"ready\":%d,\"port\":%d,\"baud\":%u}",
             nc_uart_drv_ready() ? 1 : 0, nc_uart_drv_port(),
             (unsigned)nc_uart_drv_baud());
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

// ── 0xF040／0xF041 NETBENCH：網絡 RX 真實統計 ──
//
// 主機連續送 [seq u32] + payload；每幀 handler 只記帳，不假裝成功：
//   frames／bytes／seq_max／lost／ooo／elapsed
// 主機最後送 op=2 取 result，跟實際送出的 seq 對照。
#define NC4_CMD_NETBENCH        0xF040
#define NC4_CMD_NETBENCH_RESULT 0xF041

static struct {
    bool     active;
    uint32_t frames;
    uint32_t bytes;
    uint32_t seq_max;
    uint32_t last_seq;
    uint32_t lost;
    uint32_t ooo;
    int64_t  t0_us;
    int64_t  t1_us;
} s_nb;

static uint32_t nb_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void on_netbench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const uint8_t op = f->data[0];
    if (op == 0) {
        memset(&s_nb, 0, sizeof(s_nb));
        s_nb.active = true;
        s_nb.t0_us = esp_timer_get_time();
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"started\":1}");
        return;
    }
    if (op == 1 || op == 3) {
        if (f->len < 5) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"seq\"}");
            return;
        }
        if (!s_nb.active) {
            s_nb.active = true;
            s_nb.t0_us = esp_timer_get_time();
        }
        const uint32_t seq = nb_u32(&f->data[1]);
        if (s_nb.frames == 0) {
            s_nb.seq_max = seq;
            s_nb.last_seq = seq;
        } else if (seq > s_nb.last_seq + 1u) {
            s_nb.lost += seq - s_nb.last_seq - 1u;
            s_nb.last_seq = seq;
        } else if (seq <= s_nb.last_seq) {
            s_nb.ooo++;
        } else {
            s_nb.last_seq = seq;
        }
        if (seq > s_nb.seq_max) {
            s_nb.seq_max = seq;
        }
        s_nb.frames++;
        s_nb.bytes += (uint32_t)(f->len - 5u);
        s_nb.t1_us = esp_timer_get_time();
        if (op == 1) {
            char j[80];
            snprintf(j, sizeof(j), "{\"seq\":%u,\"n\":%u}", (unsigned)seq,
                     (unsigned)s_nb.frames);
            reply_json(ctx, NC4_CMD_HUB_RSP, j);
        }
        return;
    }
    if (op == 2) {
        s_nb.active = false;
        const uint64_t us = (s_nb.t1_us > s_nb.t0_us) ?
                            (uint64_t)(s_nb.t1_us - s_nb.t0_us) : 0;
        const uint32_t expected = s_nb.seq_max + 1u;
        const uint32_t missing = (expected > s_nb.frames) ? expected - s_nb.frames : 0;
        const double fps = (us != 0) ? (double)s_nb.frames * 1000000.0 / (double)us : 0.0;
        const double kbps = (us != 0) ? (double)s_nb.bytes * 1000000.0 / (double)us / 1024.0 : 0.0;
        char j[320];
        snprintf(j, sizeof(j),
                 "{\"frames\":%u,\"bytes\":%u,\"seq_max\":%u,\"expected\":%u,"
                 "\"lost\":%u,\"missing\":%u,\"ooo\":%u,\"us\":%llu,"
                 "\"fps\":%.1f,\"kbps\":%.2f}",
                 (unsigned)s_nb.frames, (unsigned)s_nb.bytes, (unsigned)s_nb.seq_max,
                 (unsigned)expected, (unsigned)s_nb.lost, (unsigned)missing,
                 (unsigned)s_nb.ooo, (unsigned long long)us, fps, kbps);
        reply_json(ctx, NC4_CMD_HUB_RSP, j);
        return;
    }
    reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"op\"}");
}

// ── 0xF039–0xF03A：PWM（LEDC）測試 ──
#define NC4_CMD_PWM_SET   0xF039
#define NC4_CMD_PWM_INFO  0xF03A

static void on_pwm_set(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 4) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int idx = f->data[0];
    const uint32_t duty = (uint32_t)f->data[1] | ((uint32_t)f->data[2] << 8) |
                          ((uint32_t)f->data[3] << 16);
    const esp_err_t e = nc_pwm_drv_set(idx, duty);
    char j[128];
    snprintf(j, sizeof(j), "{\"idx\":%d,\"count\":%d,\"duty\":%u,\"err\":\"%s\"}",
             idx, nc_pwm_drv_count(), (unsigned)nc_pwm_drv_get(idx),
             esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

static void on_pwm_info(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char j[256];
    int off = snprintf(j, sizeof(j), "{\"count\":%d,\"duty\":[", nc_pwm_drv_count());
    for (int i = 0; i < nc_pwm_drv_count() && off > 0 && (size_t)off < sizeof(j); i++) {
        off += snprintf(j + off, sizeof(j) - (size_t)off, "%s%u", i ? "," : "",
                        (unsigned)nc_pwm_drv_get(i));
    }
    if (off > 0 && (size_t)off + 2 < sizeof(j)) {
        j[off++] = ']';
        j[off++] = '}';
        j[off] = '\0';
    }
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

// ── 0xF038：Wi-Fi 憑證管理（最多 10 組；只回 SSID，不回密碼）──
//   [0] list
//   [1][ssid_len][ssid][pw_len][pw] add/update
//   [2][ssid_len][ssid] remove
//   [3] clear
#define NC4_CMD_WIFI_CRED  0xF038

static void on_wifi_cred(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const uint8_t op = f->data[0];
    esp_err_t e = ESP_OK;
    if (op == 0) {
        /* list */
    } else if (op == 1) {
        if (f->len < 3) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
            return;
        }
        const int sl = f->data[1];
        if (sl < 1 || sl > 32 || 2 + sl >= f->len) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"ssid\"}");
            return;
        }
        char ssid[33];
        memcpy(ssid, f->data + 2, (size_t)sl);
        ssid[sl] = '\0';
        const int off = 2 + sl;
        const int pl = f->data[off];
        if (pl < 0 || pl > 64 || off + 1 + pl > f->len) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"pw\"}");
            return;
        }
        char pw[65];
        memcpy(pw, f->data + off + 1, (size_t)pl);
        pw[pl] = '\0';
        e = nc_wifi_cred_add(ssid, pw);
    } else if (op == 2) {
        if (f->len < 3) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
            return;
        }
        const int sl = f->data[1];
        if (sl < 1 || sl > 32 || 2 + sl > f->len) {
            reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"ssid\"}");
            return;
        }
        char ssid[33];
        memcpy(ssid, f->data + 2, (size_t)sl);
        ssid[sl] = '\0';
        e = nc_wifi_cred_remove(ssid);
    } else if (op == 3) {
        e = nc_wifi_cred_clear();
    } else {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"op\"}");
        return;
    }
    // 唔好 j[512] + out[640] 兩份同時上棧：decode 任務棧只有幾 KB。
    char out[640];
    const int pre = snprintf(out, sizeof(out), "{\"err\":\"%s\",\"cred\":",
                             esp_err_to_name(e));
    if (pre < 0 || (size_t)pre >= sizeof(out) - 2) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"trunc\"}");
        return;
    }
    size_t n = nc_wifi_cred_json(out + pre, sizeof(out) - (size_t)pre - 2);
    if (n == 0) {
        n = (size_t)snprintf(out + pre, sizeof(out) - (size_t)pre,
                             "{\"count\":0,\"ssids\":[]}");
    }
    if (pre + (int)n + 1 < (int)sizeof(out)) {
        out[pre + (int)n] = '}';
        out[pre + (int)n + 1] = '\0';
    }
    reply_json(ctx, NC4_CMD_HUB_RSP, out);
}

// ── 0xF028–0xF02A：ESP-NOW 測試（S3 WiFi；P4 冇 radio）──
//
// 為什麼要有這幾支：ESP-NOW 唔需要連 AP，但需要 WiFi radio 起來、固定頻道。
// 兩端要同頻道；broadcast peer 用 FF:FF:FF:FF:FF:FF。
// payload：
//   INIT  [ch:u8]（0 → 1）
//   SEND  [len:u8][data]（1..250）
//   STATS 無
#if NC_HAVE_ESPNOW
#define NC4_CMD_ESPNOW_INIT   0xF028
#define NC4_CMD_ESPNOW_SEND   0xF029
#define NC4_CMD_ESPNOW_STATS  0xF02A

static bool s_now_ready;
static uint8_t s_now_ch;
static volatile uint32_t s_now_rx, s_now_tx_ok, s_now_tx_fail, s_now_bad;
static volatile int s_now_last_len, s_now_last_rssi;
static uint8_t s_now_last_mac[6];
static uint8_t s_now_last[32];
static volatile int s_now_last_n;
static nc4_parser_t *s_now_parser;
static uint8_t s_now_ring[64];

typedef struct {
    uint16_t len;
    uint8_t data[250];
} now_msg_t;

static QueueHandle_t s_now_q;
static TaskHandle_t s_now_task;
static volatile uint32_t s_now_drop;

// 不在 Wi-Fi receive callback 裡 parse：ESP-NOW 在 callback 內再 send 會失敗。
// callback 只 copy 入 queue；由 task 做 parser_frame／handler／回覆。
static void now_task(void *arg)
{
    (void)arg;
    now_msg_t m;
    for (;;) {
        if (xQueueReceive(s_now_q, &m, portMAX_DELAY) == pdTRUE && s_now_parser != NULL) {
            const esp_err_t e = nc4_parser_frame(s_now_parser, m.data, m.len);
            if (e != ESP_OK) {
                s_now_bad++;
            }
        }
    }
}

// 轉寄出口：把整幀交給 ESP-NOW（broadcast）。>250 B 先回 -1（之後可加 fragmentation）。
static int now_link_write(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    if (!s_now_ready || frame == NULL || len == 0 || len > 250) {
        return -1;
    }
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return (esp_now_send(bcast, frame, len) == ESP_OK) ? (int)len : -1;
}

static void now_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    __atomic_add_fetch(&s_now_rx, 1, __ATOMIC_RELAXED);
    s_now_last_len = len;
    if (info != NULL && info->rx_ctrl != NULL) {
        s_now_last_rssi = info->rx_ctrl->rssi;
    }
    if (info != NULL && info->src_addr != NULL) {
        memcpy(s_now_last_mac, info->src_addr, 6);
    }
    const int n = (len < (int)sizeof(s_now_last)) ? len : (int)sizeof(s_now_last);
    if (n > 0 && data != NULL) {
        memcpy(s_now_last, data, (size_t)n);
    }
    s_now_last_n = n;

    // 收到的是 NC4 幀就 copy 入 queue；由 now_task 解析（唔在 Wi-Fi callback 內 send）。
    if (s_now_q != NULL && s_now_parser != NULL && data != NULL &&
        len >= NC4_HDR_LEN && len <= 250 &&
        data[0] == NC4_SOF_0 && data[1] == NC4_SOF_1) {
        now_msg_t m;
        m.len = (uint16_t)len;
        memcpy(m.data, data, (size_t)len);
        if (xQueueSend(s_now_q, &m, 0) != pdTRUE) {
            s_now_drop++;
        }
    }
}

static void now_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) {
        __atomic_add_fetch(&s_now_tx_ok, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&s_now_tx_fail, 1, __ATOMIC_RELAXED);
    }
}

static esp_err_t now_ensure(uint8_t ch, nc4_decoder_t *dec)
{
    if (s_now_ready) {
        if (ch != 0 && ch != s_now_ch) {
            const esp_err_t e = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            if (e == ESP_OK) {
                s_now_ch = ch;
            }
        }
        // 已經 init 過（例如 boot 自動 init）但 parser 未建立：補建。
        if (s_now_parser == NULL && dec != NULL) {
            s_now_parser = nc4_parser_create_in(s_now_ring, sizeof(s_now_ring), dec,
                                                now_link_write, NULL, "ESPNOW");
            ESP_LOGI(TAG, "ESP-NOW parser late create: %s", s_now_parser ? "ok" : "fail");
        }
        if (s_now_q == NULL) {
            s_now_q = xQueueCreate(8, sizeof(now_msg_t));
            if (s_now_q != NULL) {
                xTaskCreatePinnedToCore(now_task, "espnow_rx", 4096, NULL, 5, &s_now_task, 0);
            }
        }
        return ESP_OK;
    }
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        e = esp_wifi_init(&ic);
        if (e != ESP_OK) {
            return e;
        }
    }
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) {
        return e;
    }
    e = esp_wifi_start();
    if (e != ESP_OK) {
        // 已經 start 過會回錯誤；下面 set_channel 會是最終判斷。
        ESP_LOGW(TAG, "esp_wifi_start: %s（繼續）", esp_err_to_name(e));
    }
    if (ch != 0) {
        e = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (e != ESP_OK) {
            return e;
        }
    } else {
        // 0 = 沿用目前 channel（STA 連 AP 時由 AP 決定）
        uint8_t cur = 1;
        wifi_second_chan_t sec;
        if (esp_wifi_get_channel(&cur, &sec) == ESP_OK) {
            ch = cur;
        } else {
            ch = 1;
        }
    }
    e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(e));
        return e;
    }
    esp_now_register_recv_cb(now_recv_cb);
    esp_now_register_send_cb(now_send_cb);
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;   // 跟隨目前 channel（STA 連 AP 後由 AP 決定）
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    e = esp_now_add_peer(&peer);
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer(broadcast): %s", esp_err_to_name(e));
        return e;
    }
    s_now_ch = ch;
    s_now_ready = true;

    // 轉寄出口 + 接收 parser（frame mode，不需要 ring）。
    (void)nc4_link_register("ESPNOW", now_link_write, NULL);
    if (s_now_parser == NULL && dec != NULL) {
        s_now_parser = nc4_parser_create_in(s_now_ring, sizeof(s_now_ring), dec,
                                            now_link_write, NULL, "ESPNOW");
        if (s_now_parser == NULL) {
            ESP_LOGE(TAG, "ESP-NOW parser 建立失敗");
        }
    }
    if (s_now_q == NULL) {
        s_now_q = xQueueCreate(8, sizeof(now_msg_t));
        if (s_now_q != NULL) {
            xTaskCreatePinnedToCore(now_task, "espnow_rx", 4096, NULL, 5, &s_now_task, 0);
        }
    }
    return ESP_OK;
}

static void on_espnow_init(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    const uint8_t ch = (f->len >= 1) ? f->data[0] : 1;
    const esp_err_t e = now_ensure(ch, ctx->decoder);
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char json[160];
    snprintf(json, sizeof(json),
             "{\"init\":%d,\"ch\":%u,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"err\":\"%s\"}",
             s_now_ready ? 1 : 0, (unsigned)s_now_ch,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_espnow_send(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (!s_now_ready) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"not_init\"}");
        return;
    }
    if (f->len < 1 || f->data[0] == 0 || f->data[0] > 250) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const uint8_t n = f->data[0];
    if ((int)f->len < 1 + n) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const esp_err_t e = esp_now_send(bcast, f->data + 1, n);
    char json[96];
    snprintf(json, sizeof(json), "{\"len\":%u,\"queued\":%d,\"err\":\"%s\"}",
             (unsigned)n, e == ESP_OK ? 1 : 0, esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_espnow_stats(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char hex[2 * 16 + 1];
    size_t off = 0;
    const int n = s_now_last_n;
    for (int i = 0; i < n && i < 16; i++) {
        off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%02x", s_now_last[i]);
    }
    if (n == 0) {
        hex[0] = '\0';
    }
    char json[320];
    snprintf(json, sizeof(json),
             "{\"init\":%d,\"ch\":%u,\"rx\":%u,\"tx_ok\":%u,\"tx_fail\":%u,\"bad\":%u,\"drop\":%u,"
             "\"parser\":%d,\"last_len\":%d,\"last_rssi\":%d,"
             "\"last_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"last_hex\":\"%s\"}",
             s_now_ready ? 1 : 0, (unsigned)s_now_ch,
             (unsigned)s_now_rx, (unsigned)s_now_tx_ok, (unsigned)s_now_tx_fail,
             (unsigned)s_now_bad, (unsigned)s_now_drop, s_now_parser ? 1 : 0,
             s_now_last_len, s_now_last_rssi,
             s_now_last_mac[0], s_now_last_mac[1], s_now_last_mac[2],
             s_now_last_mac[3], s_now_last_mac[4], s_now_last_mac[5], hex);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}
#endif  // CONFIG_NETCORE_WIFI

void nc_test_action_espnow_boot(nc4_decoder_t *decoder)
{
#if NC_HAVE_ESPNOW
    if (decoder == NULL) {
        return;
    }
    const esp_err_t e = now_ensure(0, decoder);
    ESP_LOGI(TAG, "ESP-NOW boot init: %s", esp_err_to_name(e));
#else
    (void)decoder;
#endif
}

// ── 0xF02C–0xF02E：Wi-Fi STA / AP / INFO（測試用；憑證只在命令裡，不落檔案）──
#if CONFIG_NETCORE_WIFI
#define NC4_CMD_WIFI_STA   0xF02C
#define NC4_CMD_WIFI_AP    0xF02D
#define NC4_CMD_WIFI_INFO  0xF02E

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile bool s_sta_got_ip;
static uint32_t s_sta_ip_addr;

static void wifi_ip_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_sta_ip_addr = e->ip_info.ip.addr;
        s_sta_got_ip = true;
        ESP_LOGI(TAG, "Wi-Fi STA IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void wifi_ensure_netif_sta(void)
{
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    static bool reg;
    if (!reg) {
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ip_cb, NULL);
        reg = true;
    }
}

// Wi-Fi 只初始化一次。S3（真 radio）走 now_ensure（它同時處理 ESP-NOW）；
// P4（Wi-Fi 在外掛協處理器）冇 esp_now，所以只做通用初始化。
static esp_err_t wifi_bringup_once(nc4_decoder_t *dec)
{
#if NC_HAVE_ESPNOW
    return now_ensure(0, dec);
#else
    (void)dec;
    static bool done;
    if (done) {
        return ESP_OK;
    }
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        e = esp_wifi_init(&ic);
        if (e != ESP_OK) {
            return e;
        }
    }
    done = true;
    return ESP_OK;
#endif
}

static void on_wifi_sta(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 2) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int sl = f->data[0];
    if (sl < 1 || sl > 32 || 1 + sl >= f->len) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"ssid\"}");
        return;
    }
    char ssid[33]; memcpy(ssid, f->data + 1, sl); ssid[sl] = '\0';
    const int off = 1 + sl;
    const int pl = f->data[off];
    if (pl < 0 || pl > 64 || off + 1 + pl > f->len) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"pw\"}");
        return;
    }
    char pw[65]; memcpy(pw, f->data + off + 1, pl); pw[pl] = '\0';

    esp_err_t e = wifi_bringup_once(ctx->decoder);
    if (e != ESP_OK) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"wifi_init\"}");
        return;
    }
    wifi_ensure_netif_sta();
    wifi_config_t wc; memset(&wc, 0, sizeof(wc));
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pw, strnlen(pw, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = (pw[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e == ESP_OK) e = esp_wifi_connect();
    if (s_sta_netif != NULL) {
        (void)esp_netif_dhcpc_start(s_sta_netif);   // 明確啟動 DHCP（避免 netif 建立時序）
    }

    char json[160];
    snprintf(json, sizeof(json), "{\"sta\":1,\"ssid\":\"%s\",\"err\":\"%s\"}",
             ssid, esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_wifi_ap(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 3) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const int sl = f->data[0];
    if (sl < 1 || sl > 32 || 1 + sl >= f->len) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"ssid\"}");
        return;
    }
    char ssid[33]; memcpy(ssid, f->data + 1, sl); ssid[sl] = '\0';
    const int off = 1 + sl;
    const int pl = f->data[off];
    if (pl < 8 || pl > 64 || off + 1 + pl >= f->len) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"pw\"}");
        return;
    }
    char pw[65]; memcpy(pw, f->data + off + 1, pl); pw[pl] = '\0';
    const int ch = f->data[off + 1 + pl];

    esp_err_t e = wifi_bringup_once(ctx->decoder);
    if (e != ESP_OK) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"wifi_init\"}");
        return;
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t wc; memset(&wc, 0, sizeof(wc));
    memcpy(wc.ap.ssid, ssid, strnlen(ssid, sizeof(wc.ap.ssid)));
    wc.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(wc.ap.password, pw, strnlen(pw, sizeof(wc.ap.password)));
    wc.ap.channel = (ch >= 1 && ch <= 13) ? ch : 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (e == ESP_OK) e = esp_wifi_start();

    char json[160];
    snprintf(json, sizeof(json), "{\"ap\":1,\"ssid\":\"%s\",\"ch\":%d,\"err\":\"%s\"}",
             ssid, wc.ap.channel, esp_err_to_name(e));
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_wifi_info(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    esp_netif_ip_info_t ip4; memset(&ip4, 0, sizeof(ip4));
    if (s_sta_netif != NULL) {
        esp_netif_get_ip_info(s_sta_netif, &ip4);
    }
    esp_netif_ip_info_t ap4; memset(&ap4, 0, sizeof(ap4));
    if (s_ap_netif != NULL) {
        esp_netif_get_ip_info(s_ap_netif, &ap4);
    }
    int rssi = 0;
    esp_wifi_sta_get_rssi(&rssi);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"sta\":%d,\"ip\":\"" IPSTR "\",\"ap\":%d,\"ap_ip\":\"" IPSTR "\",\"rssi\":%d}",
             s_sta_got_ip ? 1 : 0, IP2STR(&ip4.ip),
             s_ap_netif ? 1 : 0, IP2STR(&ap4.ip), rssi);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}
#endif  // CONFIG_NETCORE_WIFI

// ── 0xF031 STRESS：另一核負載（CPU / PSRAM），配合 I80 bench 看 render 核是否被影響 ──
#define NC4_CMD_STRESS  0xF031
static volatile bool s_stress_run;
static volatile uint32_t s_stress_ops;
static volatile uint8_t s_stress_mode;
static TaskHandle_t s_stress_task;

static void stress_task(void *arg)
{
    (void)arg;
    uint8_t *a = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    if (a == NULL || b == NULL) {
        free(a); free(b);
        s_stress_run = false;
        vTaskDelete(NULL);
        return;
    }
    memset(a, 0x5a, 32 * 1024);
    while (s_stress_run) {
        if (s_stress_mode == 1) {
            memcpy(b, a, 32 * 1024);
        } else {
            volatile uint32_t x = 0;
            for (uint32_t i = 0; i < 100000; i++) x += i;
        }
        s_stress_ops++;
    }
    free(a); free(b);
    s_stress_task = NULL;
    vTaskDelete(NULL);
}

static void on_stress(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 1) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    const uint8_t start = f->data[0];
    if (start) {
        if (s_stress_task == NULL) {
            s_stress_mode = (f->len >= 2) ? f->data[1] : 0;
            s_stress_ops = 0;
            s_stress_run = true;
            if (xTaskCreatePinnedToCore(stress_task, "stress", 4096, NULL, 4, &s_stress_task, 1) != pdPASS) {
                s_stress_run = false;
                reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"task\"}");
                return;
            }
        }
    } else {
        s_stress_run = false;
    }
    char j[128];
    snprintf(j, sizeof(j), "{\"run\":%d,\"mode\":%u,\"ops\":%u}",
             s_stress_run ? 1 : 0, (unsigned)s_stress_mode, (unsigned)s_stress_ops);
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

// ── 0xF030 I80_DMA：I80 16-lane DMA 的 CPU 釋放／切片測試（合成，不需要 LED）──
//
// 目的：回答「I80 用 DMA、一塊一塊發、換緩衝，CPU 是否真的空出來」。
// 用 16-bit I80 bus + 28,800 B frame（16 lane × 600 LED × 3 B）做時序；
// 只 toggle 測試 GPIO，不驅動真 LED。量 submit（CPU）／wire（DMA）／fill（換緩衝）。
#if SOC_LCD_I80_SUPPORTED && !CONFIG_IDF_TARGET_ESP32P4
#define NC4_CMD_I80_DMA     0xF030
#define I80_FRAME_BYTES     28800      /* 16 × 600 × 3 */
#define I80_SLICES          4
#define I80_PCLK_HZ         1000000    /* 1 MHz；28.8 KB → 14.4 ms wire */

static volatile bool s_i80_done;
static volatile int64_t s_i80_done_us;

static bool i80_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    (void)io; (void)ed; (void)ctx;
    s_i80_done_us = esp_timer_get_time();
    s_i80_done = true;
    return false;
}

static void on_i80_dma(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t iters = 3;
    if (f->len >= 2) iters = (uint16_t)(f->data[0] | (f->data[1] << 8));
    if (iters < 1) iters = 1;
    if (iters > 10) iters = 10;
    uint32_t pclk_hz = 2000000;   // 1 MHz 被驅動拒絕（NOT_SUPPORTED），先由 2 MHz 起
    if (f->len >= 4) {
        const uint16_t mhz = (uint16_t)(f->data[2] | (f->data[3] << 8));
        if (mhz >= 1 && mhz <= 20) pclk_hz = (uint32_t)mhz * 1000000u;
    }

    // 16 data pins：避開 SD(4-7/15/16)、USB(19/20)、八角 PSRAM(26-37)、strap(45/46)
    // N16R8 的 octal PSRAM 用了 33-37；先前用 33-48 會直接打爛 PSRAM → reset。
    static const int data_pins[16] = {1,2,3,8,9,10,11,12,13,14,17,18,21,38,39,40};
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = 47,
        .wr_gpio_num = 41,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .bus_width = 16,
        .max_transfer_bytes = I80_FRAME_BYTES,
        .dma_burst_size = 64,
    };
    for (int i = 0; i < 16; i++) bus_cfg.data_gpio_nums[i] = data_pins[i];
    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_err_t e = esp_lcd_new_i80_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        char j[96]; snprintf(j,sizeof(j),"{\"err\":\"bus\",\"e\":\"%s\"}",esp_err_to_name(e));
        reply_json(ctx, NC4_CMD_HUB_RSP, j); return;
    }

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = 42,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = 2,
        .on_color_trans_done = i80_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    e = esp_lcd_new_panel_io_i80(bus, &io_cfg, &io);
    if (e != ESP_OK) {
        esp_lcd_del_i80_bus(bus);
        char j[96]; snprintf(j,sizeof(j),"{\"err\":\"io\",\"e\":\"%s\"}",esp_err_to_name(e));
        reply_json(ctx, NC4_CMD_HUB_RSP, j); return;
    }

    uint8_t *buf[2] = {0};
    buf[0] = heap_caps_aligned_alloc(64, I80_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf[1] = heap_caps_aligned_alloc(64, I80_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf[0] || !buf[1]) {
        free(buf[0]); free(buf[1]);
        esp_lcd_panel_io_del(io); esp_lcd_del_i80_bus(bus);
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"nomem\"}"); return;
    }
    memset(buf[0], 0x55, I80_FRAME_BYTES);
    memset(buf[1], 0xAA, I80_FRAME_BYTES);

    // 整幀：submit 回來後 CPU 做 busy loop，直到 DMA 完成 → 量 CPU 可用的時間
    int64_t submit_total = 0, wire_total = 0;
    uint32_t busy_total = 0;
    for (uint16_t k = 0; k < iters; k++) {
        s_i80_done = false;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t se = esp_lcd_panel_io_tx_color(io, 0, buf[k & 1], I80_FRAME_BYTES);
        const int64_t t1 = esp_timer_get_time();
        if (se != ESP_OK) { submit_total += t1 - t0; continue; }
        uint32_t spin = 0;
        while (!s_i80_done) { spin++; }
        const int64_t t2 = esp_timer_get_time();
        submit_total += t1 - t0;
        wire_total += t2 - t0;
        busy_total += spin;
    }

    // 切片：submit 一片後，CPU 立即 fill 下一片；量 fill 與 wire 是否重疊
    const size_t slice = I80_FRAME_BYTES / I80_SLICES;
    int64_t fill_total = 0, slice_wire_total = 0;
    for (int s = 0; s < I80_SLICES; s++) {
        s_i80_done = false;
        const uint8_t *cur = buf[s & 1];
        const esp_err_t se = esp_lcd_panel_io_tx_color(io, 0, cur, slice);
        const int64_t t0 = esp_timer_get_time();
        if (se != ESP_OK) continue;
        // 換緩衝：fill 另一塊（不是 DMA 正在讀的那塊）
        uint8_t *nxt = buf[(s + 1) & 1];
        const int64_t f0 = esp_timer_get_time();
        memset(nxt, (uint8_t)(0x30 + s), slice);
        const int64_t f1 = esp_timer_get_time();
        fill_total += f1 - f0;
        while (!s_i80_done) { }
        slice_wire_total += esp_timer_get_time() - t0;
    }

    char json[420];
    snprintf(json, sizeof(json),
             "{\"pclk\":%u,\"frame\":%u,\"iters\":%u,"
             "\"whole\":{\"submit_us\":%lld,\"wire_us\":%lld,\"busy\":%u},"
             "\"slice\":{\"n\":%u,\"len\":%u,\"fill_us\":%lld,\"wire_us\":%lld}}",
             (unsigned)pclk_hz, (unsigned)I80_FRAME_BYTES, (unsigned)iters,
             (long long)(submit_total / iters), (long long)(wire_total / iters), (unsigned)busy_total,
             (unsigned)I80_SLICES, (unsigned)slice,
             (long long)fill_total, (long long)slice_wire_total);

    free(buf[0]); free(buf[1]);
    esp_lcd_panel_io_del(io);
    esp_lcd_del_i80_bus(bus);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}
#endif  // SOC_LCD_I80_SUPPORTED

// ── 0xF033／0xF034 RX_BENCH：板內最高速的收／解析量測（**完全唔經 USB**）──
//
// 為什麼要這支：主機量到的 RTT 一定夾住 USB（1 ms 幀 ＋ OS 驅動），睇唔到 firmware 的成本。
// 這裡建一個**專用 parser**（send 用假 sink），一次過灌 N 幀，量「解析 + dispatch + handler」。
//
// ⚠️ **一定要用獨立任務跑**：handler 內再 feed → dispatch 會重入同一把 decoder 互斥鎖
// （非遞迴）→ 自我死鎖。2026-11-10 實際撞到，板子要人手復位。
//
// 0xF033 [u16 frames][u16 payload_bytes] → 起一次 bench（回 started／busy）
// 0xF034 → 上一次的結果
#define NC4_CMD_RX_BENCH  0xF033
#define NC4_CMD_RX_RESULT 0xF034
#define NC4_CMD_RX_NOOP   0xF035

static volatile uint32_t s_rxb_noop;
static volatile uint32_t s_rxb_sent;
static volatile int      s_rxb_busy;
static struct {
    volatile int done;
    uint32_t frames, psize, plen, us, noop, send;
} s_rxb;

static esp_err_t rxb_sink(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    (void)frame;
    (void)len;
    s_rxb_sent++;
    return ESP_OK;
}

static void on_rx_noop(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)ctx;
    (void)f;
    s_rxb_noop++;
}

static void rxb_task(void *arg)
{
    nc4_decoder_t *dec = (nc4_decoder_t *)arg;
    const uint32_t frames = s_rxb.frames;
    const uint32_t psize = s_rxb.psize;
    const size_t total = NC4_HDR_LEN + psize + NC4_CRC_LEN;

    nc4_parser_t *p = nc4_parser_create(dec, rxb_sink, NULL, "rbench", total + 64);
    uint8_t *fr = malloc(total);
    uint8_t *pl = malloc(psize ? psize : 1);
    if (p != NULL && fr != NULL && pl != NULL) {
        memset(pl, 0x5A, psize ? psize : 1);
        const int plen = nc4_pack_into(fr, total, NC4_CMD_RX_NOOP, NC4_ADDR_BROADCAST,
                                       pl, psize);
        if (plen > 0) {
            s_rxb_noop = 0;
            s_rxb_sent = 0;
            const int64_t t0 = esp_timer_get_time();
            for (uint32_t i = 0; i < frames; i++) {
                nc4_parser_feed(p, fr, (size_t)plen);
            }
            const int64_t t1 = esp_timer_get_time();
            s_rxb.plen = (uint32_t)plen;
            s_rxb.us = (uint32_t)(t1 - t0);
            s_rxb.noop = s_rxb_noop;
            s_rxb.send = s_rxb_sent;
            s_rxb.done = 1;
        }
    }
    free(fr);
    free(pl);
    nc4_parser_destroy(p);      // 順便驗 runtime release（環會除名）
    s_rxb_busy = 0;
    vTaskDelete(NULL);
}

static void on_rx_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_rxb_busy) {
        reply_json(ctx, NC4_CMD_RX_BENCH, "{\"err\":\"busy\"}");
        return;
    }
    uint32_t frames = 1000;
    uint32_t psize = 8;
    if (f->len >= 2) {
        frames = (uint32_t)(f->data[0] | (f->data[1] << 8));
    }
    if (f->len >= 4) {
        psize = (uint32_t)(f->data[2] | (f->data[3] << 8));
    }
    if (frames == 0 || frames > 20000) {
        frames = 20000;
    }
    if (psize > NC4_MAX_PAYLOAD) {
        psize = NC4_MAX_PAYLOAD;
    }

    s_rxb.done = 0;
    s_rxb.frames = frames;
    s_rxb.psize = psize;
    s_rxb_busy = 1;
    if (xTaskCreatePinnedToCore(rxb_task, "rxbench", 4096, ctx->decoder, 2, NULL, 0) != pdPASS) {
        s_rxb_busy = 0;
        reply_json(ctx, NC4_CMD_RX_BENCH, "{\"err\":\"task\"}");
        return;
    }
    char js[80];
    snprintf(js, sizeof(js), "{\"started\":1,\"n\":%u,\"payload\":%u}",
             (unsigned)frames, (unsigned)psize);
    reply_json(ctx, NC4_CMD_RX_BENCH, js);
}

static void on_rx_result(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    const uint64_t us = s_rxb.us;
    const double per = (s_rxb.frames != 0) ? (double)us / (double)s_rxb.frames : 0.0;
    const double mbps = (us != 0) ? ((double)s_rxb.frames * (double)s_rxb.plen) / (double)us : 0.0;
    char js[220];
    snprintf(js, sizeof(js),
             "{\"done\":%d,\"n\":%u,\"frame_b\":%u,\"us\":%u,\"us_frame\":%.2f,"
             "\"mbps\":%.2f,\"noop\":%u,\"send\":%u}",
             s_rxb.done, (unsigned)s_rxb.frames, (unsigned)s_rxb.plen, (unsigned)us,
             per, mbps, (unsigned)s_rxb.noop, (unsigned)s_rxb.send);
    reply_json(ctx, NC4_CMD_RX_RESULT, js);
}

// ── 0xF032 BUF_TEST：統一緩衝（nc_buf）的上板自測 ──
//
// 為什麼要有這支：host 測試（tools/hosttest/buf_test）只能證明「邏輯對」，
// 只有上板才證明得到這四件事：
//   ① PSRAM 的 buffer 真的拿得到、而且 64 B 對齊（DMA／cache line）
//   ② S→P、P→S、P→P、S→S 四條 copy 路徑都通（PSRAM 寫入後的內容正確）
//   ③ reserve（自己放表頭）之後 off/len 仍然對
//   ④ 換手（handover）在真 buffer 上會成功、記憶體位置不變
//
// **唔 destroy**：正常路徑冇 destroy（見 docs/03_notes/buffers.md 的鐵律），
// 測試都唔例外 —— 兩條測試 buffer 只建一次，之後每次重跑都重用。
#define NC4_CMD_BUF_TEST 0xF032

static nc_buf_t *s_t_sram;
static nc_buf_t *s_t_psram;
static nc_buf_t *s_t_stream;

// 抽乾一條 buffer（測試用；正常路徑冇這個動作）
static void buf_drain(nc_buf_t *b)
{
    for (int guard = 0; guard < 64; guard++) {
        void *p = nc_buf_get(b, NULL, NULL);
        if (p == NULL) {
            return;
        }
        nc_buf_get_end(b);
    }
}

static void on_buf_test(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    nc4_reply_t r;
    char *js = reply_open(&r, ctx, NC4_CMD_BUF_TEST);
    if (js == NULL) {
        return;
    }

    // 只建一次（registry 有上限；而且冇 destroy 這回事）
    if (s_t_sram == NULL) {
        s_t_sram = nc_buf_find("t.sram");
    }
    if (s_t_psram == NULL) {
        s_t_psram = nc_buf_find("t.psram");
    }
    if (s_t_sram == NULL) {
        s_t_sram = nc_buf_create("t.sram", NC_BUF_SRAM, 256, 4, NC_BUF_F_FIFO);
    }
    if (s_t_psram == NULL) {
        s_t_psram = nc_buf_create("t.psram", NC_BUF_PSRAM, 1024, 2, NC_BUF_F_FIFO);
    }
    if (s_t_stream == NULL) {
        s_t_stream = nc_buf_find("t.stream");
    }
    if (s_t_stream == NULL) {
        s_t_stream = nc_buf_create("t.stream", NC_BUF_SRAM, 1024, 1, NC_BUF_F_STREAM);
    }

    int ok_align = 0, ok_fifo = 0, ok_full = 0, ok_res = 0;
    int ok_s2s = 0, ok_s2p = 0, ok_p2s = 0, ok_p2p = 0, ok_ho = 0;
    int ok_stream = 0, ok_abort = 0;

    uint8_t v[512];
    if (s_t_sram != NULL && s_t_psram != NULL) {
        buf_drain(s_t_sram);
        buf_drain(s_t_psram);

        // ① 對齊（64 B）
        ok_align = (((uintptr_t)nc_buf_raw(s_t_sram, 0) % NC_BUF_ALIGN) == 0) &&
                   (((uintptr_t)nc_buf_raw(s_t_psram, 0) % NC_BUF_ALIGN) == 0);

        // ② FIFO 次序 ＋ 滿了回 NULL
        for (int i = 0; i < 4; i++) {
            memset(v, i + 1, 256);
            nc_buf_put(s_t_sram, v, 256);
        }
        ok_full = (nc_buf_put(s_t_sram, v, 256) == ESP_ERR_NO_MEM);
        ok_fifo = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t len = 0;
            const uint8_t *p = nc_buf_get(s_t_sram, &len, NULL);
            if (p == NULL || len != 256 || p[0] != (uint8_t)(i + 1)) {
                ok_fifo = 0;
                break;
            }
            nc_buf_get_end(s_t_sram);
        }
        buf_drain(s_t_sram);

        // ③ reserve（表頭在前）＋ len（實際用量）
        uint8_t *base = nc_buf_put_begin(s_t_sram, 16);
        if (base != NULL) {
            *(uint32_t *)(base - 16) = 0xB0F0B0F0u;
            memset(base, 0x5A, 100);
            nc_buf_put_end(s_t_sram, 100);
            uint32_t len = 0, off = 0;
            uint8_t *p = nc_buf_get(s_t_sram, &len, &off);
            ok_res = (p != NULL) && (off == 16) && (len == 100) &&
                     (p[0] == 0x5A) && (*(uint32_t *)(p - 16) == 0xB0F0B0F0u);
            nc_buf_get_end(s_t_sram);
        }
        buf_drain(s_t_sram);

        // ④ 四條空間路徑
        memset(v, 0x11, sizeof(v));
        ok_s2p = (nc_buf_put(s_t_psram, v, sizeof(v)) == ESP_OK);            // S → P
        if (ok_s2p) {
            uint32_t len = 0;
            const uint8_t *p = nc_buf_get(s_t_psram, &len, NULL);
            ok_s2p = (p != NULL) && (len == sizeof(v)) && (p[0] == 0x11) && (p[511] == 0x11);
            nc_buf_get_end(s_t_psram);
        }
        ok_p2p = (nc_buf_put(s_t_psram, nc_buf_raw(s_t_psram, 0), 64) == ESP_OK);  // P → P
        ok_p2s = (nc_buf_put(s_t_sram, nc_buf_raw(s_t_psram, 0), 128) == ESP_OK);  // P → S
        ok_s2s = (nc_buf_put(s_t_sram, v, 128) == ESP_OK);                        // S → S

        // ⑤ S→P 的內容要真的一樣（copy 之後讀返出嚟對）
        if (ok_p2s) {
            uint32_t len = 0;
            const uint8_t *p = nc_buf_get(s_t_sram, &len, NULL);
            ok_p2s = (p != NULL) && (len == 128) && (p[0] == 0x11);
            nc_buf_get_end(s_t_sram);
        }
        buf_drain(s_t_sram);
        buf_drain(s_t_psram);

        // ⑥ 串流：部分寫、部分讀、壓實、滿了部分寫入（總線的 parser 環用這個模式）
        if (s_t_stream != NULL) {
            nc_buf_skip(s_t_stream, nc_buf_pending(s_t_stream));
            uint8_t sv[600];
            memset(sv, 0x7E, sizeof(sv));
            ok_stream = (nc_buf_write(s_t_stream, sv, 600) == 600) &&
                        (nc_buf_pending(s_t_stream) == 600);
            nc_buf_skip(s_t_stream, 100);
            ok_stream = ok_stream && (nc_buf_pending(s_t_stream) == 500) &&
                        (nc_buf_space(s_t_stream) == 524);
            // 尾部只淨 524 → 寫 400 要壓實
            ok_stream = ok_stream && (nc_buf_write(s_t_stream, sv, 400) == 400) &&
                        (nc_buf_pending(s_t_stream) == 900);
            uint32_t slen = 0;
            const uint8_t *sp = nc_buf_read_ptr(s_t_stream, &slen);
            ok_stream = ok_stream && (sp != NULL) && (slen == 900) && (sp[0] == 0x7E);
            // 只剩 124 → 部分寫入，唔覆蓋未讀
            ok_stream = ok_stream && (nc_buf_write(s_t_stream, sv, 600) == 124) &&
                        (nc_buf_pending(s_t_stream) == 1024);
            ok_stream = ok_stream && (nc_buf_write(s_t_stream, sv, 8) == 0);
            nc_buf_skip(s_t_stream, 1024);
            ok_stream = ok_stream && (nc_buf_pending(s_t_stream) == 0);
        }

        // ⑦ 中途放棄：唔會卡死環（nc4_bus 刻意唔標 writing，這裡跟同一個語義）
        buf_drain(s_t_sram);
        if (nc_buf_put_begin(s_t_sram, 0) != NULL) {
            // 唔好喺上板測試撞 handover（會令 viol +1，污染診斷）；host test 已覆蓋。
            ok_abort = (nc_buf_put_begin(s_t_sram, 0) == NULL) &&   // 一次只揸一格
                       (nc_buf_put_abort(s_t_sram) == ESP_OK);
            if (nc_buf_put_begin(s_t_sram, 0) != NULL) {
                ok_abort = ok_abort && (nc_buf_put_end(s_t_sram, 8) == ESP_OK);
            } else {
                ok_abort = 0;
            }
        }
        buf_drain(s_t_sram);

        // ⑧ 換手：全部空 → 成功；位置不變
        const void *before = nc_buf_raw(s_t_sram, 0);
        ok_ho = (nc_buf_handover(s_t_sram, "t.hand") == ESP_OK) &&
                (nc_buf_raw(s_t_sram, 0) == before) &&
                (nc_buf_handover(s_t_sram, "t.sram") == ESP_OK);
    }

    const int bad = 11 - (ok_align + ok_fifo + ok_full + ok_res + ok_s2s + ok_s2p +
                          ok_p2s + ok_p2p + ok_ho + ok_stream + ok_abort);

    size_t off = 0;
    jappend(js, NC4_REPLY_JSON_MAX + 1, &off,
            "{\"ok\":%d,\"fail\":%d,\"align\":%d,\"fifo\":%d,\"full\":%d,"
            "\"reserve\":%d,\"s2s\":%d,\"s2p\":%d,\"p2s\":%d,\"p2p\":%d,"
            "\"handover\":%d,\"stream\":%d,\"abort\":%d,\"buf\":",
            bad == 0, bad, ok_align, ok_fifo, ok_full, ok_res,
            ok_s2s, ok_s2p, ok_p2s, ok_p2p, ok_ho, ok_stream, ok_abort);
    jchain(js, NC4_REPLY_JSON_MAX + 1, &off, nc_buf_json);
    jappend(js, NC4_REPLY_JSON_MAX + 1, &off, "}");

    if (bad != 0) {
        ESP_LOGW(TAG, "BUF_TEST %d 項失敗（align=%d fifo=%d full=%d res=%d "
                      "s2s=%d s2p=%d p2s=%d p2p=%d ho=%d stream=%d abort=%d）",
                 bad, ok_align, ok_fifo, ok_full, ok_res, ok_s2s, ok_s2p,
                 ok_p2s, ok_p2p, ok_ho, ok_stream, ok_abort);
    }
    reply_send(&r, off);
}

// ── 註冊 ──

// ── 0xF050 TCP_SEND：主動連 peer，送一批 NETBENCH 幀（量**單向**吞吐）──
// payload: [ip:4][port:u16 LE][frames:u16 LE][size:u16 LE][flags:u8]
//   flags bit0 = 送完之後問 peer 的 0xF040 op=2 取統計（peer 要係 Net-Core）
// 回覆：{"sent":N,"bytes":B,"ms":T,"kbps":K,"rx":{...}|null}
#define NC4_CMD_TCP_SEND 0xF050

static int nc_tcp_dial(const uint8_t ip[4], uint16_t port, int timeout_ms)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    memcpy(&a.sin_addr.s_addr, ip, 4);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r != 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        r = select(fd + 1, NULL, &wf, NULL, &tv);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// 送 frames 幀、每幀 size bytes 的 NETBENCH payload；回傳實際送出 bytes。
static int64_t nc_tcp_blast(int fd, uint16_t frames, uint16_t size)
{
    const size_t cap = (size_t)size + 64;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return -1;
    }
    uint8_t *pl = heap_caps_malloc((size_t)size + 5, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pl == NULL) {
        free(buf);
        return -1;
    }
    memset(pl, 0x5A, (size_t)size + 5);
    pl[0] = 0x03;    // NETBENCH op=3：只記帳、不回覆

    // 先清對方的帳（op=0），否則 stats 係累加的
    {
        uint8_t r[16];
        const int rn = nc4_pack_into(r, sizeof(r), 0xF040, 0xFFFF,
                                     (const uint8_t *)"\x00", 1);
        if (rn > 0) {
            (void)send(fd, r, (size_t)rn, 0);
        }
    }

    int64_t total = 0;
    for (uint16_t i = 0; i < frames; i++) {
        const uint32_t sq = i;
        memcpy(pl + 1, &sq, sizeof(sq));
        const int n = nc4_pack_into(buf, cap, 0xF040, 0xFFFF, pl, (size_t)size + 5);
        if (n <= 0) {
            break;
        }
        int off = 0;
        while (off < n) {
            const int w = send(fd, buf + off, (size_t)(n - off), 0);
            if (w <= 0) {
                goto done;
            }
            off += w;
        }
        total += n;
    }
done:
    free(pl);
    free(buf);
    return total;
}

// 送 op=2，讀回 peer 的統計 JSON（找 "NC" + 0xF004 + CRC 不驗，測試用）
static size_t nc_tcp_fetch_stats(int fd, char *out, size_t cap)
{
    uint8_t q[16];
    const int n = nc4_pack_into(q, sizeof(q), 0xF040, 0xFFFF, (const uint8_t *)"\x02", 1);
    if (n <= 0 || send(fd, q, (size_t)n, 0) != n) {
        return 0;
    }
    uint8_t *rb = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (rb == NULL) {
        return 0;
    }
    size_t got = 0, off = 0;
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (esp_timer_get_time() < deadline && got < 1024) {
        const int r = recv(fd, rb + got, 1024 - got, 0);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
        while (off + 9 <= got) {
            if (rb[off] != 'N' || rb[off + 1] != 'C') {
                off++;
                continue;
            }
            const uint16_t cmd = (uint16_t)(rb[off + 5] | (rb[off + 6] << 8));
            const uint16_t len = (uint16_t)(rb[off + 7] | (rb[off + 8] << 8));
            if (cmd != 0xF004 || off + 9 + len > got) {
                off++;
                continue;
            }
            const uint8_t *d = rb + off + 9;
            size_t dl = len;
            if (dl >= 2 && ((size_t)d[0] | ((size_t)d[1] << 8)) == dl - 2) {
                d += 2;
                dl -= 2;
            }
            if (dl >= cap) {
                dl = cap - 1;
            }
            memcpy(out, d, dl);
            out[dl] = '\0';
            free(rb);
            return dl;
        }
    }
    free(rb);
    return 0;
}

static void on_tcp_send(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 11) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    uint8_t ip[4];
    memcpy(ip, f->data, 4);
    uint16_t port, frames, size;
    memcpy(&port, f->data + 4, 2);
    memcpy(&frames, f->data + 6, 2);
    memcpy(&size, f->data + 8, 2);
    const uint8_t flags = f->data[10];
    if (frames == 0 || frames > 4096 || size > NC4_MAX_PAYLOAD - 16) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"range\"}");
        return;
    }

    const int fd = nc_tcp_dial(ip, port, 5000);
    if (fd < 0) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"connect\"}");
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const int64_t sent = nc_tcp_blast(fd, frames, size);
    const int64_t us = esp_timer_get_time() - t0;

    char rx[512];
    size_t rxn = 0;
    if (flags & 1) {
        rxn = nc_tcp_fetch_stats(fd, rx, sizeof(rx));
    }
    close(fd);

    const double kbps = (us > 0 && sent > 0)
                        ? (double)sent * 1000000.0 / (double)us / 1024.0 : 0.0;
    char j[900];
    snprintf(j, sizeof(j),
             "{\"sent\":%lld,\"frames\":%u,\"size\":%u,\"us\":%lld,\"kbps\":%.0f,\"rx\":%s%s%s}",
             (long long)sent, (unsigned)frames, (unsigned)size, (long long)us, kbps,
             rxn ? "" : "null", rxn ? "" : "", rxn ? rx : "");
    reply_json(ctx, NC4_CMD_HUB_RSP, j);
}

// P4 開機自動掃描：等 Wi-Fi 連上之後，對 peer 逐個 size 跑一次。
// peer 寫死 192.168.4.1:6000（S3 的 AP + NC4 TCP listener），純測試用。
#if CONFIG_IDF_TARGET_ESP32P4
#define NETAUTO_PEER  "192.168.4.1"
#define NETAUTO_PORT  6000

static void netauto_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(25000));      // 等 Wi-Fi 連上
    static const uint16_t sizes[] = { 512, 1024, 2048, 4096, 8192 };
    const uint8_t ip[4] = { 192, 168, 4, 1 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const int fd = nc_tcp_dial(ip, NETAUTO_PORT, 5000);
        if (fd < 0) {
            ESP_LOGE(TAG, "NETAUTO size=%u connect 失敗", (unsigned)sizes[i]);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const int64_t t0 = esp_timer_get_time();
        const int64_t sent = nc_tcp_blast(fd, 100, sizes[i]);
        const int64_t us = esp_timer_get_time() - t0;
        char rx[512];
        const size_t rxn = nc_tcp_fetch_stats(fd, rx, sizeof(rx));
        close(fd);
        ESP_LOGW(TAG, "NETAUTO size=%u sent=%lld us=%lld kbps=%.0f rx=%s",
                 (unsigned)sizes[i], (long long)sent, (long long)us,
                 (us > 0) ? (double)sent * 1e6 / (double)us / 1024.0 : 0.0,
                 rxn ? rx : "-");
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    ESP_LOGW(TAG, "NETAUTO 完成");
    vTaskDelete(NULL);
}

void nc_test_action_netauto_boot(void)
{
    xTaskCreatePinnedToCore(netauto_task, "netauto", 6144, NULL, 3, NULL, 1);
}
#else
void nc_test_action_netauto_boot(void) { }
#endif

static const nc_action_t k_actions[] = {
    { NC4_CMD_DEV_BENCH, on_dev_bench, "test" },
    { NC4_CMD_HUB_BENCH, on_hub_bench, "test" },
    { NC4_CMD_TX_BENCH, on_tx_bench, "test" },
    { NC4_CMD_BUS_STATS, on_bus_stats, "test" },
    { NC4_CMD_NET_TUNE, on_net_tune, "test" },
    { NC4_CMD_BENCH_READY, on_bench_ready, "test" },
    { NC4_CMD_BENCH_DATA, on_bench_data, "test" },
    { NC4_CMD_BENCH_RESULT, on_bench_result, "test" },
    { NC4_CMD_UART_LOOP, on_uart_loop, "test" },
    { NC4_CMD_UART_STREAM, on_uart_stream, "test" },
    { NC4_CMD_UART_INJECT, on_uart_inject, "test" },
    { NC4_CMD_LOOP_STAT, on_loop_stat, "test" },
    { NC4_CMD_LOOP_PING, on_loop_ping, "test" },
    { NC4_CMD_BIG_REPLY, on_big_reply, "test" },
    { NC4_CMD_LINK_FWD, on_link_fwd, "test" },
    { NC4_CMD_MEM_BENCH, on_mem_bench, "test" },
    { NC4_CMD_TILE_BENCH, on_tile_bench, "test" },
    { NC4_CMD_STAGE_BENCH, on_stage_bench, "test" },
    { NC4_CMD_HUB_STRESS, on_hub_stress, "test" },
    { NC4_CMD_HUB_DUPLEX, on_hub_duplex, "test" },
    { NC4_CMD_HUB_DUPLEX_SWEEP, on_hub_sweep, "test" },
    { NC4_CMD_HUB_DUPLEX4, on_hub_duplex4, "test" },
    { NC4_CMD_HUB_SINGLE, on_hub_single, "test" },
    { NC4_CMD_HUB_MESH, on_hub_mesh, "test" },
    { NC4_CMD_GPIO_CFG, on_gpio_cfg, "test" },
    { NC4_CMD_GPIO_SET, on_gpio_set, "test" },
    { NC4_CMD_GPIO_GET, on_gpio_get, "test" },
    { NC4_CMD_HW_GPIO, on_hw_gpio, "test" },
    { NC4_CMD_HW_SNAP, on_hw_snap, "test" },
    { NC4_CMD_HW_INFO, on_hw_info, "test" },
    { NC4_CMD_PWM_SET, on_pwm_set, "test" },
    { NC4_CMD_PWM_INFO, on_pwm_info, "test" },
    { NC4_CMD_UART_INFO, on_uart_info, "test" },
    { NC4_CMD_PWM_STAY, on_pwm_stay, "test" },
    { NC4_CMD_PWM_ZERO, on_pwm_zero, "test" },
    { NC4_CMD_NETBENCH, on_netbench, "test" },
    { NC4_CMD_NETBENCH_RESULT, on_netbench, "test" },
    { NC4_CMD_TCP_SEND, on_tcp_send, "test" },
#if NC_HAVE_ESPNOW
    { NC4_CMD_ESPNOW_INIT, on_espnow_init, "test" },
    { NC4_CMD_ESPNOW_SEND, on_espnow_send, "test" },
    { NC4_CMD_ESPNOW_STATS, on_espnow_stats, "test" },
#endif
#if CONFIG_NETCORE_WIFI
    { NC4_CMD_WIFI_STA, on_wifi_sta, "test" },
    { NC4_CMD_WIFI_AP, on_wifi_ap, "test" },
    { NC4_CMD_WIFI_INFO, on_wifi_info, "test" },
#endif
    { NC4_CMD_WIFI_CRED, on_wifi_cred, "test" },
#if SOC_LCD_I80_SUPPORTED && !CONFIG_IDF_TARGET_ESP32P4
    { NC4_CMD_I80_DMA, on_i80_dma, "test" },
#endif
    { NC4_CMD_STRESS, on_stress, "test" },
    { NC4_CMD_BUF_TEST, on_buf_test, "test" },
    { NC4_CMD_RX_BENCH, on_rx_bench, "test" },
    { NC4_CMD_RX_RESULT, on_rx_result, "test" },
    { NC4_CMD_RX_NOOP, on_rx_noop, "test" },
};

NC_ACTION_TABLE_DEF(nc_test_action_table, "test", k_actions);

void nc_test_action_init(const nc_test_action_hooks_t *hooks)
{
    if (hooks != NULL) {
        s_hooks = *hooks;
    }
}
