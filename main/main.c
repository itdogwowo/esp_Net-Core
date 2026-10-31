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
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
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
#include "nc_mem_cfg.h"
#include "ota_cmds.h"

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "netcore";

#define BOOT_BUTTON_GPIO   GPIO_NUM_0
#define MSC_MOUNT_POINT    "/data"
#define PRESET_CONFIG_PATH MSC_MOUNT_POINT "/config.json"
#define VOLUME_LABEL       "NETCORE"
#define FW_VERSION         "0.1.0"

// 解碼效能測試（0x18xx = bench 域，與 MP 版同名域）
#define NC4_CMD_BENCH_REQ  0x1801
#define NC4_CMD_BENCH_RSP  0x1802

// sys 域（sys.json）
#define NC4_CMD_SET_MASTER 0x1016
#define NC4_CMD_SYS_TASK_QUERY 0x1005
#define NC4_CMD_SYS_TASK_RSP   0x1006

// ports/<board>/data/config.json 由 EMBED_FILES 嵌進 firmware
extern const uint8_t preset_config_start[] asm("_binary_config_json_start");
extern const uint8_t preset_config_end[]   asm("_binary_config_json_end");

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc = NULL;

// ── 軟體進入 download mode ──
static void enter_download_mode(const char *why)
{
    ESP_LOGW(TAG, "進入 download mode（%s）", why);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 兩層都要：digital hold 對「CPU reset」不夠，RTC hold 才會跨 reset 保持電位
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BOOT_BUTTON_GPIO, 0);
    gpio_hold_en(BOOT_BUTTON_GPIO);

    rtc_gpio_init(BOOT_BUTTON_GPIO);
    rtc_gpio_set_direction(BOOT_BUTTON_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(BOOT_BUTTON_GPIO, 0);
    rtc_gpio_hold_en(BOOT_BUTTON_GPIO);

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

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

// ── BOOT 長按 3 秒 → download mode ──
static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    for (;;) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= 3000) {
                enter_download_mode("BOOT 長按");
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── 回覆路徑：把打包好的幀寫回 CDC ──
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
    uint8_t payload[2 + 256];
    size_t n = strlen(json);
    if (n > 256) {
        n = 256;
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

static void on_download(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    ESP_LOGW(TAG, "[%s] NC4 RESET_TO_DOWNLOAD (0x1019)", ctx->source);
    enter_download_mode("NC4 0x1019");
}

static void on_status_get(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char json[192];
    snprintf(json, sizeof(json),
             "{\"fw\":\"%s\",\"heap\":%u,\"up_s\":%lld,\"idf\":\"%s\"}",
             FW_VERSION,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (long long)(esp_timer_get_time() / 1000000),
             esp_get_idf_version());
    reply_json(ctx, NC4_CMD_STATUS_RSP, json);
}

// 解碼 benchmark：在板子上量「餵 N 個 size-byte 幀要多久」
// payload: size(u16) iters(u16)；回 0x1802 JSON
static void on_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    uint16_t size = 8192, iters = 100;
    uint8_t mode = 0;   // 0 = decode, 1 = pack
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
    if (frame == NULL || payload == NULL || dec == NULL) {
        free(frame);
        free(payload);
        nc4_decoder_destroy(dec);
        reply_json(ctx, NC4_CMD_BENCH_RSP, "{\"err\":\"oom\"}");
        return;
    }

    memset(payload, 0xA5, size);
    // 0x7FFE 故意不註冊 handler → 只量 framing + CRC
    int packed = nc4_pack_into(frame, total, 0x7FFE, NC4_ADDR_BROADCAST, payload, size);
    if (packed <= 0) {
        free(payload);
        free(frame);
        nc4_decoder_destroy(dec);
        return;
    }

    nc4_parser_t *parser = nc4_parser_create(dec, NULL, NULL, "BENCH", NC4_MAX_FRAME);
    if (parser == NULL) {
        free(frame);
        nc4_decoder_destroy(dec);
        reply_json(ctx, NC4_CMD_BENCH_RSP, "{\"err\":\"parser\"}");
        return;
    }

    int64_t t0;
    int64_t dt;

    if (mode == 1) {
        // pack：只量組包（CRC + 兩次複製），不經解碼
        nc4_pack_into(frame, total, 0x7FFE, NC4_ADDR_BROADCAST, payload, size);  // 暖機
        t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < iters; i++) {
            nc4_pack_into(frame, total, 0x7FFE, NC4_ADDR_BROADCAST, payload, size);
        }
    } else {
        nc4_parser_feed(parser, frame, total);   // 暖機
        t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < iters; i++) {
            nc4_parser_feed(parser, frame, total);
        }
    }
    dt = esp_timer_get_time() - t0;

    double us_per_frame = (double)dt / (double)iters;
    double mbps = (dt > 0) ? ((double)total * (double)iters / (double)dt) : 0.0;

    char json[192];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"size\":%u,\"iters\":%u,"
             "\"us_per_frame\":%.1f,\"mb_s\":%.2f}",
             mode == 1 ? "pack" : "decode",
             (unsigned)size, (unsigned)iters, us_per_frame, mbps);
    reply_json(ctx, NC4_CMD_BENCH_RSP, json);

    nc4_parser_destroy(parser);
    nc4_decoder_destroy(dec);
    free(payload);
    free(frame);
}

static void on_sys_task_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    char json[768];
    nc_tasks_status_json(json, sizeof(json));
    reply_json(ctx, NC4_CMD_SYS_TASK_RSP, json);
}

static nc4_decoder_t *s_decoder = NULL;

// ── 身份：cid（自己的 2B 協議位址）+ master_cid（上級）──
static uint16_t s_cid = NC4_ADDR_BROADCAST;

static uint16_t derive_cid_from_mac(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return NC4_ADDR_BROADCAST;
    }
    return (uint16_t)((mac[4] << 8) | mac[5]);
}

static void node_identity_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失敗: %s", esp_err_to_name(err));
    }

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
static int cdc_read(void *arg, uint8_t *buf, size_t cap)
{
    (void)arg;
    size_t n = 0;
    if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, cap, &n) != ESP_OK) {
        return -1;
    }
    return (int)n;
}

// 讀一整份文字檔（config.json）；失敗回 NULL
static char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len <= 0 || len > 16 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

void app_main(void)
{
    // 清掉上個 cycle 留下的 hold（否則板子會一直卡在 download mode）
    rtc_gpio_hold_dis(BOOT_BUTTON_GPIO);
    rtc_gpio_deinit(BOOT_BUTTON_GPIO);
    gpio_hold_dis(BOOT_BUTTON_GPIO);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "esp_Net-Core boot — ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "chip: %s rev %d, %d core(s)",
             CONFIG_IDF_TARGET, chip.revision, chip.cores);

    // ── /data 先掛起來：配置要從那裡讀 ──
    esp_err_t err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage 初始化失敗: %s", esp_err_to_name(err));
    }

    // ── 緩衝區：預設值寫在程式裡，config.json 決定這次用多少 ──
    nc_mem_limits_t lim;
    nc_mem_limits_default(&lim);
    if (err == ESP_OK) {
        char *cfg = read_text_file(PRESET_CONFIG_PATH);
        if (cfg != NULL) {
            esp_err_t cerr = nc_mem_limits_from_json(&lim, cfg);
            if (cerr != ESP_OK) {
                ESP_LOGW(TAG, "config 沒有 Buffer 段（%s），用預設", esp_err_to_name(cerr));
            }
            free(cfg);
        }
    }
    nc_mem_limits_log(&lim);

    nc_mem_pool_t *frame_pool = nc_mem_pool_create("frame", NC_MEM_DMA,
                                                   lim.frame_block, lim.frame_count);
    nc_mem_arena_t *asset_arena = nc_mem_arena_create("assets", NC_MEM_PSRAM,
                                                       lim.asset_bytes);
    if (frame_pool == NULL) {
        ESP_LOGE(TAG, "frame pool 建立失敗");
    }
    if (asset_arena == NULL) {
        ESP_LOGW(TAG, "asset arena 建立失敗（PSRAM 不可用？）");
    }

    // 煙霧測試：取一塊、還回去，確認 free list 正常
    void *probe = nc_mem_pool_get(frame_pool, 0);
    if (probe != NULL) {
        nc_mem_pool_put(frame_pool, probe);
        nc_mem_pool_stats_t st;
        nc_mem_pool_stats(frame_pool, &st);
        ESP_LOGI(TAG, "frame pool: %u x %u B, used=%u, peak=%u",
                 (unsigned)st.count, (unsigned)st.block_size,
                 (unsigned)st.used, (unsigned)st.peak);
    }

    err = usb_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB 初始化失敗: %s", esp_err_to_name(err));
    }

    err = storage_hand_over_to_usb();
    ESP_LOGI(TAG, "碟交給 host: %s", esp_err_to_name(err));

    s_decoder = nc4_decoder_create();
    if (s_decoder != NULL) {
        nc4_decoder_on(s_decoder, NC4_CMD_REBOOT, on_reboot);
        nc4_decoder_on(s_decoder, NC4_CMD_RESET_TO_DOWNLOAD, on_download);
        nc4_decoder_on(s_decoder, NC4_CMD_STATUS_GET, on_status_get);
        nc4_decoder_on(s_decoder, NC4_CMD_BENCH_REQ, on_bench);
        nc4_decoder_on(s_decoder, NC4_CMD_SET_MASTER, on_set_master);
        nc4_decoder_on(s_decoder, NC4_CMD_SYS_TASK_QUERY, on_sys_task_query);
        node_identity_init();
        ota_register(s_decoder);
    }

    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 2, NULL);
    if (s_decoder != NULL) {
        const nc4_bus_cfg_t cdc_bus = {
            .name = "CDC",
            .mode = NC4_BUS_PULL,
            .read = cdc_read,
            .send = cdc_send,
            .read_chunk = 1024,   // 一次多讀一點，配合加大的 TX/RX ring
            // 其餘留 0 = 用框架預設；實際值之後從 config.json 餵
        };
        esp_err_t berr = nc4_bus_add(s_decoder, &cdc_bus, NULL);
        ESP_LOGI(TAG, "NC4 通道: %s", esp_err_to_name(berr));

        // 任務註冊表：L1 起解碼（core 0），L2 起按鍵監看
        nc_task_register(&(nc_task_def_t){
            .name = "nc4_bus", .fn = nc4_bus_task_entry, .arg = NULL,
            .stack = 4096, .prio = 4, .core = 0, .layer = 1,
        });
        nc_task_register(&(nc_task_def_t){
            .name = "boot_btn", .fn = boot_button_task, .arg = NULL,
            .stack = 3072, .prio = 2, .core = 0, .layer = 2,
        });
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
