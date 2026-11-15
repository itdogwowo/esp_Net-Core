#include "nc_net_raw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nc_cfg.h"

static const char *TAG = "netraw";

static int raw_bind(uint16_t port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 2) != 0) {
        close(ls);
        return -1;
    }
    return ls;
}

static void raw_sink(void *arg)
{
    const uint16_t port = (uint16_t)(uintptr_t)arg;
    const int ls = raw_bind(port);
    if (ls < 0) { ESP_LOGE(TAG, "sink bind 5000 失敗"); vTaskDelete(NULL); return; }
    uint8_t *buf = malloc(16384);
    if (buf == NULL) { close(ls); vTaskDelete(NULL); return; }
    ESP_LOGW(TAG, "RAW sink :%u", (unsigned)port);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { vTaskDelay(20); continue; }
        uint64_t total = 0;
        int64_t t0 = esp_timer_get_time();
        for (;;) {
            int n = recv(c, buf, 16384, 0);
            if (n <= 0) break;
            total += (uint64_t)n;
        }
        int64_t dt = esp_timer_get_time() - t0;
        ESP_LOGW(TAG, "RAW sink: %.2f MB / %.3f s = %.2f MB/s",
                 (double)total / 1048576.0, (double)dt / 1e6,
                 dt > 0 ? (double)total / (double)dt : 0.0);
        close(c);
    }
}

static void raw_src(void *arg)
{
    const uint16_t port = (uint16_t)(uintptr_t)arg;
    const int ls = raw_bind(port);
    if (ls < 0) { ESP_LOGE(TAG, "src bind 5001 失敗"); vTaskDelete(NULL); return; }
    // 對照舊 MPY：上游 send buffer 用內部 SRAM（PSRAM 留給大緩衝）。
    uint8_t *tx = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (tx == NULL) tx = malloc(4096);
    if (tx == NULL) { close(ls); vTaskDelete(NULL); return; }
    memset(tx, 0x5A, 4096);
    ESP_LOGW(TAG, "RAW src :%u（4K send cap）", (unsigned)port);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { vTaskDelay(20); continue; }
        char cmd[64] = {0};
        int n = recv(c, cmd, sizeof(cmd) - 1, 0);
        unsigned total = 0;
        if (n <= 0 || sscanf(cmd, "GO %u", &total) != 1 || total == 0) {
            close(c); continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int snd = 32768;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
        size_t sent = 0;
        int64_t t0 = esp_timer_get_time();
        while (sent < total) {
            size_t want = total - sent;
            if (want > 4096) want = 4096;
            int w = send(c, tx, want, 0);
            if (w <= 0) break;
            sent += (size_t)w;
        }
        int64_t dt = esp_timer_get_time() - t0;
        ESP_LOGW(TAG, "RAW src: %.2f MB / %.3f s = %.2f MB/s",
                 (double)sent / 1048576.0, (double)dt / 1e6,
                 dt > 0 ? (double)sent / (double)dt : 0.0);
        close(c);
    }
}

void nc_net_raw_start_if_configured(void)
{
    cfg_file_t *cfg = nc_cfg_open("network");
    if (cfg == NULL) return;
    const cJSON *root = nc_cfg_root(cfg);
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "rawbench");
    const bool want = cJSON_IsNumber(en) ? (en->valueint != 0) : cJSON_IsTrue(en);
    nc_cfg_close(cfg);
    if (!want) return;
    xTaskCreatePinnedToCore(raw_sink, "raw_sink0", 6144, (void *)5000, 8, NULL, 1);
    xTaskCreatePinnedToCore(raw_sink, "raw_sink3", 6144, (void *)5003, 8, NULL, 0);
    xTaskCreatePinnedToCore(raw_src,  "raw_src0",  6144, (void *)5001, 8, NULL, 1);
    xTaskCreatePinnedToCore(raw_src,  "raw_src1",  6144, (void *)5002, 8, NULL, 0);
    ESP_LOGW(TAG, "RAW test endpoints enabled :5000/:5001");
}
