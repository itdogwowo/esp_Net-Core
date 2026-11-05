// TCP 吞吐量測端點：sink 5000/5003、src 5001/5002。測試用，受 NC_ENABLE_TESTS gate。
#include "nc_test_tcp.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "test_tcp";

// 上傳測試：接受連線 → 讀一行 "GO <bytes>" → 全力送出 → 印速率
static void tcp_src_task(void *arg)
{
    const int port = (int)(intptr_t)arg;
    // 4 KB 送出緩衝由 PSRAM 出：這條 src 線只係吞吐測試用，
    // 唔應該為咗量一次而長期霸住 SRAM。
    uint8_t *txbuf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (txbuf == NULL) {
        txbuf = malloc(4096);
    }
    if (txbuf == NULL) {
        ESP_LOGE(TAG, "src%d：送出緩衝配唔到", port);
        vTaskDelete(NULL);
        return;
    }
    memset(txbuf, 0x5A, 4096);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "src%d bind/listen 失敗", port);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "TCP src 在 %d 等連線", port);

    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            vTaskDelay(10);
            continue;
        }
        char cmd[64] = {0};
        int n = recv(c, cmd, sizeof(cmd) - 1, 0);
        size_t total = 0;
        if (n > 0 && sscanf(cmd, "GO %u", (unsigned *)&total) != 1) {
            total = 0;
        }
        if (total == 0) {
            close(c);
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const int64_t t0 = esp_timer_get_time();
        size_t sent = 0;
        while (sent < total) {
            size_t want = total - sent;
            if (want > 4096) {
                want = 4096;
            }
            int w = send(c, txbuf, want, 0);
            if (w <= 0) {
                break;
            }
            sent += (size_t)w;
        }
        const int64_t dt = esp_timer_get_time() - t0;
        ESP_LOGW(TAG, "src%d: %.2f MB / %.3f s = %.2f MB/s", port,
                 (double)sent / 1048576.0, (double)dt / 1e6,
                 dt > 0 ? (double)sent / (double)dt : 0.0);
        close(c);
    }
}

// TCP 吞吐測試：接受連線 → 一直讀 → 每秒印一次速率
// arg = port（可以同時開兩條，用來量「單向 2 條 stream 的合計」）
static void tcp_sink_task(void *arg)
{
    const int port = (int)(intptr_t)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        ESP_LOGE(TAG, "socket 失敗");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "sink%d bind/listen 失敗", port);
        close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "TCP sink 在 port %d 等連線", port);

    const size_t sink_buf = 16384;   // 每條 sink 自己的緩衝（兩條同時跑不能共用）
    uint8_t *buf = malloc(sink_buf);
    if (buf == NULL) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(ls, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            vTaskDelay(100);
            continue;
        }
        ESP_LOGW(TAG, "TCP 連線進來");
        uint64_t total = 0;
        int64_t t0 = esp_timer_get_time();
        int64_t t_last = t0;
        uint64_t last_report = 0;
        for (;;) {
            int n = recv(c, buf, sink_buf, 0);   // ★ 不能用 sizeof(buf)：buf 是指標 (4 B)
            if (n <= 0) {
                break;
            }
            total += (uint64_t)n;
            int64_t now = esp_timer_get_time();
            if (now - t_last >= 1000000) {
                double mbps = (double)(total - last_report) / (double)(now - t_last);
                ESP_LOGW(TAG, "RX %.2f MB/s（累計 %.2f MB）", mbps,
                         (double)total / (1024.0 * 1024.0));
                last_report = total;
                t_last = now;
            }
        }
        int64_t dt = esp_timer_get_time() - t0;
        ESP_LOGW(TAG, "TCP 結束：%.2f MB / %.2f s = %.2f MB/s",
                 (double)total / (1024.0 * 1024.0), (double)dt / 1e6,
                 dt > 0 ? (double)total / (double)dt : 0.0);
        close(c);
    }
}

void nc_test_tcp_start(void)
{
    xTaskCreatePinnedToCore(tcp_sink_task, "tcp_sink0", 6144, (void *)5000, 5, NULL, 1);
    xTaskCreatePinnedToCore(tcp_sink_task, "tcp_sink3", 6144, (void *)5003, 5, NULL, 0);
    xTaskCreatePinnedToCore(tcp_src_task, "tcp_src0", 6144, (void *)5001, 5, NULL, 0);
    xTaskCreatePinnedToCore(tcp_src_task, "tcp_src1", 6144, (void *)5002, 5, NULL, 1);
}
