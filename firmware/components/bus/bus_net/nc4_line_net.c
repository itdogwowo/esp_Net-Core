#include "nc4_line_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nc4_bus.h"
#include "nc4_link.h"
#include "nc_prio.h"

static const char *TAG = "net_line";

#define NET_TCP_PORT 6000
#define NET_UDP_PORT 6001
#define NET_WS_PORT  8080
#define NET_WS_MAX    (24 * 1024)

static bool s_declared;
static bool s_started;
static nc4_bus_handle_t s_tcp_bus;
static nc4_bus_handle_t s_udp_bus;
static nc4_bus_handle_t s_ws_bus;
static uint8_t *s_tcp_rx;
static uint8_t *s_udp_rx;
static uint8_t *s_ws_rx;

static volatile int s_tcp_fd = -1;
static int s_tcp_listen = -1;
static SemaphoreHandle_t s_tcp_tx;

static int s_udp_fd = -1;
static struct sockaddr_in s_udp_peer;
static volatile bool s_udp_has_peer;
static SemaphoreHandle_t s_udp_tx;

static httpd_handle_t s_ws_http;
static volatile int s_ws_fd = -1;
static QueueHandle_t s_ws_q;
static SemaphoreHandle_t s_ws_tx;

static void close_tcp(const char *why)
{
    const int fd = s_tcp_fd;
    if (fd < 0) {
        return;
    }
    s_tcp_fd = -1;
    close(fd);
    ESP_LOGW(TAG, "TCP 關閉（%s）", why);
}

// ── TCP ────────────────────────────────────────────────────────────────

static int tcp_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    const int fd = s_tcp_fd;
    if (fd < 0 || buf == NULL || cap == 0) {
        return 0;
    }
    const int n = recv(fd, buf, cap, 0);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        close_tcp("peer 關閉");
        return -1;
    }
    close_tcp("recv 錯誤");
    return -1;
}

static esp_err_t tcp_send(void *arg, const uint8_t *buf, size_t len)
{
    (void)arg;
    const int fd = s_tcp_fd;
    if (fd < 0) {
        return ESP_FAIL;
    }
    if (s_tcp_tx != NULL) {
        xSemaphoreTake(s_tcp_tx, portMAX_DELAY);
    }
    size_t off = 0;
    while (off < len) {
        const int n = send(fd, buf + off, len - off, 0);
        if (n <= 0) {
            if (s_tcp_tx != NULL) {
                xSemaphoreGive(s_tcp_tx);
            }
            close_tcp("send 失敗");
            return ESP_FAIL;
        }
        off += (size_t)n;
    }
    if (s_tcp_tx != NULL) {
        xSemaphoreGive(s_tcp_tx);
    }
    return ESP_OK;
}

static int tcp_link_write(void *arg, const uint8_t *frame, size_t len)
{
    return (tcp_send(arg, frame, len) == ESP_OK) ? (int)len : -1;
}

static void tcp_accept_task(void *arg)
{
    (void)arg;
    const int ls = s_tcp_listen;
    for (;;) {
        const int c = accept(ls, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (s_tcp_fd >= 0) {
            close(c);
            continue;
        }
        const int one = 1;
        int rcv = 32768;
        int snd = 32768;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
        s_tcp_fd = c;
        if (s_tcp_bus != NULL) {
            (void)nc4_bus_reset(s_tcp_bus);
        }
        ESP_LOGW(TAG, "TCP 連線 fd=%d", c);
        while (s_tcp_fd == c) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        close(c);
        if (s_tcp_bus != NULL) {
            (void)nc4_bus_reset(s_tcp_bus);
        }
        ESP_LOGW(TAG, "TCP 連線結束");
    }
}

// ── UDP ────────────────────────────────────────────────────────────────

static int udp_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    if (s_udp_fd < 0 || buf == NULL || cap == 0) {
        return 0;
    }
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = recvfrom(s_udp_fd, buf, cap, 0,
                           (struct sockaddr *)&from, &fl);
    if (n < 0) {
        return -1;
    }
    if (n > 0) {
        if (s_udp_tx != NULL) {
            xSemaphoreTake(s_udp_tx, portMAX_DELAY);
        }
        s_udp_peer = from;
        s_udp_has_peer = true;
        if (s_udp_tx != NULL) {
            xSemaphoreGive(s_udp_tx);
        }
    }
    return n;
}

static esp_err_t udp_send(void *arg, const uint8_t *buf, size_t len)
{
    (void)arg;
    if (s_udp_fd < 0 || !s_udp_has_peer) {
        return ESP_FAIL;
    }
    if (s_udp_tx != NULL) {
        xSemaphoreTake(s_udp_tx, portMAX_DELAY);
    }
    const int n = sendto(s_udp_fd, buf, len, 0,
                         (const struct sockaddr *)&s_udp_peer, sizeof(s_udp_peer));
    if (s_udp_tx != NULL) {
        xSemaphoreGive(s_udp_tx);
    }
    if (n != (int)len) {
        ESP_LOGW(TAG, "UDP sendto %d/%u", n, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int udp_link_write(void *arg, const uint8_t *frame, size_t len)
{
    return (udp_send(arg, frame, len) == ESP_OK) ? (int)len : -1;
}

// ── WebSocket ──────────────────────────────────────────────────────────

typedef struct {
    size_t len;
    uint8_t data[];
} ws_msg_t;

#if CONFIG_HTTPD_WS_SUPPORT
static int ws_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    if (s_ws_q == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    ws_msg_t *m = NULL;
    if (xQueueReceive(s_ws_q, &m, portMAX_DELAY) != pdTRUE || m == NULL) {
        return 0;
    }
    size_t n = (m->len < cap) ? m->len : cap;
    memcpy(buf, m->data, n);
    if (m->len > cap) {
        ESP_LOGW(TAG, "WS 訊息 %u > cap %u → 截斷", (unsigned)m->len, (unsigned)cap);
    }
    free(m);
    return (int)n;
}

static esp_err_t ws_send(void *arg, const uint8_t *buf, size_t len)
{
    (void)arg;
    const int fd = s_ws_fd;
    if (s_ws_http == NULL || fd < 0) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)buf,
        .len = len,
    };
    if (s_ws_tx != NULL) {
        xSemaphoreTake(s_ws_tx, portMAX_DELAY);
    }
    const esp_err_t e = httpd_ws_send_frame_async(s_ws_http, fd, &pkt);
    if (s_ws_tx != NULL) {
        xSemaphoreGive(s_ws_tx);
    }
    if (e != ESP_OK) {
        s_ws_fd = -1;
    }
    return e;
}

static int ws_link_write(void *arg, const uint8_t *frame, size_t len)
{
    return (ws_send(arg, frame, len) == ESP_OK) ? (int)len : -1;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        s_ws_fd = fd;
        ESP_LOGW(TAG, "WS 連線 fd=%d", fd);
        return ESP_OK;
    }
    if (fd != s_ws_fd) {
        s_ws_fd = fd;
    }
    httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_BINARY };
    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        s_ws_fd = -1;
        return ESP_OK;
    }
    if (pkt.type != HTTPD_WS_TYPE_BINARY || pkt.len == 0 || pkt.len > NET_WS_MAX) {
        return ESP_OK;
    }
    ws_msg_t *m = malloc(sizeof(*m) + pkt.len);
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    m->len = pkt.len;
    pkt.payload = m->data;
    if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) {
        free(m);
        return ESP_FAIL;
    }
    if (s_ws_q == NULL || xQueueSend(s_ws_q, &m, 0) != pdTRUE) {
        free(m);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else
// 這個 target 沒開 HTTPD WS：保留 link 名但回 not-supported，TCP/UDP 照用。
static int ws_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg; (void)buf; (void)cap;
    return 0;
}

static esp_err_t ws_send(void *arg, const uint8_t *buf, size_t len)
{
    (void)arg; (void)buf; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

static int ws_link_write(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg; (void)frame; (void)len;
    return -1;
}
#endif

// ── 建立／啟動 ─────────────────────────────────────────────────────────

esp_err_t nc4_line_net_declare(nc4_decoder_t *decoder, int core)
{
    if (decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_declared) {
        return ESP_OK;
    }
    s_tcp_tx = xSemaphoreCreateMutex();
    s_udp_tx = xSemaphoreCreateMutex();
    s_ws_tx = xSemaphoreCreateMutex();
    s_ws_q = xQueueCreate(32, sizeof(ws_msg_t *));
    if (s_tcp_tx == NULL || s_udp_tx == NULL || s_ws_tx == NULL || s_ws_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_tcp_rx = heap_caps_malloc(8208, MALLOC_CAP_SPIRAM);
    s_udp_rx = heap_caps_malloc(8208, MALLOC_CAP_SPIRAM);
    s_ws_rx = heap_caps_malloc(NET_WS_MAX + 64, MALLOC_CAP_SPIRAM);
    const nc4_bus_cfg_t tcp = {
        .name = "TCP", .mode = NC4_BUS_PUSH, .read = tcp_read, .send = tcp_send,
        .core = core, .rx_ring_size = 2048, .rx_slots = 4, .slots_psram = true,
        .rx_region = s_tcp_rx, .rx_region_bytes = s_tcp_rx ? 8208 : 0,
    };
    const nc4_bus_cfg_t udp = {
        .name = "UDP", .mode = NC4_BUS_PUSH, .read = udp_read, .send = udp_send,
        .core = core, .rx_ring_size = 2048, .rx_slots = 4, .slots_psram = true,
        .rx_region = s_udp_rx, .rx_region_bytes = s_udp_rx ? 8208 : 0,
    };
    const nc4_bus_cfg_t ws = {
        .name = "WS", .mode = NC4_BUS_PUSH, .read = ws_read, .send = ws_send,
        .core = core, .rx_ring_size = NET_WS_MAX, .rx_slots = 2, .slots_psram = true,
        .rx_region = s_ws_rx, .rx_region_bytes = s_ws_rx ? (NET_WS_MAX + 64) : 0,
    };
    if (nc4_bus_add(decoder, &tcp, &s_tcp_bus) != ESP_OK ||
        nc4_bus_add(decoder, &udp, &s_udp_bus) != ESP_OK ||
        nc4_bus_add(decoder, &ws, &s_ws_bus) != ESP_OK) {
        ESP_LOGE(TAG, "nc4_bus_add 失敗（bus 不夠）");
        return ESP_ERR_NO_MEM;
    }
    (void)nc4_link_register("TCP", tcp_link_write, NULL);
    (void)nc4_link_register("UDP", udp_link_write, NULL);
    (void)nc4_link_register("WS", ws_link_write, NULL);
    s_declared = true;
    ESP_LOGW(TAG, "line net declared：TCP/%d UDP/%d WS/%d（%s/%s/%s）",
             NET_TCP_PORT, NET_UDP_PORT, NET_WS_PORT,
             s_tcp_rx ? "psram" : "sram", s_udp_rx ? "psram" : "sram",
             s_ws_rx ? "psram" : "sram");
    return ESP_OK;
}

// 跟 CDC 一樣：**先等資料，再拿 slot**，唔可以 push_begin 後 blocking read。
// 這樣不需要 staging buffer，也不需要 copy（TCP/UDP 直接 recv 入 slot）。
static void tcp_producer(void *arg)
{
    (void)arg;
    for (;;) {
        const int fd = s_tcp_fd;
        if (fd < 0 || s_tcp_bus == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        const int pr = select(fd + 1, &rf, NULL, NULL, &tv);
        if (pr <= 0 || !FD_ISSET(fd, &rf)) {
            continue;
        }
        uint8_t *dst = nc4_bus_push_begin(s_tcp_bus, nc4_bus_slot_size(s_tcp_bus));
        if (dst == NULL) {
            vTaskDelay(1);
            continue;
        }
        const int n = tcp_read(NULL, dst, (size_t)nc4_bus_slot_size(s_tcp_bus));
        if (n > 0 && nc4_bus_push_commit(s_tcp_bus, (size_t)n) == ESP_OK) {
            nc4_bus_kick(s_tcp_bus);
        } else {
            nc4_bus_push_abort(s_tcp_bus);
            if (n < 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

static void udp_producer(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_udp_fd < 0 || s_udp_bus == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s_udp_fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        const int pr = select(s_udp_fd + 1, &rf, NULL, NULL, &tv);
        if (pr <= 0 || !FD_ISSET(s_udp_fd, &rf)) {
            continue;
        }
        uint8_t *dst = nc4_bus_push_begin(s_udp_bus, nc4_bus_slot_size(s_udp_bus));
        if (dst == NULL) {
            vTaskDelay(1);
            continue;
        }
        const int n = udp_read(NULL, dst, (size_t)nc4_bus_slot_size(s_udp_bus));
        if (n > 0 && nc4_bus_push_commit(s_udp_bus, (size_t)n) == ESP_OK) {
            nc4_bus_kick(s_udp_bus);
        } else {
            nc4_bus_push_abort(s_udp_bus);
        }
    }
}

static void ws_producer(void *arg)
{
    (void)arg;
    for (;;) {
        ws_msg_t *m = NULL;
        if (xQueuePeek(s_ws_q, &m, portMAX_DELAY) != pdTRUE || m == NULL) {
            continue;
        }
        if (m->len > NET_WS_MAX) {
            (void)xQueueReceive(s_ws_q, &m, 0);
            free(m);
            continue;
        }
        uint8_t *dst = nc4_bus_push_begin(s_ws_bus, m->len);
        if (dst == NULL) {
            vTaskDelay(1);          // queue 有頭，但 slot 滿：等 decode 排空
            continue;
        }
        if (xQueueReceive(s_ws_q, &m, 0) != pdTRUE || m == NULL) {
            nc4_bus_push_abort(s_ws_bus);
            continue;
        }
        memcpy(dst, m->data, m->len);
        const size_t len = m->len;
        free(m);
        if (nc4_bus_push_commit(s_ws_bus, len) == ESP_OK) {
            nc4_bus_kick(s_ws_bus);
        } else {
            nc4_bus_push_abort(s_ws_bus);
        }
    }
}

static esp_err_t start_tcp(void)
{
    if (s_tcp_listen >= 0) {
        return ESP_OK;
    }
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        return ESP_FAIL;
    }
    int yes = 1;
    int rcv = 32768;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(ls, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(NET_TCP_PORT),
    };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
        close(ls);
        return ESP_FAIL;
    }
    s_tcp_listen = ls;
    if (xTaskCreatePinnedToCore(tcp_accept_task, "net_tcp", 4096, NULL, 4, NULL, 0) != pdPASS) {
        close(ls);
        s_tcp_listen = -1;
        return ESP_ERR_NO_MEM;
    }
    if (s_tcp_bus != NULL) {
        (void)xTaskCreatePinnedToCore(tcp_producer, "net_tcp_rx", 4096, NULL,
                                      NC_PRIO_PRODUCER, NULL, 0);
    }
    return ESP_OK;
}

static esp_err_t start_udp(void)
{
    if (s_udp_fd >= 0) {
        return ESP_OK;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return ESP_FAIL;
    }
    int yes = 1;
    int rcv = 131072;
    int snd = 65536;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(NET_UDP_PORT),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    s_udp_fd = fd;
    if (s_udp_bus != NULL) {
        (void)xTaskCreatePinnedToCore(udp_producer, "net_udp_rx", 4096, NULL,
                                      NC_PRIO_PRODUCER, NULL, 0);
    }
    return ESP_OK;
}

#if CONFIG_HTTPD_WS_SUPPORT
static esp_err_t start_ws(void)
{
    if (s_ws_http != NULL) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = NET_WS_PORT;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_ws_http, &cfg) != ESP_OK) {
        s_ws_http = NULL;
        return ESP_FAIL;
    }
    const httpd_uri_t uri = {
        .uri = "/nc4",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    if (httpd_register_uri_handler(s_ws_http, &uri) != ESP_OK) {
        httpd_stop(s_ws_http);
        s_ws_http = NULL;
        return ESP_FAIL;
    }
    if (s_ws_bus != NULL) {
        (void)xTaskCreatePinnedToCore(ws_producer, "net_ws_rx", 4096, NULL,
                                      NC_PRIO_PRODUCER, NULL, 0);
    }
    return ESP_OK;
}

#else
static esp_err_t start_ws(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

httpd_handle_t nc4_line_net_httpd(void)
{
#if CONFIG_HTTPD_WS_SUPPORT
    return s_ws_http;
#else
    return NULL;
#endif
}

esp_err_t nc4_line_net_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    const esp_err_t t = start_tcp();
    const esp_err_t u = start_udp();
    const esp_err_t w = start_ws();
    // WS 可以係「這個 target 未開 HTTPD_WS_SUPPORT」；TCP/UDP 成功就算啟動。
    s_started = (t == ESP_OK && u == ESP_OK &&
                 (w == ESP_OK || w == ESP_ERR_NOT_SUPPORTED));
    ESP_LOGW(TAG, "line net start：TCP=%s UDP=%s WS=%s",
             esp_err_to_name(t), esp_err_to_name(u), esp_err_to_name(w));
    return s_started ? ESP_OK : ESP_FAIL;
}

size_t nc4_line_net_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return (size_t)snprintf(buf, cap,
                            "{\"tcp\":{\"fd\":%d,\"port\":%d},"
                            "\"udp\":{\"fd\":%d,\"port\":%d,\"peer\":%d},"
                            "\"ws\":{\"fd\":%d,\"port\":%d}}",
                            s_tcp_fd, NET_TCP_PORT, s_udp_fd, NET_UDP_PORT,
                            s_udp_has_peer ? 1 : 0, s_ws_fd, NET_WS_PORT);
}
