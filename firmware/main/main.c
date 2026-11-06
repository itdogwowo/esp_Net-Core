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
#include "esp_app_desc.h"      // esp_app_get_description()：韌體指紋（換版時格式化 data）
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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
#ifdef NC_ENABLE_TESTS
#include "nc_hello.h"
#include "nc_bench.h"
#include "nc_probe.h"
#include "nc_test_tcp.h"
#endif
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "nc_mem.h"
#include "nc_plan.h"
#include "nc_hub.h"
#include "nc_ota_action.h"
#include "nc4_ws.h"
#include "nc4_udp.h"
#include "nc_cfg.h"
#include "cJSON.h"
#include "nc4_http.h"
#include "nc_store.h"
#include "nc_boot.h"
#include "nc_log.h"
#include "nc_state.h"
#include "nc_file_action.h"
#ifdef NC_ENABLE_TESTS
#include "nc_fx.h"
#endif
#include "nc_wifi.h"
#include "nc_epoch.h"
#include "nc_core.h"
#ifdef NC_ENABLE_TESTS
#endif

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"              // tud_cdc_n_connected()：log 出口要知主機有冇開 port

static const char *TAG = "netcore";

// 開機階段 HWM：main 究竟邊一段用最多 stack（之後決定 stack 大細）
#define MAIN_HWM(tag) ESP_LOGW(TAG, "main hwm %-10s = %u", tag, \
                                (unsigned)uxTaskGetStackHighWaterMark(NULL))

// 解碼跑在哪個核心：router 決定（`nc4.routes[].out` 的第一個 `decodeN`），沒有編譯期巨集。

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
#define FW_VERSION         "0.1.1"

// ── 專屬測試域 0xFxxx（不屬於任何產品功能，可隨時刪）──
// 0xF001 DEV_BENCH  size(u16) iters(u16) mode(u8) → 0xF002 DEV_RSP (str_u16len JSON)
//   mode 0 = decode、1 = pack（純 CPU，不碰 flash／網路）
//   （mode 2/3/4 擦寫 OTA 分區，已移除：任何通道打一幀就能擦 OTA slot）
#define NC4_CMD_DEV_BENCH  0xF001
#define NC4_CMD_DEV_RSP    0xF002
// 0xF003 HUB_BENCH  iters(u32) → 0xF004 HUB_RSP (str JSON)
//   雙核交換：writer 固定 core 1、reader 固定 core 0
#define NC4_CMD_HUB_BENCH  0xF003
#define NC4_CMD_HUB_RSP    0xF004
// 0xF00A NET_TUNE  split(u8) core(u8) → 寫 NVS，下次開機生效
#define NC4_CMD_NET_TUNE   0xF00A
// 0xF008 BUS_STATS  [reset:u8] → 0xF009 {"bus":[...],"resync":N}
//   每條 bus 的 frames / mb / copied / parse_ms / us_per_frame（copied=0 就是零複製）
#define NC4_CMD_BUS_STATS  0xF008
#define NC4_CMD_BUS_RSP    0xF009
// 0xF00D CDC_TX_BENCH  bytes(u32) chunk(u16) mode(u8) → 0xF002 DEV_RSP
//   量「裝置 → 主機」CDC 吞吐；log 管道要幾大個 ring 由這支決定。
//   mode 0 = 連續寫、最後 flush 一次（理想吞吐）
//   mode 1 = 每個 chunk flush 一次（模擬「一行 flush 一次」的成本）
#define NC4_CMD_CDC_TX_BENCH 0xF00D

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

// ── 設定正規化：已移除（2026-11-06）──
// 以前開機把 network.json 的 bt.pin 正規化之後**寫回用戶的檔**。
// 按「firmware 只寫用戶無法掌握的資訊」的規則，這不合法：
// 用戶打的 PIN 是他自己的輸入，格式不合法應該在**讀取端**當場處理（RAM），
// 不應該改他的檔。而且現時沒有任何程式會讀 bt.pin（BLE 未實作），
// 所以這段「寫回」只是純粹的副作用。等 BT 真的接上，正規化寫在讀取處。

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

// ── data 分區的生命週期（2026-11-06 改）──
// 以前這裡有四個旗標（fs_init / fs_format / fw_fp / s_fs_first）＋「換版刪 config 檔」。
// 它們其實都在答同一條問題：「這塊分區而家喺生命週期的哪一步？」
// 現在全部由 state manager 的一條 NVS 記錄（nc_boot）回答：
//   PROVISIONED     初始化過、底座已寫 → 之後唔准自動格式化
//   FORMAT_PENDING  明確重建（用戶指令，或偵測到換韌體）
// 換韌體 = 回到起點：格式化 → seed，唔再有「刪 4 個 config 檔」這條另類路徑。
static int s_fmt_err = -99;          // -99 = 未嘗試，0 = 正常／已重建，>0 = esp_err_t

static esp_err_t storage_init(void)
{
    const esp_partition_t *part = find_data_partition();
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "data 分區: offset 0x%08" PRIx32 ", size %" PRIu32 " KB",
             part->address, (uint32_t)(part->size / 1024));

    // 一個記錄管住生命週期（provisioned / fmt_pending / degraded / fw_hash）。
    // 換韌體會在這裡被偵測到：唔再係「刪 config 檔」，而係回到起點重建。
    nc_boot_init();
    const bool first = !nc_boot_provisioned();
    const bool force = nc_boot_format_pending();
    ESP_LOGI(TAG, "data 初始化：%s（provisioned=%d fmt_pending=%d fw_new=%d）",
             first ? "第一次／重建，允許格式化" : "已初始化過，唔會自動格式化",
             nc_boot_provisioned() ? 1 : 0, force ? 1 : 0,
             nc_boot_fw_changed() ? 1 : 0);

    // WL / FatFS / MSC 的整個生命週期都由 store 管 —— 它有辦法在 claim 時
    // 把 LUN 拆掉，否則主機在 APP 模式仍然寫得進同一塊 flash。
    esp_err_t serr;
    if (force) {
        // 明確重建：換韌體，或用戶下了 STATUS_SET fs_format=1。
        // 只喺開機（tasks 未起）消費，唔會喺解碼路徑抹 7 MB。
        ESP_LOGW(TAG, "%s → 抹 data 分區並重建 FAT",
                 nc_boot_fw_changed() ? "換韌體" : "System.fs_format=1");
        serr = nc_store_format(part);
        s_fmt_err = (serr == ESP_OK) ? 0 : (int)serr;
    } else {
        serr = nc_store_init(part, first);

        if (serr != ESP_OK && first) {
            // **只有「第一次／重建」才允許抹分區**。之後掛唔上就保持原狀，
            // 由 host 端格式化／修復；唔會因為一次 mount 失敗清掉使用者資料。
            ESP_LOGW(TAG, "/data 第一次初始化掛唔上（%s）→ 抹分區重建 FAT",
                     esp_err_to_name(serr));
            serr = nc_store_format(part);
            s_fmt_err = (serr == ESP_OK) ? 0 : (int)serr;
        }
    }
    // 冇走 nc_store_format() 就保持 -99（= 這次開機冇重建）。
    // 這個數字是「這次開機到底有冇抹」的唯一證據，唔好用成功／失敗去蓋掉它。

    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "/data 初始化失敗：%s（用 STATUS_SET System.fs_format=1 "
                      "＋ REBOOT 可要求下次開機重建）", esp_err_to_name(serr));
        // 唔 commit：FORMAT_PENDING 留返，下次開機再試。
        return serr;
    }

    // 底座（seed）：缺檔才寫；已有的檔一律不動（seed_file 自己檢查）。
    // 換韌體走的是「格式化 → 這裡自然寫回」同一條路，唔需要另寫刪檔邏輯。
    seed_presets();

    if (nc_store_app_ready()) {
        if (first || force) {
            nc_boot_mark_provisioned();   // PROVISIONED=1、FORMAT_PENDING=0
        }
        (void)nc_boot_commit();           // 新的 fw_hash 亦喺呢度落盤
    }
    return ESP_OK;
}

// ── link 的核歸屬與 rx 參數（定義在下方「link 啟用狀態」一節）──
// 唯一真相是 system.json：
//   核   = routes[].out[] 裡面第一個 decodeN
//   參數 = links[].cfg.rx.{region,chunk,budget,slots,slot_size}
// 代碼裡面唔准再有硬編碼 fallback —— 打錯名就會靜靜跑喺錯嘅核，最難查。
// 注意：呢個 typedef 唔可以擺喺 #if !P4 裡面，因為 P4 都要用。
typedef struct {
    size_t   region;   // parser 緩衝大小（會拿去 plan 宣告）
    size_t   chunk;    // 一次 read 最多拿幾多 bytes
    uint32_t budget;   // 一條線一輪最多讀幾次
    size_t   slots;    // PUSH 模式插槽數
    size_t   slot;     // PUSH 模式每格大小
} link_rx_t;

static bool link_core(const char *line, int *out);
static int link_rx(const char *name, link_rx_t *rx);   // 回傳 config 實際套用了幾多個欄位

// ── USB（TinyUSB）只在 S3 這版存在 ──
// P4 的 USB-C 只接 USB-Serial-JTAG，沒有 OTG → 這幾個函式永遠不會被呼叫。
// 但它們在初始器裡被取址（cdc_bus.read/.send），linker 就一路把 tinyusb
// 拉進來：_cdcd_itf 12,532 + _mscd_epbuf 8,192 + _cdcd_epbuf 1,024 ≈ 21.7 KB。
// 所以整個 P4 分支連「定義」都唔編 —— 唔單止唔呼叫。
#if !CONFIG_IDF_TARGET_ESP32P4
// USB 服務組：device stack + CDC + console（+ MSC）全部喺同一個核安裝。
// 核心由 router 決定（system.json 的 CDC 行），唔寫死。
static esp_err_t usb_install(void *arg)
{
    const int core = (int)(intptr_t)arg;
#ifdef CONFIG_IDF_TARGET_ESP32S3
    // MSC 都屬於 USB 服務組：同一個核、TinyUSB 之前
    ESP_RETURN_ON_ERROR(nc_store_usb_driver(), TAG, "MSC driver 安裝失敗");
#endif
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task.xCoreID = core;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB driver 安裝失敗");

    const tinyusb_config_cdcacm_t acm_cfg = { .cdc_port = TINYUSB_CDC_ACM_0 };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), TAG, "CDC-ACM 初始化失敗");

    // **故意不呼叫 tinyusb_console_init()**：
    // console 留在 USB-Serial-JTAG（開機即存在、唔靠 TinyUSB），
    // 咁就算 TinyUSB / FAT / 任何後續初始化卡死，仍然有 log 可以睇。
    // 以前把 console 搬去 CDC，結果 usb_init() 之後任何 hang 都完全冇輸出 ——
    // 2026-11-05 就係咁樣變成「裝置身完全冇反應」。
    return ESP_OK;
}

static esp_err_t usb_init(void)
{
    // USB 服務組（TinyUSB + CDC + console）需要一條核：正常跟 CDC 的 routes[]。
    // CDC 未路由時 USB 仍然要初始化（console 要活），退回 core0 並明確記錄。
    int cdc_core = 0;
    if (nc4_router_core("CDC") != NC4_ROUTE_NONE) {
        cdc_core = nc4_router_core("CDC");
    } else {
        ESP_LOGW(TAG, "CDC 未路由 → USB 服務組放 core0（服務放置，唔代表數據走 core0）");
    }
    ESP_LOGI(TAG, "USB 服務組（TinyUSB + CDC + console）→ core %d", cdc_core);
    // 喺 CDC 所屬核執行：USB ISR、TinyUSB task、RX callback 之後全部同核
    return nc_core_run_on(cdc_core, usb_install, (void *)(intptr_t)cdc_core);
}

static esp_err_t storage_hand_over_to_usb(void)
{
    // 交出去之後裝置只剩唯讀視圖（/ro）；要拿回來請把 System.file_mode 設成 1
    //（store 會把 LUN 拆掉，主機的碟會消失、再出現）。
    return nc_store_release_usb();
}

// ── CDC 出口仲裁（一個出口、兩個入口）──
//
// 為什麼需要：TinyUSB 的 write_queue 沒有鎖（直接寫 FIFO）。回覆（decode 任務）
// 和 log（log 任務）是兩個 writer，交錯起來可以切斷一幀。
//
// 規則（唔用 mutex，只有本核幾 µs 的 critical section）：
//   · 回覆優先：整幀獨佔出口（s_tx_reply），FIFO 滿時讓出 CPU 等 TinyUSB 排空。
//   · log 永遠唔跨「等待」持有出口：空間不夠就整行稍後再試，唔會卡住回覆。
//   · 兩個 writer 都在 CDC 核，所以 s_tx_reply 只會由同一核讀寫。
static portMUX_TYPE s_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_tx_reply;      // 回覆正在寫一整幀
static uint32_t s_tx_stalls;          // 回覆等 FIFO 的次數（可觀察）

#define CDC_TX_WAIT_US  100000        // 回覆最多等 100 ms；超過就丟，避免卡死 decode

static esp_err_t cdc_send(void *arg, const uint8_t *frame, size_t len)
{
    (void)arg;
    if (frame == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tx_reply = true;
    size_t off = 0;
    const int64_t deadline = esp_timer_get_time() + CDC_TX_WAIT_US;
    while (off < len) {
        size_t k = 0;
        portENTER_CRITICAL(&s_tx_mux);
        k = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, frame + off, len - off);
        portEXIT_CRITICAL(&s_tx_mux);
        if (k == 0) {
            // FIFO 滿：一定要讓出 CPU，TinyUSB task（prio 5）高過 decode（4）
            s_tx_stalls++;
            if (esp_timer_get_time() > deadline) {
                s_tx_reply = false;
                ESP_LOGW(TAG, "CDC TX 塞住 → 丟回覆（%u B 未送）", (unsigned)(len - off));
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(1);
            continue;
        }
        off += k;
    }
    s_tx_reply = false;
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

// log 入口：整行一次寫；回覆正在用出口、或者 FIFO 空間不夠 → BUSY（稍後再試）。
// 主機未開 port → OFFLINE：consumer 會保留資料，唔會白白掉走開機 log。
// 這裡必然在 critical section 內完成「檢查 + 寫入」，所以不會切開回覆的幀。
static nc_log_tx_result_t cdc_tx_text(const char *buf, size_t len)
{
    // 上限要對得起一「批」：batch 最多 NC_LOG_BATCH_BYTES(512)，
    // 加一行餘裕；TinyUSB TX FIFO 是 4 KB，一次寫 1 KB 完全冇問題。
    if (buf == NULL || len == 0 || len > 1024) {
        return NC_LOG_TX_BUSY;
    }
    if (!tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
        return NC_LOG_TX_OFFLINE;      // 主機未開這個 port（DTR 未拉高）
    }
    bool ok = false;
    portENTER_CRITICAL(&s_tx_mux);
    if (!s_tx_reply) {
        ok = (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, len) == len);
    }
    portEXIT_CRITICAL(&s_tx_mux);
    if (ok) {
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        return NC_LOG_TX_OK;
    }
    return NC_LOG_TX_BUSY;
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

// log 管道（兩個核的 ESP_LOG → 每核 ring → 單一 consumer → CDC 出口）
static size_t status_put_log(char *buf, size_t cap)
{
    size_t off = 0;
    char lj[256];
    nc_log_json(lj, sizeof(lj));
    jappend(buf, cap, &off, "\"log\":%s", lj);
    return off;
}

// 執行期任務表（名稱 / 核 / 優先序 / stack 剩餘）。需要 CONFIG_FREERTOS_USE_TRACE_FACILITY。
// 用途：判斷每個 task 的 stack 到底用幾多，唔好靠估。
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static size_t status_put_rt(char *buf, size_t cap)
{
    static TaskStatus_t st[24];
    const UBaseType_t n = uxTaskGetSystemState(st, 24, NULL);
    size_t off = 0;
    jappend(buf, cap, &off, "\"rt\":[");
    for (UBaseType_t i = 0; i < n && off < cap; i++) {
        jappend(buf, cap, &off, "%s{\"n\":\"%s\",\"prio\":%u,\"hwm\":%u}",
                (i != 0) ? "," : "", st[i].pcTaskName,
                (unsigned)st[i].uxCurrentPriority,
                (unsigned)st[i].usStackHighWaterMark);
    }
    jappend(buf, cap, &off, "]");
    return off;
}
#endif

// ── config 探測結果（宣告階段填一次，STATUS 回報）──
// -1 = 未探測；app_ready = 探測當刻 FAT 有冇掛上；errno = fopen 失敗原因。
static int s_cfg_app_ready = -1;
static int s_cfg_errno;
static int s_cfg_open_ok;            // nc_cfg_open("system") 有冇成功（0 = NULL）
static uint32_t s_cfg_hdr;           // 檔案頭 4 bytes（偵測 BOM／亂碼）
#if !CONFIG_IDF_TARGET_ESP32P4
static int s_cdc_cfg_src = -9;       // CDC 的 rx 參數在「宣告階段」套用了幾多項
                                     // （bus 建立嗰刻 FAT 已交咗俾 host，讀唔到 config）
#endif

static size_t status_put_storage(char *buf, size_t cap)
{
    size_t off = 0;
    char boot[96];
    nc_boot_json(boot, sizeof(boot));   // {"prov":1,"fmtq":0,"degr":0,"fw_new":0}
    // FAT 現在在誰手上、裝置能不能直接讀；app_decl / cfg_errno = 宣告階段的探測結果。
    // boot = state manager 的開機狀態（provisioned / format_pending / degraded / fw_new）
    jappend(buf, cap, &off,
            "\"storage\":{\"mode\":\"%s\",\"app_ready\":%d,\"boot\":%s,"
            "\"app_decl\":%d,\"cfg_errno\":%d,\"cfg_open\":%d,"
            "\"cfg_hdr\":\"%02X%02X%02X%02X\",\"fmt\":%d,\"reconn_hwm\":%u}",
            nc_store_mode_name(), nc_store_app_ready() ? 1 : 0, boot,
            s_cfg_app_ready, s_cfg_errno, s_cfg_open_ok,
            (unsigned)(s_cfg_hdr & 0xFF), (unsigned)((s_cfg_hdr >> 8) & 0xFF),
            (unsigned)((s_cfg_hdr >> 16) & 0xFF), (unsigned)((s_cfg_hdr >> 24) & 0xFF),
            s_fmt_err, (unsigned)nc_store_reconn_hwm());
    return off;
}

// config 的載入結果（哪一層用咗預設、有冇壞檔、有幾多不合法）——呢啲數字
// 以前只係「睇 code 推」；現在每次開機都答得出來。
static size_t status_put_cfg(char *buf, size_t cap)
{
    size_t off = 0;
    char cj[288];
    nc_cfg_json(cj, sizeof(cj));
    jappend(buf, cap, &off, "\"cfg\":%s", cj);
    return off;
}

static size_t status_put_sys(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off,
            "\"sys\":{\"fw\":\"%s\",\"app\":\"%02x%02x%02x%02x\",\"heap\":%u,\"up_s\":%lld,\"idf\":\"%s\",\"cid\":%u}",
            FW_VERSION,
            (unsigned)esp_app_get_description()->app_elf_sha256[0],
            (unsigned)esp_app_get_description()->app_elf_sha256[1],
            (unsigned)esp_app_get_description()->app_elf_sha256[2],
            (unsigned)esp_app_get_description()->app_elf_sha256[3],
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (long long)(esp_timer_get_time() / 1000000),
            esp_get_idf_version(), (unsigned)s_cid);
    return off;
}

// nc_epoch_json 輸出的是「整份物件」{"core0":…}，但 status 區塊要 "key":{…} 片段。
static size_t status_put_epoch(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"epoch\":");
    jchain(buf, cap, &off, nc_epoch_json);
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

// nc_wifi_json 只回物件本體（模組不認得 STATUS 的 key），外層要補 "wifi": 前綴。
// 以前直接 status_add(nc_wifi_json) → 出咗 `...],{"compiled":0,...}` 呢個無 key 的成員，
// 整份 STATUS 即刻變成非法 JSON。
static size_t status_put_wifi(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"wifi\":");
    jchain(buf, cap, &off, nc_wifi_json);
    return off;
}

// bus 統計（S3／P4 都要；WS／UDP 只有 P4 有）
static size_t status_put_net(char *buf, size_t cap)
{
    size_t off = 0;
    jappend(buf, cap, &off, "\"net\":{\"bus\":");
    jchain(buf, cap, &off, nc4_bus_stats_json);
#if CONFIG_IDF_TARGET_ESP32P4
    jappend(buf, cap, &off, ",\"ws\":");
    jchain(buf, cap, &off, nc4_ws_json);
    jappend(buf, cap, &off, ",\"udp\":");
    jchain(buf, cap, &off, nc4_udp_json);
#endif
    jappend(buf, cap, &off, "}");
    return off;
}

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
                   "\"tasks\",\"fx\",\"wifi\",\"epoch\",\"net\"]}");
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
    if (keys_want(keys, "cfg")) {
        status_add(json, cap, &off, &first, status_put_cfg);
    }
    if (keys_want(keys, "log")) {
        status_add(json, cap, &off, &first, status_put_log);
    }
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    if (keys_want(keys, "rt")) {
        status_add(json, cap, &off, &first, status_put_rt);
    }
#endif
    if (keys_want(keys, "epoch")) {
        status_add(json, cap, &off, &first, status_put_epoch);
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
#ifdef NC_ENABLE_TESTS
    if (keys_want(keys, "fx")) {
        status_add(json, cap, &off, &first, nc_fx_status_json);
    }
#endif
    if (keys_want(keys, "wifi")) {
        status_add(json, cap, &off, &first, status_put_wifi);
    }
    if (keys_want(keys, "net")) {
        status_add(json, cap, &off, &first, status_put_net);
    }
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


static void on_sys_task_query(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    (void)f;
    // 緊湊清單（name/level/affinity/started/loops），保證合法 JSON。
    // 以前用 768 B 裝完整逐 loop JSON → 一定截斷成無效 JSON。
    char json[1024];
    nc_task_list_json(json, sizeof(json));
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

    // cID 的真相在 config/system.json（已由 nc_cfg_boot 型別化）：
    //   有值 → 用使用者的；空的 → 用 MAC 末 2 bytes 推導，並寫回 config。
    // 寫回是允許的（FW_FILL 規則）：用戶睇唔到 MAC，冇辦法自己填這個值。
    uint16_t cid = NC4_ADDR_BROADCAST;
    bool present = false;

    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys != NULL && sys->cid[0] != '\0') {
        char *endp = NULL;
        const unsigned long x = strtoul(sys->cid, &endp, 16);
        if (endp != NULL && *endp == '\0' && x <= 0xFFFF) {
            cid = (uint16_t)x;
            present = true;
        } else {
            ESP_LOGW(TAG, "cID \"%s\" 不是 4 位 hex → 改由 MAC 推導", sys->cid);
        }
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
    // producer 用：無限等（ISR 喺同一核，wake 係本地；唔用 timeout）
    int n = uart_read_bytes(UART_TEST_PORT, buf, cap, portMAX_DELAY);
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

// bus 的 driver 初始化：只會在 UART bus 所屬的那個核上跑（owner_init）。
static esp_err_t uart_owner_init(void *arg)
{
    (void)arg;
    return uart_loop_init(UART_TEST_DEF_BAUD);
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

// CDC 的 rx 參數：唔需要寫入 config —— 已經量測好，直接做 code 預設。
// 實測依據：RX 幀 = 9 + payload + 4；OTA chunk 4,096 ⇒ 幀 4,113 ⇒ region 4,192。
// config 仍然可以覆寫（links[].cfg.rx.region 等），但正常情況唔應該寫。
#define CDC_RX_REGION_DEF  4192
// 安全上限只係協議層的理智檢查（一個幀唔可能大過 NC4_MAX_FRAME），唔係政策。
#define CDC_RX_REGION_CEIL NC4_MAX_FRAME

static size_t cdc_region(size_t want)
{
    if (want > CDC_RX_REGION_CEIL) {
        ESP_LOGW(TAG, "CDC rx.region %u 超過協議上限 → 夾到 %u",
                 (unsigned)want, (unsigned)CDC_RX_REGION_CEIL);
        return CDC_RX_REGION_CEIL;
    }
    return want;
}

// ── CDC 接收任務（receiver）：一條總線一條 task ──
// decode 唔再親自讀 CDC（PULL 嘅 10 ms 輪詢，實測 RTT = 10.0 ms）。
// 呢條 task 只做「讀走 → 放環」，唔做解析；解析仍然由 decode 嘅 parser 做。
//
// 喚醒：TinyUSB 嘅 CDC_EVENT_RX callback（喺 TinyUSB task 內、同核）→ xTaskNotifyGive。
// 安全網：ulTaskNotifyTake 帶 2 tick 超時 —— 就算 callback 冇註冊成功，CDC 仍然會
// 每 2 ms 自己醒一次，唔會斷線（RX 一斷就冇辦法用 OTA 救返呢點很關鍵）。
#define CDC_RX_SLOT       256        // 實測每次 push 平均 68 B（USB FS 64 B 封包）⇒ 256 已有 3.7× 餘裕
#define CDC_RX_SLOTS      4          // 4 × 256 = 1 KB（實測峰值 3 格）
#define CDC_RX_TASK_PRIO  5
#define CDC_RX_TASK_STACK 1536       // 實測用 840 B；1 KB 只剩 184 B（太薄），1.5 KB 留 696 B 餘裕
#define CDC_RX_WAIT_MS    2

static TaskHandle_t s_cdc_rx_task = NULL;

static void cdc_rx_notify(int itf, cdcacm_event_t *ev)
{
    (void)itf;
    (void)ev;
    const TaskHandle_t t = s_cdc_rx_task;      // 喺 TinyUSB task（task context，同核）
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

static void cdc_rx_task(void *arg)
{
    nc4_bus_handle_t bus = (nc4_bus_handle_t)arg;
    // 每次搬一格，格大小由 bus 自己答（config 可以改 rx.slot_size，不用改這裡）
    const size_t slot = nc4_bus_slot_size(bus);

    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CDC_RX_WAIT_MS)) == 0) {
            // 超時：安全網。正常情況下面嘅 RX callback 會即刻叫醒我哋。
        }
        for (;;) {
            uint8_t *dst = nc4_bus_push_begin(bus, slot);
            if (dst == NULL) {
                break;                          // 環滿：等 decode 排空
            }
            size_t off = 0;
            while (off < slot) {
                const int n = cdc_read(NULL, dst + off, slot - off);
                if (n <= 0) {
                    break;
                }
                off += (size_t)n;
            }
            if (off == 0) {
                nc4_bus_push_abort(bus);
                break;
            }
            nc4_bus_push_commit(bus, off);
            nc4_bus_kick(bus);                  // 同核 notify decode
        }
    }
}
#endif   // !CONFIG_IDF_TARGET_ESP32P4

// ── 資源宣告（模組在宣告期寫「我要多少」，main 不介入）──
// 收/解是否分核（NVS: net/rx_split, net/rx_core；預設 0 / 1）
#if CONFIG_IDF_TARGET_ESP32P4
// 只有 P4 有 ETH；S3 上呢兩個永遠冇人讀，唔 guard 就會 -Wunused-variable
static bool s_eth_ok;              // 硬體/驅動真的起來了才宣告網路資源
#endif
static void load_net_tuning(void)
{
    // 核心歸屬只有一個來源：router（system.json 的 routes[].out）。
    // 以前呢度有 rx_split / rx_core 兩個 NVS 旋鈕，正好同 router 打架 → 已刪。
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, "frame_align", &v) == ESP_OK) {
        s_frame_align = (v != 0);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "net tuning: frame_align=%d（NVS）", s_frame_align ? 1 : 0);
}

static void *s_nc4_rx_region;
static size_t s_nc4_rx_bytes;
static void *s_uart_rx_region;
// UART 的 producer 要延後到 decode 任務起咗之後才建立（見 app_main 的說明）。
static nc4_bus_handle_t s_uart_bus;
#if CONFIG_IDF_TARGET_ESP32P4
static void *s_tcp_rx_region;      // 同上：S3 冇 TCP 通道
#endif

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

#ifdef NC_ENABLE_TESTS
static const nc_task_provide_t s_fx_provides[] = {
    { .name = "fx.info", .kind = NC_PROVIDE_JSON, .desc = "live 效果 / arena / 每個實例",
      .json = nc_fx_info_json },
};
#endif

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
    const nc_link_cfg_t *l = nc_cfg_link(name);
    if (l == NULL) {
        // 讀不到 config（或 links[] 冇這條線）→ 照硬體探測結果。
        // 這是「最後保底」：config 壞掉時不應該連通道都沒有。
        ESP_LOGW(TAG, "link %s：config 冇呢一條 → 當啟用（要關就寫 \"enable\": 0）", name);
        return true;
    }
    return l->enable;
}

// 核歸屬：routes[].out[] 是唯一真相。取唔到就唔建立該通道（fail-visible）。
static bool link_core(const char *line, int *out)
{
    const int c = nc4_router_core(line);
    if (c == NC4_ROUTE_NONE) {
        ESP_LOGE(TAG, "link %s：routes[] 冇指去任何 decodeN → 唔建立"
                      "（核歸屬只由 system.json 決定，code 冇 fallback）", line);
        return false;
    }
    if (out != NULL) {
        *out = c;
    }
    return true;
}

// rx 參數：直接由 struct 拿（不再逐項覆寫呼叫端的預設）。
// 回傳「config 真的寫了幾個欄位」（rx_mask 的位數），給 STATUS 的 cfg_src 用。
// 找不到這條 link 回 -2；struct 未載入回 -1。
static int link_rx(const char *name, link_rx_t *rx)
{
    if (rx == NULL) {
        return -3;
    }
    const nc_link_cfg_t *l = nc_cfg_link(name);
    if (l == NULL) {
        return -1;
    }
    rx->region = l->rx.region;
    rx->chunk = l->rx.chunk;
    rx->budget = l->rx.budget;
    rx->slots = l->rx.slots;
    rx->slot = l->rx.slot_size;

    int applied = 0;
    for (int i = 0; i < 5; i++) {
        if (l->rx_mask & (1u << i)) {
            applied++;
        }
    }
    return applied;
}

// ── config 探測：一次性，結果經 STATUS 回報（開機 log 搶唔到，因為 USB 會重列舉）──
static void cfg_probe_once(void)
{
    if (s_cfg_app_ready >= 0) {
        return;
    }
    s_cfg_app_ready = nc_store_app_ready() ? 1 : 0;
    errno = 0;
    FILE *f = fopen("/data/config/system.json", "rb");
    s_cfg_errno = (f != NULL) ? 0 : errno;
    if (f != NULL) {
        uint8_t hdr[4] = {0};
        if (fread(hdr, 1, 4, f) == 4) {
            s_cfg_hdr = ((uint32_t)hdr[0]) | ((uint32_t)hdr[1] << 8) |
                        ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        }
        fclose(f);
    }
    cfg_file_t *c = nc_cfg_open("system");
    s_cfg_open_ok = (c != NULL) ? 1 : 0;
    if (c != NULL) {
        nc_cfg_close(c);
    }
    ESP_LOGI(TAG, "cfg 探測：app_ready=%d fopen_errno=%d open_ok=%d hdr=%08x",
             s_cfg_app_ready, s_cfg_errno, s_cfg_open_ok, (unsigned)s_cfg_hdr);
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
    // 兩個條件都要：link 啟用（enable）＋ routes[] 有指去某個 decodeN。
    // 少咗後者，bus 建立處（link_core）會拒絕，呢 8,205 B 就白佔。
    if (max < 1 || !link_enabled("CDC") || nc4_router_core("CDC") == NC4_ROUTE_NONE) {
        return 0;
    }
    link_rx_t rx = { .region = CDC_RX_REGION_DEF };
    cfg_probe_once();
    const int ncfg = link_rx("CDC", &rx);
    s_cdc_cfg_src = ncfg;
    rx.region = cdc_region(rx.region);
    out[0] = (nc_task_res_t){
        .name = "nc4.rx", .bytes = rx.region, .cls = NC_PLAN_INTERNAL, .align = 32,
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
    cfg_probe_once();          // STATUS 要的探測結果（S3／P4 都用得著）
#if CONFIG_IDF_TARGET_ESP32P4
    // P4 冇 USB OTG → 唔會有 nc4.rx，亦冇 CDC 的 rx 參數可讀。
    // cdc_region()／CDC_RX_REGION_DEF 只在 !P4 定義，唔可以喺呢邊引用。
    s_nc4_rx_bytes = 0;
    ESP_LOGW(TAG, "svc nc4: 冇 parser 緩衝（本板冇 USB）");
#else
    link_rx_t rx = { .region = CDC_RX_REGION_DEF };
    link_rx("CDC", &rx);
    rx.region = cdc_region(rx.region);
    s_nc4_rx_bytes = (s_nc4_rx_region != NULL) ? rx.region : 0;
    if (s_nc4_rx_region != NULL) {
        ESP_LOGI(TAG, "svc nc4: parser 緩衝 ok");
    } else {
        ESP_LOGW(TAG, "svc nc4: 冇 parser 緩衝（CDC 未啟用 / 本板冇 USB）");
    }
#endif
    return ESP_OK;
}

// ── net：UART / TCP 的 parser 緩衝（TCP 只在 P4）──
// 只宣告**真的會建立**嘅 bus；冇啟用就 0 B。
static int task_net_declare(nc_task_res_t *out, int max)
{
    int n = 0;
    // 同 CDC 一樣：enable ＋ 有路由，兩樣都有才宣告（否則 bus 唔會建立）。
    if (n < max && link_enabled("UART") && nc4_router_core("UART") != NC4_ROUTE_NONE) {
        link_rx_t rx = { .region = 1024 };
        link_rx("UART", &rx);
        out[n++] = (nc_task_res_t){
            .name = "uart.rx", .bytes = rx.region, .cls = NC_PLAN_INTERNAL, .align = 32,
        };
    }
#if CONFIG_IDF_TARGET_ESP32P4
    // 兩個條件都要：硬體真的起來（ETH）＋ config 有啟用（TCP）
    if (n < max && s_eth_ok && link_enabled("TCP") &&
        nc4_router_core("TCP") != NC4_ROUTE_NONE) {
        link_rx_t rx = { .region = 16410 };   // 2 幀：跨讀時需要餘裕
        link_rx("TCP", &rx);
        out[n++] = (nc_task_res_t){
            .name = "tcp.rx", .bytes = rx.region, .cls = NC_PLAN_INTERNAL, .align = 32,
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
    // 注意：UART driver **唔在這裡裝**。bus 綁邊個核，就由嗰個核的 decode task
    // 在開 loop 之前初始化（nc4_bus_cfg_t.owner_init）。
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

// ④ Action：NC4 指令（核心執行能力，不屬於任何任務）
//
// 指令是 decoder 的表；開機時註冊一次，之後 decode task 只負責收發與分派。
// 任務不擁有指令 —— 任務只是把「已經存在的執行能力」恆常化、排程化。
static void on_route_get(nc4_ctx_t *ctx, const nc4_frame_t *f);   // 定義喺下面

typedef struct {
    uint16_t cmd;
    nc4_handler_t fn;
} core_action_t;

static const core_action_t s_core_actions[] = {
    { NC4_CMD_REBOOT,            on_reboot },
    { NC4_CMD_STATUS_GET,        on_status_get },
    { NC4_CMD_ROUTE_GET,         on_route_get },
    { NC4_CMD_SET_MASTER,        on_set_master },
    { NC4_CMD_SYS_TASK_QUERY,    on_sys_task_query },
};

// 開機把三組 action 註冊進 decoder：core（上面這張）、ota（0x22xx）、file（0x2005/0x1103）。
// 沒有任務參與 —— 這就是「指令是核心、任務是表面」的實際長相。
#ifdef NC_ENABLE_TESTS
#include "nc_test_action.h"

// 測試指令要讀的 RX 診斷（住喺通道裝配層）
static uint32_t test_rx_counter(int idx)
{
    switch (idx) {
    case 0: return s_rx_resync;
    case 1: return s_rx_frames;
    case 2: return s_rx_hub_full;
    case 3: return s_rx_sel;
    case 4: return s_rx_sel_to;
    default: return 0;
    }
}
static uint64_t test_rx_fill_us(void) { return s_rx_fill_us; }
static void test_rx_reset(void)
{
    s_rx_resync = 0;
    s_rx_frames = 0;
    s_rx_hub_full = 0;
    s_rx_sel = 0;
    s_rx_sel_to = 0;
    s_rx_fill_us = 0;
}
static bool test_frame_align(void) { return s_frame_align; }
static const nc_test_action_hooks_t s_test_hooks = {
    .rx_counter = test_rx_counter,
    .rx_fill_us = test_rx_fill_us,
    .frame_align = test_frame_align,
    .reset_rx = test_rx_reset,
};
#endif


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

// ── 0xF00D CDC_TX_BENCH：量裝置→主機的 CDC 吞吐（決定 log ring 要幾大）──
// 為什麼在 main.c 而不是 test_action：要直接碰 TinyUSB FIFO（唔經 NC4 分幀），
// 而且要知道 FIFO 滿的時候阻塞幾多次。
// 樣式刻意只用 hex 字元（冇 "NC"），免得干擾主機的分幀器。
#ifdef NC_ENABLE_TESTS
#if !CONFIG_IDF_TARGET_ESP32P4
static uint8_t s_bench_pat[4096];

static void bench_pat_fill(void)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(s_bench_pat); i++) {
        s_bench_pat[i] = (uint8_t)hex[i % (sizeof(hex) - 1)];
    }
}

static void on_cdc_tx_bench(nc4_ctx_t *ctx, const nc4_frame_t *f)
{
    if (f->len < 7) {
        reply_json(ctx, NC4_CMD_DEV_RSP, "{\"err\":\"bytes/chunk/mode\"}");
        return;
    }
    uint32_t bytes = (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) |
                     ((uint32_t)f->data[2] << 16) | ((uint32_t)f->data[3] << 24);
    const uint16_t chunk = (uint16_t)f->data[4] | ((uint16_t)f->data[5] << 8);
    const uint8_t mode = f->data[6];

    const size_t ch = (chunk == 0 || chunk > sizeof(s_bench_pat)) ? 64 : chunk;
    if (bytes == 0 || bytes > (1u << 20)) {
        bytes = 65536;      // 上限 1 MB，預設 64 KB
    }

    bench_pat_fill();
    const int64_t t0 = esp_timer_get_time();
    uint32_t stalls = 0;
    size_t left = bytes;
    while (left > 0) {
        size_t n = (left > ch) ? ch : left;
        size_t off = 0;
        while (off < n) {
            // FIFO 滿會回比較少 → 讓 TinyUSB task（prio 5，高過 decode 的 4）排空
            const size_t k = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                                        s_bench_pat, n - off);
            if (k == 0) {
                stalls++;
                taskYIELD();
                continue;
            }
            off += k;
        }
        left -= n;
        if (mode == 1) {
            (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }
    (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(1000));
    const int64_t us = esp_timer_get_time() - t0;

    char js[160];
    snprintf(js, sizeof(js),
             "{\"bytes\":%u,\"chunk\":%u,\"mode\":%u,\"dev_us\":%lld,\"stalls\":%u}",
             (unsigned)bytes, (unsigned)ch, (unsigned)mode, (long long)us,
             (unsigned)stalls);
    ESP_LOGW(TAG, "CDC_TX_BENCH %s", js);
    reply_json(ctx, NC4_CMD_DEV_RSP, js);
}
#endif   // !P4
#endif   // NC_ENABLE_TESTS

static void actions_register(nc4_decoder_t *dec)
{
    if (dec == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof(s_core_actions) / sizeof(s_core_actions[0]); i++) {
        nc4_decoder_on_owner(dec, s_core_actions[i].cmd, s_core_actions[i].fn, "core");
    }
    // cid / master_cid 已在開機最前面初始化（卷標要用）
#ifdef NC_ENABLE_TESTS
    nc_test_action_register(dec, &s_test_hooks);   // 0xF00x / 0x181x
#if !CONFIG_IDF_TARGET_ESP32P4
    nc4_decoder_on_owner(dec, NC4_CMD_CDC_TX_BENCH, on_cdc_tx_bench, "core");
#endif
#endif
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
// router 的名稱池：cJSON 樹喺 router_from_cfg() 結尾就會 free，
// 但 nc4_router_set() 係 caller 之後才呼叫 —— 借 cJSON 的指標會 use-after-free。
// 所以名稱先抄一份入呢個 pool；表開機只讀一次，生命週期 = 整個開機。
static char s_route_names[NC4_ROUTE_MAX][NC4_ROUTE_NAME_MAX];

static int router_from_cfg(nc4_route_t *out, int max)
{
    const nc_system_cfg_t *sys = nc_cfg_system();
    if (sys == NULL || sys->n_routes == 0) {
        return 0;
    }
    int n = 0;
    for (uint8_t i = 0; i < sys->n_routes && n < max && n < NC4_ROUTE_MAX; i++) {
        const nc_route_cfg_t *row = &sys->routes[i];
        // 呢條 link 冇啟用 → 佢唔會存在，路由都唔應該令嗰個核「有嘢做」
        if (strcmp(row->in, "nctx") != 0 && strcmp(row->in, "outbound") != 0 &&
            !link_enabled(row->in)) {
            ESP_LOGW(TAG, "router：%s 未啟用（links[].enable=0）→ 跳過呢行", row->in);
            continue;
        }
        int core = NC4_ROUTE_NONE;
        for (uint8_t k = 0; k < row->n_out; k++) {
            const char *s = row->out[k];
            if (strncmp(s, "decode", 6) == 0 && (s[6] == '0' || s[6] == '1') && s[7] == '\0') {
                core = s[6] - '0';
                break;
            }
        }
        if (core == NC4_ROUTE_NONE) {
            continue;   // 只轉發／丟棄 → 本地冇人要解
        }
        // 名字已喺 struct 裡面（壽命 = 程式執行期），但仍抄入 router 自己的 pool：
        // router 的介面契約是「唔借 caller 的字串」。
        const size_t nlen = strlen(row->in);
        if (nlen >= NC4_ROUTE_NAME_MAX) {
            ESP_LOGW(TAG, "router：線名太長（%u）→ 跳過", (unsigned)nlen);
            continue;
        }
        memcpy(s_route_names[n], row->in, nlen + 1);
        out[n].line = s_route_names[n];
        out[n].core = core;
        n++;
    }
    if (n > 0) {
        ESP_LOGI(TAG, "router：由 config 讀到 %d 列", n);
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


#ifdef NC_ENABLE_TESTS
// ⑥ fx（Normal L4）：**POC／原始形態**（2026-11-04 標記）—— arena + 實例表 + render task。
//    運作方式未定義：核心、幀來源、同緩衝交換的接口都係待規劃，唔好當成正式功能。
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

#endif
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

    // ★ NVS 要先起來：log 的開機模式（runtime/boot/debug）存在 NVS，
    //   而 log 擷取要最早開始（見下面理由），所以次序是 NVS → log → 其他。
    nvs_boot_init();

    // ★ log 擷取要最早開始：
    //   S3 的 USB-Serial-JTAG 同 OTG 共用 PHY，usb_init() 之後 JTAG 的 port 會消失，
    //   之後才裝 hook 的話，NVS／storage／config／router 這整段開機 log 都會冇。
    //   這裡只裝 hook（寫入 RAM ring）；consumer 要到 usb_init() 之後才有出口。
    (void)nc_log_capture();

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

    // ── NVS → /data 掛載 → config 型別化 → 身份 → 卷標 ──
    // 次序是硬性的：config 的檔案就住在 data 分區，所以「掛載」一定先過「讀 config」。
    // 而 config 之後只讀一次（struct），任何人再用都唔會再碰檔案 ——
    // 所以「FAT 交咗俾 host 之後 config 讀唔到」這個舊問題由設計上消失。
    // nvs_boot_init() 已在 app_main 最前面（log 需要讀 NVS 設定）
    MAIN_HWM("pre-storage");
    esp_err_t err = storage_init();
    MAIN_HWM("post-storage");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage 初始化失敗: %s", esp_err_to_name(err));
    }
    (void)nc_cfg_boot();    // 永遠成功：最差 = 全部用預設
    // system.json 的 "log_debug_mode"：
    //   0 = 正常（INFO、2 KB、USB 起來之後開始記 → 看到服務/bus 啟動）
    //   1 = 除錯（全部等級、8 KB、第一行開始）
    {
        const nc_system_cfg_t *sys = nc_cfg_system();
        uint8_t cur = 0;
        nc_log_get_levels(&cur);
        if (sys != NULL && sys->log_debug_mode != cur) {
            (void)nc_log_set_levels(sys->log_debug_mode);
            ESP_LOGW(TAG, "log_debug_mode=%u（即時生效，並記住下次開機）",
                     (unsigned)sys->log_debug_mode);
        }
    }
    node_identity_init();
    MAIN_HWM("post-ident");
    ensure_volume_label();
    load_net_tuning();

    // ── ③④ 硬體 / Driver：先確認「有沒有這個硬體」──
#if CONFIG_IDF_TARGET_ESP32P4
    esp_err_t nerr = net_eth_start();
    s_eth_ok = (nerr == ESP_OK);
    if (s_eth_ok) {
#ifdef NC_ENABLE_TESTS
        nc_test_tcp_start();   // 吞吐量測端點（5000–5003）
#endif
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
#ifdef NC_ENABLE_TESTS
    nc_task_register(nc_test_probe_task());   // L3：開機自測（hub 交換）
    nc_task_register(&s_task_fx);      // L4：效果 arena + render task
    nc_task_register(nc_hello_task());  // L6：最小框架自測（每秒 Hello World）
    nc_task_register(nc_bench_task_a()); // L4：量測驗證用（預設 (0,0) = 唔跑）
    nc_task_register(nc_bench_task_b());
    nc_task_register(nc_bench_task_c());
#endif

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
#ifdef NC_ENABLE_TESTS
    if (nc_task_started("fx")) {
        nc_fx_scan("/data/pixel/effects");   // 效果類（kind = effect）
        nc_fx_scan("/data/tasks");           // 任務類（kind = normal）
        nc_task_sync_cfg();                  // 掃到的 .fxe 也補進 tasks.json
    }
#endif

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
    MAIN_HWM("post-usb");
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB 初始化失敗: %s", esp_err_to_name(err));
    }

#if !CONFIG_IDF_TARGET_ESP32P4
    // log 管道要在 TinyUSB 起來之後、FAT 交接之前啟動：
    //   · 之前：log 直接去 USB-Serial-JTAG（開機一定有）
    //   · 之後：JTAG 的 PHY 被 OTG 拿走了，log 改走 CDC（同一個出口，由 arbiter 仲裁）
    if (err == ESP_OK) {
        int lc = nc4_router_core("CDC");
        if (lc < 0) {
            lc = 0;      // CDC 未路由：仍要有一條出路，記錄喺 boot log
        }
        const esp_err_t lerr = nc_log_start(lc, cdc_tx_text);
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "log 管道未啟動：%s → 仍然直出 console", esp_err_to_name(lerr));
        }
    }
#endif

#if CONFIG_IDF_TARGET_ESP32P4
    // P4 這版沒有 USB（TinyUSB 沒列舉成功）→ 不要把 FAT 交給 USB，
    // 否則裝置自己與網路都讀不到 /data（就沒有替代的檔案存取方式了）。
    err = ESP_ERR_NOT_SUPPORTED;
    ESP_LOGW(TAG, "P4 無 USB → /data 留在 APP，改用 http://<ip>:8080/config.json");
#else
    if (err == ESP_OK) {
        const esp_err_t herr = storage_hand_over_to_usb();
        ESP_LOGI(TAG, "碟交給 host: %s", esp_err_to_name(herr));
    } else {
        // USB 沒起來就不要交 FAT：主機看不到碟，裝置自己也會失去 /data。
        // 留在 APP 至少 config／seed／HTTP 都仍然可用，唔會兩邊都拿唔到。
        ESP_LOGE(TAG, "USB 未起來 → /data 留在 APP（唔交 FAT）");
    }
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
            int cdc_core = 0;
            // config 已型別化：link_rx 只係由 struct 抄數，冇檔案 I/O，
            // 所以「FAT 交咗俾 host」唔再影響這裡（舊問題已消失）。
            link_rx_t rx = { .region = CDC_RX_REGION_DEF };
            (void)link_rx("CDC", &rx);
            rx.region = cdc_region(rx.region);
            if (s_nc4_rx_region == NULL || !link_core("CDC", &cdc_core)) {
                berr = ESP_ERR_INVALID_STATE;   // 冇緩衝，或者 routes[] 冇指去 decodeN
            } else {
                const nc4_bus_cfg_t cdc_bus = {
                    .name = "CDC",
                    // PUSH：由 cdc_rx_task（同核）讀走放環，decode 只消費。
                    // 之前係 PULL → decode 每 10 ms 親自讀一次，實測 RTT = 10.0 ms。
                    .mode = NC4_BUS_PUSH,
                    .core = cdc_core,
                    .read = cdc_read,       // 由 cdc_rx_task 呼叫（非阻塞）
                    .send = cdc_send,
                    .rx_region = s_nc4_rx_region,     // plan 給的緩衝
                    .rx_region_bytes = s_nc4_rx_bytes,
                    .read_chunk = rx.chunk,
                    .budget_per_pass = rx.budget,
                    .rx_ring_size = rx.slot,
                    .rx_slots = rx.slots,
                };
                nc4_bus_handle_t cdc_handle = (nc4_bus_handle_t)0;
                berr = nc4_bus_add(s_decoder, &cdc_bus, &cdc_handle);
                if (berr == ESP_OK && cdc_handle != NULL) {
                    if (xTaskCreatePinnedToCore(cdc_rx_task, "cdc.rx", CDC_RX_TASK_STACK,
                                                cdc_handle, CDC_RX_TASK_PRIO,
                                                &s_cdc_rx_task,
                                                (BaseType_t)cdc_core) != pdPASS) {
                        ESP_LOGE(TAG, "cdc.rx 建立失敗");
                        berr = ESP_ERR_NO_MEM;
                    } else {
                        nc4_bus_set_producer(cdc_handle, (void *)s_cdc_rx_task);
                        nc4_bus_set_cfg_src(cdc_handle, s_cdc_cfg_src);
                        // 核心隔離的開機自檢：receiver 同 decode 必須同核，
                        // 否則 kick 會變成跨核 notify（設計禁止）。
                        const int rcore = (int)xTaskGetCoreID(s_cdc_rx_task);
                        if (rcore != cdc_core) {
                            ESP_LOGE(TAG, "核隔離失敗：cdc.rx 喺 core %d，但 bus 屬於 core %d",
                                     rcore, cdc_core);
                        } else {
                            ESP_LOGI(TAG, "核隔離：cdc.rx 同 decode%d 同喺 core %d",
                                     cdc_core, cdc_core);
                        }
                        const esp_err_t rerr = tinyusb_cdcacm_register_callback(
                            TINYUSB_CDC_ACM_0, CDC_EVENT_RX, cdc_rx_notify);
                        ESP_LOGI(TAG, "CDC receiver：core %d、%d 槽 × %d B、RX callback %s",
                                 cdc_core, CDC_RX_SLOTS, CDC_RX_SLOT,
                                 esp_err_to_name(rerr));
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "CDC：links[].enable=0 → 唔建立（連緩衝都冇要）");
        }
#endif

        int uart_core = -1;
        if (s_uart_rx_region != NULL && link_core("UART", &uart_core)) {
            link_rx_t rx = { .region = 1024, .chunk = 512, .budget = 8, .slots = 4, .slot = 1024 };
            const int uapplied = link_rx("UART", &rx);   // config 真的寫了幾個 rx 欄位
            const nc4_bus_cfg_t uart_bus = {
                .name = "UART",
                // PUSH：driver 由 owner core 的 producer 讀，讀到就 push + kick（同核）
                // kind=uart：driver 的 read 會阻塞，所以一定要 producer，唔可以 PULL。
                .mode = NC4_BUS_PUSH,
                .core = uart_core,
                .owner_init = uart_owner_init,   // driver 喺 bus 所屬核初始化
                .read = uart_bus_read,
                .rx_region = s_uart_rx_region,
                .rx_region_bytes = rx.region,
                .send = NULL,
                .read_chunk = rx.chunk,
                .budget_per_pass = rx.budget,
                .rx_ring_size = rx.slot,
                .rx_slots = rx.slots,
            };
            nc4_bus_handle_t uart_handle = (nc4_bus_handle_t)0;
            esp_err_t uerr = nc4_bus_add(s_decoder, &uart_bus, &uart_handle);
            if (uerr == ESP_OK) {
                // producer **唔喺這裡建立**：要等 decode 任務起咗（owner_init 裝 driver）
                // 才可以開始讀。見 nc_task_boot() 之後的說明。
                s_uart_bus = uart_handle;
                nc4_bus_set_cfg_src(uart_handle, uapplied);   // STATUS 的 cfg_src
            }
            ESP_LOGI(TAG, "UART bus: %s", esp_err_to_name(uerr));
        } else {
            ESP_LOGW(TAG, "UART：未啟用（links[].enable=0）或冇緩衝 → 唔建立");
        }

#if CONFIG_IDF_TARGET_ESP32P4
        // TCP 傳輸只在 P4（乙太網）且「ETH 真的起來」時建立
        int tcp_core = -1;
        if (s_eth_ok && s_tcp_rx_region != NULL && link_core("TCP", &tcp_core)) {
            link_rx_t rx = { .region = 16410, .chunk = 8192, .budget = 8, .slots = 8, .slot = 8205 };
            link_rx("TCP", &rx);
            const nc4_bus_cfg_t tcp_bus = {
                .name = "TCP",
                .mode = NC4_BUS_PULL,          // pull：decode 喺 bus 所屬核直接 recv（非阻塞）
                .core = tcp_core,
                .read = tcp_nc4_read,          // 只在 PULL 模式使用
                .rx_ring_size = rx.slot,        // 每格剛好一幀（PUSH 模式用；留著等將來）
                .rx_slots = rx.slots,
                .send = tcp_nc4_send,
                .rx_region = s_tcp_rx_region,
                .rx_region_bytes = rx.region,
                .read_chunk = rx.chunk,
                .budget_per_pass = rx.budget,
                .frame_aligned = s_frame_align ? 1 : 0,
            };
            nc4_bus_handle_t tcp_handle = (nc4_bus_handle_t)0;
            esp_err_t terr = nc4_bus_add(s_decoder, &tcp_bus, &tcp_handle);
            ESP_LOGI(TAG, "NC4/TCP bus: %s", esp_err_to_name(terr));
            s_tcp_nc4_bus = tcp_handle;
            ESP_LOGW(TAG, "NC4/TCP：PULL（decode 喺 bus 所屬核讀 socket）%s",
                     s_frame_align ? "，frame-aligned" : "");
        }
#endif
        ESP_LOGI(TAG, "NC4 通道: %s", esp_err_to_name(berr));

        // 任務已經由各模組在 start 時登記（decode0/1、fx、log）；
        // 這裡只負責「依 layer 啟動」，不再手動 xTaskCreate 任何 loop。
        nc_task_boot();
        MAIN_HWM("post-boot");

        // producer 一定要喺 decode 起咗之後才建立：
        //   decode 的 entry 會跑 owner_init（裝 UART driver）才開始等；
        //   太早建立 producer，佢會喺「driver 未好」時自旋，而且優先級一旦高過
        //   decode 就會把整個核餓死（實測：開機停在 USB 已列舉、但冇任何回應）。
        // prio 由 nc4_bus_start_producer 夾到 decode 之下（不變式寫在那裡）。
        if (s_uart_bus != NULL) {
            const esp_err_t perr = nc4_bus_start_producer(s_uart_bus, "uart.rx", 3, 4096);
            ESP_LOGI(TAG, "UART producer: %s", esp_err_to_name(perr));
        }
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
