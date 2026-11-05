#include "nc_store.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "diskio_impl.h"       // ff_diskio_unregister
#include "diskio_wl.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "nvs.h"               // boot/fs_format：STATUS_SET 的「下次開機格式化」旗標
#include "tinyusb_msc.h"
#include "wear_levelling.h"
#include "tusb.h"              // tud_disconnect / tud_connect：強制 host 重新列舉

static const char *TAG = "store";

#define APP_BASE     "/data"
#define APP_DRIVE    "0:"
#define RO_BASE      "/ro"
#define RO_DRIVE     "1:"
#define RO_PDRV      1
#define PART_LABEL   "data"
#define NVS_NS       "boot"
#define NVS_FMT_KEY  "fs_format"

static const esp_partition_t *s_part;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc;
static volatile nc_nc_store_mode_t s_mode = NC_STORE_MODE_UNAVAILABLE;
static SemaphoreHandle_t s_lock;
static bool s_driver_installed;
static int s_usb_core = tskNO_AFFINITY;  // USB 服務組的核心（MSC driver 安裝時決定）
static int s_fmt_req = -1;               // -1 = 未讀；0/1 = NVS boot/fs_format 的快取
static bool s_app_mounted;
static FATFS *s_ro_fs;
static bool s_ro_mounted;

static const char *const k_names[] = {
    [NC_STORE_MODE_UNAVAILABLE] = "unavailable",
    [NC_STORE_MODE_APP] = "app",
    [NC_STORE_MODE_USB] = "usb",
    [NC_STORE_MODE_SWITCHING] = "switching",
};

// ───────────────────────── APP：自己掛 VFS ─────────────────────────

// allow_format 由呼叫端決定：只有「第一次初始化」才會 true。
// 之後的開機（包括 claim APP 的 runtime 重掛）一律 false —— 掛唔上就
// 保持原狀，唔會因為一次 mount 失敗就清掉使用者的資料。
static esp_err_t open_app(bool allow_format)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = allow_format,
        // 5 太細：console 的 stdin/stdout/stderr 已佔 3 個，只剩 2 個。
        // config 讀取（nc_cfg_open）會連續開檔，撞到上限就無聲回 NULL
        // ⇒ 上層以為「冇 config」而靜靜用預設。
        .max_files = 12,
    };
    const esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(APP_BASE, PART_LABEL, &cfg, &s_wl);
    s_app_mounted = (err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "掛載 /data 失敗：%s", esp_err_to_name(err));
        // IDF 的 mount 失敗路徑只會 unregister pdrv／VFS，**唔會** wl_unmount；
        // handle 留在 s_wl 只會洩漏，而且之後 nc_store_format() 再 wl_mount
        // 會多開一份 WL 狀態。這裡自己收乾淨。
        if (s_wl != WL_INVALID_HANDLE) {
            (void)wl_unmount(s_wl);
            s_wl = WL_INVALID_HANDLE;
        }
    }
    return err;
}

static void close_app(void)
{
    if (s_wl != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(APP_BASE, s_wl);   // 連 WL 一起卸載
        s_wl = WL_INVALID_HANDLE;
    }
    s_app_mounted = false;
}

// ───────────────────────── USB 模式下的唯讀視圖 ─────────────────────────
//
// 主機擁有 FAT 時，裝置自己仍然「讀」得到 —— 用同一個 WL handle 再掛一份
// 唯讀 FatFS（diskio 支援多個 pdrv 指向同一個 handle，WL 每個 handle 有鎖）。
// 限制：主機正在寫入時，讀到的內容可能過期或半截（不會寫壞，因為我們只讀）。

static esp_err_t ro_open(void)
{
    if (s_ro_mounted) {
        return ESP_OK;
    }
    if (s_wl == WL_INVALID_HANDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ff_diskio_register_wl_partition(RO_PDRV, s_wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "唯讀視圖：diskio 註冊失敗（%s）", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_conf_t conf = { .base_path = RO_BASE, .fat_drive = RO_DRIVE, .max_files = 2 };
    err = esp_vfs_fat_register(&conf, &s_ro_fs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "唯讀視圖：VFS 註冊失敗（%s）", esp_err_to_name(err));
        ff_diskio_unregister(RO_PDRV);
        return err;
    }

    const FRESULT fr = f_mount(s_ro_fs, RO_DRIVE, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "唯讀視圖：掛載失敗（FRESULT %d）", (int)fr);
        esp_vfs_fat_unregister_path(RO_BASE);
        ff_diskio_unregister(RO_PDRV);
        s_ro_fs = NULL;
        return ESP_FAIL;
    }

    s_ro_mounted = true;
    ESP_LOGI(TAG, "唯讀視圖已掛載（%s）", RO_BASE);
    return ESP_OK;
}

static void ro_close(void)
{
    if (!s_ro_mounted) {
        return;
    }
    f_mount(NULL, RO_DRIVE, 0);
    esp_vfs_fat_unregister_path(RO_BASE);
    ff_diskio_unregister(RO_PDRV);
    s_ro_fs = NULL;
    s_ro_mounted = false;
    ESP_LOGI(TAG, "唯讀視圖已卸載");
}

// ───────────────────────── USB：有 LUN 才算 ─────────────────────────

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

static esp_err_t open_usb(void)
{
    esp_err_t err = wl_mount(s_part, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount 失敗：%s", esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
        return err;
    }

    // 絕不自動格式化：一個掛載失敗就把使用者 7 MB 的檔案清掉，代價太大
    tinyusb_msc_storage_config_t cfg = {
        .medium.wl_handle = s_wl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 5,
            },
            .do_not_format = true,
        },
    };

    err = tinyusb_msc_new_storage_spiflash(&cfg, &s_msc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建立 USB storage 失敗：%s", esp_err_to_name(err));
        s_msc = NULL;
        wl_unmount(s_wl);
        s_wl = WL_INVALID_HANDLE;
    }
    return err;
}

static void close_usb(void)
{
    if (s_msc != NULL) {
        // 拆 LUN：主機立刻看不到這顆碟，READ10/WRITE10 全部失效
        const esp_err_t err = tinyusb_msc_delete_storage(s_msc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete_storage：%s", esp_err_to_name(err));
        }
        s_msc = NULL;
    }
    if (s_wl != WL_INVALID_HANDLE) {
        wl_unmount(s_wl);
        s_wl = WL_INVALID_HANDLE;
    }
}

// ───────────────────────── 強制重新列舉 ─────────────────────────
//
// 拆掉/重建 LUN 之後，主機不會自己發現（它只在列舉時問 Get Max LUN）。
// 實測：release 之後 macOS 要幾分鐘才重掛，有時完全不回來。
// 所以在切換後重拉一次 D+/D-，讓 host 重新列舉 —— CDC 也會斷一下（幾百 ms），
// 所以等回覆送出去之後才做。

static void reconnect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));      // 留時間把 NC4 回覆送出去（client 要在 0.8s 內讀走）
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    tud_connect();
    ESP_LOGI(TAG, "USB 已重新列舉");
    vTaskDelete(NULL);
}

static void usb_reconnect(void)
{
    // 同 TinyUSB task 同核：tud_disconnect/connect 會動 USB 狀態機，
    // 落在另一核就係跨核競態（而且會由 core0 的 tick 叫醒）。
    const BaseType_t core = (BaseType_t)s_usb_core;
    if (xTaskCreatePinnedToCore(reconnect_task, "usb_reconn", 3072, NULL, 5, NULL,
                                core) != pdPASS) {
        ESP_LOGW(TAG, "無法建立 reconnect task");
    }
}

// ───────────────────────── 對外 ─────────────────────────

// USB 服務組的一員：MSC driver 安裝。
// 由 main 喺 router 之後、USB 所屬核執行（TinyUSB 之前），咁 MSC 同 CDC 同核。
esp_err_t nc_store_usb_driver(void)
{
    if (s_driver_installed) {
        return ESP_OK;
    }

    // 記住安裝核：async IO worker 同之後的 usb_reconn 都要釘在同一核，
    // 否則「USB 服務組同核」只係一句註解（預設 tskNO_AFFINITY 會四處跑）。
    s_usb_core = (int)xPortGetCoreID();
    const tinyusb_msc_driver_config_t drv_cfg = {
        .callback = msc_event_cb,
        // 主人只由 nc_store 的 switch_to() 決定。esp_tinyusb 預設會在
        // tud_mount_cb / tud_umount_cb 自動把 FAT 掛給 host／掛回 APP：
        // 那會同 s_mode、/ro 唯讀視圖打架 —— 同一個 WL handle 被掛到多個 pdrv，
        // 之後 unmount 可能拆錯那一份，碟就初始化唔返。
        .user_flags.auto_mount_off = 1,
        .async_io_task = {
            .size = TINYUSB_MSC_ASYNC_IO_TASK_DEFAULT_SIZE,
            .priority = TINYUSB_MSC_ASYNC_IO_TASK_DEFAULT_PRIO,
            .xCoreID = s_usb_core,
        },
    };
    const esp_err_t err = tinyusb_msc_install_driver(&drv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC driver 安裝失敗：%s", esp_err_to_name(err));
        return err;
    }
    s_driver_installed = true;
    ESP_LOGI(TAG, "MSC driver 已安裝（async_io → core %d）", s_usb_core);
    return ESP_OK;
}

esp_err_t nc_store_init(const esp_partition_t *part, bool allow_format)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_part = part;

    if (part == NULL) {
        s_mode = NC_STORE_MODE_UNAVAILABLE;
        ESP_LOGW(TAG, "沒有 data 分區 → FAT 不可用");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t err = open_app(allow_format);   // 開機先留在 APP（寫出廠預置）
    s_mode = (err == ESP_OK) ? NC_STORE_MODE_APP : NC_STORE_MODE_UNAVAILABLE;
    ESP_LOGI(TAG, "FAT 擁有者：%s（allow_format=%d）", nc_store_mode_name(), allow_format ? 1 : 0);
    return err;
}

// 明確的重建路徑：抹除 → 重新建立 FAT。
// 只應該在「第一次初始化失敗」或使用者明確要求 factory reset 時呼叫。
// 分區抹完之後一定要用 allow_format=true 重新 init，否則 FatFS 只會回
// 「No filesystem」，碟就永遠停在「需要格式化」。
esp_err_t nc_store_format(const esp_partition_t *part)
{
    if (part == NULL) {
        part = s_part;
    }
    if (part == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    // 先按目前模式拆乾淨。正常「init 失敗」時兩邊都未建立，這裡只是防禦性處理：
    //   USB 模式：先拆 LUN／唯讀視圖，再放 WL
    //   APP 模式：先卸 VFS（連 WL 一起卸）
    ro_close();
    if (s_mode == NC_STORE_MODE_USB) {
        close_usb();
        close_app();
    } else {
        close_app();
        close_usb();
    }
    s_mode = NC_STORE_MODE_UNAVAILABLE;

    ESP_LOGW(TAG, "抹除 data 分區 0x%08" PRIx32 "（%u KB）→ 重新格式化",
             part->address, (unsigned)(part->size / 1024));
    const esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "抹除 data 分區失敗：%s", esp_err_to_name(err));
        return err;
    }

    // 分區已空白：這次重新 init 一定要允許格式化。
    s_part = part;
    xSemaphoreGive(s_lock);

    return nc_store_init(part, true);
}

// 讀 NVS `boot/fs_format`（只讀一次，之後用快取）。
// 為什麼放 NVS 而不是 /data/config：STATUS_SET 到來時 FAT 通常由 host 持有
//（USB 模式），裝置寫唔到 /data；而且「格式化請求」本身唔應該依賴它要格式化的 FS。
bool nc_store_format_requested(void)
{
    if (s_fmt_req >= 0) {
        return s_fmt_req != 0;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_fmt_req = 0;
        return false;
    }
    uint8_t v = 0;
    const esp_err_t e = nvs_get_u8(h, NVS_FMT_KEY, &v);
    nvs_close(h);
    s_fmt_req = (e == ESP_OK && v != 0) ? 1 : 0;
    return s_fmt_req != 0;
}

// 0x1103 STATUS_SET 的 System.fs_format：只寫旗標，唔會喺解碼路徑抹 7 MB。
// 下一次 storage_init() 見到旗標才 nc_store_format()；成功後由 main 清掉。
void nc_store_request_format(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 開唔到 → fs_format 未寫入");
        return;
    }
    (void)nvs_set_u8(h, NVS_FMT_KEY, on ? 1 : 0);
    (void)nvs_commit(h);
    nvs_close(h);
    s_fmt_req = on ? 1 : 0;
    ESP_LOGW(TAG, "fs_format=%d（下次開機%s格式化 data 分區）",
             on ? 1 : 0, on ? "會" : "唔會");
}

nc_nc_store_mode_t nc_store_mode(void)
{
    return s_mode;
}

const char *nc_store_mode_name(void)
{
    const nc_nc_store_mode_t m = s_mode;
    return (m >= 0 && m <= NC_STORE_MODE_SWITCHING) ? k_names[m] : "?";
}

bool nc_store_app_ready(void)
{
    return s_mode == NC_STORE_MODE_APP && s_app_mounted;
}

const char *nc_store_fs_base(void)
{
    if (s_mode == NC_STORE_MODE_APP && s_app_mounted) {
        return APP_BASE;
    }
    if (s_mode == NC_STORE_MODE_USB && s_ro_mounted) {
        return RO_BASE;
    }
    return NULL;
}

const char *nc_store_fs_drive(void)
{
    if (s_mode == NC_STORE_MODE_APP && s_app_mounted) {
        return APP_DRIVE;
    }
    if (s_mode == NC_STORE_MODE_USB && s_ro_mounted) {
        return RO_DRIVE;
    }
    return NULL;
}

static esp_err_t switch_to(nc_nc_store_mode_t want)
{
    if (s_part == NULL || !s_driver_installed) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_mode == want) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_mode == NC_STORE_MODE_SWITCHING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const nc_nc_store_mode_t prev = s_mode;
    s_mode = NC_STORE_MODE_SWITCHING;

    esp_err_t err;
    if (want == NC_STORE_MODE_APP) {
        ro_close();                  // 先放掉唯讀視圖，才能接管整個 FatFS
        close_usb();
        ESP_LOGW(TAG, "從 host 收回 FAT（主機端若還掛著，會看到「未正確退出」）");
        err = open_app(false);       // runtime 重掛：永遠唔准自動格式化
    } else {
        close_app();
        ESP_LOGI(TAG, "交出 FAT 給 host");
        err = open_usb();
        if (err == ESP_OK) {
            ro_open();               // 主機用碟的同時，裝置仍可讀（唯讀視圖）
        }
    }

    if (err != ESP_OK) {
        // 回不去時至少讓裝置自己還能用
        ESP_LOGE(TAG, "切換失敗 → 嘗試回到 %s", k_names[prev]);
        if (want == NC_STORE_MODE_APP) {
            close_app();
            err = open_usb();
        } else {
            close_usb();
            err = open_app(false);   // 回落路徑同樣唔准格式化
        }
        s_mode = (err == ESP_OK) ? prev : NC_STORE_MODE_UNAVAILABLE;
    } else {
        s_mode = want;
        ESP_LOGI(TAG, "FAT 擁有者 → %s", nc_store_mode_name());
        // 只在交還給 host 時重拉：claim 之後 LUN 已經拆掉，主機的 I/O 自然失敗，
        // 不需要為此把 CDC（NC4）也斷掉 —— 用戶正在用那條線下指令。
        if (want == NC_STORE_MODE_USB) {
            usb_reconnect();
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nc_store_claim_app(void)
{
    return switch_to(NC_STORE_MODE_APP);
}

esp_err_t nc_store_release_usb(void)
{
    return switch_to(NC_STORE_MODE_USB);
}
