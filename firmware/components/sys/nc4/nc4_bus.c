#include "nc4_bus.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nc4_bus";

#define DEFAULT_READ_CHUNK      256
#define DEFAULT_BUDGET_PER_PASS 8
#define DEFAULT_POLL_MS         10
#define DEFAULT_RING_SIZE       2048
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

    // PUSH 模式：N 個插槽（producer 寫、consumer 讀；每格獨立，可零複製）
    uint8_t *slots;
    size_t slot_size;
    int slot_n;
    int slot_w;             // 下一個要寫的格子
    int slot_r;             // 下一個要讀的格子
    int push_idx;           // 正在寫的格子（-1 = 沒有）
    int rd_idx;             // 正在讀的格子（-1 = 沒有）
    size_t rd_off;
    volatile uint8_t slot_state[16];   // 0=free 1=ready 2=writing
    size_t slot_len[16];
    portMUX_TYPE mux;

    // 零複製模式：producer 一次 commit「剛好一幀」，consumer 直接在插槽上解析
    uint8_t frame_mode;
    int frame_idx;                    // consumer 目前持有哪一格（-1 = 沒有）

    bool owner_ready;                 // owner_init 已跑（每個核一次）

    // 統計（0xF008）：證明 copy 有沒有真的消失
    uint32_t st_frames;
    uint64_t st_bytes;                // 收進來的 bytes（含表頭/CRC）
    uint64_t st_copied;               // 為了「把資料交給 parser」所做的 memcpy bytes
    uint64_t st_parse_us;             // 解析佔用的時間
};

static struct nc4_bus s_buses[NC4_MAX_BUSES];

static struct nc4_bus *find_bus(nc4_bus_handle_t h) { return (struct nc4_bus *)h; }
static TaskHandle_t s_decode_task[2];   // 每個核心一個 decode task
static size_t s_count = 0;

// producer 每 commit 一格就叫醒 decode task。
// 沒有這個的話，decode 在 hub 剛好空的那一刻會睡 10 ms —— 小幀的吞吐會死在
// 「每 10 ms 只吃到 8 幀」（實測 1 KB：0.78 MB/s vs 6.24 MB/s）。
void nc4_bus_kick(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL) {
        return;
    }
    const int core = (b->cfg.core == 1) ? 1 : 0;
    if (s_decode_task[core] != NULL) {
        xTaskNotifyGive(s_decode_task[core]);
    }
}

// ───────────────────────── 插槽（PUSH 模式）─────────────────────────

static int slot_find_free(struct nc4_bus *b)
{
    for (int i = 0; i < b->slot_n; i++) {
        int idx = (b->slot_w + i) % b->slot_n;
        if (b->slot_state[idx] == 0) {
            return idx;
        }
    }
    return -1;
}

uint8_t *nc4_bus_push_begin(nc4_bus_handle_t bus, size_t want)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL || b->push_idx >= 0) {
        return NULL;
    }
    if (want > b->slot_size) {
        want = b->slot_size;
    }
    taskENTER_CRITICAL(&b->mux);
    int idx = slot_find_free(b);
    if (idx >= 0) {
        b->slot_state[idx] = 2;      // writing
        b->push_idx = idx;
    }
    taskEXIT_CRITICAL(&b->mux);
    return idx >= 0 ? b->slots + (size_t)idx * b->slot_size : NULL;
}

esp_err_t nc4_bus_push_commit(nc4_bus_handle_t bus, size_t len)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->push_idx < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&b->mux);
    const int idx = b->push_idx;
    b->slot_len[idx] = len > b->slot_size ? b->slot_size : len;
    b->slot_state[idx] = 1;          // ready
    b->slot_w = (idx + 1) % b->slot_n;
    b->push_idx = -1;
    taskEXIT_CRITICAL(&b->mux);
    return ESP_OK;
}

esp_err_t nc4_bus_push_abort(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->push_idx < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&b->mux);
    b->slot_state[b->push_idx] = 0;      // free
    b->push_idx = -1;
    taskEXIT_CRITICAL(&b->mux);
    return ESP_OK;
}

esp_err_t nc4_bus_push(nc4_bus_handle_t bus, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint8_t *dst = nc4_bus_push_begin(bus, len - off);
        if (dst == NULL) {
            return ESP_ERR_NO_MEM;
        }
        size_t n = len - off;
        struct nc4_bus *b = find_bus(bus);
        if (n > b->slot_size) {
            n = b->slot_size;
        }
        memcpy(dst, data + off, n);
        nc4_bus_push_commit(bus, n);
        off += n;
    }
    return ESP_OK;
}

esp_err_t nc4_bus_reset(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nc4_parser_reset(b->parser);      // parser 可能還沒配置（第一次收到資料才配）
    if (b->slots != NULL) {
        taskENTER_CRITICAL(&b->mux);
        for (int i = 0; i < b->slot_n; i++) {
            if (b->slot_state[i] == 1) {   // 只丟 READY；WRITING 是 producer 正在寫
                b->slot_state[i] = 0;
            }
        }
        b->rd_idx = -1;
        b->rd_off = 0;
        taskEXIT_CRITICAL(&b->mux);
    }
    return ESP_OK;
}

// ── 零複製：整個 frame 一格 ──

// 取下一格「已 commit 的整幀」，直接回插槽指標（不 memcpy）。
// 呼叫端解析完必須呼叫 nc4_bus_frame_end() 歸還。
uint8_t *nc4_bus_frame_begin(nc4_bus_handle_t bus, size_t *len)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL || b->frame_idx >= 0) {
        return NULL;
    }
    uint8_t *p = NULL;
    taskENTER_CRITICAL(&b->mux);
    for (int i = 0; i < b->slot_n; i++) {
        int idx = (b->slot_r + i) % b->slot_n;
        if (b->slot_state[idx] == 1) {          // READY
            b->slot_state[idx] = 3;             // READING（frame 模式專用值）
            b->slot_r = (idx + 1) % b->slot_n;
            b->frame_idx = idx;
            if (len != NULL) {
                *len = b->slot_len[idx];
            }
            p = b->slots + (size_t)idx * b->slot_size;
            break;
        }
    }
    taskEXIT_CRITICAL(&b->mux);
    return p;
}

void nc4_bus_frame_end(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->frame_idx < 0) {
        return;
    }
    taskENTER_CRITICAL(&b->mux);
    b->slot_state[b->frame_idx] = 0;            // free
    b->frame_idx = -1;
    taskEXIT_CRITICAL(&b->mux);
}

// 從插槽取資料；跨插槽自動接續（consumer 端只有這一份複製）
static size_t slot_pop(struct nc4_bus *b, uint8_t *out, size_t cap)
{
    size_t produced = 0;
    while (produced < cap) {
        taskENTER_CRITICAL(&b->mux);
        if (b->rd_idx < 0) {
            int idx = -1;
            for (int i = 0; i < b->slot_n; i++) {
                int cand = (b->slot_r + i) % b->slot_n;
                if (b->slot_state[cand] == 1) {
                    idx = cand;
                    break;
                }
            }
            if (idx < 0) {
                taskEXIT_CRITICAL(&b->mux);
                break;
            }
            b->rd_idx = idx;
            b->rd_off = 0;
        }
        const int idx = b->rd_idx;
        const size_t avail = b->slot_len[idx] - b->rd_off;
        size_t n = cap - produced;
        if (n > avail) {
            n = avail;
        }
        memcpy(out + produced, b->slots + (size_t)idx * b->slot_size + b->rd_off, n);
        b->rd_off += n;
        if (b->rd_off >= b->slot_len[idx]) {
            b->slot_state[idx] = 0;      // free
            b->slot_r = (idx + 1) % b->slot_n;
            b->rd_idx = -1;
        }
        taskEXIT_CRITICAL(&b->mux);
        produced += n;
    }
    return produced;
}

// ───────────────────────── 註冊 ─────────────────────────

// PULL 通道的 parser/ring 延後到第一次收到資料才配置（閒置通道只花 chunk）。
static esp_err_t bus_alloc(struct nc4_bus *b)
{
    if (b->parser != NULL) {
        return ESP_OK;
    }

    if (b->slots == NULL && b->cfg.mode == NC4_BUS_PUSH) {
        b->slot_size = b->cfg.rx_ring_size ? b->cfg.rx_ring_size : DEFAULT_RING_SIZE;
        b->slot_n = b->cfg.rx_slots ? b->cfg.rx_slots : 4;
        if (b->slot_n > 16) {
            b->slot_n = 16;
        }
        b->slots = calloc((size_t)b->slot_n, b->slot_size);
        if (b->slots == NULL) {
            return ESP_ERR_NO_MEM;
        }
        b->push_idx = -1;
        b->rd_idx = -1;
        ESP_LOGI(TAG, "[%s] %d 個插槽 × %u B（零複製收斂）", b->cfg.name,
                 b->slot_n, (unsigned)b->slot_size);
    }

    if (b->cfg.rx_region != NULL && b->cfg.rx_region_bytes > 0) {
        b->parser = nc4_parser_create_in(b->cfg.rx_region, b->cfg.rx_region_bytes,
                                         b->decoder, b->cfg.send, b->cfg.send_arg,
                                         b->cfg.name ? b->cfg.name : "?");
        ESP_LOGI(TAG, "[%s] parser 用 plan 的 %u B", b->cfg.name,
                 (unsigned)b->cfg.rx_region_bytes);
    } else {
        b->parser = nc4_parser_create(b->decoder, b->cfg.send, b->cfg.send_arg,
                                      b->cfg.name ? b->cfg.name : "?", b->max_frame);
        ESP_LOGI(TAG, "[%s] 配置 parser %u B", b->cfg.name, (unsigned)b->max_frame);
    }
    if (b->parser == NULL) {
        free(b->slots);
        b->slots = NULL;
        return ESP_ERR_NO_MEM;
    }
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
        b->frame_mode = cfg->frame_aligned ? 1 : 0;
        b->frame_idx = -1;

        b->chunk = malloc(b->read_chunk);
        if (b->chunk == NULL) {
            return ESP_ERR_NO_MEM;
        }

        b->decoder = decoder;

        if (cfg->mode == NC4_BUS_PUSH) {
            // PUSH 的資料可能從 ISR 進來，ISR 不能 malloc → 註冊時就要備好
            esp_err_t aerr = bus_alloc(b);
            if (aerr != ESP_OK) {
                free(b->chunk);
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
                 b->slots ? ", slots=" : "",
                 b->slots ? (unsigned)(b->slot_n * b->slot_size) : 0u);

        if (out_handle != NULL) {
            *out_handle = b;
        }
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

size_t nc4_bus_count(void)
{
    return s_count;
}

void nc4_bus_stats_reset(void)
{
    for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (!b->used) {
            continue;
        }
        b->st_frames = 0;
        b->st_bytes = 0;
        b->st_copied = 0;
        b->st_parse_us = 0;
    }
}

size_t nc4_bus_stats_json(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = 0;
    bool first = true;
    off += snprintf(buf + off, cap - off, "[");
    for (size_t i = 0; i < NC4_MAX_BUSES && off < cap; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (!b->used) {
            continue;
        }
        const double mb = (double)b->st_bytes / (1024.0 * 1024.0);
        int st_cnt[4] = {0, 0, 0, 0};      // free / ready / writing / reading
        for (int k = 0; k < b->slot_n; k++) {
            int stt = b->slot_state[k];
            if (stt == 0) st_cnt[0]++;
            else if (stt == 1) st_cnt[1]++;
            else if (stt == 2) st_cnt[2]++;
            else st_cnt[3]++;
        }
        uint32_t crc_err = 0;
        uint32_t bad_hdr = 0;
        nc4_parser_errors(b->parser, &crc_err, &bad_hdr);
        off += snprintf(buf + off, cap - off,
                        "%s{\"name\":\"%s\",\"mode\":\"%s\",\"core\":%d,"
                        "\"frame\":%u,"
                        "\"frames\":%u,\"mb\":%.2f,\"copied\":%llu,"
                        "\"parse_ms\":%.1f,\"us_per_frame\":%.1f,"
                        "\"crc_err\":%u,\"bad_hdr\":%u,"
                        "\"slots\":[%d,%d,%d,%d]}",
                        first ? "" : ",", b->cfg.name ? b->cfg.name : "?",
                        b->cfg.mode == NC4_BUS_PULL ? "pull"
                                                    : (b->frame_mode ? "push-frame" : "push-slot"),
                        b->cfg.core,
                        (unsigned)((b->cfg.mode == NC4_BUS_PUSH && b->frame_mode) ? 1 : 0),
                        (unsigned)b->st_frames, mb,
                        (unsigned long long)b->st_copied,
                        (double)b->st_parse_us / 1000.0,
                        b->st_frames ? (double)b->st_parse_us / (double)b->st_frames : 0.0,
                        (unsigned)crc_err, (unsigned)bad_hdr,
                        st_cnt[0], st_cnt[1], st_cnt[2], st_cnt[3]);
        first = false;
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]");
    }
    return off;
}

// ───────────────────────── 解碼任務 ─────────────────────────

// ── producer：同核讀 driver → push → kick ──
static void producer_task(void *arg)
{
    struct nc4_bus *b = arg;
    for (;;) {
        uint8_t *dst = nc4_bus_push_begin(b, b->slot_size);
        if (dst == NULL) {
            taskYIELD();          // 插槽滿：local yield（唔用 vTaskDelay，避免 core1 跨核 tick）
            continue;
        }
        const int n = b->cfg.read(b->cfg.read_arg, dst, b->slot_size);
        if (n > 0) {
            nc4_bus_push_commit(b, (size_t)n);
            nc4_bus_kick(b);      // 同核 notify
        } else {
            nc4_bus_push_abort(b);
            taskYIELD();
        }
    }
}

esp_err_t nc4_bus_start_producer(nc4_bus_handle_t handle, const char *name,
                                 uint32_t prio, uint32_t stack)
{
    struct nc4_bus *b = find_bus(handle);
    if (b == NULL || b->cfg.mode != NC4_BUS_PUSH || b->cfg.read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskCreatePinnedToCore(producer_task, name, stack, b, prio, NULL,
                                (BaseType_t)b->cfg.core) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[%s] producer 啟動（core %d）", b->cfg.name, b->cfg.core);
    return ESP_OK;
}

void nc4_bus_task_entry(void *arg)
{
    (void)arg;
    const int my_core = (int)(intptr_t)arg & 1;
    s_decode_task[my_core] = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "解碼任務啟動（core %d），共 %u 條通道", my_core, (unsigned)s_count);

    // 這一核如果有 PULL bus（decode 自己讀 driver），就冇 producer 會 kick，
    // 只能用 10 ms 逾時輪詢；如果全部係 PUSH（有同核 producer），就可以無限等。
    bool has_pull = false;
    for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (b->used && b->cfg.core == my_core && b->cfg.mode == NC4_BUS_PULL) {
            has_pull = true;
        }
    }
    ESP_LOGI(TAG, "core %d 等待策略：%s", my_core,
             has_pull ? "PULL（10 ms 輪詢）" : "PUSH（portMAX_DELAY）");

    // bus 的 driver 在「自己嘅核」初始化（所有權原則：唔跨核）
    for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (!b->used || b->cfg.core != my_core || b->owner_ready) {
            continue;
        }
        b->owner_ready = true;
        if (b->cfg.owner_init != NULL) {
            const esp_err_t e = b->cfg.owner_init(b->cfg.owner_arg);
            ESP_LOGI(TAG, "[%s] owner_init on core %d: %s", b->cfg.name, my_core,
                     esp_err_to_name(e));
        }
    }

    for (;;) {
        bool busy = false;

        for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
            struct nc4_bus *b = &s_buses[i];
            if (!b->used) {
                continue;
            }
            if (b->cfg.core != (int)(intptr_t)arg) {
                continue;       // 只服務自己核心的 bus
            }

            // 每條 bus 有獨立預算，避免一條忙線把其他通道餓死
            for (uint32_t n = 0; n < b->budget; n++) {
                size_t got = 0;

                // ── 零複製路徑：整幀已在插槽裡，parser 直接在上面解析 ──
                if (b->cfg.mode == NC4_BUS_PUSH && b->frame_mode) {
                    size_t flen = 0;
                    uint8_t *fp = nc4_bus_frame_begin(b, &flen);
                    if (fp == NULL) {
                        break;
                    }
                    if (b->parser == NULL && bus_alloc(b) != ESP_OK) {
                        ESP_LOGE(TAG, "[%s] parser 配置失敗，丟棄 %u B",
                                 b->cfg.name, (unsigned)flen);
                        nc4_bus_frame_end(b);
                        break;
                    }
                    const int64_t t0 = esp_timer_get_time();
                    const esp_err_t perr = nc4_parser_frame(b->parser, fp, flen);
                    b->st_parse_us += (uint64_t)(esp_timer_get_time() - t0);
                    b->st_frames++;
                    b->st_bytes += flen;
                    if (perr != ESP_OK) {
                        ESP_LOGW(TAG, "[%s] 幀解析失敗 %s（%u B）", b->cfg.name,
                                 esp_err_to_name(perr), (unsigned)flen);
                    }
                    nc4_bus_frame_end(b);
                    busy = true;
                    continue;
                }

                if (b->cfg.mode == NC4_BUS_PULL) {
                    int r = b->cfg.read(b->cfg.read_arg, b->chunk, b->read_chunk);
                    if (r < 0) {
                        ESP_LOGW(TAG, "[%s] read 錯誤", b->cfg.name);
                        break;
                    }
                    got = (size_t)r;
                } else {
                    got = slot_pop(b, b->chunk, b->read_chunk);
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
                // 這條路徑有兩次複製：slot→chunk，然後 feed 內 chunk→parser ring。
                // 統計時一起算，0xF008 才看得出「零複製」省掉多少。
                b->st_copied += (uint64_t)got * 2;
                b->st_frames++;
                b->st_bytes += got;
                const int64_t t0 = esp_timer_get_time();
                nc4_parser_feed(b->parser, b->chunk, got);
                b->st_parse_us += (uint64_t)(esp_timer_get_time() - t0);
            }
        }

        if (busy) {
            taskYIELD();       // 有資料就繼續解，不要睡（HZ=100 時 1 tick = 10 ms 會鎖死在 1.6 MB/s）
        } else {
            // PUSH：只等 producer 的同核 kick（無限等，冇 tick、冇跨核）。
            // PULL：冇人 kick，只能 10 ms 醒一次輪詢（本核；core0 無害）。
            ulTaskNotifyTake(pdTRUE, has_pull ? pdMS_TO_TICKS(DEFAULT_POLL_MS) : portMAX_DELAY);
        }
    }
}
