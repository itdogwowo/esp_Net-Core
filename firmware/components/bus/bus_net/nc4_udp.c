#include "nc4_udp.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nc4_link.h"

static const char *TAG = "nc4_udp";

// 一則 datagram 的上限。8 KB 幀 + 幾個小幀都放得下；
// 超過 MTU 會由 IP 分片（LAN 上可接受，實測見 testing.md §2.16）。
#define UDP_MAX_MSG   (16 * 1024)

static int               s_sock = -1;
static uint16_t          s_port;
static nc4_parser_t     *s_parser;
static SemaphoreHandle_t s_tx_lock;
static TaskHandle_t      s_task;

static struct sockaddr_in s_peer;      // 最後一則 datagram 的來源（回覆對象）
static volatile bool      s_has_peer;

static volatile uint32_t s_msgs;
static volatile uint32_t s_bytes;
static volatile uint32_t s_tx;
static volatile uint32_t s_bad;
static volatile uint32_t s_trunc;

// frame 模式的 parser 不需要 ring
static uint8_t s_ring_stub[64];
// 16 KB 接收暫存：改由 PSRAM 出（start 時才配）。CPU copy，唔經 DMA。
static uint8_t *s_rx;

static esp_err_t udp_send(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    if (s_sock < 0 || !s_has_peer) {
        return ESP_FAIL;
    }
    if (s_tx_lock != NULL) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    const int n = sendto(s_sock, frame, len, 0,
                         (const struct sockaddr *)&s_peer, sizeof(s_peer));
    if (s_tx_lock != NULL) {
        xSemaphoreGive(s_tx_lock);
    }
    if (n != (int)len) {
        ESP_LOGW(TAG, "sendto 失敗/短送（%d/%u）", n, (unsigned)len);
        return ESP_FAIL;
    }
    s_tx++;
    return ESP_OK;
}

// 轉寄出口：UDP 的 sendto 是一則 datagram（整幀交出去）。
// 目標是「最後一則 datagram 的來源」—— 沒有 peer 就回錯，由 link 表計 drop。
static int udp_link_write(void *arg, const uint8_t *frame, size_t len)
{
    return (udp_send(arg, frame, len) == ESP_OK) ? (int)len : -1;
}

static void udp_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(s_sock, s_rx, UDP_MAX_MSG, 0,
                               (struct sockaddr *)&from, &fl);
        if (n < 0) {
            vTaskDelay(1);
            continue;
        }
        if (n == 0) {
            continue;
        }
        if (n == (int)UDP_MAX_MSG) {
            s_trunc++;                  // 可能被截斷（datagram 比 buffer 大）
        }

        // 回覆對象 = 這一則的來源
        if (s_tx_lock != NULL) {
            xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        }
        s_peer = from;
        s_has_peer = true;
        if (s_tx_lock != NULL) {
            xSemaphoreGive(s_tx_lock);
        }

        s_msgs++;
        s_bytes += (uint32_t)n;

        // 一期 datagram 可以塞多個連續 NC4 幀；逐幀原地解析（零應用層複製）
        size_t off = 0;
        while (off + NC4_HDR_LEN <= (size_t)n) {
            const size_t total = nc4_header_total(s_rx + off);
            if (total == 0 || off + total > (size_t)n) {
                s_bad++;
                break;
            }
            const esp_err_t perr = nc4_parser_frame(s_parser, s_rx + off, total);
            if (perr != ESP_OK) {
                s_bad++;
            }
            off += total;
        }
    }
}

esp_err_t nc4_udp_start(nc4_decoder_t *decoder, uint16_t port)
{
    if (decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sock >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // 接收暫存：優先 PSRAM，拿不到退回內部 RAM（行為不變，位置不同）。
    s_rx = heap_caps_malloc(UDP_MAX_MSG, MALLOC_CAP_SPIRAM);
    if (s_rx == NULL) {
        s_rx = malloc(UDP_MAX_MSG);
    }
    if (s_rx == NULL) {
        ESP_LOGE(TAG, "UDP 接收暫存（%u B）配唔到", (unsigned)UDP_MAX_MSG);
        return ESP_ERR_NO_MEM;
    }

    s_parser = nc4_parser_create_in(s_ring_stub, sizeof(s_ring_stub), decoder,
                                    udp_send, NULL, "UDP");
    if (s_parser == NULL) {
        free(s_rx);
        s_rx = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_tx_lock = xSemaphoreCreateMutex();
    (void)nc4_link_register("UDP", udp_link_write, NULL);

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket 失敗");
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
        free(s_rx);
        s_rx = NULL;
        return ESP_FAIL;
    }
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int rcvbuf = 64 * 1024;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind %u 失敗", (unsigned)port);
        close(s_sock);
        s_sock = -1;
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
        free(s_rx);
        s_rx = NULL;
        return ESP_FAIL;
    }

    // blocking recvfrom：睡在 lwIP，不輪詢
    if (xTaskCreatePinnedToCore(udp_task, "nc4_udp", 6144, NULL, 10, &s_task, 1) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
        free(s_rx);
        s_rx = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_port = port;
    ESP_LOGW(TAG, "NC4 over UDP 在 %u", (unsigned)port);
    return ESP_OK;
}

void nc4_udp_stop(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    if (s_parser != NULL) {
        nc4_parser_destroy(s_parser);
        s_parser = NULL;
    }
    if (s_rx != NULL) {
        free(s_rx);
        s_rx = NULL;
    }
}

size_t nc4_udp_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    char peer[24] = "-";
    if (s_has_peer) {
        snprintf(peer, sizeof(peer), "%s:%u", inet_ntoa(s_peer.sin_addr),
                 (unsigned)ntohs(s_peer.sin_port));
    }
    return (size_t)snprintf(buf, cap,
                            "{\"port\":%u,\"peer\":\"%s\",\"msgs\":%u,\"bytes\":%u,"
                            "\"tx\":%u,\"bad\":%u,\"trunc\":%u}",
                            (unsigned)s_port, peer, (unsigned)s_msgs,
                            (unsigned)s_bytes, (unsigned)s_tx,
                            (unsigned)s_bad, (unsigned)s_trunc);
}
