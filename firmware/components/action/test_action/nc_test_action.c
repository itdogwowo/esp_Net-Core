// 測試指令（dev bench / hub bench / tx bench / bus stats / net tune / bench 域）。
// 由 main.c 搬出：正式映像唔編譯（NC_ENABLE_TESTS=OFF 時 component 唔存在）。
#include "nc_test_action.h"
#include "nc_action.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "nc4_bus.h"
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
        len = NC4_REPLY_JSON_MAX;
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
        n = NC4_REPLY_JSON_MAX;
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

// ── 註冊 ──
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
};

NC_ACTION_TABLE_DEF(nc_test_action_table, "test", k_actions);

void nc_test_action_init(const nc_test_action_hooks_t *hooks)
{
    if (hooks != NULL) {
        s_hooks = *hooks;
    }
}
