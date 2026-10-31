#include "nc4_bus.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nc4_bus";

#define DEFAULT_READ_CHUNK      256
#define DEFAULT_BUDGET_PER_PASS 8
#define DEFAULT_POLL_MS         10
#define DEFAULT_RING_SIZE       2048
#define DEFAULT_TASK_STACK      4096
#define DEFAULT_TASK_PRIO       4
#define MAX_CHUNK               1024

struct nc4_bus {
    bool used;
    nc4_bus_cfg_t cfg;
    nc4_decoder_t *decoder;
    nc4_parser_t *parser;

    size_t max_frame;
    size_t read_chunk;
    uint32_t budget;

    // 每條 bus 自己的暫存區（任務用它跟 driver/ring 交換資料）
    // 刻意不共用：兩條線的位元組永遠不會出現在同一塊記憶體裡
    uint8_t *chunk;

    // PUSH 模式的中間緩衝（ISR → 任務）
    uint8_t *ring;
    size_t ring_size;
    volatile size_t head;   // 寫入端（producer）
    volatile size_t tail;   // 讀取端（consumer）
    portMUX_TYPE mux;
};

static struct nc4_bus s_buses[NC4_MAX_BUSES];
static TaskHandle_t s_task = NULL;
static size_t s_count = 0;

// ───────────────────────── ring ─────────────────────────

static size_t ring_used(const struct nc4_bus *b)
{
    size_t head = b->head, tail = b->tail;
    return (head >= tail) ? (head - tail) : (b->ring_size - tail + head);
}

static size_t ring_space(const struct nc4_bus *b)
{
    return b->ring_size - 1 - ring_used(b);
}

static esp_err_t ring_push(struct nc4_bus *b, const uint8_t *data, size_t len)
{
    if (len > ring_space(b)) {
        return ESP_ERR_NO_MEM;
    }
    size_t head = b->head;
    size_t first = b->ring_size - head;
    if (first > len) {
        first = len;
    }
    memcpy(b->ring + head, data, first);
    if (len > first) {
        memcpy(b->ring, data + first, len - first);
    }
    b->head = (head + len) % b->ring_size;
    return ESP_OK;
}

static size_t ring_pop(struct nc4_bus *b, uint8_t *out, size_t cap)
{
    size_t used = ring_used(b);
    if (used == 0) {
        return 0;
    }
    size_t n = used > cap ? cap : used;
    size_t tail = b->tail;
    size_t first = b->ring_size - tail;
    if (first > n) {
        first = n;
    }
    memcpy(out, b->ring + tail, first);
    if (n > first) {
        memcpy(out + first, b->ring, n - first);
    }
    b->tail = (tail + n) % b->ring_size;
    return n;
}

// ───────────────────────── 註冊 ─────────────────────────

// PULL 通道的 parser/ring 延後到第一次收到資料才配置（閒置通道只花 chunk）。
static esp_err_t bus_alloc(struct nc4_bus *b)
{
    if (b->parser != NULL) {
        return ESP_OK;
    }

    if (b->ring == NULL && b->cfg.mode == NC4_BUS_PUSH) {
        b->ring = malloc(b->ring_size);
        if (b->ring == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    b->parser = nc4_parser_create(b->decoder, b->cfg.send, b->cfg.send_arg,
                                  b->cfg.name ? b->cfg.name : "?", b->max_frame);
    if (b->parser == NULL) {
        free(b->ring);
        b->ring = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[%s] 配置 parser %u B", b->cfg.name, (unsigned)b->max_frame);
    return ESP_OK;
}

esp_err_t nc4_bus_add(nc4_decoder_t *decoder, const nc4_bus_cfg_t *cfg,
                      nc4_bus_handle_t *out_handle)
{
    if (decoder == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->mode == NC4_BUS_PULL && cfg->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= NC4_MAX_BUSES) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (b->used) {
            continue;
        }

        memset(b, 0, sizeof(*b));
        b->cfg = *cfg;
        // 0 = 用框架預設（不是硬性值 —— 之後由 config.json 覆寫）
        b->max_frame = cfg->max_frame ? cfg->max_frame : NC4_MAX_FRAME;
        b->read_chunk = cfg->read_chunk ? cfg->read_chunk : DEFAULT_READ_CHUNK;
        b->budget = cfg->budget_per_pass ? cfg->budget_per_pass : DEFAULT_BUDGET_PER_PASS;
        b->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

        b->chunk = malloc(b->read_chunk);
        if (b->chunk == NULL) {
            return ESP_ERR_NO_MEM;
        }

        if (cfg->mode == NC4_BUS_PUSH) {
            b->ring_size = cfg->rx_ring_size ? cfg->rx_ring_size : DEFAULT_RING_SIZE;
            b->ring = malloc(b->ring_size);
            if (b->ring == NULL) {
                free(b->chunk);
                b->chunk = NULL;
                return ESP_ERR_NO_MEM;
            }
        }

        b->decoder = decoder;

        if (cfg->mode == NC4_BUS_PUSH) {
            // PUSH 的資料可能從 ISR 進來，ISR 不能 malloc → 註冊時就要備好
            esp_err_t aerr = bus_alloc(b);
            if (aerr != ESP_OK) {
                free(b->ring);
                free(b->chunk);
                b->ring = NULL;
                b->chunk = NULL;
                return aerr;
            }
        }

        b->used = true;
        s_count++;
        ESP_LOGI(TAG, "bus +%s（%s, frame_buf=%u, chunk=%u, budget=%u%s%u）",
                 cfg->name ? cfg->name : "?",
                 cfg->mode == NC4_BUS_PUSH ? "push" : "pull",
                 (unsigned)b->max_frame, (unsigned)b->read_chunk, (unsigned)b->budget,
                 b->ring ? ", ring=" : "",
                 b->ring ? (unsigned)b->ring_size : 0u);

        if (out_handle != NULL) {
            *out_handle = b;
        }
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t nc4_bus_push(nc4_bus_handle_t bus, const uint8_t *data, size_t len)
{
    if (bus == NULL || !bus->used || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus->cfg.mode != NC4_BUS_PUSH) {
        return ESP_ERR_INVALID_STATE;
    }

    if (bus->parser == NULL) {
        return ESP_ERR_INVALID_STATE;   // PUSH 通道應該在註冊時就備好
    }

    taskENTER_CRITICAL(&bus->mux);
    esp_err_t err = ring_push(bus, data, len);
    taskEXIT_CRITICAL(&bus->mux);
    return err;
}

size_t nc4_bus_count(void)
{
    return s_count;
}

// ───────────────────────── 解碼任務 ─────────────────────────

void nc4_bus_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "解碼任務啟動，%u 條通道", (unsigned)s_count);

    for (;;) {
        bool busy = false;

        for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
            struct nc4_bus *b = &s_buses[i];
            if (!b->used) {
                continue;
            }

            // 每條 bus 有獨立預算，避免一條忙線把其他通道餓死
            for (uint32_t n = 0; n < b->budget; n++) {
                size_t got = 0;

                if (b->cfg.mode == NC4_BUS_PULL) {
                    int r = b->cfg.read(b->cfg.read_arg, b->chunk, b->read_chunk);
                    if (r < 0) {
                        ESP_LOGW(TAG, "[%s] read 錯誤", b->cfg.name);
                        break;
                    }
                    got = (size_t)r;
                } else {
                    taskENTER_CRITICAL(&b->mux);
                    got = ring_pop(b, b->chunk, b->read_chunk);
                    taskEXIT_CRITICAL(&b->mux);
                }

                if (got == 0) {
                    break;
                }
                if (b->parser == NULL && bus_alloc(b) != ESP_OK) {
                    ESP_LOGE(TAG, "[%s] parser 配置失敗，丟棄 %u B",
                             b->cfg.name, (unsigned)got);
                    break;
                }
                busy = true;
                nc4_parser_feed(b->parser, b->chunk, got);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(busy ? 1 : DEFAULT_POLL_MS));
    }
}

esp_err_t nc4_bus_start(uint32_t poll_ms)
{
    (void)poll_ms;   // 目前用「有資料 1ms、沒有 10ms」的自適應節奏
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(nc4_bus_task_entry, "nc4_bus", DEFAULT_TASK_STACK, NULL,
                    DEFAULT_TASK_PRIO, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
