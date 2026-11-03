// esp_Net-Core — Phase A：USB composite（MSC + CDC）
//
// 一條線同時提供：
//   • MSC  → 一個隨身碟，內容是 data 分區（config.json / pixel assets）
//   • CDC  → 一個 COM port，當 console（之後接 NC4）
//
// 開機順序（使用者不需要手動格式化）：
//   自動格式化（若需要）→ 寫入出廠預置 config.json → 把碟交給 host
//
// 逃生門：Windows 端要重燒時，USB 被 TinyUSB 佔住，esptool 進不了 download mode。
// 兩個軟體路徑可解，都不需要精確按鍵時機：
//   1. 按住 BOOT 3 秒 → 拉低 GPIO0 + hold + restart → ROM 進 download mode
//   2. CDC 收到字串 "DFU" → 同上
//
// 量測：心跳印「絕對 µs + 間隔」。複製大檔時看 dt，若出現 ~900ms 缺口就是 async IO 沒生效。

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/uart.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#endif
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_psram.h"
#include <ctype.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"

#include "ff.h"
#include "nc4.h"
#include "nc4_bus.h"
#include "nc4_router.h"
#include "nc_task.h"
#include "nc_hello.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "nc_mem.h"
#include "nc_plan.h"
#include "nc_hub.h"
#include "nc_task.h"
#include "nc_ota_action.h"
#include "nc4_ws.h"
#include "nc4_udp.h"
#include "nc_cfg.h"
#include "cJSON.h"
#include "nc4_http.h"
#include "nc_store.h"
#include "nc_file_action.h"
#include "nc_fx.h"
#include "nc_wifi.h"

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "netcore";

// 解碼任務跑在哪個核心（改這裡就換核；之後可由 NVS/0x1007 設定）
#ifndef NC_DECODE_CORE
#define NC_DECODE_CORE 0
#endif

// UART 迴路（lazy 初始化：只有跑 UART 測試時才碰 IO7/8）
static bool s_uart_ready;
static esp_err_t uart_loop_init(uint32_t baud);

static uint16_t s_cid = NC4_ADDR_BROADCAST;   // 自己的 2B 協議位址

// 零複製：RX task 一次 recv 進「剛好一整幀」，decode task 直接在插槽上解析
// （PUSH 模式才有意義；NVS net/frame_align，預設 1）
static bool s_frame_align = true;
static volatile uint32_t s_rx_resync;   // 丟掉幾個 byte 才重新同步（正常應該 0）
// RX 診斷（0xF008 會一起回）：看出時間花在「等資料」還是「等插槽」
static volatile uint32_t s_rx_hub_full;      // push_begin 拿不到格子
static volatile uint32_t s_rx_sel;           // select() 次數
static volatile uint32_t s_rx_sel_to;        // select() 逾時次數
static volatile uint64_t s_rx_fill_us;       // fill_frame 總耗時
static volatile uint32_t s_rx_frames;        // 成功收到的幀數

#define BOOT_BUTTON_GPIO   GPIO_NUM_0
#define MSC_MOUNT_POINT    "/data"
#define PRESET_DIR          MSC_MOUNT_POINT "/config"
#define PRESET_SYSTEM_PATH  PRESET_DIR "/system.json"
#define PRESET_NETWORK_PATH PRESET_DIR "/network.json"
#define PRESET_TASKS_PATH   PRESET_DIR "/tasks.json"
// 卷標 = 品牌縮寫 + "-" + 16-bit CID（FAT 卷標上限 11 字元）：
//   "NC-" + "A57C" → "NC-A57C"
// 不能寫成 Net-Core_A57C（14 字元，f_setlabel 會回 FR_INVALID_NAME）。
#define VOLUME_PREFIX      "NC-"

// UART 測試迴路（IO7 / IO8 外部短接）：速度動態可調
#define UART_TEST_PORT     UART_NUM_1
#define UART_TEST_TX_PIN   7
#define UART_TEST_RX_PIN   8
#define UART_TEST_RX_BUF   4096
#define UART_TEST_DEF_BAUD 921600
#define FW_VERSION         "0.1.0"

// 解碼效能測試（0x18xx = bench 域，與 MP 版同名域）
// ── 專屬測試域 0xFxxx（不屬於任何產品功能，可隨時刪）──
// 0xF001 DEV_BENCH  size(u16) iters(u16) mode(u8) → 0xF002 DEV_RSP (str_u16len JSON)
//   mode 0 = decode、1 = pack、2 = erase 4K、3 = write 4K、4 = erase 64K
#define NC4_CMD_DEV_BENCH  0xF001
#define NC4_CMD_DEV_RSP    0xF002
// 0xF003 HUB_BENCH  iters(u32) → 0xF004 HUB_RSP (str JSON)
//   雙核交換：writer 固定 core 1、reader 固定 core 0
#define NC4_CMD_HUB_BENCH  0xF003
#define NC4_CMD_HUB_RSP    0xF004
// 0xF005 SLOW   ms(u16) → 0xF004 {"slow_ms":N}
//   故意在 decode task 裡睡，量「慢 handler 會不會卡住其他線路」
#define NC4_CMD_DEV_SLOW   0xF005
// 0xF00A NET_TUNE  split(u8) core(u8) → 寫 NVS，下次開機生效
#define NC4_CMD_NET_TUNE   0xF00A
// 0xF008 BUS_STATS  [reset:u8] → 0xF009 {"bus":[...],"resync":N}
//   每條 bus 的 frames / mb / copied / parse_ms / us_per_frame（copied=0 就是零複製）
#define NC4_CMD_BUS_STATS  0xF008
#define NC4_CMD_BUS_RSP    0xF009

// bench 域（對齊 slave/schema/bench.json）
#define NC4_CMD_BENCH_READY   0x1811
#define NC4_CMD_BENCH_DATA    0x1812
#define NC4_CMD_BENCH_RESULT  0x1813
#define NC4_CMD_BENCH_REPORT  0x1814

// sys 域（sys.json）
#define NC4_CMD_SET_MASTER 0x1016
#define NC4_CMD_SYS_TASK_QUERY 0x1005
#define NC4_CMD_SYS_TASK_RSP   0x1006

// status 域：0x1101–0x1104 已用（GET / RSP / SET / ACK），路由器由 0x1105 開始
#define NC4_CMD_ROUTE_GET 0x1105
#define NC4_CMD_ROUTE_RSP 0x1106

// ports/<board>/data/config/*.json 由 EMBED_FILES 嵌進 firmware（一個層一個檔）
extern const uint8_t preset_system_start[]  asm("_binary_preset_system_json_start");
extern const uint8_t preset_system_end[]    asm("_binary_preset_system_json_end");
extern const uint8_t preset_network_start[] asm("_binary_preset_network_json_start");
extern const uint8_t preset_network_end[]   asm("_binary_preset_network_json_end");

#if NETCORE_HAS_PRESET_TASKS
extern const uint8_t preset_tasks_start[]   asm("_binary_preset_tasks_json_start");
extern const uint8_t preset_tasks_end[]     asm("_binary_preset_tasks_json_end");
#endif


// ── MSC 事件 ──
static const esp_partition_t *find_data_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_FAT, "data");
}

// ── 出廠預置：缺檔才寫，不覆蓋使用者改過的內容 ──
static void seed_file(const char *path, const uint8_t *start, const uint8_t *end)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "預置已存在（%s, %d B），不動它", path, (int)st.st_size);
        return;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "寫入預置失敗：開不了 %s", path);
        return;
    }
    const size_t len = (size_t)(end - start);
    const size_t written = fwrite(start, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "已寫入預置 %s（%u B）", path, (unsigned)written);
}

// 目錄結構也是「出廠內容」的一部分：config/ 與 pixel/effects/ 先建好，
// 使用者插上隨身碟就看得到該把東西放哪裡。
static void seed_dirs(void)
{
    static const char *const dirs[] = {
        PRESET_DIR,
        MSC_MOUNT_POINT "/pixel",
        MSC_MOUNT_POINT "/pixel/effects",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "建目錄失敗 %s（errno %d）", dirs[i], errno);
        }
    }
}

static void seed_presets(void)
{
    seed_dirs();
    seed_file(PRESET_SYSTEM_PATH, preset_system_start, preset_system_end);
    seed_file(PRESET_NETWORK_PATH, preset_network_start, preset_network_end);
#if NETCORE_HAS_PRESET_TASKS
    seed_file(PRESET_TASKS_PATH, preset_tasks_start, preset_tasks_end);
#endif
}

// ── 卷標：預設叫 NETCORE，不是 NO NAME ──
// 這個卷標是不是「我們自己產生的」（可以隨 cID 更新）。
// 使用者自己取的名字（例如 "MYBOARD"）一律不動。
static bool label_is_ours(const char *label)
{
    if (label == NULL || label[0] == '\0') {
        return true;
    }
    if (strcmp(label, "NO NAME") == 0 || strcmp(label, "NETCORE") == 0) {
        return true;
    }
    const size_t plen = strlen(VOLUME_PREFIX);
    if (strncmp(label, VOLUME_PREFIX, plen) != 0) {
        return false;
    }
    const char *p = label + plen;
    for (int i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)p[i])) {
            return false;
        }
    }
    return p[4] == '\0';
}

// ── 設定正規化：把「只有人能讀」的寫法整理成機器用的值，並寫回檔案 ──
// 目前只有一個：bt.pin 只取數字，合法長度 4–6 位（PIN 規格）：
//   "1234" → "1234"；"123456" → "123456"；"dfgd321fgjf55" → "32155"；
//   多於 6 位取前 6 位；不足 4 位或沒有數字 → 清成 ""（Just Works）。
static void config_sanitize(void)
{
    cfg_file_t *cfg = nc_cfg_open("network");
    if (cfg == NULL) {
        return;
    }
    const cJSON *bt = cJSON_GetObjectItemCaseSensitive(nc_cfg_root(cfg), "bt");
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(bt, "pin");
    const char *raw = (cJSON_IsString(pin) && pin->valuestring != NULL) ? pin->valuestring : NULL;
    if (raw != NULL && raw[0] != '\0') {
        char norm[7] = {0};    // 4–6 位 + NUL
        int n = 0;
        for (const char *q = raw; *q != '\0' && n < 6; q++) {
            if (*q >= '0' && *q <= '9') {
                norm[n++] = *q;
            }
        }
        if (n < 4) {
            norm[0] = '\0';
        }
        if (strcmp(norm, raw) != 0) {
            if (nc_cfg_set_string("network", "bt.pin", norm)) {
                ESP_LOGI(TAG, "bt.pin 正規化：\"%s\" → \"%s\"", raw, norm);
            } else {
                ESP_LOGW(TAG, "bt.pin 正規化失敗（記憶體值仍用 \"%s\"）", norm);
            }
        }
    }
    nc_cfg_close(cfg);
}

static void ensure_volume_label(void)
{
#if FF_USE_LABEL
    char label[24] = {0};
    DWORD sn = 0;
    if (f_getlabel("0:", label, &sn) != FR_OK) {
        ESP_LOGW(TAG, "讀不到卷標");
        return;
    }
    char want[12] = {0};
    if (s_cid != NC4_ADDR_BROADCAST) {
        snprintf(want, sizeof(want), "%s%04X", VOLUME_PREFIX, (unsigned)s_cid);  // ≤11 字元
    } else {
        snprintf(want, sizeof(want), "%s", VOLUME_PREFIX);
    }

    if (strcmp(label, want) == 0) {
        return;
    }
    // 使用者自己命名過就不覆寫；我們自己產生的（空白 / NO NAME / NETCORE / NC-XXXX）可以更新
    if (!label_is_ours(label)) {
        ESP_LOGI(TAG, "卷標 \"%s\" 由使用者命名，不動", label);
        return;
    }

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "0:%s", want);
    if (f_setlabel(cmd) == FR_OK) {
        ESP_LOGI(TAG, "卷標: \"%s\" → \"%s\"", label, want);
    } else {
        ESP_LOGW(TAG, "設定卷標失敗（原 \"%s\" → \"%s\"）", label, want);
    }
#else
    ESP_LOGW(TAG, "CONFIG_FATFS_USE_LABEL 未開，卷標會停在 NO NAME");
#endif
}

// ── 自動格式化 → 寫預置 → 之後交給 host ──
static esp_err_t storage_init(void)
{
    const esp_partition_t *part = find_data_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "data 分區: offset 0x%08" PRIx32 ", size %" PRIu32 " KB",
             part->address, (uint32_t)(part->size / 1024));

    // WL / FatFS / MSC 的整個生命週期都由 store 管 —— 它有辦法在 claim 時
    // 把 LUN 拆掉，否則主機在 APP 模式仍然寫得進同一塊 flash。
    ESP_RETURN_ON_ERROR(nc_store_init(part), TAG, "store 初始化失敗");

    seed_presets();
    return ESP_OK;
}

// ── USB（TinyUSB）只在 S3 這版存在 ──
// P4 的 USB-C 只接 USB-Serial-JTAG，沒有 OTG → 這幾個函式永遠不會被呼叫。
// 但它們在初始器裡被取址（cdc_bus.read/.send），linker 就一路把 tinyusb
// 拉進來：_cdcd_itf 12,532 + _mscd_epbuf 8,192 + _cdcd_epbuf 1,024 ≈ 21.7 KB。
// 所以整個 P4 分支連「定義」都唔編 —— 唔單止唔呼叫。
#if !CONFIG_IDF_TARGET_ESP32P4
static esp_err_t usb_init(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB driver 安裝失敗");

    const tinyusb_config_cdcacm_t acm_cfg = { .cdc_port = TINYUSB_CDC_ACM_0 };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), TAG, "CDC-ACM 初始化失敗");

    // stdio 從 USB-Serial-JTAG 搬到 TinyUSB CDC
    // （CONFIG_ESP_CONSOLE_USB_CDC 與 TinyUSB 互斥，只能這樣做）
    ESP_RETURN_ON_ERROR(tinyusb_console_init(TINYUSB_CDC_ACM_0), TAG, "console 重導失敗");
    return ESP_OK;
}

static esp_err_t storage_hand_over_to_usb(void)
{
    // 交出去之後裝置只剩唯讀視圖（/ro）；要拿回來請把 System.file_mode 設成 1
    //（store 會把 LUN 拆掉，主機的碟會消失、再出現）。
    return nc_store_release_usb();
}

static esp_err_t cdc_send(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, frame, len) != len) {
        return ESP_FAIL;
    }
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}
#endif   // !CONFIG_IDF_TARGET_ESP32P4

// str_u16len 編碼（與 schema 的 str_u16len 一致：2B 長度前綴 + UTF-8）
//
// 回覆直接砌入 TX 幀：DATA = [len u16][json]，中間冇獨立工作區。
// 所以這塊緩衝**跟任務棧走**（每個 decode 任務私有），兩核同時回覆不會撞。
// 之前那塊 static payload[2102] 是全域一份，正是「不能開第二個 decoder」的原因。
//
// 2,600 而不是 2,100：完整的 STATUS_GET（sys+mem+tasks+provides+storage+fx）
// 大約 2.4 KB，用 2,100 會**靜靜截斷**成無效 JSON。
#define NC4_REPLY_JSON_MAX 2600

// 回覆建構器：整塊幀在呼叫者棧上。
//   nc4_reply_t r; char *j = reply_open(&r, ctx, CMD); …寫 j…; reply_send(&r, len);
// 兩個 decoder 各自一份，所以「砌 JSON」這件事本身不再需要全域鎖。
typedef struct {
    nc4_ctx_t *ctx;
    uint16_t cmd;
    uint8_t frame[NC4_HDR_LEN + 2 + NC4_REPLY_JSON_MAX + NC4_CRC_LEN];
} nc4_reply_t;

// 回傳可寫的 JSON 區（容量 NC4_REPLY_JSON_MAX + 1，已清空）
static char *reply_open(nc4_reply_t *r, nc4_ctx_t *ctx, uint16_t cmd)
{
    r->ctx = ctx;
    r->cmd = cmd;
    char *json = (char *)r->frame + NC4_HDR_LEN + 2;
    json[0] = '\0';
    return json;
}

static void reply_send(nc4_reply_t *r, size_t len)
{
    if (r->ctx == NULL || r->ctx->send == NULL) {
        return;
    }
    if (len > NC4_REPLY_JSON_MAX) {
        len = NC4_REPLY_JSON_MAX;
    }
    uint8_t *data = r->frame + NC4_HDR_LEN;   // DATA 區：2B 長度前綴 + JSON
    data[0] = (uint8_t)len;
    data[1] = (uint8_t)(len >> 8);

    // 就地打包：payload 指向 frame 自己的 DATA 區（pack_into 用 memmove，安全）
    const int total = nc4_pack_into(r->frame, sizeof(r->frame), r->cmd,
                                    nc4_reply_addr(r->ctx), data, 2 + len);
    if (total > 0) {
        r->ctx->send(r->ctx->send_arg, r->frame, (size_t)total);
    }
}

// 舊介面：已經有現成字串的（多數 handler）走這條
static void reply_json(nc4_ctx_t *ctx, uint16_t cmd, const char *json)
{
    if (ctx->send == NULL) {
        return;
    }
    nc4_reply_t r;
    char *dst = reply_open(&r, ctx, cmd);
    size_t n = strlen(json);
    if (n > NC4_REPLY_JSON_MAX) {
        n = NC4_REPLY_JSON_MAX;
    }
    memcpy(dst, json, n);
    reply_send(&r, n);
}

static void on_reboot(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint32_t delay_ms = 0;
    if (f->len >= 4) {
        delay_ms = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                   ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    }
    ESP_LOGW(TAG, "[%s] NC4 REBOOT (0x100F) delay=%ums", ctx->source, (unsigned)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms + 50));
    esp_restart();
}

// ── 0x1101 STATUS_GET → 0x1102 STATUS_RSP ──
// keys 語義照 mp 版：空 = 全部、"? "= 目錄、其餘逗號分隔
static uint32_t s_bench_ok;

// ── 有界 JSON append ──
// snprintf 回的是「本來想寫多少」，off 可能超過 cap；直接拿 cap - off 下去會 size_t 下溢。
// 所有 status 區塊都走這裡。
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
            *off = cap - 1;      // 截斷：保住 NUL，不要再往後寫
        }
    }
}

// 把子系統的 JSON 直接接到 buf + *off。
// 這些函式（nc_plan_json / nc_task_json / nc_fx_status_json …）都回傳「實際寫了幾多」，
// 所以**不需要中間緩衝**——這正是以前那些 static char plan[700] 存在的原因。
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

// 把一個 status 區塊接上去（自動補逗號）。以前是「砌入 part[] 再 jappend("%s")」，
// 現在直接接在同一個緩衝，少一次複製、少一塊 static。
static void status_add(char *buf, size_t cap, size_t *off, bool *first, nc_json_fn fn)
{
    if (*off >= cap) {
        return;
    }
    if (!*first) {
        jappend(buf, cap, off, ",");
    }
    jchain(buf, cap, off, fn);
    *first = false;
}

// ── status 的各區塊 ──
// 全部改成「寫入呼叫者給的緩衝並回傳長度」：沒有 static，所以兩個 decoder 同時
// 問 STATUS_GET 也不會互相踩（以前那 ~10 KB static 正是「開第二個 decoder」的硬阻塞）。

static size_t status_put_storage(char *buf, size_t cap)
{
    size_t off = 0;
    // FAT 現在在誰手上、裝置能不能直接讀
    jappend(buf, cap, &off, "\"storage\":{\"mode\":\"%s\",\"app_ready\":%d}",
            nc_store_mode_name(), nc_store_app_ready() ? 1 : 0);
    return off;
}

static size_t status_put_sys(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off,
            "\"sys\":{\"fw\":\"%s\",\"heap\":%u,\"up_s\":%lld,\"idf\":\"%s\",\"cid\":%u}",
            FW_VERSION,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (long long)(esp_timer_get_time() / 1000000),
            esp_get_idf_version(), (unsigned)s_cid);
    return off;
}

static size_t status_put_mem(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"mem\":{\"plan\":");
    jchain(buf, cap, &off, nc_plan_json);
    jappend(buf, cap, &off, ",\"hub\":");
    jchain(buf, cap, &off, nc_hub_registry_json);
    jappend(buf, cap, &off, "}");
    return off;
}

static size_t status_put_provides(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"provides\":");
    jchain(buf, cap, &off, nc_provide_list_json);
    return off;
}

#if CONFIG_IDF_TARGET_ESP32P4
static size_t status_put_net(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"net\":{\"bus\":");
    jchain(buf, cap, &off, nc4_bus_stats_json);
    jappend(buf, cap, &off, ",\"ws\":");
    jchain(buf, cap, &off, nc4_ws_json);
    jappend(buf, cap, &off, ",\"udp\":");
    jchain(buf, cap, &off, nc4_udp_json);
    jappend(buf, cap, &off, "}");
    return off;
}
#endif

// 任務表（含每個 loop 的核心、量測）。"task" / "modules" / "svc" 都是舊名，照樣接受。
static size_t status_put_tasks(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"tasks\":");
    jchain(buf, cap, &off, nc_task_json);
    return off;
}

static bool keys_want(const char *keys, const char *name)
{
    if (keys == NULL || keys[0] == '\0' || strcmp(keys, "all") == 0) {
        return true;
    }
    size_t n = strlen(name);
    const char *p = keys;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (strncmp(p, name, n) == 0) {
            return true;
        }
        while (*p && *p != ',') p++;
    }
    return false;
}

// ── 0x1101 STATUS_GET → 0x1102 STATUS_RSP ──
// keys 語義照 mp 版：空 = 全部、"? "= 目錄、其餘逗號分隔
static void on_status_get(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    // keys 是 str_u16len：2B 長度 + 內容
    char keys[64] = {0};
    if (f->len >= 2) {
        uint16_t klen = (uint16_t)(f->data[0] | (f->data[1] << 8));
        if (klen < sizeof(keys) && 2 + klen <= f->len) {
            memcpy(keys, f->data + 2, klen);
        }
    }

    if (strcmp(keys, "?") == 0) {
        reply_json(ctx, NC4_CMD_STATUS_RSP,
                   "{\"keys\":[\"sys\",\"storage\",\"mem\",\"provides\","
                   "\"tasks\",\"fx\",\"wifi\""
#if CONFIG_IDF_TARGET_ESP32P4
                   ",\"net\""
#endif
                   "]}");
        return;
    }

    // 整份 JSON 砌在**回覆幀的 DATA 區**（呼叫者棧上），冇 static、冇中間 part 緩衝。
    nc4_reply_t r;
    char *json = reply_open(&r, ctx, NC4_CMD_STATUS_RSP);
    const size_t cap = NC4_REPLY_JSON_MAX + 1;
    size_t off = 0;
    bool first = true;

    // 由便宜排到貴：真係放唔落的時候，至少前面幾項係完整嘅。
    // （全部 key 加埋大約 5.8 KB，一個 2.6 KB 的幀一定裝唔落 —— 所以「空 keys」係
    //   「盡量裝」，唔係「保證全部」。要齊就分批問。）
    jappend(json, cap, &off, "{");
    if (keys_want(keys, "sys")) {
        status_add(json, cap, &off, &first, status_put_sys);
    }
    if (keys_want(keys, "storage")) {
        status_add(json, cap, &off, &first, status_put_storage);
    }
    if (keys_want(keys, "mem")) {
        status_add(json, cap, &off, &first, status_put_mem);
    }
    if (keys_want(keys, "provides")) {
        status_add(json, cap, &off, &first, status_put_provides);
    }
    if (keys_want(keys, "task") || keys_want(keys, "tasks")) {
        status_add(json, cap, &off, &first, status_put_tasks);
    }
    if (keys_want(keys, "fx")) {
        status_add(json, cap, &off, &first, nc_fx_status_json);
    }
    if (keys_want(keys, "wifi")) {
        status_add(json, cap, &off, &first, nc_wifi_json);
    }
#if CONFIG_IDF_TARGET_ESP32P4
    if (keys_want(keys, "net")) {
        status_add(json, cap, &off, &first, status_put_net);
    }
#endif
    if (off >= cap - 1) {
        // 放唔落：**唔可以送半截 JSON 出去**（主機一定 parse 失敗）。
        // 寧願回一個合法、講清楚發生咩事的物件，叫主機分批問。
        off = 0;
        jappend(json, cap, &off,
                "{\"truncated\":1,\"cap\":%u,"
                "\"tips\":\"keys=sys,storage,mem,provides,tasks,fx 分批問\"}",
                (unsigned)NC4_REPLY_JSON_MAX);
    } else {
        jappend(json, cap, &off, "}");
    }
    reply_send(&r, off);
}

// ── 0x1811~0x1814 bench 域（照 slave/schema/bench.json）──
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

// ── 0xF001 專屬測試：解碼 / 組包 / flash 量測 ──
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

    if (mode >= 2) {
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (part == NULL) {
            free(payload);
            free(frame);
            nc4_decoder_destroy(dec);
            reply_json(ctx, NC4_CMD_DEV_RSP, "{\"err\":\"no part\"}");
            return;
        }
        const size_t unit = (mode == 4) ? 0x10000 : 4096;
        const size_t off = part->size - unit * 2;
        if (mode == 2 || mode == 4) {
            esp_partition_erase_range(part, off, unit);
            t0 = esp_timer_get_time();
            for (uint32_t i = 0; i < iters; i++) {
                esp_partition_erase_range(part, off, unit);
            }
            dt = esp_timer_get_time() - t0;
            snprintf(json, sizeof(json),
                     "{\"mode\":\"%s\",\"iters\":%u,\"us_per_op\":%.1f}",
                     mode == 4 ? "erase64k" : "erase4k",
                     (unsigned)iters, (double)dt / (double)iters);
        } else {
            uint32_t n = iters > 8 ? 8 : iters;
            esp_partition_erase_range(part, off, 4096 * n);
            t0 = esp_timer_get_time();
            for (uint32_t i = 0; i < n; i++) {
                esp_partition_write(part, off + 4096 * i, payload, 4096);
            }
            dt = esp_timer_get_time() - t0;
            snprintf(json, sizeof(json),
                     "{\"mode\":\"write\",\"iters\":%u,\"us_per_op\":%.1f}",
                     (unsigned)n, (double)dt / (double)n);
        }
        reply_json(ctx, NC4_CMD_DEV_RSP, json);
        free(payload);
        free(frame);
        nc4_decoder_destroy(dec);
        return;
    }

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

// ── 0xF008 匯流排統計（證明 copy 有沒有真的消失）──
static void on_bus_stats(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len >= 1 && f->data[0] != 0) {
        nc4_bus_stats_reset();
        s_rx_resync = 0;
        s_rx_hub_full = 0;
        s_rx_sel = 0;
        s_rx_sel_to = 0;
        s_rx_fill_us = 0;
        s_rx_frames = 0;
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
            (unsigned)s_rx_resync, (unsigned)s_rx_frames, (unsigned)s_rx_hub_full,
            (unsigned)s_rx_sel, (unsigned)s_rx_sel_to,
            (unsigned long long)s_rx_fill_us);
    reply_send(&r, off);
}

// ── 0x1105 ROUTE_GET → 0x1106 ROUTE_RSP ──
// 查「邊條線去邊個核」。表本身在 nc4_router，開機時由 system.json 的 nc4.routes[] 填。
// 以前只有開機 log 印一次，出貨之後查唔到；現在可以隨時問。
static void on_route_get(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    nc4_reply_t r;
    char *json = reply_open(&r, ctx, NC4_CMD_ROUTE_RSP);
    const size_t n = nc4_router_json(json, NC4_REPLY_JSON_MAX + 1);
    reply_send(&r, n);
}

// ── 慢工作佇列：decode task 只「轉交」，不在這裡做 ──
typedef struct {
    uint16_t cmd;
    uint16_t arg;         // SLOW 的毫秒數 / UART_BENCH 的幀數
    uint32_t baud;        // UART_BENCH 用
    uint16_t size;        // UART_BENCH 用（每幀 payload）
    nc4_send_fn send;
    void *send_arg;
} slow_job_t;

static nc_hub_t *s_job_hub;

#define NC4_CMD_UART_BENCH 0xF006

static void run_uart_bench(const slow_job_t *job)
{
    const uint16_t frames = job->arg;
    size_t size = job->size < 16 ? 16 : (job->size > 512 ? 512 : job->size);
    const size_t total = NC4_HDR_LEN + size + NC4_CRC_LEN;

    uint8_t *payload = malloc(size);
    uint8_t *frame = malloc(total);
    if (payload == NULL || frame == NULL) {
        free(payload);
        free(frame);
        return;
    }
    memset(payload, 0xA5, size);
    int n = nc4_pack_into(frame, total, NC4_CMD_BENCH_DATA, NC4_ADDR_BROADCAST, payload, size);
    free(payload);
    if (n <= 0) {
        free(frame);
        return;
    }

    if (!s_uart_ready) {
        esp_err_t ierr = uart_loop_init(job->baud);
        if (ierr != ESP_OK) {
            char err[96];
            snprintf(err, sizeof(err), "{\"err\":\"uart init %s\"}", esp_err_to_name(ierr));
            goto reply;
        }
    }
    uart_wait_tx_done(UART_TEST_PORT, pdMS_TO_TICKS(1000));
    uart_set_baudrate(UART_TEST_PORT, job->baud);
    uart_flush_input(UART_TEST_PORT);
    __atomic_store_n(&s_bench_ok, 0, __ATOMIC_RELAXED);

    const int64_t t0 = esp_timer_get_time();
    for (uint16_t i = 0; i < frames; i++) {
        uart_write_bytes(UART_TEST_PORT, (const char *)frame, total);
    }
    uart_wait_tx_done(UART_TEST_PORT, pdMS_TO_TICKS(5000));

    int64_t deadline = esp_timer_get_time() + 5 * 1000 * 1000;
    while (__atomic_load_n(&s_bench_ok, __ATOMIC_RELAXED) < frames &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(1);
    }
    const int64_t dt = esp_timer_get_time() - t0;
    const uint32_t got = __atomic_load_n(&s_bench_ok, __ATOMIC_RELAXED);

    char json[192];
    snprintf(json, sizeof(json),
             "{\"baud\":%u,\"sent\":%u,\"got\":%u,\"ms\":%lld,"
             "\"frame\":%u,\"kbps\":%.2f}",
             (unsigned)job->baud, (unsigned)frames, (unsigned)got, (long long)(dt / 1000),
             (unsigned)total,
             dt > 0 ? (double)got * (double)total / (double)dt : 0.0);

reply:
    {
    uint8_t pl[2 + 192];
    size_t len = strlen(json);
    pl[0] = (uint8_t)len;
    pl[1] = (uint8_t)(len >> 8);
    memcpy(pl + 2, json, len);
    uint8_t out[NC4_HDR_LEN + 2 + 192 + NC4_CRC_LEN];
    int m = nc4_pack_into(out, sizeof(out), NC4_CMD_HUB_RSP, NC4_ADDR_BROADCAST, pl, 2 + len);
    if (m > 0 && job->send != NULL) {
        job->send(job->send_arg, out, (size_t)m);
    }
    }
    free(frame);
}

static void slow_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_job_hub == NULL) {      // hub 沒配到 → 這個 loop 沒有工作可做
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        slow_job_t *job = nc_hub_read_begin(s_job_hub);
        if (job == NULL) {
            vTaskDelay(1);
            continue;
        }
        if (job->cmd == NC4_CMD_UART_BENCH) {
            run_uart_bench(job);
            nc_hub_read_end(s_job_hub);
            continue;
        }
        if (job->cmd == NC4_CMD_DEV_SLOW) {
            vTaskDelay(pdMS_TO_TICKS(job->arg));   // 模擬寫 flash 之類的慢工作
            char json[64];
            snprintf(json, sizeof(json), "{\"slow_ms\":%u,\"deferred\":1}", (unsigned)job->arg);
            uint8_t payload[2 + 64];
            size_t n = strlen(json);
            payload[0] = (uint8_t)n;
            payload[1] = (uint8_t)(n >> 8);
            memcpy(payload + 2, json, n);
            uint8_t frame[NC4_HDR_LEN + 2 + 64 + NC4_CRC_LEN];
            int total = nc4_pack_into(frame, sizeof(frame), NC4_CMD_HUB_RSP,
                                      NC4_ADDR_BROADCAST, payload, 2 + n);
            if (total > 0 && job->send != NULL) {
                job->send(job->send_arg, frame, (size_t)total);
            }
        }
        nc_hub_read_end(s_job_hub);
    }
}

// 0xF005：只把工作排進佇列就返回 —— decode task 不再被卡住
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
    const uint8_t split = f->data[0] ? 1 : 0;
    const uint8_t core = f->data[1] > 1 ? 1 : f->data[1];
    const uint8_t frame = (f->len >= 3) ? (f->data[2] ? 1 : 0) : (s_frame_align ? 1 : 0);
    esp_err_t e1 = nvs_set_u8(h, "rx_split", split);
    esp_err_t e2 = nvs_set_u8(h, "rx_core", core);
    esp_err_t e4 = nvs_set_u8(h, "frame_align", frame);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    char json[128];
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
        snprintf(json, sizeof(json),
                 "{\"err\":\"nvs write\",\"set\":\"%s/%s\",\"commit\":\"%s\"}",
                 esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3));
        reply_json(ctx, NC4_CMD_HUB_RSP, json);
        return;
    }
    snprintf(json, sizeof(json),
             "{\"rx_split\":%u,\"rx_core\":%u,\"frame_align\":%u,\"next_boot\":1}",
             (unsigned)split, (unsigned)core, (unsigned)frame);
    reply_json(ctx, NC4_CMD_HUB_RSP, json);
}

static void on_dev_slow(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t ms = 200;
    if (f->len >= 2) {
        ms = (uint16_t)(f->data[0] | (f->data[1] << 8));
    }
    if (s_job_hub == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"no queue\"}");
        return;
    }
    slow_job_t *job = nc_hub_write_begin(s_job_hub);
    if (job == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"full\"}");
        return;
    }
    job->cmd = NC4_CMD_DEV_SLOW;
    job->arg = ms;
    job->send = ctx->send;
    job->send_arg = ctx->send_arg;
    nc_hub_write_commit(s_job_hub);
}

// 0xF006 UART_BENCH：baud(u32) frames(u16) size(u16) → 交給 worker 做
static void on_uart_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (s_job_hub == NULL || f->len < 8) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"args\"}");
        return;
    }
    uint32_t baud = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                    ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    uint16_t frames = (uint16_t)(f->data[4] | (f->data[5] << 8));
    uint16_t size = (uint16_t)(f->data[6] | (f->data[7] << 8));
    if (baud < 9600) {
        baud = 9600;
    }
    if (frames == 0) {
        frames = 100;
    }

    slow_job_t *job = nc_hub_write_begin(s_job_hub);
    if (job == NULL) {
        reply_json(ctx, NC4_CMD_HUB_RSP, "{\"err\":\"busy\"}");
        return;
    }
    job->cmd = NC4_CMD_UART_BENCH;
    job->arg = frames;
    job->baud = baud;
    job->size = size;
    job->send = ctx->send;
    job->send_arg = ctx->send_arg;
    nc_hub_write_commit(s_job_hub);
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

static void on_sys_task_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char json[768];
    nc_task_json(json, sizeof(json));
    reply_json(ctx, NC4_CMD_SYS_TASK_RSP, json);
}

// 0x1105 MEM_QUERY → 0x1106 MEM_RSP（plan + buf 一起回）
static nc4_decoder_t *s_decoder = NULL;

// ── 身份：cid（自己的 2B 協議位址）+ master_cid（上級）──

static uint16_t derive_cid_from_mac(void)
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return NC4_ADDR_BROADCAST;
    }
    return (uint16_t)((mac[4] << 8) | mac[5]);
}

// NVS 只初始化一次；而且必須在「讀任何設定」之前
// （踩過：load_net_tuning 比 node_identity_init 早跑，nvs_open 一定失敗 →
//  0xF00A 寫的 rx_split 從來沒有在開機時生效過）
static void nvs_boot_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失敗: %s", esp_err_to_name(err));
    }
}

static void node_identity_init(void)
{
    nvs_boot_init();

    // cID 的真相在 config/system.json，每次開機都讀：
    //   有值 → 用使用者的；空的/缺的 → 用 MAC 末 2 bytes 推導，並寫回 config。
    uint16_t cid = NC4_ADDR_BROADCAST;
    bool present = false;

    cfg_file_t *cfg = nc_cfg_open("system");
    if (cfg != NULL) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(nc_cfg_root(cfg), "cID");
        if (cJSON_IsString(v) && v->valuestring != NULL && v->valuestring[0] != '\0') {
            char *endp = NULL;
            const unsigned long x = strtoul(v->valuestring, &endp, 16);
            if (endp != NULL && *endp == '\0') {
                cid = (uint16_t)x;
                present = true;
            }
        } else if (cJSON_IsNumber(v)) {
            cid = (uint16_t)v->valueint;
            present = true;
        }
        nc_cfg_close(cfg);
    }

    if (!present) {
        cid = derive_cid_from_mac();
        char hex[8];
        snprintf(hex, sizeof(hex), "%04X", (unsigned)cid);
        nc_cfg_set_string("system", "cID", hex);   // 寫回失敗不影響這次的值
    }

    // master_cid 是「我跟誰」，不是「我是誰」→ 仍然放 NVS
    uint16_t master = NC4_ADDR_BROADCAST;
    nvs_handle_t h = 0;
    if (nvs_open("node", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_u16(h, "master_cid", &master) != ESP_OK) {
            master = NC4_ADDR_BROADCAST;
            nvs_set_u16(h, "master_cid", master);
            nvs_commit(h);
        }
        nvs_close(h);
    }

    s_cid = cid;
    if (s_decoder != NULL) {
        nc4_decoder_set_cid(s_decoder, cid);
        nc4_decoder_set_master_cid(s_decoder, master);
    }
    ESP_LOGI(TAG, "cid=0x%04X（config） master_cid=0x%04X（NVS）", cid, master);
}

static void on_set_master(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 2) {
        return;
    }
    const uint16_t master = (uint16_t)(f->data[0] | (f->data[1] << 8));

    nvs_handle_t h = 0;
    if (nvs_open("node", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "master_cid", master);
        nvs_commit(h);
        nvs_close(h);
    }
    nc4_decoder_set_master_cid(ctx->decoder, master);
    ESP_LOGW(TAG, "[%s] SET_MASTER → 0x%04X（之後回覆都填它）", ctx->source, master);
}


// 這條通道「怎麼讀」—— 框架不關心它背後是 USB / UART / Wi-Fi
static int uart_bus_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    if (!s_uart_ready) {
        return 0;       // 尚未安裝（第一次 UART 測試時才裝）
    }
    int n = uart_read_bytes(UART_TEST_PORT, buf, cap, 0);
    return n < 0 ? -1 : n;
}

static esp_err_t uart_loop_init(uint32_t baud)
{
    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_TEST_PORT, UART_TEST_RX_BUF, 8192, 0, NULL, 0),
                        TAG, "uart driver");
    ESP_RETURN_ON_ERROR(uart_param_config(UART_TEST_PORT, &cfg), TAG, "uart param");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_TEST_PORT, UART_TEST_TX_PIN, UART_TEST_RX_PIN,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    s_uart_ready = true;
    ESP_LOGW(TAG, "UART 迴路已啟動：TX=%d RX=%d baud=%u", UART_TEST_TX_PIN, UART_TEST_RX_PIN,
             (unsigned)baud);
    return ESP_OK;
}

#if !CONFIG_IDF_TARGET_ESP32P4
static int cdc_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    size_t n = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, cap, &n) != ESP_OK) {
        return -1;
    }
    return (int)n;
}
#endif

// ── 資源宣告（模組在宣告期寫「我要多少」，main 不介入）──
// 收/解是否分核（NVS: net/rx_split, net/rx_core；預設 0 / 1）
#if CONFIG_IDF_TARGET_ESP32P4
// 只有 P4 有 ETH；S3 上呢兩個永遠冇人讀，唔 guard 就會 -Wunused-variable
static bool s_eth_ok;              // 硬體/驅動真的起來了才宣告網路資源
#endif
static bool s_rx_split = false;   // 預設走已驗證的 PULL；零複製插槽待修
static uint8_t s_rx_core = 1;

static void load_net_tuning(void)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "net tuning: 讀不到 NVS（用預設 split=0 core=1）");
        return;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, "rx_split", &v) == ESP_OK) {
        s_rx_split = (v != 0);
    }
    if (nvs_get_u8(h, "rx_core", &v) == ESP_OK && v <= 1) {
        s_rx_core = v;
    }
    if (nvs_get_u8(h, "frame_align", &v) == ESP_OK) {
        s_frame_align = (v != 0);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "net tuning: rx_split=%d rx_core=%u frame_align=%d（NVS）",
             s_rx_split ? 1 : 0, (unsigned)s_rx_core, s_frame_align ? 1 : 0);
}

static void *s_nc4_rx_region;
static size_t s_nc4_rx_bytes;
static void *s_uart_rx_region;
#if CONFIG_IDF_TARGET_ESP32P4
static void *s_tcp_rx_region;      // 同上：S3 冇 TCP 通道
#endif
static nc_hub_t *s_probe_hub;

// ── 對外提供（provides）──
// claims[] 是「我要什麼」，provides[] 是「我給什麼」。
// 查詢端一律走 nc_provide_*；提供者 stop/detach 之後就查不到，不會拿到懸空指標。
static bool prov_nc4_rx(void **ptr, size_t *bytes)
{
    if (s_nc4_rx_region == NULL) {
        return false;
    }
    *ptr = s_nc4_rx_region;
    *bytes = s_nc4_rx_bytes;
    return true;
}

static const nc_task_provide_t s_nc4_provides[] = {
    { .name = "nc4.rx", .kind = NC_PROVIDE_BUF, .desc = "CDC parser 緩衝",
      .buf = prov_nc4_rx },
    { .name = "nc4.busstats", .kind = NC_PROVIDE_JSON, .desc = "每條通道的 frames/copied",
      .json = nc4_bus_stats_json },
};

static const nc_task_provide_t s_fx_provides[] = {
    { .name = "fx.info", .kind = NC_PROVIDE_JSON, .desc = "live 效果 / arena / 每個實例",
      .json = nc_fx_info_json },
};

// ── link 啟用狀態：config 是唯一真相 ──
// system.json 的 nc4.links[] 每一條有 "enable"（缺省 = 1，向後相容）。
// 開機讀到之後就決定「這條線存不存在」：
//   冇啟用 → **唔宣告緩衝**（0 B）、唔建立 bus、路由表跳過嗰行
//   要中途開啟 → 改 config 再重啟（runtime 開開關關正是碎片來源）
//
// 為什麼要喺宣告之前問：宣告就係「向 plan 要記憶體」，
// 一旦要咗就開機到 reset 都收唔返。
static bool link_enabled(const char *name)
{
    cfg_file_t *cfg = nc_cfg_open("system");
    if (cfg == NULL) {
        return true;                 // 讀不到 → 照硬體探測結果
    }
    const cJSON *nc4 = cJSON_GetObjectItemCaseSensitive(nc_cfg_root(cfg), "nc4");
    const cJSON *links = cJSON_GetObjectItemCaseSensitive(nc4, "links");
    bool en = true;
    bool found = false;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, links) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (!cJSON_IsString(nm) || nm->valuestring == NULL ||
            strcmp(nm->valuestring, name) != 0) {
            continue;
        }
        found = true;
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(it, "enable");
        en = cJSON_IsNumber(e) ? (e->valueint != 0)
             : (cJSON_IsBool(e) ? cJSON_IsTrue(e) : true);
        break;
    }
    nc_cfg_close(cfg);
    if (!found) {
        ESP_LOGW(TAG, "link %s：links[] 冇呢一條 → 當啟用（要關就寫 \"enable\": 0）", name);
    }
    return en;
}

// ── nc4：CDC 那條線的 parser 緩衝 ──
// P4 冇 USB OTG → CDC 永遠唔會建立，所以**唔宣告**（以前照宣告 8,205 B，永遠冇人用）。
static int task_nc4_declare(nc_task_res_t *out, int max)
{
#if CONFIG_IDF_TARGET_ESP32P4
    (void)out;
    (void)max;
    return 0;
#else
    if (max < 1 || !link_enabled("CDC")) {
        return 0;
    }
    out[0] = (nc_task_res_t){
        .name = "nc4.rx", .bytes = NC4_MAX_FRAME, .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
#endif
}

static esp_err_t task_nc4_start(const nc_task_res_view_t *res, int n)
{
    (void)res;
    (void)n;
    // 用 plan 名字取，不用 view 次序：冇宣告 / 被 shed 就係 NULL（天然嘅「冇就唔要」）
    s_nc4_rx_region = nc_plan_get("nc4.rx");
    s_nc4_rx_bytes = (s_nc4_rx_region != NULL) ? NC4_MAX_FRAME : 0;
    if (s_nc4_rx_region != NULL) {
        ESP_LOGI(TAG, "svc nc4: parser 緩衝 ok");
    } else {
        ESP_LOGW(TAG, "svc nc4: 冇 parser 緩衝（CDC 未啟用 / 本板冇 USB）");
    }
    return ESP_OK;
}

// ── net：UART / TCP 的 parser 緩衝（TCP 只在 P4）──
// 只宣告**真的會建立**嘅 bus；冇啟用就 0 B。
static int task_net_declare(nc_task_res_t *out, int max)
{
    int n = 0;
    if (n < max && link_enabled("UART")) {
        out[n++] = (nc_task_res_t){
            .name = "uart.rx", .bytes = 1024, .cls = NC_PLAN_INTERNAL, .align = 32,
        };
    }
#if CONFIG_IDF_TARGET_ESP32P4
    // 兩個條件都要：硬體真的起來（ETH）＋ config 有啟用（TCP）
    if (n < max && s_eth_ok && link_enabled("TCP")) {
        out[n++] = (nc_task_res_t){
            // 2 幀：跨讀時需要餘裕
            .name = "tcp.rx", .bytes = 16410, .cls = NC_PLAN_INTERNAL, .align = 32,
        };
    }
#endif
    return n;
}

static esp_err_t task_net_start(const nc_task_res_view_t *res, int n)
{
    (void)res;
    (void)n;
    s_uart_rx_region = nc_plan_get("uart.rx");
#if CONFIG_IDF_TARGET_ESP32P4
    s_tcp_rx_region = nc_plan_get("tcp.rx");
#endif
    ESP_LOGI(TAG, "svc net: parser 緩衝 uart=%s tcp=%s",
             s_uart_rx_region ? "ok" : "-",
#if CONFIG_IDF_TARGET_ESP32P4
             s_tcp_rx_region ? "ok" : "-"
#else
             "-"
#endif
    );
    return ESP_OK;
}

// ── core.jobs：慢工作佇列 ──
static int task_core_declare(nc_task_res_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_task_res_t){
        .name = "jobs", .bytes = 8 * sizeof(slow_job_t), .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
}

static esp_err_t task_core_start(const nc_task_res_view_t *res, int n)
{
    (void)n;
    s_job_hub = nc_hub_create_in(res[0].ptr, res[0].len, "jobs",
                                 sizeof(slow_job_t), 8, 32);
    return s_job_hub != NULL ? ESP_OK : ESP_FAIL;
}

// ── probe：開機自測（hub 交換 4 格）──
static int task_probe_declare(nc_task_res_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_task_res_t){
        .name = "probe.hub", .bytes = 4 * 128, .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
}

static esp_err_t task_probe_start(const nc_task_res_view_t *res, int n)
{
    (void)n;
    s_probe_hub = nc_hub_create_in(res[0].ptr, res[0].len, "probe", 128, 4, 32);
    if (s_probe_hub == NULL) {
        return ESP_FAIL;
    }

    char *w = nc_hub_write_begin(s_probe_hub);
    bool ok = (w != NULL);
    if (ok) {
        memset(w, 0x5A, 128);
        ok = (nc_hub_write_commit(s_probe_hub) == ESP_OK);
        char *r = nc_hub_read_begin(s_probe_hub);
        ok = ok && (r == w) && (r[0] == 0x5A) && (r[127] == 0x5A);
        nc_hub_read_end(s_probe_hub);
    }
    ESP_LOGI(TAG, "svc probe: hub 自測 %s", ok ? "OK" : "FAIL");
    return ok ? ESP_OK : ESP_FAIL;
}

// ── 任務宣告（裝配層）──
//
// 這裡是「誰存在、誰先起、誰吃什麼、誰擁有哪條指令」的唯一答案。
// 任務管理器（nc_task）負責：宣告記憶體 → 帳本檢查 → 依 level 啟動 →
// 註冊指令（帶擁有權）→ 開 loop。main 只回答「有哪些任務」。
//
// 三種 kind 的分工：
//   system  = 硬體 / driver / 通道 / fx 載入器（固定順序、不可卸載、唔入 tasks.json）
//   normal  = 一般任務（以前的 service 就是這個；probe、hello）
//   effect  = 由 .fxe 載入的任務（在 nc_fx_load 裡 attach，不在這張表）
//
// 租戶（normal / effect）的 level / affinity 由 /data/config/tasks.json 決定；
// 未設定 = (0,0) = 唔跑。核心（system）永遠照這裡的宣告跑。

// ① nc4（System L0）：CDC parser 緩衝 + 兩核的解碼任務
//
// decode0 / decode1 開唔開，由路由器答：冇線去嗰核就索性唔建立，
// 6 KB 棧同 parser 緩衝由頭到尾都唔存在。呢個開關係固件宣告，
// config 改唔到（SYSTEM 任務唔讀 tasks.json，見 nc_task.c 的 load_cfg）。
static bool nc4_want_core0(void) { return nc4_router_core_used(0); }
static bool nc4_want_core1(void) { return nc4_router_core_used(1); }

static const nc_task_loop_t s_nc4_loops[] = {
    { .name = "decode0", .fn = nc4_bus_task_entry, .arg = (void *)0,
      .stack = 6144, .prio = 4, .core = 0, .layer = 1,
      .enabled = nc4_want_core0 },
    { .name = "decode1", .fn = nc4_bus_task_entry, .arg = (void *)1,
      .stack = 6144, .prio = 4, .core = 1, .layer = 1,
      .enabled = nc4_want_core1 },
};

static const nc_task_t s_task_nc4 = {
    .name = "nc4", .kind = NC_TASK_SYSTEM, .level = 0,
    .declare = task_nc4_declare, .on_start = task_nc4_start,
    .loops = s_nc4_loops, .nloops = sizeof(s_nc4_loops) / sizeof(s_nc4_loops[0]),
    .provides = s_nc4_provides,
    .nprovides = sizeof(s_nc4_provides) / sizeof(s_nc4_provides[0]),
};

// ② net（System L1）：UART / TCP 的 parser 緩衝（照實際硬體宣告）
static const nc_task_t s_task_net = {
    .name = "net", .kind = NC_TASK_SYSTEM, .level = 1,
    .declare = task_net_declare, .on_start = task_net_start,
};

// ③ core（System L1）：慢工作 hub + 唯一的 worker loop
static const nc_task_loop_t s_core_loops[] = {
    { .name = "slow_worker", .fn = slow_worker, .arg = NULL,
      .stack = 4096, .prio = 2, .core = 0, .layer = 2 },
};

static const nc_task_t s_task_core = {
    .name = "core", .kind = NC_TASK_SYSTEM, .level = 1,
    .declare = task_core_declare, .on_start = task_core_start,
    .loops = s_core_loops, .nloops = sizeof(s_core_loops) / sizeof(s_core_loops[0]),
};

// ④ Action：NC4 指令（核心執行能力，不屬於任何任務）
//
// 指令是 decoder 的表；開機時註冊一次，之後 decode task 只負責收發與分派。
// 任務不擁有指令 —— 任務只是把「已經存在的執行能力」恆常化、排程化。
typedef struct {
    uint16_t cmd;
    nc4_handler_t fn;
} core_action_t;

static const core_action_t s_core_actions[] = {
    { NC4_CMD_REBOOT,            on_reboot },
    { NC4_CMD_STATUS_GET,        on_status_get },
    { NC4_CMD_ROUTE_GET,         on_route_get },
    { NC4_CMD_DEV_BENCH,         on_dev_bench },
    { NC4_CMD_HUB_BENCH,         on_hub_bench },
    { NC4_CMD_TX_BENCH,          on_tx_bench },
    { NC4_CMD_BUS_STATS,         on_bus_stats },
    { NC4_CMD_DEV_SLOW,          on_dev_slow },
    { NC4_CMD_NET_TUNE,          on_net_tune },
    { NC4_CMD_UART_BENCH,        on_uart_bench },
    { NC4_CMD_BENCH_READY,       on_bench_ready },
    { NC4_CMD_BENCH_DATA,        on_bench_data },
    { NC4_CMD_BENCH_RESULT,      on_bench_result },
    { NC4_CMD_SET_MASTER,        on_set_master },
    { NC4_CMD_SYS_TASK_QUERY,    on_sys_task_query },
};

// 開機把三組 action 註冊進 decoder：core（上面這張）、ota（0x22xx）、file（0x2005/0x1103）。
// 沒有任務參與 —— 這就是「指令是核心、任務是表面」的實際長相。
static void actions_register(nc4_decoder_t *dec)
{
    if (dec == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof(s_core_actions) / sizeof(s_core_actions[0]); i++) {
        nc4_decoder_on_owner(dec, s_core_actions[i].cmd, s_core_actions[i].fn, "core");
    }
    // cid / master_cid 已在開機最前面初始化（卷標要用）
    nc_ota_action_register(dec);             // 0x22xx
    nc_file_action_register(dec);       // 0x2005 FILE_QUERY / 0x1103 STATUS_SET
    ESP_LOGI(TAG, "actions：core %u 條 + ota + file 已註冊（decoder 共 %u 條）",
             (unsigned)(sizeof(s_core_actions) / sizeof(s_core_actions[0])),
             (unsigned)nc4_decoder_count(dec));
}

// ── 路由器：張表由邊度嚟 ──
//
// 真相在 `config/system.json` 的 `nc4.routes[]`（一列一個來源）。這裡只讀「這一列喺邊個核」：
// `out` 裡第一個 `decodeN` 就是它。冇 `decodeN`（只轉發／丟棄）→ 唔需要本地 decoder，跳過。
//
// 今日只實作 `in → decodeN` 這一段；`out` 裡的 link（轉發）同 `outbound` 未實作。
//
// 一條 link 的 **RX 解碼核同時就係它的 TX 打包核**：decode 任務收幀、解幀、回覆／打包
// 都在同一個核上做，所以 `out:["decode1"]` 一次決定兩個方向，唔需要另外寫。
static int router_from_cfg(nc4_route_t *out, int max)
{
    cfg_file_t *cfg = nc_cfg_open("system");
    if (cfg == NULL) {
        return 0;
    }
    const cJSON *nc4 = cJSON_GetObjectItemCaseSensitive(nc_cfg_root(cfg), "nc4");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(nc4, "routes");
    int n = 0;
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, rows) {
        if (n >= max) {
            break;
        }
        const cJSON *in = cJSON_GetObjectItemCaseSensitive(row, "in");
        if (!cJSON_IsString(in)) {
            continue;
        }
        // 呢條 link 冇啟用 → 佢唔會存在，路由都唔應該令嗰個核「有嘢做」
        const char *inname = in->valuestring;
        if (strcmp(inname, "nctx") != 0 && strcmp(inname, "outbound") != 0 &&
            !link_enabled(inname)) {
            ESP_LOGW(TAG, "router：%s 未啟用（links[].enable=0）→ 跳過呢行", inname);
            continue;
        }
        int core = NC4_ROUTE_NONE;
        const cJSON *o = NULL;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(row, "out")) {
            if (!cJSON_IsString(o)) {
                continue;
            }
            const char *s = o->valuestring;
            if (strncmp(s, "decode", 6) == 0 && (s[6] == '0' || s[6] == '1') && s[7] == '\0') {
                core = s[6] - '0';
                break;
            }
        }
        if (core == NC4_ROUTE_NONE) {
            continue;
        }
        out[n].line = in->valuestring;   // router 會複製，指標生命週期唔緊要
        out[n].core = core;
        n++;
    }
    nc_cfg_close(cfg);
    if (n > 0) {
        ESP_LOGI(TAG, "router：由 system.json 讀到 %d 列", n);
    }
    return n;
}

// 冇 config（或 config 冇有效列）時的預設表：照「實際探測到的硬體」決定。
static int router_defaults(nc4_route_t *out, int max)
{
    int n = 0;
#if !CONFIG_IDF_TARGET_ESP32P4
    if (n < max) out[n++] = (nc4_route_t){ .line = "CDC", .core = 0 };
    if (n < max) out[n++] = (nc4_route_t){ .line = "UART", .core = 1 };
#else
    if (s_eth_ok) {
        if (n < max) out[n++] = (nc4_route_t){ .line = "TCP", .core = 0 };
        if (n < max) out[n++] = (nc4_route_t){ .line = "UART", .core = 1 };
    } else {
        // 冇乙太網 → core0 閒置，UART 搬去 core0；core1 索性唔開，
        // 省一個 6 KB 棧同一個 parser 緩衝。
        if (n < max) out[n++] = (nc4_route_t){ .line = "UART", .core = 0 };
    }
#endif
    return n;
}

// ⑤ probe（Normal L3）：開機自測（hub 交換 4 格）
static const nc_task_t s_task_probe = {
    .name = "probe", .kind = NC_TASK_NORMAL, .level = 3,
    .declare = task_probe_declare, .on_start = task_probe_start,
};

// ⑥ fx（Normal L4）：政策層 —— arena + 實例表 + 唯一的 render task
#define NC_FX_TASK_CORE 1
#define NC_FX_TASK_PRIO 3

static esp_err_t task_fx_start(const nc_task_res_view_t *res, int n)
{
    if (n < 1 || res[0].ptr == NULL) {
        return ESP_ERR_NO_MEM;
    }
    nc_fx_set_task_placement(NC_FX_TASK_CORE, NC_FX_TASK_PRIO);
    const esp_err_t e = nc_fx_init(res[0].ptr, res[0].len);
    if (e != ESP_OK) {
        return e;
    }
    // 掃目錄不在這裡做：載入 .fxe = attach 租戶 = 會插入 registry，
    // 而此刻 nc_task_start_all() 正在走同一張表（插入會搬動它）。
    // main 在 start_all 之後呼叫 nc_fx_scan("/data/pixel/effects")。
    return ESP_OK;
}

static const nc_task_loop_t s_fx_loops[] = {
    { .name = "fx", .fn = nc_fx_task, .arg = NULL,
      .stack = 4096, .prio = NC_FX_TASK_PRIO, .core = NC_FX_TASK_CORE,
      // period_ms = 0：幀週期由 manifest 的 fps 決定，nc_fx_task 自己延遲。
      // 兩邊都延遲會變成兩倍週期。
      .layer = 3, .period_ms = 0, .budget_us = 16000 },
};

static int task_fx_declare(nc_task_res_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_task_res_t){
        .name = "fx.arena", .bytes = NC_FX_ARENA_MAX,
        .cls = NC_PLAN_PSRAM, .align = 32,
    };
    return 1;
}

static const nc_task_t s_task_fx = {
    // fx 是載入器／renderer，屬核心基建（房東），不是租戶：唔寫入 tasks.json。
    .name = "fx", .kind = NC_TASK_SYSTEM, .level = 4,
    .declare = task_fx_declare, .on_start = task_fx_start,
    .loops = s_fx_loops, .nloops = sizeof(s_fx_loops) / sizeof(s_fx_loops[0]),
    .provides = s_fx_provides,
    .nprovides = sizeof(s_fx_provides) / sizeof(s_fx_provides[0]),
};

#if CONFIG_IDF_TARGET_ESP32P4
// ── P4：RMII 乙太網（IP101，設定取自 ports/P4/ESP32-ETH/config.json）──
#define ETH_MDC_GPIO     31
#define ETH_MDIO_GPIO    52
#define ETH_REFCLK_GPIO  50
#define ETH_PHY_ADDR     1
#define TCP_SINK_PORT    5000
#define TCP_NC4_PORT     6000

// NC4 over TCP：一個連線 = 一條 bus
static volatile int s_tcp_nc4_fd = -1;
static nc4_bus_handle_t s_tcp_nc4_bus;

// 收掉連線（唯一出口：fd 先歸零，accept task 才接得到下一條）
static void tcp_nc4_close(const char *why)
{
    const int fd = s_tcp_nc4_fd;
    if (fd < 0) {
        return;
    }
    s_tcp_nc4_fd = -1;
    close(fd);
    ESP_LOGW(TAG, "NC4/TCP 關閉連線（%s）", why);
}

static int tcp_nc4_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    const int fd = s_tcp_nc4_fd;
    if (fd < 0) {
        return 0;
    }
    int n = recv(fd, buf, cap, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;                 // 暫時沒資料：不是錯誤
        }
        // RST / 逾時等真錯誤：一定要收掉 socket。
        // 只回 -1 不收線的話，s_tcp_nc4_fd 永遠 >= 0 → accept task 卡在等它 → 再也接不到新連線。
        tcp_nc4_close("recv 錯誤");
        return 0;
    }
    if (n == 0) {                 // peer 關閉
        tcp_nc4_close("peer 關閉");
        return 0;
    }
    return n;
}

static int tcp_nc4_send(void *arg, const uint8_t *buf, size_t len)
{
    (void)arg;
    const int fd = s_tcp_nc4_fd;
    if (fd < 0) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        int w = send(fd, buf + off, len - off, 0);
        if (w <= 0) {
            // 送不出去就沒有「之後」了：半截 frame 會讓對端永遠對不上，直接斷
            tcp_nc4_close("send 失敗");
            return -1;
        }
        off += (size_t)w;
    }
    return (int)len;
}

// 等 socket 可讀：睡在 select 上，資料一到就醒。
// （用 vTaskDelay(1) 輪詢的話，每 catch-up 一次就固定賠 1 ms —— 1 KB 幀只有
//   0.17 ms 的間隔，實測吞吐會從 6.3 掉到 0.78 MB/s。）
static bool rx_wait_readable(int fd, int ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = ms * 1000 };
    s_rx_sel++;
    const int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r == 0) {
        s_rx_sel_to++;
        return false;
    }
    return r > 0 && FD_ISSET(fd, &rfds);
}

// 零複製核心：把「剛好一整幀」讀進 dst，回傳整幀長度（失敗 -1，已關線或 abort）。
// 先 peek 9 B 表頭決定長度 → 再 recv 剛好 total bytes，所以幀一定是連續的，
// decode task 可以原地解析（不需要 ring、不需要 memcpy）。
static int tcp_nc4_fill_frame(int fd, uint8_t *dst)
{
    size_t hdr = 0;                  // dst[0..hdr) 已收到（最多 9）
    int64_t stall_since = 0;
    const int64_t stall_us = 2000000;

    for (;;) {
        // ① 湊滿 9 B 表頭（不用 MSG_PEEK：peek 版本在「一幀拆 3 段送」時卡住，改用先讀 9 B）
        while (hdr < NC4_HDR_LEN) {
            int r = recv(fd, dst + hdr, NC4_HDR_LEN - hdr, MSG_DONTWAIT);
            if (r > 0) {
                hdr += (size_t)r;
                stall_since = 0;
                continue;
            }
            if (r == 0) {
                tcp_nc4_close("peer 關閉");
                return -1;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                tcp_nc4_close("recv 錯誤");
                return -1;
            }
            // 完全沒有 byte = 連線閒著，可以無限等；有半個表頭才算壞線
            if (hdr > 0) {
                if (stall_since == 0) {
                    stall_since = esp_timer_get_time();
                } else if (esp_timer_get_time() - stall_since > stall_us) {
                    tcp_nc4_close("表頭不完整");
                    return -1;
                }
            }
            rx_wait_readable(fd, 5);
        }
        stall_since = 0;

        // ② 驗表頭；不是就把 9 B 視窗往後滑 1 byte 重新對齊
        const size_t total = nc4_header_total(dst);
        if (total == 0) {
            memmove(dst, dst + 1, NC4_HDR_LEN - 1);
            hdr = NC4_HDR_LEN - 1;
            s_rx_resync++;
            continue;
        }

        // ③ 把這一幀剩下的 bytes 讀進同一格（dst[0..9) 已經是表頭）
        size_t got = NC4_HDR_LEN;
        int64_t last_progress = esp_timer_get_time();
        while (got < total) {
            int r = recv(fd, dst + got, total - got, MSG_DONTWAIT);
            if (r > 0) {
                got += (size_t)r;
                last_progress = esp_timer_get_time();
                continue;
            }
            if (r == 0) {
                tcp_nc4_close("peer 關閉");
                return -1;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                tcp_nc4_close("recv 錯誤");
                return -1;
            }
            if (esp_timer_get_time() - last_progress > stall_us) {
                tcp_nc4_close("幀只收到一半");
                return -1;
            }
            rx_wait_readable(fd, 5);
        }
        return (int)total;
    }
}

// 專用接收 task：只把 socket 的資料灌進插槽，解析由 decode task 做
static void tcp_nc4_rx_task(void *arg)
{
    nc4_bus_handle_t bus = (nc4_bus_handle_t)(intptr_t)arg;
    uint32_t spins = 0;
    for (;;) {
        const int fd = s_tcp_nc4_fd;
        if (fd < 0) {
            vTaskDelay(10);
            continue;
        }
        // 直接 recv 進插槽，沒有中間 ring
        uint8_t *dst = nc4_bus_push_begin(bus, NC4_MAX_FRAME);
        if (dst == NULL) {
            s_rx_hub_full++;
            // consumer 在另一個核心，通常幾十 µs 就放一格；
            // 這裡睡 1 ms 會直接把小幀的吞吐壓到 ~780 fps（實測 6.2 → 0.78 MB/s）
            if (++spins < 300) {
                taskYIELD();
            } else {
                spins = 0;
                vTaskDelay(1);      // 真的卡住了才睡
            }
            continue;
        }
        spins = 0;

        if (s_frame_align) {
            // 零複製：一格 = 一整幀（先讀 9 B 表頭決定長度）
            const int64_t t0 = esp_timer_get_time();
            const int flen = tcp_nc4_fill_frame(fd, dst);
            s_rx_fill_us += (uint64_t)(esp_timer_get_time() - t0);
            if (flen > 0) {
                s_rx_frames++;
                nc4_bus_push_commit(bus, (size_t)flen);
                nc4_bus_kick(bus);
            } else {
                nc4_bus_push_abort(bus);
            }
            continue;
        }

        // 舊路徑（stream 模式）：recv 到哪算哪，由 decode task 的 slot_pop 拼接
        int n = recv(fd, dst, NC4_MAX_FRAME, MSG_DONTWAIT);
        if (n > 0) {
            nc4_bus_push_commit(bus, (size_t)n);
            nc4_bus_kick(bus);
            continue;
        }
        nc4_bus_push_abort(bus);         // 沒資料／錯誤：插槽一律還回去
        if (n == 0) {
            tcp_nc4_close("peer 關閉");
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vTaskDelay(1);
            continue;
        }
        // 其他錯誤（RST/逾時）：關掉這個連線，否則永遠無法接受新連線
        tcp_nc4_close("recv 錯誤");
    }
}

static void tcp_nc4_accept_task(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_NC4_PORT),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "NC4/TCP bind 失敗");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "NC4 over TCP 在 %d 等連線", TCP_NC4_PORT);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            vTaskDelay(10);
            continue;
        }
        if (s_tcp_nc4_fd >= 0) {       // 只服務一個連線
            close(c);
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // send 不准無限等：對端不讀時，decode task 會卡在 send 裡（還握著 dispatch mutex）
        struct timeval snd_to = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
        // 新連線 = 新的位元組流：上一條連線可能停在半個 frame 中間，
        // 那些殘留 bytes 會把這一條的小指令全部吞掉（看起來像「卡住」）→ 先清掉，
        // 而且要在 fd 交出去之前清（fd >= 0 之後 decode task 就會開始讀）
        nc4_bus_reset(s_tcp_nc4_bus);
        s_tcp_nc4_fd = c;
        ESP_LOGW(TAG, "NC4/TCP 連線進來（parser 已重置）");
        while (s_tcp_nc4_fd >= 0) {
            vTaskDelay(50);
        }
        // 斷線後也清一次：殘幀不跨連線，下一條從乾淨的狀態開始
        nc4_bus_reset(s_tcp_nc4_bus);
        ESP_LOGW(TAG, "NC4/TCP 連線結束");
    }
}

static void eth_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGW(TAG, "ETH link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "ETH link down");
    }
}

static void got_ip_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    ESP_LOGW(TAG, "ETH IP: " IPSTR "  gw: " IPSTR, IP2STR(&e->ip_info.ip),
             IP2STR(&e->ip_info.gw));
}

static esp_err_t net_eth_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    if (netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = ETH_REFCLK_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);   // IDF 6 只留 generic 802.3 PHY
    if (mac == NULL || phy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &handle), TAG, "eth install");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)), TAG, "glue");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event_cb, NULL), TAG, "eth evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                                   got_ip_cb, NULL), TAG, "ip evt");
    ESP_RETURN_ON_ERROR(esp_eth_start(handle), TAG, "eth start");
    ESP_LOGW(TAG, "ETH started (RMII, IP101@%d, mdc=%d mdio=%d refclk=%d)",
             ETH_PHY_ADDR, ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_REFCLK_GPIO);

    // 測試用：固定 IP（DHCP 拿不到時也能量吞吐）
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton("10.10.1.50");
    ip.netmask.addr = esp_ip4addr_aton("255.255.0.0");
    ip.gw.addr = esp_ip4addr_aton("10.10.1.1");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG, "static ip");
    ESP_LOGW(TAG, "ETH 靜態 IP: 10.10.1.50/16");
    {   // 印 link 速度，確認是 100M 還是 10M
        eth_speed_t speed = ETH_SPEED_100M;
        eth_duplex_t duplex = ETH_DUPLEX_FULL;
        esp_eth_ioctl(handle, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGW(TAG, "ETH link: %d Mbps, %s duplex", speed == ETH_SPEED_100M ? 100 : 10,
                 duplex == ETH_DUPLEX_FULL ? "full" : "half");

        // ★ 自動協商失敗會掉到 10M half（雙工不匹配 → 幾乎不通）。
        //   IP101 沒有 reset GPIO，MCU 重開不會重置 PHY，所以這裡主動重啟 autoneg。
        if (speed != ETH_SPEED_100M || duplex != ETH_DUPLEX_FULL) {
            uint32_t bmcr = 0;
            esp_eth_phy_reg_rw_data_t rw = { .reg_addr = 0, .reg_value_p = &bmcr };
            if (esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &rw) == ESP_OK) {
                bmcr = (bmcr & ~(1u << 8)) | (1u << 9) | (1u << 12);   // 清 power-down、啟用+重啟 autoneg
                rw.reg_value_p = &bmcr;
                if (esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &rw) == ESP_OK) {
                    ESP_LOGW(TAG, "ETH: 重啟 PHY autoneg（BMCR=0x%04x）", (unsigned)bmcr);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    speed = ETH_SPEED_100M;
                    duplex = ETH_DUPLEX_FULL;
                    esp_eth_ioctl(handle, ETH_CMD_G_SPEED, &speed);
                    esp_eth_ioctl(handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
                    ESP_LOGW(TAG, "ETH link（重試後）: %d Mbps, %s duplex",
                             speed == ETH_SPEED_100M ? 100 : 10,
                             duplex == ETH_DUPLEX_FULL ? "full" : "half");
                }
            }
        }
        if (speed != ETH_SPEED_100M || duplex != ETH_DUPLEX_FULL) {
            ESP_LOGE(TAG, "ETH 鏈路不是 100M full（雙工不匹配會完全不通）");
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

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
#endif

void app_main(void)
{
#ifdef NETCORE_MINIMAL
    ESP_LOGI(TAG, "MINIMAL build start");
    int64_t prev = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "t=%lld dt=%lld", (long long)now, (long long)(now - prev));
        prev = now;
    }
#endif
    // 清掉上個 cycle 留下的 hold（否則板子會一直卡在 download mode）
    rtc_gpio_hold_dis(BOOT_BUTTON_GPIO);
    rtc_gpio_deinit(BOOT_BUTTON_GPIO);
    gpio_hold_dis(BOOT_BUTTON_GPIO);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "esp_Net-Core boot — ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "chip: %s rev %d, %d core(s)",
             CONFIG_IDF_TARGET, chip.revision, chip.cores);
#if CONFIG_SPIRAM
    ESP_LOGW(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
#endif
    ESP_LOGW(TAG, "頻率：CPU=%u MHz  APB=%u MHz  XTAL=%u MHz",
             (unsigned)(esp_clk_cpu_freq() / 1000000),
             (unsigned)(esp_clk_apb_freq() / 1000000),
             (unsigned)(esp_clk_xtal_freq() / 1000000));

    // ── NVS → /data 掛載 → 身份（cID 在 config）→ 卷標（用 cid）──
    nvs_boot_init();        // ★ 一定要比任何 nvs_open 早
    esp_err_t err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage 初始化失敗: %s", esp_err_to_name(err));
    }
    config_sanitize();
    node_identity_init();
    ensure_volume_label();
    load_net_tuning();

    // ── ③④ 硬體 / Driver：先確認「有沒有這個硬體」──
#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t nerr = net_eth_start();
    s_eth_ok = (nerr == ESP_OK);
    if (s_eth_ok) {
        xTaskCreatePinnedToCore(tcp_sink_task, "tcp_sink0", 6144, (void *)5000, 5, NULL, 1);
        xTaskCreatePinnedToCore(tcp_sink_task, "tcp_sink3", 6144, (void *)5003, 5, NULL, 0);
        xTaskCreatePinnedToCore(tcp_src_task, "tcp_src0", 6144, (void *)5001, 5, NULL, 0);
        xTaskCreatePinnedToCore(tcp_src_task, "tcp_src1", 6144, (void *)5002, 5, NULL, 1);
        xTaskCreatePinnedToCore(tcp_nc4_accept_task, "tcp_nc4", 6144, NULL, 9, NULL, 0);
    } else {
        ESP_LOGE(TAG, "乙太網啟動失敗: %s → 不宣告/不啟動 TCP 通道", esp_err_to_name(nerr));
    }
#endif

    // ── 路由器：邊條線由邊個核服務（探測完硬體才決定）──
    // 呢張表係「線 → 核」唯一嘅決定處；bus 只係查表，唔自己諗。
    // 冇線去某一核 → 嗰核嘅 decode 任務連棧都唔會建立（見 s_nc4_loops）。
    {
        nc4_route_t routes[NC4_ROUTE_MAX];
        int n = router_from_cfg(routes, NC4_ROUTE_MAX);
        if (n <= 0) {
            n = router_defaults(routes, NC4_ROUTE_MAX);
            ESP_LOGW(TAG, "router：system.json 冇有效 routes[] → 用預設表（%d 列）", n);
        }
        const esp_err_t rerr = nc4_router_set(routes, n);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "路由表無效: %s → 只保留 core0", esp_err_to_name(rerr));
            nc4_router_reset();
        }
        if (!nc4_router_core_used(0) && !nc4_router_core_used(1)) {
            nc4_router_keep(0);   // 一條線都冇：至少留一個 decode 任務答「點解冇線」
        }
        char rj[256];
        nc4_router_json(rj, sizeof(rj));
        ESP_LOGI(TAG, "router: %s", rj);
    }

    // ── ⑤ 宣告（照「實際存在的硬體」決定）──
    // 這個 registry 管兩件事：記憶體（nc_plan）、迴圈（nc_task）。
    // 指令不在這裡 —— 它是 decoder 的表，下面 actions_register() 一次註冊完。
    nc_plan_reset();
    nc_task_reset();

    s_decoder = nc4_decoder_create();
    if (s_decoder != NULL) {
        actions_register(s_decoder);       // 核心執行能力先就位（不需要任何任務）
    } else {
        ESP_LOGE(TAG, "解碼器建立失敗 → 裝置收不到任何指令");
    }

    nc_task_register(&s_task_nc4);     // L0：parser 緩衝 + decode0/1
    nc_task_register(&s_task_net);     // L1：UART / TCP 緩衝（照 ETH 是否起來）
    nc_task_register(&s_task_core);    // L1：慢工作 hub + worker
    nc_task_register(&s_task_probe);   // L3：開機自測
    nc_task_register(&s_task_fx);      // L4：效果 arena + render task
    nc_task_register(nc_hello_task());  // L6：最小框架自測（每秒 Hello World）

    nc_task_declare_all();       // → nc_plan_declare + 帳本檢查

    // ── ② 探測可用量，一次 commit（每 class 一次 carve）──
    size_t avail[NC_PLAN_CLASSES] = {0};
    avail[NC_PLAN_DMA] = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    avail[NC_PLAN_INTERNAL] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    avail[NC_PLAN_PSRAM] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    esp_err_t perr = nc_plan_commit(avail, false);
    {
        // 開機一次用：唔應該為咗印一行 log 長期佔住 700 B SRAM。
        char *pj = malloc(700);
        if (pj != NULL) {
            nc_plan_json(pj, 700);
            ESP_LOGI(TAG, "plan(%s): %s", esp_err_to_name(perr), pj);
            free(pj);
        }
    }

    // ── Wi-Fi：兩道閘（編譯期 CONFIG_NETCORE_WIFI ＋ network.json wifi.enable）──
    // 放在 plan commit 之後：核心通道先要記憶體，Wi-Fi 係可選項。
    // 未啟用時 nc_wifi_boot() 只回報狀態，一個 byte 都唔配。
    nc_wifi_boot();

    // ── ③ 任務啟動：資源 → on_start → 指令 → 準備 loop（還不開）──
    nc_task_start_all();

    // ④ 效果租戶：載入 .fxe → 解碼 manifest → attach 到 registry
    //    （放在 start_all 之後：attach 會插入 registry，不能跟 start_all 同時走）
    if (nc_task_started("fx")) {
        nc_fx_scan("/data/pixel/effects");   // 效果類（kind = effect）
        nc_fx_scan("/data/tasks");           // 任務類（kind = normal）
        nc_task_sync_cfg();                  // 掃到的 .fxe 也補進 tasks.json
    }

    {
        char *ledger = malloc(900);
        if (ledger != NULL) {
            nc_task_ledger(ledger, 900);
            ESP_LOGI(TAG, "帳本：\n%s", ledger);
            free(ledger);
        }
    }

#if CONFIG_IDF_TARGET_ESP32P4
    // 診斷結論（2026-11-02）：這塊板子的 USB-C 只接 USB-Serial-JTAG，沒有接 USB-OTG。
    // 啟用 TinyUSB 後裝置照跑、但主機端完全沒有新裝置（列舉不到），
    // 而且 tinyusb_console_init() 會把 console 搬走 → 連 log 都看不到。
    err = ESP_ERR_NOT_SUPPORTED;
#else
    err = usb_init();
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB 初始化失敗: %s", esp_err_to_name(err));
    }

#if CONFIG_IDF_TARGET_ESP32P4
    // P4 這版沒有 USB（TinyUSB 沒列舉成功）→ 不要把 FAT 交給 USB，
    // 否則裝置自己與網路都讀不到 /data（就沒有替代的檔案存取方式了）。
    err = ESP_ERR_NOT_SUPPORTED;
    ESP_LOGW(TAG, "P4 無 USB → /data 留在 APP，改用 http://<ip>:8080/config.json");
#else
    err = storage_hand_over_to_usb();
    ESP_LOGI(TAG, "碟交給 host: %s", esp_err_to_name(err));
#endif

    // 解碼器已在 ⑤ 宣告期建立；核心指令由 cmds 模組註冊（帶擁有權）。
    // 這裡只掛「需要硬體」的通道。
    if (s_decoder != NULL) {
#if CONFIG_IDF_TARGET_ESP32P4
        if (s_eth_ok) {
            // NC4 over WebSocket（訊息即一幀；直接解析，不經 bus）
            esp_err_t werr = nc4_ws_start(s_decoder, 8080);
            ESP_LOGW(TAG, "NC4 over WS: %s", esp_err_to_name(werr));
            // UDP 可以喺 config 關掉；WS 暫時唔提供關閉（同一個 httpd 亦係 HTTP 設定頁）
            esp_err_t uerr = ESP_ERR_NOT_SUPPORTED;
            if (link_enabled("UDP")) {
                uerr = nc4_udp_start(s_decoder, 6001);
            } else {
                ESP_LOGW(TAG, "UDP：links[].enable=0 → 唔建立（0 B）");
            }
            ESP_LOGW(TAG, "NC4 over UDP: %s", esp_err_to_name(uerr));
            nc4_http_register(nc4_ws_httpd());
        } else {
            ESP_LOGW(TAG, "沒有乙太網 → WS / UDP 通道不啟動");
        }
#endif
    }

    if (s_decoder != NULL) {
#if CONFIG_IDF_TARGET_ESP32P4
        // P4 冇 USB OTG：連 initializer 都唔要有 ——
        // 一旦喺 initializer 取 cdc_read/cdc_send 的址，linker 就會拉入 tinyusb（21.7 KB）。
        esp_err_t berr = ESP_ERR_NOT_SUPPORTED;
#else
        esp_err_t berr = ESP_ERR_NOT_SUPPORTED;
        if (link_enabled("CDC")) {
            const nc4_bus_cfg_t cdc_bus = {
                .name = "CDC",
                .mode = NC4_BUS_PULL,
                .core = nc4_router_core_or("CDC", 0),
                .read = cdc_read,
                .send = cdc_send,
                .rx_region = s_nc4_rx_region,     // plan 給的緩衝；NULL = 自己配
                .rx_region_bytes = s_nc4_rx_bytes,
                .read_chunk = 1024,   // 一次多讀一點，配合加大的 TX/RX ring
                // 其餘留 0 = 用框架預設；實際值之後從 config.json 餵
            };
            berr = nc4_bus_add(s_decoder, &cdc_bus, NULL);
        } else {
            ESP_LOGW(TAG, "CDC：links[].enable=0 → 唔建立（連緩衝都冇要）");
        }
#endif

        if (s_uart_rx_region != NULL) {
            const nc4_bus_cfg_t uart_bus = {
                .name = "UART",
                .mode = NC4_BUS_PULL,
                .core = nc4_router_core_or("UART", 1),
                .read = uart_bus_read,
                .rx_region = s_uart_rx_region,
                .rx_region_bytes = 1024,
                .send = NULL,
                .read_chunk = 512,
                .budget_per_pass = 8,
            };
            esp_err_t uerr = nc4_bus_add(s_decoder, &uart_bus, NULL);
            ESP_LOGI(TAG, "UART bus: %s", esp_err_to_name(uerr));
        } else {
            ESP_LOGW(TAG, "UART：未啟用（links[].enable=0）或冇緩衝 → 唔建立");
        }

#if CONFIG_IDF_TARGET_ESP32P4
        // TCP 傳輸只在 P4（乙太網）且「ETH 真的起來」時建立
        if (s_eth_ok && s_tcp_rx_region != NULL) {
            const nc4_bus_cfg_t tcp_bus = {
                .name = "TCP",
                .mode = s_rx_split ? NC4_BUS_PUSH : NC4_BUS_PULL,
                .core = nc4_router_core_or("TCP", 0),
                .read = tcp_nc4_read,          // 只在 PULL 模式使用
                .rx_ring_size = 8205,      // 每格剛好一幀
                .rx_slots = 8,
                .send = tcp_nc4_send,
                .rx_region = s_tcp_rx_region,
                .rx_region_bytes = 16410,
                .read_chunk = 8192,
                .budget_per_pass = 8,
                .frame_aligned = s_frame_align ? 1 : 0,
            };
            nc4_bus_handle_t tcp_handle = (nc4_bus_handle_t)0;
            esp_err_t terr = nc4_bus_add(s_decoder, &tcp_bus, &tcp_handle);
            ESP_LOGI(TAG, "NC4/TCP bus: %s", esp_err_to_name(terr));
            s_tcp_nc4_bus = tcp_handle;
            if (terr == ESP_OK && s_rx_split) {
                xTaskCreatePinnedToCore(tcp_nc4_rx_task, "tcp_nc4_rx", 4096,
                                        (void *)(intptr_t)tcp_handle, 10, NULL, (BaseType_t)s_rx_core);
            }
            ESP_LOGW(TAG, "NC4/TCP 收斂模式: %s（RX core=%u）%s",
                     s_rx_split ? "分核(PUSH)" : "同核(PULL)", (unsigned)s_rx_core,
                     (s_rx_split && s_frame_align) ? "，零複製(frame-aligned)" : "");
        }
#endif
        ESP_LOGI(TAG, "NC4 通道: %s", esp_err_to_name(berr));

        // 任務已經由各模組在 start 時登記（decode0/1、slow_worker、fx）；
        // 這裡只負責「依 layer 啟動」，不再手動 xTaskCreate 任何 loop。
        nc_task_boot();
    }

    // ── 開機帳：所有初始化／任務都起完之後，實際仲剩幾多 ──
    // 兩個數字要分開睇：
    //   free     = 現在仲可以 malloc 幾多（會隨任務／連線變動）
    //   largest  = 最大連續塊（決定「仲配唔配到一個大緩衝」）
    {
        const size_t in_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t in_big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t ps_big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGW(TAG, "開機剩餘：內部 %u B（最大連續 %u）｜PSRAM %u B（最大連續 %u）",
                 (unsigned)in_free, (unsigned)in_big, (unsigned)ps_free, (unsigned)ps_big);
    }

    const TickType_t period = pdMS_TO_TICKS(1000);
    TickType_t last_wake = xTaskGetTickCount();
    int64_t prev_us = esp_timer_get_time();

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        int64_t now_us = esp_timer_get_time();
        int64_t dt_us = now_us - prev_us;
        prev_us = now_us;

        ESP_LOGI(TAG, "t=%" PRId64 " us  dt=%" PRId64 " us  heap=%" PRIu32,
                 now_us, dt_us,
                 (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
