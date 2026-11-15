#include "nc_net_lines.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_random.h"

#include "nc4_bus.h"
#include "nc4_router.h"
#include "nc4_web.h"
#include "nc_cfg.h"


// ── WS handshake 用嘅 SHA1／Base64（唔拉 mbedtls，P4 預設冇開）──
typedef struct {
    uint32_t h[5];
    uint64_t bits;
    uint8_t buf[64];
    size_t n;
} sha1_t;

static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = tmp;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(sha1_t *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->bits = 0; s->n = 0;
}

static void sha1_update(sha1_t *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bits += (uint64_t)len * 8;
    while (len > 0) {
        const size_t take = (len < sizeof(s->buf) - s->n) ? len : sizeof(s->buf) - s->n;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == sizeof(s->buf)) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->bits;
    const uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    const uint8_t zero = 0;
    while (s->n != 56) sha1_update(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

static size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) |
                           ((i + 1 < len) ? ((uint32_t)in[i + 1] << 8) : 0) |
                           ((i + 2 < len) ? in[i + 2] : 0);
        if (o + 4 >= cap) break;
        out[o++] = tb[(v >> 18) & 63];
        out[o++] = tb[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tb[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tb[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static const char *TAG = "netline";

#define NET_MAX_LINES 8
#define NET_TASK_STACK 6144
#define NET_TASK_PRIO 9
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef enum {
    NL_TCP = 0,
    NL_UDP,
    NL_WS,
    NL_HTTP,
} nl_kind_t;

typedef enum {
    NLS_NONE = 0,
    NLS_NC4,
    NLS_WS,
    NLS_HTTP,
} nl_service_t;

typedef struct {
    const nc_link_cfg_t *cfg;
    nl_kind_t kind;
    nl_service_t service;
    int core;
    int listen_fd;
    volatile int fd;
    int udp_peer_set;
    struct sockaddr_in udp_peer;
    nc4_bus_handle_t bus;
    uint8_t *rx_region;
    size_t rx_region_bytes;
    SemaphoreHandle_t tx_lock;
    httpd_handle_t httpd;
    TaskHandle_t task;
} nl_line_t;

static nl_line_t s_lines[NET_MAX_LINES];
static size_t s_nlines;
static bool s_declared;
static bool s_started;
static char s_selftest_host[32];
static nc_net_decoder_fn s_get_decoder;

// ── 一次性 loopback／self-IP selftest（network.json: selftest=1）──
static uint32_t nl_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
    }
    return ~c;
}

static size_t nl_nc4(uint8_t *out, uint16_t cmd, const uint8_t *payload, size_t n)
{
    out[0] = 'N'; out[1] = 'C';
    out[2] = 4; out[3] = 0xFF; out[4] = 0xFF;
    out[5] = (uint8_t)cmd; out[6] = (uint8_t)(cmd >> 8);
    out[7] = (uint8_t)n; out[8] = (uint8_t)(n >> 8);
    if (n > 0 && payload != NULL) memcpy(out + 9, payload, n);
    const uint32_t crc = nl_crc32(out + 2, 7 + n);
    const size_t total = 9 + n + 4;
    out[9 + n] = (uint8_t)crc;
    out[10 + n] = (uint8_t)(crc >> 8);
    out[11 + n] = (uint8_t)(crc >> 16);
    out[12 + n] = (uint8_t)(crc >> 24);
    return total;
}

static int nl_connect(const char *host, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool nl_wait_cmd(int fd, uint16_t want, int timeout_ms)
{
    uint8_t buf[512];
    size_t n = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        fd_set rf;
        FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) {
            continue;
        }
        const int r = recv(fd, buf + n, sizeof(buf) - n, 0);
        if (r <= 0) return false;
        n += (size_t)r;
        for (size_t i = 0; i + 13 <= n; i++) {
            if (buf[i] != 'N' || buf[i + 1] != 'C') continue;
            const uint16_t ln = (uint16_t)(buf[i + 7] | (buf[i + 8] << 8));
            if (i + 9 + ln + 4 > n) break;
            const uint16_t cmd = (uint16_t)(buf[i + 5] | (buf[i + 6] << 8));
            if (cmd == want) return true;
        }
    }
    return false;
}

static bool nl_http_test(const char *host)
{
    const int fd = nl_connect(host, 80);
    if (fd < 0) return false;
    static const char req[] = "GET / HTTP/1.1\r\nHost: selftest\r\nConnection: close\r\n\r\n";
    (void)send(fd, req, sizeof(req) - 1, 0);
    char buf[128];
    const int n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, " 200 ") != NULL;
}

static bool nl_tcp_test(const char *host)
{
    const int fd = nl_connect(host, 6000);
    if (fd < 0) return false;
    uint8_t f[64];
    const uint8_t key[] = {3, 0, 's', 'y', 's'};
    const size_t n = nl_nc4(f, 0x1101, key, sizeof(key));
    (void)send(fd, f, n, 0);
    const bool ok = nl_wait_cmd(fd, 0x1102, 1500);
    close(fd);
    return ok;
}

static bool nl_udp_test(const char *host)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(6001);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return false;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    uint8_t f[64];
    const uint8_t key[] = {3, 0, 's', 'y', 's'};
    const size_t n = nl_nc4(f, 0x1101, key, sizeof(key));
    (void)sendto(fd, f, n, 0, (struct sockaddr *)&a, sizeof(a));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t rb[512];
    const int r = recv(fd, rb, sizeof(rb), 0);
    close(fd);
    if (r < 13) return false;
    return rb[0] == 'N' && rb[1] == 'C' &&
           (uint16_t)(rb[5] | (rb[6] << 8)) == 0x1102;
}

static bool nl_ws_test(const char *host)
{
    const int fd = nl_connect(host, 8080);
    if (fd < 0) return false;
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)esp_random();
    char key[32];
    const size_t kn = b64_encode(rnd, sizeof(rnd), key, sizeof(key));
    char req[256];
    const int rn = snprintf(req, sizeof(req),
                            "GET /nc4 HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Key: %.*s\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n",
                            host, (int)kn, key);
    (void)send(fd, req, (size_t)rn, 0);
    char resp[256];
    const int r = recv(fd, resp, sizeof(resp) - 1, 0);
    if (r <= 0 || strstr(resp, "101") == NULL) {
        close(fd);
        return false;
    }
    uint8_t f[64];
    const uint8_t keys[] = {3, 0, 's', 'y', 's'};
    const size_t n = nl_nc4(f, 0x1101, keys, sizeof(keys));
    uint8_t mask[4] = {(uint8_t)esp_random(), (uint8_t)esp_random(),
                       (uint8_t)esp_random(), (uint8_t)esp_random()};
    uint8_t frame[80];
    size_t o = 0;
    frame[o++] = 0x82;
    frame[o++] = (uint8_t)(0x80 | n);
    memcpy(frame + o, mask, 4); o += 4;
    for (size_t i = 0; i < n; i++) frame[o++] = f[i] ^ mask[i & 3u];
    (void)send(fd, frame, o, 0);
    const bool ok = nl_wait_cmd(fd, 0x1102, 1500);
    close(fd);
    return ok;
}

static void nl_selftest_task(void *arg)
{
    char host[32];
    snprintf(host, sizeof(host), "%s", (const char *)arg);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "selftest host=%s", host);
    ESP_LOGW(TAG, "selftest http :80  %s", nl_http_test(host) ? "PASS" : "FAIL");
    ESP_LOGW(TAG, "selftest tcp  :6000 %s", nl_tcp_test(host) ? "PASS" : "FAIL");
    ESP_LOGW(TAG, "selftest udp  :6001 %s", nl_udp_test(host) ? "PASS" : "FAIL");
    ESP_LOGW(TAG, "selftest ws   :8080 %s", nl_ws_test(host) ? "PASS" : "FAIL");
    vTaskDelete(NULL);
}

static bool kind_is_net(nc_link_kind_t k)
{
    return k == NC_LINK_TCP || k == NC_LINK_UDP || k == NC_LINK_WS || k == NC_LINK_HTTP;
}

static nl_kind_t kind_of(nc_link_kind_t k)
{
    switch (k) {
    case NC_LINK_TCP: return NL_TCP;
    case NC_LINK_UDP: return NL_UDP;
    case NC_LINK_WS:  return NL_WS;
    default:          return NL_HTTP;
    }
}

static const char *kind_name(nl_kind_t k)
{
    switch (k) {
    case NL_TCP:  return "tcp";
    case NL_UDP:  return "udp";
    case NL_WS:   return "ws";
    default:      return "http";
    }
}
static bool mem_is(const nc_link_cfg_t *l, const char *m)
{
    return l != NULL && strcmp(l->mem, m) == 0;
}

static void *line_alloc(size_t bytes, const nc_link_cfg_t *l)
{
    if (mem_is(l, "sram")) {
        return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (mem_is(l, "psram")) {
        return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (p != NULL) ? p : malloc(bytes);
}

static void close_fd(nl_line_t *ln)
{
    const int fd = ln->fd;
    if (fd >= 0) {
        ln->fd = -1;
        close(fd);
    }
}

static void close_listen(nl_line_t *ln)
{
    if (ln->listen_fd >= 0) {
        close(ln->listen_fd);
        ln->listen_fd = -1;
    }
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int n = send(fd, buf + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static esp_err_t nl_send(void *arg, const uint8_t *buf, size_t len)
{
    nl_line_t *ln = (nl_line_t *)arg;
    if (ln == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ln->tx_lock != NULL) {
        xSemaphoreTake(ln->tx_lock, portMAX_DELAY);
    }
    int r = -1;
    if (ln->kind == NL_UDP) {
        if (ln->fd >= 0 && ln->udp_peer_set) {
            r = sendto(ln->fd, buf, len, 0,
                       (struct sockaddr *)&ln->udp_peer, sizeof(ln->udp_peer));
        }
    } else if (ln->fd >= 0) {
        /* WS 回覆要加 frame header；TCP 直接送。 */
        if (ln->kind == NL_WS) {
            uint8_t hdr[10];
            size_t hn = 0;
            hdr[0] = 0x82;                      // FIN + binary
            if (len < 126) {
                hdr[1] = (uint8_t)len;
                hn = 2;
            } else if (len <= 0xFFFF) {
                hdr[1] = 126;
                hdr[2] = (uint8_t)(len >> 8);
                hdr[3] = (uint8_t)len;
                hn = 4;
            } else {
                hdr[1] = 127;
                for (int i = 0; i < 8; i++) {
                    hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
                }
                hn = 10;
            }
            if (len <= 512) {
                uint8_t tmp[10 + 512];
                memcpy(tmp, hdr, hn);
                memcpy(tmp + hn, buf, len);
                r = send_all(ln->fd, tmp, hn + len);
            } else if (send_all(ln->fd, hdr, hn) == 0) {
                r = send_all(ln->fd, buf, len);
            }
        } else {
            r = send_all(ln->fd, buf, len);
        }
    }
    if (ln->tx_lock != NULL) {
        xSemaphoreGive(ln->tx_lock);
    }
    return (r == 0) ? ESP_OK : ESP_FAIL;
}

static bool ws_read_exact(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int n = recv(fd, buf + off, len - off, 0);
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static bool ws_handshake(int fd)
{
    char req[1024];
    size_t n = 0;
    while (n < sizeof(req) - 1) {
        const int r = recv(fd, req + n, 1, 0);
        if (r <= 0) {
            return false;
        }
        n++;
        if (n >= 4 && memcmp(req + n - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    req[n] = '\0';
    if (n < 4 || memcmp(req + n - 4, "\r\n\r\n", 4) != 0) {
        return false;
    }

    const char *key = NULL;
    char *line = req;
    while (line != NULL && *line != '\0') {
        char *eol = strstr(line, "\r\n");
        if (eol != NULL) {
            *eol = '\0';
        }
        if (strncasecmp(line, "Sec-WebSocket-Key:", 18) == 0) {
            key = line + 18;
            while (*key == ' ' || *key == '\t') {
                key++;
            }
        }
        line = (eol != NULL) ? eol + 2 : NULL;
    }
    if (key == NULL || *key == '\0') {
        return false;
    }

    char src[128];
    snprintf(src, sizeof(src), "%s%s", key, WS_GUID);
    uint8_t sha[20];
    sha1_t sh;
    sha1_init(&sh);
    sha1_update(&sh, src, strlen(src));
    sha1_final(&sh, sha);
    char b64[64];
    const size_t b64n = b64_encode(sha, sizeof(sha), b64, sizeof(b64));
    char resp[256];
    const int rn = snprintf(resp, sizeof(resp),
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %.*s\r\n\r\n",
                            (int)b64n, b64);
    return rn > 0 && send_all(fd, (const uint8_t *)resp, (size_t)rn) == 0;
}

static void ws_loop(nl_line_t *ln)
{
    uint8_t *slot = NULL;
    while (ln->fd >= 0) {
        uint8_t hdr[2];
        if (!ws_read_exact(ln->fd, hdr, 2)) {
            break;
        }
        const uint8_t op = hdr[0] & 0x0F;
        const bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;
        if (len == 126) {
            uint8_t e[2];
            if (!ws_read_exact(ln->fd, e, 2)) break;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (!ws_read_exact(ln->fd, e, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
        }
        uint8_t mask[4] = {0};
        if (masked && !ws_read_exact(ln->fd, mask, 4)) {
            break;
        }
        if (len > nc4_bus_slot_size(ln->bus)) {
            ESP_LOGW(TAG, "[%s] WS frame %u > slot，關閉", ln->cfg->name, (unsigned)len);
            break;
        }
        slot = nc4_bus_push_begin(ln->bus, (size_t)len);
        while (slot == NULL) {
            // WS header 已讀走；唔可以 continue 再讀「下一個 header」，
            // 否則 payload 會被當成 header → 連線 reset。等 decoder 放返 slot 再讀 payload。
            vTaskDelay(pdMS_TO_TICKS(1));
            slot = nc4_bus_push_begin(ln->bus, (size_t)len);
        }
        if (len > 0 && !ws_read_exact(ln->fd, slot, (size_t)len)) {
            nc4_bus_push_abort(ln->bus);
            break;
        }
        if (masked) {
            uint32_t m32;
            memcpy(&m32, mask, sizeof(m32));
            uint64_t i = 0;
            for (; i + 4 <= len; i += 4) {
                uint32_t v;
                memcpy(&v, slot + i, sizeof(v));
                v ^= m32;
                memcpy(slot + i, &v, sizeof(v));
            }
            for (; i < len; i++) {
                slot[i] ^= mask[i & 3u];
            }
        }
        if (op == 0x8) {                     // close
            nc4_bus_push_abort(ln->bus);
            break;
        }
        if (op == 0x9 || op == 0xA) {        // ping / pong
            nc4_bus_push_abort(ln->bus);
            continue;
        }
        if (op != 0x1 && op != 0x2 && op != 0x0) {
            nc4_bus_push_abort(ln->bus);
            continue;
        }
        if (nc4_bus_push_commit(ln->bus, (size_t)len) != ESP_OK) {
            break;
        }
        nc4_bus_kick(ln->bus);
    }
}

static void tcp_task(void *arg)
{
    nl_line_t *ln = (nl_line_t *)arg;
    for (;;) {
        const int c = accept(ln->listen_fd, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ln->fd = c;
        int nodelay = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        nc4_bus_reset(ln->bus);
        ESP_LOGI(TAG, "[%s] tcp 連線", ln->cfg->name);
        for (;;) {
            uint8_t *slot = nc4_bus_push_begin(ln->bus, nc4_bus_slot_size(ln->bus));
            if (slot == NULL) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            const int n = recv(c, slot, nc4_bus_slot_size(ln->bus), 0);
            if (n <= 0) {
                nc4_bus_push_abort(ln->bus);
                break;
            }
            if (nc4_bus_push_commit(ln->bus, (size_t)n) != ESP_OK) {
                break;
            }
            nc4_bus_kick(ln->bus);
        }
        close_fd(ln);
        nc4_bus_reset(ln->bus);
        ESP_LOGW(TAG, "[%s] tcp 斷線", ln->cfg->name);
    }
}

static void udp_task(void *arg)
{
    nl_line_t *ln = (nl_line_t *)arg;
    for (;;) {
        uint8_t *slot = nc4_bus_push_begin(ln->bus, nc4_bus_slot_size(ln->bus));
        if (slot == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(ln->fd, slot, nc4_bus_slot_size(ln->bus), 0,
                               (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            nc4_bus_push_abort(ln->bus);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (ln->tx_lock != NULL) xSemaphoreTake(ln->tx_lock, portMAX_DELAY);
        ln->udp_peer = from;
        ln->udp_peer_set = 1;
        if (ln->tx_lock != NULL) xSemaphoreGive(ln->tx_lock);
        if (nc4_bus_push_commit(ln->bus, (size_t)n) != ESP_OK) {
            continue;
        }
        nc4_bus_kick(ln->bus);
    }
}

static void ws_task(void *arg)
{
    nl_line_t *ln = (nl_line_t *)arg;
    for (;;) {
        const int c = accept(ln->listen_fd, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ln->fd = c;
        nc4_bus_reset(ln->bus);
        int nodelay = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        if (!ws_handshake(c)) {
            ESP_LOGW(TAG, "[%s] WS handshake 失敗", ln->cfg->name);
            close_fd(ln);
            continue;
        }
        ESP_LOGI(TAG, "[%s] WS 連線", ln->cfg->name);
        ws_loop(ln);
        close_fd(ln);
        nc4_bus_reset(ln->bus);
        ESP_LOGW(TAG, "[%s] WS 斷線", ln->cfg->name);
    }
}

static int bind_port(uint16_t port, int type)
{
    const int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static esp_err_t start_http(nl_line_t *ln)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = ln->cfg->port;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.task_priority = 10;
    cfg.core_id = ln->core;
    esp_err_t e = httpd_start(&ln->httpd, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "[%s] httpd_start(%u): %s", ln->cfg->name,
                 (unsigned)ln->cfg->port, esp_err_to_name(e));
        return e;
    }
    (void)nc4_web_register(ln->httpd);
    ESP_LOGW(TAG, "[%s] HTTP :%u core=%d", ln->cfg->name,
             (unsigned)ln->cfg->port, ln->core);
    return ESP_OK;
}

// Service manager 的最小政策：line 只講 transport；service 由需求領取。
// 現階段用 well-known port 做預設 claim；日後可改成 services[] descriptor。
static nl_service_t service_claim(const nc_link_cfg_t *l)
{
    if (l->kind == NC_LINK_HTTP) return NLS_HTTP;
    if (l->kind == NC_LINK_WS) return NLS_WS;
    if (l->kind == NC_LINK_TCP && l->port == 80) return NLS_HTTP;
    if (l->kind == NC_LINK_TCP && l->port == 8080) return NLS_WS;
    if (kind_is_net(l->kind)) return NLS_NC4;
    return NLS_NONE;
}

esp_err_t nc_net_lines_declare(nc_net_decoder_fn get_decoder)
{
    if (s_declared) {
        return ESP_OK;
    }
    s_get_decoder = get_decoder;
    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t i = 0; i < sys->n_links && s_nlines < NET_MAX_LINES; i++) {
        const nc_link_cfg_t *l = &sys->links[i];
        ESP_LOGW(TAG, "scan %s kind=%d enable=%d managed=%d port=%u iface=%s service=%s",
                 l->name, (int)l->kind, (int)l->enable, (int)l->managed,
                 (unsigned)l->port, l->iface, l->service);
        if (!l->enable || !l->managed || !kind_is_net(l->kind)) {
            continue;
        }
        if (l->port == 0) {
            ESP_LOGW(TAG, "[%s] port=0 → 不建立", l->name);
            continue;
        }
        nl_line_t *ln = &s_lines[s_nlines];
        memset(ln, 0, sizeof(*ln));
        ln->cfg = l;
        ln->service = service_claim(l);
        if (ln->service == NLS_NONE) {
            ESP_LOGE(TAG, "line %s：沒有 service claim", l->name);
            continue;
        }
        if (ln->service == NLS_HTTP) {
            ln->kind = NL_HTTP;
        } else if (ln->service == NLS_WS) {
            ln->kind = NL_WS;
        } else {
            ln->kind = kind_of(l->kind);
        }
        ln->listen_fd = -1;
        ln->fd = -1;

        if (ln->service == NLS_HTTP) {
            ln->core = 0;                 // web 不需要 decoder；core 只係 httpd task
            s_nlines++;
            ESP_LOGW(TAG, "line %s：iface=%s http/%u service=http core=%d",
                     l->name, l->iface, (unsigned)l->port, ln->core);
            continue;
        }

        const int core = nc4_router_core(l->name);
        if (core == NC4_ROUTE_NONE) {
            ESP_LOGE(TAG, "line %s：routes 沒有指向 decodeN → 不建立", l->name);
            continue;
        }
        ln->core = core;
        nc4_decoder_t *dec = (get_decoder != NULL) ? get_decoder(core) : NULL;
        if (dec == NULL) {
            ESP_LOGE(TAG, "line %s：decoder[%d] 不存在 → 不建立", l->name, core);
            continue;
        }

        size_t bytes = (size_t)l->rx.slot_size * l->rx.slots;
        if (l->rx.region > bytes) {
            bytes = l->rx.region;
        }
        ln->rx_region = line_alloc(bytes, l);
        if (ln->rx_region == NULL) {
            ESP_LOGE(TAG, "line %s：buffer %u B 配不到", l->name, (unsigned)bytes);
            continue;
        }
        ln->rx_region_bytes = bytes;

        nc4_bus_cfg_t bc = {
            .name = l->name,
            .mode = NC4_BUS_PUSH,
            .send = nl_send,
            .send_arg = ln,
            .core = core,
            .rx_region = ln->rx_region,
            .rx_region_bytes = bytes,
            .budget_per_pass = l->rx.budget,
            .rx_ring_size = l->rx.slot_size,
            .rx_slots = l->rx.slots,
            // S3-Matrix 冇 PSRAM；NC_BUF_PSRAM 不會 fallback，會直接 NO_MEM。
            .slots_psram = !mem_is(l, "sram") &&
                           (!mem_is(l, "psram") ||
                            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >=
                            (size_t)l->rx.slot_size),
        };
        const esp_err_t e = nc4_bus_add(dec, &bc, &ln->bus);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "line %s：nc4_bus_add: %s", l->name, esp_err_to_name(e));
            free(ln->rx_region);
            ln->rx_region = NULL;
            continue;
        }
        ln->tx_lock = xSemaphoreCreateMutex();
        s_nlines++;
        ESP_LOGW(TAG, "line %s：iface=%s %s/%u service=%s core=%d slots=%u×%u",
                 l->name, l->iface, kind_name(ln->kind), (unsigned)l->port,
                 (ln->service == NLS_WS) ? "ws" : "nc4", core,
                 (unsigned)l->rx.slots, (unsigned)l->rx.slot_size);
        ESP_LOGW(TAG, "  mem=%s region=%u B", l->mem, (unsigned)bytes);
    }
    s_declared = true;
    ESP_LOGW(TAG, "declared %u network lines", (unsigned)s_nlines);
    return ESP_OK;
}

esp_err_t nc_net_lines_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    for (size_t i = 0; i < s_nlines; i++) {
        nl_line_t *ln = &s_lines[i];
        if (ln->service == NLS_HTTP) {
            (void)start_http(ln);
            continue;
        }
        if (ln->kind == NL_UDP) {
            ln->fd = bind_port(ln->cfg->port, SOCK_DGRAM);
            if (ln->fd < 0) {
                ESP_LOGE(TAG, "[%s] UDP bind :%u 失敗", ln->cfg->name,
                         (unsigned)ln->cfg->port);
                continue;
            }
            if (xTaskCreatePinnedToCore(udp_task, ln->cfg->name, NET_TASK_STACK,
                                        ln, NET_TASK_PRIO, &ln->task, ln->core) != pdPASS) {
                ESP_LOGE(TAG, "[%s] UDP task 建立失敗", ln->cfg->name);
                close(ln->fd);
                ln->fd = -1;
                continue;
            }
            ESP_LOGW(TAG, "[%s] UDP :%u core=%d", ln->cfg->name,
                     (unsigned)ln->cfg->port, ln->core);
            continue;
        }
        ln->listen_fd = bind_port(ln->cfg->port, SOCK_STREAM);
        if (ln->listen_fd < 0 || listen(ln->listen_fd, 2) != 0) {
            ESP_LOGE(TAG, "[%s] listen :%u 失敗", ln->cfg->name,
                     (unsigned)ln->cfg->port);
            close_listen(ln);
            continue;
        }
        const TaskFunction_t fn = (ln->kind == NL_WS) ? ws_task : tcp_task;
        if (xTaskCreatePinnedToCore(fn, ln->cfg->name, NET_TASK_STACK,
                                    ln, NET_TASK_PRIO, &ln->task, ln->core) != pdPASS) {
            ESP_LOGE(TAG, "[%s] task 建立失敗", ln->cfg->name);
            close_listen(ln);
            continue;
        }
        ESP_LOGW(TAG, "[%s] %s :%u core=%d", ln->cfg->name,
                 (ln->kind == NL_WS) ? "WS" : "TCP",
                 (unsigned)ln->cfg->port, ln->core);
    }
    s_started = true;

    // 一次性診斷：network.json 寫 selftest=1；host 預設 127.0.0.1。
    cfg_file_t *nc = nc_cfg_open("network");
    if (nc != NULL) {
        const cJSON *root = nc_cfg_root(nc);
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "selftest");
        const bool want = cJSON_IsNumber(en) ? (en->valueint != 0) : cJSON_IsTrue(en);
        const char *host = nc_cfg_str(root, "selftest_host", "127.0.0.1");
        snprintf(s_selftest_host, sizeof(s_selftest_host), "%s",
                 (host != NULL) ? host : "127.0.0.1");
        nc_cfg_close(nc);
        if (want) {
            (void)xTaskCreate(nl_selftest_task, "net_selftest", 4096,
                              s_selftest_host, 3, NULL);
        }
    }
    return ESP_OK;
}

bool nc_net_lines_enabled(void)
{
    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < sys->n_links; i++) {
        const nc_link_cfg_t *l = &sys->links[i];
        if (l->enable && l->managed && kind_is_net(l->kind) && l->port != 0) {
            return true;
        }
    }
    return false;
}
