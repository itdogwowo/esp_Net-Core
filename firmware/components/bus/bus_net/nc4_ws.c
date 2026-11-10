#include "nc4_ws.h"

#if CONFIG_HTTPD_WS_SUPPORT

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nc4_link.h"

static const char *TAG = "nc4_ws";

// 一則 WS 訊息的上限。允許「一則訊息塞多個 NC4 幀」以攤提 httpd 的每則固定成本
// （見 §batching）：24 KB 可以放 2 個 8K 幀、4 個 4K、或 16 個 1K。
#define WS_MAX_MSG   (24 * 1024)

static httpd_handle_t    s_server;
static nc4_parser_t     *s_parser;
static SemaphoreHandle_t s_tx_lock;
static volatile int      s_fd = -1;
static uint16_t          s_port;

// 收到的一整則訊息（碎片會先累積在這裡；單幀時就是直接解析）。
// 24 KB 改由 PSRAM 出（start 時才配）：這是 CPU copy 的目的地，唔經 DMA；
// 代價是每則訊息多一次 PSRAM 寫入，好處是 P4 靜態 .bss 直接少 24 KB。
static uint8_t *s_rx;
static size_t  s_rx_len;

// frame 模式下 parser 的 ring 用不到；給一個小區域就好（不會呼叫 feed）
static uint8_t s_ring_stub[64];

static volatile uint32_t s_msgs;
static volatile uint32_t s_bytes;
static volatile uint32_t s_tx;
static volatile uint32_t s_bad;
static volatile uint32_t s_batched;   // 一則訊息裡有多幀的次數
// 診斷：訊息之間的間隔（等資料）vs handler 內的時間（我們自己花掉的）
static volatile uint32_t s_gap_us;
static volatile uint32_t s_proc_us;
static int64_t s_last_us;

static bool ws_alive(int fd)
{
    return fd >= 0 && s_server != NULL &&
           httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

// NC4 的回覆出口（handler 呼叫，可能來自 httpd task 或 slow_worker）
static esp_err_t ws_send(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    const int fd = s_fd;
    if (!ws_alive(fd)) {
        s_fd = -1;
        return ESP_FAIL;
    }
    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)frame,     // 只借用，不改
        .len = len,
    };
    if (s_tx_lock != NULL) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    const esp_err_t r = httpd_ws_send_frame_async(s_server, fd, &pkt);
    if (s_tx_lock != NULL) {
        xSemaphoreGive(s_tx_lock);
    }
    if (r == ESP_OK) {
        s_tx++;
    } else {
        ESP_LOGW(TAG, "送出失敗 %s（fd=%d）", esp_err_to_name(r), fd);
        s_fd = -1;
    }
    return r;
}

// 轉寄出口：WS 的「送出」是 httpd async send（整幀 copy 入 httpd 的傳送隊列），
// 所以 link 層同樣唔需要加等待。目標是「最後一個連上來的 WS client」。
static int ws_link_write(void *arg, const uint8_t *frame, size_t len)
{
    return (ws_send(arg, frame, len) == ESP_OK) ? (int)len : -1;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {          // 握手完成
        if (s_fd >= 0 && s_fd != fd && ws_alive(s_fd)) {
            ESP_LOGW(TAG, "拒絕第二條 WS（已有 fd=%d）", s_fd);
            return ESP_FAIL;                // httpd 會收掉這個 session
        }
        s_fd = fd;
        s_rx_len = 0;
        ESP_LOGW(TAG, "WS 連線進來 fd=%d", fd);
        return ESP_OK;
    }
    // 第一則資料幀才代表這條 WS 真的在用（v6.0.3 的 httpd 不會用 method==GET 呼 handler）
    if (fd != s_fd) {
        if (s_fd >= 0 && ws_alive(s_fd)) {
            ESP_LOGW(TAG, "拒絕第二條 WS（已有 fd=%d）", s_fd);
            return ESP_FAIL;                // httpd 會關掉這個 session
        }
        s_fd = fd;
        s_rx_len = 0;
        ESP_LOGW(TAG, "WS 使用中 fd=%d", fd);
    }

    const int64_t t_enter = esp_timer_get_time();
    if (s_last_us != 0) {
        s_gap_us += (uint32_t)(t_enter - s_last_us);
    }

    httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_BINARY };
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);   // 先問長度
    if (ret != ESP_OK) {
        return ret;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        s_fd = -1;
        return ESP_OK;
    }
    if (pkt.type == HTTPD_WS_TYPE_PING || pkt.type == HTTPD_WS_TYPE_PONG) {
        return ESP_OK;                      // control frame 由 httpd 處理
    }

    if (pkt.len > WS_MAX_MSG - s_rx_len) {
        ESP_LOGW(TAG, "訊息太大（%u B，已累積 %u B）→ 關閉",
                 (unsigned)pkt.len, (unsigned)s_rx_len);
        s_bad++;
        s_fd = -1;
        return ESP_FAIL;
    }

    pkt.payload = s_rx + s_rx_len;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) {
        return ret;
    }
    s_rx_len += pkt.len;

    if (!pkt.final) {
        return ESP_OK;                      // 還有碎片
    }

    const size_t len = s_rx_len;
    s_rx_len = 0;
    if (len == 0) {
        return ESP_OK;
    }
    s_msgs++;
    s_bytes += len;

    // ★ 零複製：直接在 s_rx 上解析，不進 bus、不進 ring。
    // 一則 WS 訊息可以塞多個連續的 NC4 幀（batching）——每則訊息的固定成本約
    // 0.6–0.7 ms（httpd 每一則要 4–6 次小 recv，每次都過一次 lwIP netconn），
    // 小幀要靠這個攤提。
    size_t off = 0;
    uint32_t frames = 0;
    while (off + NC4_HDR_LEN <= len) {
        const size_t total = nc4_header_total(s_rx + off);
        if (total == 0 || off + total > len) {
            s_bad++;
            ESP_LOGW(TAG, "訊息裡有殘缺幀（off=%u len=%u）", (unsigned)off, (unsigned)len);
            break;
        }
        const esp_err_t perr = nc4_parser_frame(s_parser, s_rx + off, total);
        if (perr != ESP_OK) {
            s_bad++;
            ESP_LOGW(TAG, "解析失敗 %s（%u B）", esp_err_to_name(perr), (unsigned)total);
        }
        off += total;
        frames++;
    }
    if (frames != 1) {
        s_batched++;
    }
    s_proc_us += (uint32_t)(esp_timer_get_time() - t_enter);
    s_last_us = esp_timer_get_time();
    return ESP_OK;
}

httpd_handle_t nc4_ws_httpd(void)
{
    return s_server;
}

esp_err_t nc4_ws_start(nc4_decoder_t *decoder, uint16_t port)
{
    if (decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // 接收暫存：優先 PSRAM（P4 32 MB HEX 200 MHz），拿不到就退回內部 RAM。
    // 這塊只是 recv 的目的地（CPU copy），唔係 DMA buffer。
    s_rx = heap_caps_malloc(WS_MAX_MSG, MALLOC_CAP_SPIRAM);
    if (s_rx == NULL) {
        s_rx = malloc(WS_MAX_MSG);
    }
    if (s_rx == NULL) {
        ESP_LOGE(TAG, "WS 接收暫存（%u B）配唔到", (unsigned)WS_MAX_MSG);
        return ESP_ERR_NO_MEM;
    }
    s_rx_len = 0;

    s_parser = nc4_parser_create_in(s_ring_stub, sizeof(s_ring_stub), decoder,
                                    ws_send, NULL, "WS");
    if (s_parser == NULL) {
        free(s_rx);
        s_rx = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_tx_lock = xSemaphoreCreateMutex();
    (void)nc4_link_register("WS", ws_link_write, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_open_sockets = 3;      // lwIP 只有 10 個 socket，留給 TCP/UDP
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;         // handler 會走 reply_json（frame 2 KB）
    // 解析跑在 httpd 這個 task 上：把它抬到跟 TCP RX 一樣的優先序，
    // 並釘到 core1（core0 有 decode 引擎與 lwIP），避免每則訊息都排隊等排程
    cfg.task_priority = 10;
    cfg.core_id = 1;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失敗 %s", esp_err_to_name(err));
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
        free(s_rx);
        s_rx = NULL;
        return err;
    }

    const httpd_uri_t uri = {
        .uri = "/nc4",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
    };
    err = httpd_register_uri_handler(s_server, &uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "註冊 /nc4 失敗 %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
        free(s_rx);
        s_rx = NULL;
        return err;
    }

    s_port = port;
    ESP_LOGW(TAG, "NC4 over WS 在 ws://<ip>:%u/nc4", (unsigned)port);
    return ESP_OK;
}

void nc4_ws_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_parser != NULL) {
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
    }
    if (s_rx != NULL) {
        free(s_rx);
        s_rx = NULL;
    }
    s_rx_len = 0;
    s_fd = -1;
}

size_t nc4_ws_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return (size_t)snprintf(buf, cap,
                            "{\"port\":%u,\"max\":%u,\"fd\":%d,\"msgs\":%u,\"bytes\":%u,"
                            "\"tx\":%u,\"bad\":%u,\"batched\":%u,"
                            "\"gap_ms\":%u,\"proc_ms\":%u}",
                            (unsigned)s_port, (unsigned)WS_MAX_MSG, s_fd, (unsigned)s_msgs,
                            (unsigned)s_bytes, (unsigned)s_tx, (unsigned)s_bad,
                            (unsigned)s_batched,
                            (unsigned)(s_gap_us / 1000), (unsigned)(s_proc_us / 1000));
}

#else  /* !CONFIG_HTTPD_WS_SUPPORT */

#include <stdio.h>

httpd_handle_t nc4_ws_httpd(void)
{
    return NULL;   /* 沒開 WS 支援時沒有可共用的 server */
}

esp_err_t nc4_ws_start(nc4_decoder_t *decoder, uint16_t port)
{
    (void)decoder;
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

void nc4_ws_stop(void) {}

size_t nc4_ws_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return (size_t)snprintf(buf, cap, "{\"err\":\"no ws support\"}");
}

#endif
