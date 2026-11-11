#include "nc4_bus.h"
#include "nc_buf.h"
#include "nc_task.h"
#include "nc_prio.h"

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

    // PUSH 模式：一條 nc_buf（FIFO）＝ N 個插槽（producer 寫、consumer 讀、零複製）
    // 2026-11-10：手寫的 slot_state[16]／slot_len[16]／w·r·push_idx·rd_off 全部收進 nc_buf。
    nc_buf_t *slots;
    size_t slot_size;
    int slot_n;
    portMUX_TYPE mux;

    // 零複製模式：producer 一次 commit「剛好一幀」，consumer 直接在插槽上解析
    uint8_t frame_mode;

    bool owner_ready;                 // owner_init 已跑（每個核一次）

    int occ;                          // 目前非 free 的格數（受 mux 保護）
    uint32_t st_slot_peak;            // 歷史最高同時佔用的格數（量「環要幾大」）
    uint32_t st_kick_remote;          // 跨核 kick 次數（核心隔離的可驗證指標，必須為 0）
    int cfg_src;                      // config 實際套用了幾多個 rx 欄位（0 = 全用 code 預設）
    void *producer;                   // 這條 bus 的 receiver task（量 stack 高水位用）

    // 統計（0xF008）：證明 copy 有沒有真的消失
    uint32_t st_frames;
    uint64_t st_bytes;                // 收進來的 bytes（含表頭/CRC）
    uint64_t st_copied;               // 為了「把資料交給 parser」所做的 memcpy bytes
    uint64_t st_parse_us;             // 解析佔用的時間
    int64_t  st_commit_us;            // 最後一次 commit 的時間（量喚醒延遲）
    uint64_t st_kick_sum_us;          // commit → decode 接手 的累計
    uint32_t st_kick_n;
    uint32_t st_kick_max_us;

    // 貼身化量測（2026-11-08）：每一個尺寸決策都要有數字支撐
    uint32_t st_reads;                // 成功 read/pop/整幀的次數
    size_t   st_max_read;             // 單次搬運的最大 bytes  → slot_size 的下限
    uint32_t st_slot_full;            // 拿不到插槽的次數      → slots 的下限
    uint32_t st_tx_count;             // 回覆次數
    size_t   st_tx_max;               // 單次回覆最大 bytes    → TX ring / FIFO 的下限
};

static struct nc4_bus s_buses[NC4_MAX_BUSES];

static struct nc4_bus *find_bus(nc4_bus_handle_t h) { return (struct nc4_bus *)h; }
static TaskHandle_t s_decode_task[2];   // 每個核心一個 decode task
static volatile uint32_t s_loop_count[2];   // 每個核 decode 迴圈跑過幾多次（停滯偵測用）
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
    TaskHandle_t t = s_decode_task[core];
    if (t == NULL) {
        return;
    }
    // 核心隔離的硬指標：kick 必須由「同一個核」發出。
    // 跨核就會變成 IPC 中斷（設計禁止），所以呢個數字要永遠係 0。
    if ((int)xTaskGetCoreID(t) != (int)xPortGetCoreID()) {
        b->st_kick_remote++;          // 只計數，唔改變行為（唔好喺熱路徑改為丟棄）
    }
    xTaskNotifyGive(t);
}

// 跨核轉寄專用：叫醒目的核 decode task。
// 呢個係設計上唯一允許的跨核 notify；bus producer 的 kick 仍然同核。
void nc4_bus_kick_core(int core)
{
    if (core < 0 || core > 1) {
        return;
    }
    TaskHandle_t t = s_decode_task[core];
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

// ───────────────────────── 插槽（PUSH 模式）＝ 一條 nc_buf FIFO ─────────────────────────
//
// 2026-11-10：原本是自己手寫的 `slot_state[16]` / `slot_len[16]` / w·r·push_idx / rd_off
// （第三份同類實作）。現在全部收進 nc_buf，對外語義不變：
//   · producer 一次揸一格（`put_begin` → `put_end`）
//   · consumer 一次拿一格（`get` → `get_end`），**整格交出去，零複製**
//   · FIFO 唔會標 WRITING ⇒ producer 死掉只會令寫指標唔前進，環唔會卡死

uint8_t *nc4_bus_push_begin(nc4_bus_handle_t bus, size_t want)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL) {
        return NULL;
    }
    (void)want;        // 格大小固定；want 只係「我打算寫幾多」的提示
    uint8_t *p = nc_buf_put_begin(b->slots, 0);
    if (p == NULL) {
        b->st_slot_full++;      // 貼身化：這個 > 0 就代表 slots 不夠（或 producer 太急）
    }
    return p;
}

esp_err_t nc4_bus_push_commit(nc4_bus_handle_t bus, size_t len)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t e = nc_buf_put_end(b->slots, (uint32_t)len);
    if (e != ESP_OK) {
        return e;
    }
    b->st_commit_us = esp_timer_get_time();   // 事件驅動：量由 commit 到 decode 接手
    const int fill = (int)nc_buf_fill(b->slots);
    taskENTER_CRITICAL(&b->mux);
    b->occ = fill;
    // 只有「已 commit、等 decode 讀」的格才計入佔用 —— 空探測唔算
    if ((uint32_t)fill > b->st_slot_peak) {
        b->st_slot_peak = (uint32_t)fill;
    }
    taskEXIT_CRITICAL(&b->mux);
    return ESP_OK;
}

esp_err_t nc4_bus_push_abort(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return nc_buf_put_abort(b->slots);
}

// 取下一格（整格、零複製）：回插槽指標，*len = 實際 bytes。解析完必須 frame_end。
uint8_t *nc4_bus_frame_begin(nc4_bus_handle_t bus, size_t *len)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL) {
        return NULL;
    }
    uint32_t n = 0;
    uint8_t *p = nc_buf_get(b->slots, &n, NULL);
    if (p != NULL && len != NULL) {
        *len = n;
    }
    return p;
}

void nc4_bus_frame_end(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL || b->slots == NULL) {
        return;
    }
    if (nc_buf_get_end(b->slots) == ESP_OK) {
        const int fill = (int)nc_buf_fill(b->slots);
        taskENTER_CRITICAL(&b->mux);
        b->occ = fill;
        taskEXIT_CRITICAL(&b->mux);
    }
}

esp_err_t nc4_bus_reset(nc4_bus_handle_t bus)
{
    struct nc4_bus *b = find_bus(bus);
    if (b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nc4_parser_reset(b->parser);      // parser 可能還沒配置（第一次收到資料才配）
    if (b->slots != NULL) {
        uint32_t n = 0;
        while (nc_buf_get(b->slots, &n, NULL) != NULL) {
            nc_buf_get_end(b->slots);   // 丟掉所有 READY
        }
        b->occ = 0;
    }
    return ESP_OK;
}

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
        char nm[NC_BUF_NAME_MAX];
        snprintf(nm, sizeof(nm), "%s.slots", b->cfg.name ? b->cfg.name : "bus");
        b->slots = nc_buf_create(nm,
                                 b->cfg.slots_psram ? NC_BUF_PSRAM : NC_BUF_SRAM,
                                 (uint32_t)b->slot_size,
                                 (uint16_t)b->slot_n, NC_BUF_F_RUNTIME);
        if (b->slots == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "[%s] %d 個插槽 × %u B（nc_buf FIFO，零複製）", b->cfg.name,
                 b->slot_n, (unsigned)b->slot_size);
    }

    if (b->cfg.rx_buf != NULL) {
        b->parser = nc4_parser_create_buf(b->cfg.rx_buf, b->decoder, b->cfg.send,
                                          b->cfg.send_arg,
                                          b->cfg.name ? b->cfg.name : "?");
        ESP_LOGI(TAG, "[%s] parser 用管理器給的緩衝區 %u B", b->cfg.name,
                 (unsigned)nc_buf_slot_bytes(b->cfg.rx_buf));
    } else if (b->cfg.rx_region != NULL && b->cfg.rx_region_bytes > 0) {
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
        nc_buf_release(b->slots);
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
        // PUSH 模式：一次最多搬「一格」，所以 chunk 冇理由大過 slot_size。
        // （以前留 1,024 而 slot 係 256，等於白配置 768 B；呢個值從來冇量測過。）
        if (cfg->mode == NC4_BUS_PUSH) {
            const size_t slot = cfg->rx_ring_size ? cfg->rx_ring_size : DEFAULT_RING_SIZE;
            if (b->read_chunk > slot) {
                b->read_chunk = slot;
            }
        }
        b->budget = cfg->budget_per_pass ? cfg->budget_per_pass : DEFAULT_BUDGET_PER_PASS;
        b->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
        b->frame_mode = cfg->frame_aligned ? 1 : 0;

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

size_t nc4_bus_slot_size(nc4_bus_handle_t handle)
{
    struct nc4_bus *b = find_bus(handle);
    return (b != NULL) ? b->slot_size : 0;
}

int nc4_bus_slot_count(nc4_bus_handle_t handle)
{
    struct nc4_bus *b = find_bus(handle);
    return (b != NULL) ? b->slot_n : 0;
}

// 由建立 receiver 的地方登記 task handle（只為量 stack 高水位）。
void nc4_bus_set_producer(nc4_bus_handle_t handle, void *task_handle)
{
    struct nc4_bus *b = find_bus(handle);
    if (b != NULL) {
        b->producer = task_handle;
    }
}

// 由 main 登記「這條 bus 的 rx 參數有幾多項來自 config」（0 = 全部用 code 預設）。
// 用途：令「config 到底有冇被讀」變成可觀察，而唔係靠推理。
void nc4_bus_set_cfg_src(nc4_bus_handle_t handle, int applied)
{
    struct nc4_bus *b = find_bus(handle);
    if (b != NULL) {
        b->cfg_src = applied;
    }
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
        b->st_slot_peak = (uint32_t)(b->occ > 0 ? b->occ : 0);   // 重新量峰值
        // 貼身化的計數器／峰值：不清就會把上一個量測窗口的數字帶進來
        b->st_reads = 0;
        b->st_max_read = 0;
        b->st_slot_full = 0;
        b->st_tx_count = 0;
        b->st_tx_max = 0;
        nc4_parser_reset_stats(b->parser);
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
        for (int k = 0; k < b->slot_n && b->slots != NULL; k++) {
            const uint8_t stt = nc_buf_state_at(b->slots, (uint16_t)k);
            if (stt < 4) {
                st_cnt[stt]++;
            }
        }
        uint32_t crc_err = 0;
        uint32_t bad_hdr = 0;
        uint32_t max_dlen = 0;
        uint32_t max_ok = 0;
        nc4_parser_errors(b->parser, &crc_err, &bad_hdr);
        nc4_parser_peaks(b->parser, &max_dlen, &max_ok);
        off += snprintf(buf + off, cap - off,
                        "%s{\"name\":\"%s\",\"mode\":\"%s\",\"core\":%d,"
                        "\"frame\":%u,"
                        "\"frames\":%u,\"mb\":%.2f,\"copied\":%llu,"
                        "\"avg_b\":%u,\"hwm\":%u,"
                        "\"parse_ms\":%.1f,\"us_per_frame\":%.1f,"
                        "\"kick_us\":%u,\"kick_max_us\":%u,"
                        "\"crc_err\":%u,\"bad_hdr\":%u,"
                        "\"remote\":%u,\"region\":%u,\"cfg_src\":%d,"
                        "\"slots\":[%d,%d,%d,%d],\"occ\":%d,\"peak\":%u,"
                        "\"reads\":%u,\"max_read\":%u,\"slot_full\":%u,"
                        "\"tx\":%u,\"tx_max\":%u,\"max_dlen\":%u,\"max_ok\":%u}",
                        first ? "" : ",", b->cfg.name ? b->cfg.name : "?",
                        b->cfg.mode == NC4_BUS_PULL ? "pull"
                                                    : (b->frame_mode ? "push-frame" : "push-slot"),
                        b->cfg.core,
                        (unsigned)((b->cfg.mode == NC4_BUS_PUSH && b->frame_mode) ? 1 : 0),
                        (unsigned)b->st_frames, mb,
                        (unsigned long long)b->st_copied,
                        // 每次 push 平均幾多 bytes（＝實際「一次傳輸」的大小）
                        (unsigned)(b->st_frames ? (b->st_bytes / b->st_frames) : 0),
                        // receiver stack 剩餘最少 bytes（0 = 未登記或非 task）
                        (unsigned)(b->producer ? uxTaskGetStackHighWaterMark(
                                                     (TaskHandle_t)b->producer) : 0),
                        (double)b->st_parse_us / 1000.0,
                        b->st_frames ? (double)b->st_parse_us / (double)b->st_frames : 0.0,
        (unsigned)(b->st_kick_n ? b->st_kick_sum_us / b->st_kick_n : 0),
        (unsigned)b->st_kick_max_us,
                        (unsigned)crc_err, (unsigned)bad_hdr,
                        (unsigned)b->st_kick_remote,
                        (unsigned)b->cfg.rx_region_bytes, b->cfg_src,
                        st_cnt[0], st_cnt[1], st_cnt[2], st_cnt[3],
                        b->occ, (unsigned)b->st_slot_peak,
                        (unsigned)b->st_reads, (unsigned)b->st_max_read,
                        (unsigned)b->st_slot_full,
                        (unsigned)b->st_tx_count, (unsigned)b->st_tx_max,
                        (unsigned)max_dlen, (unsigned)max_ok);
        first = false;
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]");
    }
    return off;
}

// ───────────────────────── 解碼任務 ─────────────────────────

// ── producer：同核讀 driver → push → kick ──
//
// ⚠️ 兩個不變式（2026-11-06 實測踩到）：
//   1. priority **一定要低過** 同核的 decode task。
//      以前 UART producer 用 prio 5、decode 用 prio 4：環一滿，producer 就永遠
//      贏得排程，decode 冇機會排空 → 死鎖；driver 未就緒時更會直接餓死整個核
//      （連 app_main 都跑不到，開機停在「USB 有列舉、但冇任何回應」）。
//   2. 任何「等」都必須真的阻塞（vTaskDelay／等 driver），唔可以 taskYIELD()。
//      taskYIELD 只會讓給同／更高優先級，低優先級的 consumer 永遠等不到。
static void producer_task(void *arg)
{
    struct nc4_bus *b = arg;
    for (;;) {
        uint8_t *dst = nc4_bus_push_begin(b, b->slot_size);
        if (dst == NULL) {
            vTaskDelay(1);        // 插槽滿：讓 decode 先排空（1 tick 對 UART 無影響）
            continue;
        }
        const int n = b->cfg.read(b->cfg.read_arg, dst, b->slot_size);
        if (n > 0) {
            b->st_reads++;
            if ((size_t)n > b->st_max_read) {
                b->st_max_read = (size_t)n;
            }
            nc4_bus_push_commit(b, (size_t)n);
            nc4_bus_kick(b);      // 同核 notify（decode 優先級更高，會即刻接手）
        } else {
            nc4_bus_push_abort(b);
            vTaskDelay(1);        // driver 未就緒／暫時冇資料：唔可以自旋
        }
    }
}

// 回覆出口的尺寸量測：由 transport 的 send 包一層呼叫（不改行為）。
// tx_max 決定 TX ring／FIFO 要幾大；tx_count 用來對照「回覆有沒有被丟」。
void nc4_bus_note_tx(nc4_bus_handle_t handle, size_t len)
{
    struct nc4_bus *b = find_bus(handle);
    if (b == NULL) {
        return;
    }
    b->st_tx_count++;
    if (len > b->st_tx_max) {
        b->st_tx_max = len;
    }
}

esp_err_t nc4_bus_start_producer(nc4_bus_handle_t handle, const char *name,
                                 uint32_t prio, uint32_t stack)
{
    struct nc4_bus *b = find_bus(handle);
    if (b == NULL || b->cfg.mode != NC4_BUS_PUSH || b->cfg.read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // 不變式 1：producer 唔可以高過 decode。寧願這裡夾住，唔好靠呼叫端記得。
    const uint32_t decode_prio = NC_PRIO_DECODE;
    if (prio >= decode_prio) {
        ESP_LOGW(TAG, "[%s] producer prio %u ≥ decode prio %u → 夾到 %u（避免餓死 decode）",
                 b->cfg.name, (unsigned)prio, (unsigned)decode_prio,
                 (unsigned)(decode_prio - 1));
        prio = decode_prio - 1;
    }
    TaskHandle_t th = NULL;
    if (xTaskCreatePinnedToCore(producer_task, name, stack, b, prio, &th,
                                (BaseType_t)b->cfg.core) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    // 自己登記 handle：STATUS 的 hwm（stack 高水位）才看得到 producer。
    b->producer = (void *)th;
    ESP_LOGI(TAG, "[%s] producer 啟動（core %d）", b->cfg.name, b->cfg.core);
    return ESP_OK;
}

// 自己核有冇「已 commit、等 decode 讀」的格。解碼迴圈睡前用它做最後檢查：
// 就算 kick 在邊界遺失，也不會睡在一個「有資料」的環上。
static bool bus_has_ready(int core)
{
    for (size_t i = 0; i < NC4_MAX_BUSES; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (b->used && b->cfg.core == core && b->occ > 0) {
            return true;
        }
    }
    return false;
}

uint32_t nc4_bus_loop_count(int core)
{
    return s_loop_count[(core == 1) ? 1 : 0];
}

// 停滯時要睇的狀態：每條 bus 的 occ／寫入指標／插槽狀態。給 monitor 印出來。
size_t nc4_bus_dump(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    size_t off = snprintf(buf, cap, "[");
    bool first = true;
    for (size_t i = 0; i < NC4_MAX_BUSES && off < cap; i++) {
        struct nc4_bus *b = &s_buses[i];
        if (!b->used) {
            continue;
        }
        off += snprintf(buf + off, cap - off,
                        "%s{\"n\":\"%s\",\"core\":%d,\"occ\":%d,\"wr\":%d,\"rd\":%d,"
                        "\"st\":[%d,%d,%d,%d]}",
                        first ? "" : ",", b->cfg.name ? b->cfg.name : "?", b->cfg.core,
                        b->occ, nc_buf_writing(b->slots) ? 1 : -1,
                        nc_buf_reading(b->slots) ? 1 : -1,
                        b->slots ? nc_buf_state_at(b->slots, 0) : 0,
                        b->slots ? nc_buf_state_at(b->slots, 1) : 0,
                        b->slots ? nc_buf_state_at(b->slots, 2) : 0,
                        b->slots ? nc_buf_state_at(b->slots, 3) : 0);
        first = false;
    }
    if (off < cap) {
        off += snprintf(buf + off, cap - off, "]");
    }
    return off;
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
             has_pull ? "PULL（10 ms 輪詢）" : "PUSH（kick 或 50 ms 安全網）");

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
        nc_task_beat();     // 長跑 loop：自己報心跳（停滯偵測靠它）
        s_loop_count[my_core]++;
        // 跨核轉寄：目的核每輪先清 queue（唔靠 producer kick）
        bool busy = (nc4_fwd_poll(my_core) > 0);

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
                    b->st_reads++;
                    if (flen > b->st_max_read) {
                        b->st_max_read = flen;
                    }
                    if (b->st_commit_us != 0) {
                        const uint32_t d = (uint32_t)(esp_timer_get_time() - b->st_commit_us);
                        b->st_kick_sum_us += d;
                        b->st_kick_n++;
                        if (d > b->st_kick_max_us) {
                            b->st_kick_max_us = d;
                        }
                        b->st_commit_us = 0;
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

                uint8_t *feed_src = b->chunk;
                uint8_t *slot_ptr = NULL;
                if (b->cfg.mode == NC4_BUS_PULL) {
                    int r = b->cfg.read(b->cfg.read_arg, b->chunk, b->read_chunk);
                    if (r < 0) {
                        ESP_LOGW(TAG, "[%s] read 錯誤", b->cfg.name);
                        break;
                    }
                    got = (size_t)r;
                } else {
                    // PUSH：整格交出來，直接由插槽解析（唔再經 chunk，省一次 copy）
                    uint32_t slen = 0;
                    slot_ptr = nc_buf_get(b->slots, &slen, NULL);
                    if (slot_ptr == NULL) {
                        break;
                    }
                    got = slen;
                    feed_src = slot_ptr;
                }

                if (got == 0) {
                    break;
                }
                if (b->st_commit_us != 0) {
                    const uint32_t d = (uint32_t)(esp_timer_get_time() - b->st_commit_us);
                    b->st_kick_sum_us += d;
                    b->st_kick_n++;
                    if (d > b->st_kick_max_us) {
                        b->st_kick_max_us = d;
                    }
                    b->st_commit_us = 0;
                }
                b->st_reads++;
                if (got > b->st_max_read) {
                    b->st_max_read = got;
                }
                if (b->parser == NULL && bus_alloc(b) != ESP_OK) {
                    ESP_LOGE(TAG, "[%s] parser 配置失敗，丟棄 %u B",
                             b->cfg.name, (unsigned)got);
                    break;
                }
                busy = true;
                // 只剩一次複製：feed 內把資料寫入 parser ring（PUSH 唔再經 chunk）。
                b->st_copied += (uint64_t)got;
                b->st_frames++;
                b->st_bytes += got;
                const int64_t t0 = esp_timer_get_time();
                nc4_parser_feed(b->parser, feed_src, got);
                b->st_parse_us += (uint64_t)(esp_timer_get_time() - t0);
                if (slot_ptr != NULL) {
                    nc_buf_get_end(b->slots);   // 解析完才還格（producer 唔會提早覆寫）
                    b->occ = (int)nc_buf_fill(b->slots);
                }
            }
        }

        if (busy) {
            taskYIELD();       // 有資料就繼續解，不要睡（HZ=100 時 1 tick = 10 ms 會鎖死在 1.6 MB/s）
        } else if (bus_has_ready(my_core)) {
            // 仲有已 commit 的格：唔好睡（唔靠 kick 也不怕 kick 遺失）
            continue;
        } else {
            // PUSH：等 producer 的同核 kick。**不可無限等**：kick 只會在 commit 之後發，
            // 一旦插槽全部卡在「writing」或 kick 在邊界遺失，decode 就會永遠睡，producer
            // 也永遠等不到空位（2026-11-07 實測：8 MB 幀洪水之後裝置完全唔答指令）。
            // 50 ms 安全網：醒來重掃一次，代價係閒置時 20 次/秒的 no-op。
            // PULL：本來就只能輪詢。
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(has_pull ? DEFAULT_POLL_MS : 50));
        }
    }
}
