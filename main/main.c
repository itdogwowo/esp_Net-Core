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
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"

#include "ff.h"
#include "nc4.h"
#include "nc4_bus.h"
#include "nc_task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "nc_mem.h"
#include "nc_plan.h"
#include "nc_hub.h"
#include "nc_service.h"
#include "ota_cmds.h"
#include "ws_nc4.h"
#include "udp_nc4.h"
#include "http_cfg.h"

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
#define PRESET_CONFIG_PATH MSC_MOUNT_POINT "/config.json"
#define VOLUME_LABEL       "NETCORE"

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

// ports/<board>/data/config.json 由 EMBED_FILES 嵌進 firmware
extern const uint8_t preset_config_start[] asm("_binary_config_json_start");
extern const uint8_t preset_config_end[]   asm("_binary_config_json_end");

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc = NULL;

// ── MSC 事件 ──
static void msc_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "[msc] mount complete (→%s)",
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "[msc] mount failed");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGW(TAG, "[msc] 需要格式化");
        break;
    default:
        break;
    }
}

static const esp_partition_t *find_data_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_FAT, "data");
}

// ── 出廠預置：缺檔才寫，不覆蓋使用者改過的內容 ──
static void seed_preset_if_missing(void)
{
    struct stat st;
    if (stat(PRESET_CONFIG_PATH, &st) == 0) {
        ESP_LOGI(TAG, "預置已存在（%d B），不動它", (int)st.st_size);
        return;
    }

    size_t len = (size_t)(preset_config_end - preset_config_start);
    FILE *f = fopen(PRESET_CONFIG_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "寫入預置失敗：開不了 %s", PRESET_CONFIG_PATH);
        return;
    }
    size_t written = fwrite(preset_config_start, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "已寫入出廠預置 %s（%u B）", PRESET_CONFIG_PATH, (unsigned)written);
}

// ── 卷標：預設叫 NETCORE，不是 NO NAME ──
static void ensure_volume_label(void)
{
#if FF_USE_LABEL
    char label[24] = {0};
    DWORD sn = 0;
    if (f_getlabel("0:", label, &sn) != FR_OK) {
        ESP_LOGW(TAG, "讀不到卷標");
        return;
    }
    if (strcmp(label, VOLUME_LABEL) == 0) {
        return;
    }
    if (f_setlabel("0:" VOLUME_LABEL) == FR_OK) {
        ESP_LOGI(TAG, "卷標: \"%s\" → \"%s\"", label, VOLUME_LABEL);
    } else {
        ESP_LOGW(TAG, "設定卷標失敗（原 \"%s\"）", label);
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

    ESP_RETURN_ON_ERROR(wl_mount(part, &s_wl), TAG, "wl_mount 失敗");

    tinyusb_msc_driver_config_t drv_cfg = { .callback = msc_event_cb };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&drv_cfg), TAG, "MSC driver 安裝失敗");

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.wl_handle = s_wl,
        // 先讓 device 自己擁有，才有機會寫預置；寫完再交給 host
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = NULL,      // 預設 /data
            .config = {
                .format_if_mount_failed = true,   // ★ 不需要人手動格式化
                .max_files = 5,
            },
            .do_not_format = false,
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_msc),
                        TAG, "MSC storage 建立失敗");

    seed_preset_if_missing();
    ensure_volume_label();
    return ESP_OK;
}

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
    if (s_msc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return tinyusb_msc_set_storage_mount_point(s_msc, TINYUSB_MSC_STORAGE_MOUNT_USB);
}

static esp_err_t cdc_send(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, frame, len) != len) {
        return ESP_FAIL;
    }
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

// str_u16len 編碼（與 schema 的 str_u16len 一致：2B 長度前綴 + UTF-8）
static void reply_json(nc4_ctx_t *ctx, uint16_t cmd, const char *json)
{
    if (ctx->send == NULL) {
        return;
    }
    static uint8_t payload[2 + 2100];
    size_t n = strlen(json);
    if (n > 2100) {
        n = 2100;
    }
    payload[0] = (uint8_t)n;
    payload[1] = (uint8_t)(n >> 8);
    memcpy(payload + 2, json, n);

    uint8_t frame[NC4_HDR_LEN + sizeof(payload) + NC4_CRC_LEN];
    int total = nc4_pack_into(frame, sizeof(frame), cmd, nc4_reply_addr(ctx), payload, 2 + n);
    if (total > 0) {
        ctx->send(ctx->send_arg, frame, (size_t)total);
    }
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

static void status_put_sys(char *buf, size_t cap)
{
    snprintf(buf, cap,
             "\"sys\":{\"fw\":\"%s\",\"heap\":%u,\"up_s\":%lld,\"idf\":\"%s\",\"cid\":%u}",
             FW_VERSION,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (long long)(esp_timer_get_time() / 1000000),
             esp_get_idf_version(), (unsigned)s_cid);
}

static void status_put_mem(char *buf, size_t cap)
{
    static char plan[700];
    static char hubs[320];
    nc_plan_json(plan, sizeof(plan));
    nc_hub_registry_json(hubs, sizeof(hubs));
    snprintf(buf, cap, "\"mem\":{\"plan\":%s,\"hub\":%s}", plan, hubs);
}

static void status_put_svc(char *buf, size_t cap)
{
    static char svc[700];
    nc_service_json(svc, sizeof(svc));
    snprintf(buf, cap, "\"svc\":%s", svc);
}

#if CONFIG_IDF_TARGET_ESP32P4
static void status_put_net(char *buf, size_t cap)
{
    static char buses[640];
    static char ws[200];
    static char udp[200];
    nc4_bus_stats_json(buses, sizeof(buses));
    ws_nc4_json(ws, sizeof(ws));
    udp_nc4_json(udp, sizeof(udp));
    snprintf(buf, cap, "\"net\":{\"bus\":%s,\"ws\":%s,\"udp\":%s}", buses, ws, udp);
}
#endif

static void status_put_task(char *buf, size_t cap)
{
    static char tasks[400];
    nc_tasks_status_json(tasks, sizeof(tasks));
    snprintf(buf, cap, "\"task\":%s", tasks);
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
        reply_json(ctx, NC4_CMD_STATUS_RSP, "{\"keys\":[\"sys\",\"mem\",\"task\"]}");
        return;
    }

    static char json[1900];
    size_t off = 0;
    off += snprintf(json + off, sizeof(json) - off, "{");
    bool first = true;
    static char part[1300];
    if (keys_want(keys, "sys")) {
        status_put_sys(part, sizeof(part));
        off += snprintf(json + off, sizeof(json) - off, "%s", part);
        first = false;
    }
    if (keys_want(keys, "mem")) {
        status_put_mem(part, sizeof(part));
        off += snprintf(json + off, sizeof(json) - off, "%s%s", first ? "" : ",", part);
        first = false;
    }
    if (keys_want(keys, "task")) {
        status_put_task(part, sizeof(part));
        off += snprintf(json + off, sizeof(json) - off, "%s%s", first ? "" : ",", part);
        first = false;
    }
    if (keys_want(keys, "svc")) {
        status_put_svc(part, sizeof(part));
        off += snprintf(json + off, sizeof(json) - off, "%s%s", first ? "" : ",", part);
        first = false;
    }
#if CONFIG_IDF_TARGET_ESP32P4
    if (keys_want(keys, "net")) {
        status_put_net(part, sizeof(part));
        off += snprintf(json + off, sizeof(json) - off, "%s%s", first ? "" : ",", part);
        first = false;
    }
#endif
    if (off < sizeof(json) - 1) {
        snprintf(json + off, sizeof(json) - off, "}");
    }
    reply_json(ctx, NC4_CMD_STATUS_RSP, json);
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
    static char st[640];
    nc4_bus_stats_json(st, sizeof(st));
    char json[720];
    snprintf(json, sizeof(json),
             "{\"bus\":%s,\"resync\":%u,\"rx\":{\"f\":%u,\"hub_full\":%u,"
             "\"sel\":%u,\"sel_to\":%u,\"us\":%llu}}",
             st, (unsigned)s_rx_resync, (unsigned)s_rx_frames, (unsigned)s_rx_hub_full,
             (unsigned)s_rx_sel, (unsigned)s_rx_sel_to,
             (unsigned long long)s_rx_fill_us);
    reply_json(ctx, NC4_CMD_BUS_RSP, json);
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

static void tx_bench_packer(void *arg)
{
    tx_bench_t *b = arg;
    static uint8_t payload[NC4_MAX_PAYLOAD];   // 靜態：任務堆疊只有 4 KB
    memset(payload, 0xA5, sizeof(payload));
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
    nc_tasks_status_json(json, sizeof(json));
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

    nvs_handle_t h = 0;
    uint16_t cid = NC4_ADDR_BROADCAST, master = NC4_ADDR_BROADCAST;
    if (nvs_open("node", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_u16(h, "cid", &cid) != ESP_OK) {
            cid = derive_cid_from_mac();
            nvs_set_u16(h, "cid", cid);
        }
        if (nvs_get_u16(h, "master_cid", &master) != ESP_OK) {
            master = NC4_ADDR_BROADCAST;
            nvs_set_u16(h, "master_cid", master);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    s_cid = cid;
    if (s_decoder != NULL) {
        nc4_decoder_set_cid(s_decoder, cid);
        nc4_decoder_set_master_cid(s_decoder, master);
    }
    ESP_LOGI(TAG, "cid=0x%04X  master_cid=0x%04X", cid, master);
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

static int cdc_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    size_t n = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, cap, &n) != ESP_OK) {
        return -1;
    }
    return (int)n;
}

// ── 服務：自己宣告、自己啟動（透過 nc_service 管理器）──
// 收/解是否分核（NVS: net/rx_split, net/rx_core；預設 0 / 1）
static bool s_eth_ok;              // 硬體/驅動真的起來了才宣告網路資源
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
static void *s_tcp_rx_region;
static nc_hub_t *s_probe_hub;

// ── nc4：CDC 那條線的 parser 緩衝（P4 目前沒開 USB，先留著）──
static int svc_nc4_declare(nc_res_req_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_res_req_t){
        .name = "nc4.rx", .bytes = NC4_MAX_FRAME, .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
}

static esp_err_t svc_nc4_start(const nc_res_view_t *res, int n)
{
    (void)n;
    s_nc4_rx_region = res[0].ptr;
    s_nc4_rx_bytes = res[0].len;
    ESP_LOGI(TAG, "svc nc4: parser 緩衝 ok");
    return ESP_OK;
}

// ── net：UART / TCP 的 parser 緩衝（TCP 只在 P4）──
static int svc_net_declare(nc_res_req_t *out, int max)
{
    int n = 0;
    if (n < max) {
        out[n++] = (nc_res_req_t){
            .name = "uart.rx", .bytes = 1024, .cls = NC_PLAN_INTERNAL, .align = 32,
        };
    }
#if CONFIG_IDF_TARGET_ESP32P4
    // ★ 只有「ETH 真的起來」才宣告 TCP 的緩衝 —— 宣告是照硬體決定，不是照 config
    if (s_eth_ok && n < max) {
        out[n++] = (nc_res_req_t){
            // 2 幀：跨讀時需要餘裕
            .name = "tcp.rx", .bytes = 16410, .cls = NC_PLAN_INTERNAL, .align = 32,
        };
    }
#endif
    return n;
}

static esp_err_t svc_net_start(const nc_res_view_t *res, int n)
{
    s_uart_rx_region = res[0].ptr;
#if CONFIG_IDF_TARGET_ESP32P4
    if (n > 1) {
        s_tcp_rx_region = res[1].ptr;
    }
#endif
    ESP_LOGI(TAG, "svc net: UART/TCP parser 緩衝 ok");
    return ESP_OK;
}

// ── core.jobs：慢工作佇列 ──
static int svc_jobs_declare(nc_res_req_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_res_req_t){
        .name = "jobs", .bytes = 8 * sizeof(slow_job_t), .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
}

static esp_err_t svc_jobs_start(const nc_res_view_t *res, int n)
{
    (void)n;
    s_job_hub = nc_hub_create_in(res[0].ptr, res[0].len, "jobs",
                                 sizeof(slow_job_t), 8, 32);
    return s_job_hub != NULL ? ESP_OK : ESP_FAIL;
}

// ── probe：開機自測（hub 交換 4 格）──
static int svc_probe_declare(nc_res_req_t *out, int max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (nc_res_req_t){
        .name = "probe.hub", .bytes = 4 * 128, .cls = NC_PLAN_INTERNAL, .align = 32,
    };
    return 1;
}

static esp_err_t svc_probe_start(const nc_res_view_t *res, int n)
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

// ── System（固定順序）：硬體/driver/NC4 通道屬於這裡，不進服務管理器 ──
// 服務管理器只管「應用服務」（效果、UI、自測…）。
typedef struct {
    const char *name;
    int level;
    int (*declare)(nc_res_req_t *out, int max);
    esp_err_t (*start)(const nc_res_view_t *res, int n);
} sys_part_t;

static const sys_part_t s_sys_parts[] = {
    { "nc4",  0, svc_nc4_declare,  svc_nc4_start  },
    { "net",  1, svc_net_declare,  svc_net_start  },
    { "core", 1, svc_jobs_declare, svc_jobs_start },
};
#define SYS_PART_COUNT (sizeof(s_sys_parts) / sizeof(s_sys_parts[0]))

static nc_res_req_t s_sys_req[SYS_PART_COUNT][NC_SERVICE_MAX_RES];
static int s_sys_nreq[SYS_PART_COUNT];

static void sys_declare_all(void)
{
    for (size_t i = 0; i < SYS_PART_COUNT; i++) {
        const int n = s_sys_parts[i].declare(s_sys_req[i], NC_SERVICE_MAX_RES);
        s_sys_nreq[i] = n < 0 ? 0 : (n > NC_SERVICE_MAX_RES ? NC_SERVICE_MAX_RES : n);
        for (int k = 0; k < s_sys_nreq[i]; k++) {
            const nc_res_req_t *r = &s_sys_req[i][k];
            nc_plan_declare(&(nc_plan_decl_t){
                .service = s_sys_parts[i].name, .name = r->name,
                .level = s_sys_parts[i].level, .cls = r->cls,
                .bytes = r->bytes, .align = r->align,
            });
        }
    }
}

static void sys_start_all(void)
{
    for (size_t i = 0; i < SYS_PART_COUNT; i++) {
        nc_res_view_t view[NC_SERVICE_MAX_RES];
        bool ok = true;
        for (int k = 0; k < s_sys_nreq[i]; k++) {
            view[k].ptr = nc_plan_get(s_sys_req[i][k].name);
            view[k].len = s_sys_req[i][k].bytes;
            if (view[k].ptr == NULL) {
                ESP_LOGE(TAG, "[sys %s] 資源 %s 拿不到", s_sys_parts[i].name,
                         s_sys_req[i][k].name);
                ok = false;
                break;
            }
        }
        if (ok) {
            s_sys_parts[i].start(view, s_sys_nreq[i]);
        }
    }
}

// 應用服務：probe（示範用；真正的應用會是 effects / UI / jobs …）
static const nc_service_t s_probe_service = {
    .name = "probe", .level = 3,
    .declare = svc_probe_declare, .start = svc_probe_start, .stop = NULL,
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
    static uint8_t txbuf[4096];   // 對齊 lwIP TCP_SND_BUF，避免 send 阻塞
    memset(txbuf, 0x5A, sizeof(txbuf));

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
            if (want > sizeof(txbuf)) {
                want = sizeof(txbuf);
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

    // ── /data 掛載（預置、卷標；不讀應用設定）──
    esp_err_t err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage 初始化失敗: %s", esp_err_to_name(err));
    }

    nvs_boot_init();     // ★ 一定要比任何 nvs_open 早
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

    // ── ⑤ 宣告（照「實際存在的硬體」決定）──
    nc_plan_reset();
    nc_service_add(&s_probe_service);
    nc_service_declare_all();       // 應用服務宣告
    sys_declare_all();              // System 宣告（硬體/driver/NC4 通道的緩衝）

    // ── ② 探測可用量，一次 commit（每 class 一次 carve）──
    size_t avail[NC_PLAN_CLASSES] = {0};
    avail[NC_PLAN_DMA] = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    avail[NC_PLAN_INTERNAL] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    avail[NC_PLAN_PSRAM] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    esp_err_t perr = nc_plan_commit(avail, false);
    {
        static char pj[700];
        nc_plan_json(pj, sizeof(pj));
        ESP_LOGI(TAG, "plan(%s): %s", esp_err_to_name(perr), pj);
    }


    // ── ③ System 啟動（NC4 通道）→ 應用服務啟動 ──
    sys_start_all();
    nc_service_start_all();

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

    s_decoder = nc4_decoder_create();
    if (s_decoder != NULL) {
        nc4_decoder_on(s_decoder, NC4_CMD_REBOOT, on_reboot);
        nc4_decoder_on(s_decoder, NC4_CMD_STATUS_GET, on_status_get);
        nc4_decoder_on(s_decoder, NC4_CMD_DEV_BENCH, on_dev_bench);
        nc4_decoder_on(s_decoder, NC4_CMD_HUB_BENCH, on_hub_bench);
        nc4_decoder_on(s_decoder, NC4_CMD_TX_BENCH, on_tx_bench);
        nc4_decoder_on(s_decoder, NC4_CMD_BUS_STATS, on_bus_stats);
        nc4_decoder_on(s_decoder, NC4_CMD_DEV_SLOW, on_dev_slow);
        nc4_decoder_on(s_decoder, NC4_CMD_NET_TUNE, on_net_tune);
        nc4_decoder_on(s_decoder, NC4_CMD_UART_BENCH, on_uart_bench);
        nc4_decoder_on(s_decoder, NC4_CMD_BENCH_READY, on_bench_ready);
        nc4_decoder_on(s_decoder, NC4_CMD_BENCH_DATA, on_bench_data);
        nc4_decoder_on(s_decoder, NC4_CMD_BENCH_RESULT, on_bench_result);
        nc4_decoder_on(s_decoder, NC4_CMD_SET_MASTER, on_set_master);
        nc4_decoder_on(s_decoder, NC4_CMD_SYS_TASK_QUERY, on_sys_task_query);
        node_identity_init();
        ota_register(s_decoder);
#if CONFIG_IDF_TARGET_ESP32P4
        if (s_eth_ok) {
            // NC4 over WebSocket（訊息即一幀；直接解析，不經 bus）
            esp_err_t werr = ws_nc4_start(s_decoder, 8080);
            ESP_LOGW(TAG, "NC4 over WS: %s", esp_err_to_name(werr));
            esp_err_t uerr = udp_nc4_start(s_decoder, 6001);
            ESP_LOGW(TAG, "NC4 over UDP: %s", esp_err_to_name(uerr));
            http_cfg_register(ws_nc4_httpd());
        } else {
            ESP_LOGW(TAG, "沒有乙太網 → WS / UDP 通道不啟動");
        }
#endif
    }

    if (s_decoder != NULL) {
        const nc4_bus_cfg_t cdc_bus = {
            .name = "CDC",
            .mode = NC4_BUS_PULL,
            .core = 0,
            .read = cdc_read,
            .send = cdc_send,
            .rx_region = s_nc4_rx_region,     // plan 給的緩衝；NULL = 自己配
            .rx_region_bytes = s_nc4_rx_bytes,
            .read_chunk = 1024,   // 一次多讀一點，配合加大的 TX/RX ring
            // 其餘留 0 = 用框架預設；實際值之後從 config.json 餵
        };
#if CONFIG_IDF_TARGET_ESP32P4
        esp_err_t berr = ESP_ERR_NOT_SUPPORTED;   // P4 這版還沒啟 USB
        (void)cdc_bus;
#else
        esp_err_t berr = nc4_bus_add(s_decoder, &cdc_bus, NULL);
#endif

        if (s_uart_rx_region != NULL) {
            const nc4_bus_cfg_t uart_bus = {
                .name = "UART",
                .mode = NC4_BUS_PULL,
                .core = 1,
                .read = uart_bus_read,
                .rx_region = s_uart_rx_region,
                .rx_region_bytes = 1024,
                .send = NULL,
                .read_chunk = 512,
                .budget_per_pass = 8,
            };
            esp_err_t uerr = nc4_bus_add(s_decoder, &uart_bus, NULL);
            ESP_LOGI(TAG, "UART bus: %s", esp_err_to_name(uerr));
        }

#if CONFIG_IDF_TARGET_ESP32P4
        // TCP 傳輸只在 P4（乙太網）且「ETH 真的起來」時建立
        if (s_eth_ok && s_tcp_rx_region != NULL) {
            const nc4_bus_cfg_t tcp_bus = {
                .name = "TCP",
                .mode = s_rx_split ? NC4_BUS_PUSH : NC4_BUS_PULL,
                .core = 0,
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

        // 任務註冊表：L1 起解碼（core 0），L2 起按鍵監看
        // 6144：reply_json 的 frame 就吃掉 2.1 KB，4096 對「大回覆」太緊
        nc_task_register(&(nc_task_def_t){
            .name = "decode0", .fn = nc4_bus_task_entry, .arg = (void *)0,
            .stack = 6144, .prio = 4, .core = 0, .layer = 1,
        });
        nc_task_register(&(nc_task_def_t){
            .name = "decode1", .fn = nc4_bus_task_entry, .arg = (void *)1,
            .stack = 6144, .prio = 4, .core = 1, .layer = 1,
        });
        if (s_job_hub != NULL) {
            nc_task_register(&(nc_task_def_t){
                .name = "slow_worker", .fn = slow_worker, .arg = NULL,
                .stack = 4096, .prio = 2, .core = 0, .layer = 2,
            });
        }
        nc_tasks_boot();
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
