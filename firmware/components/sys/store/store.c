#include "store.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "diskio_impl.h"       // ff_diskio_unregister
#include "diskio_wl.h"
#include "esp_vfs_fat.h"
#include "ff.h"
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

static const esp_partition_t *s_part;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc;
static volatile store_mode_t s_mode = STORE_MODE_UNAVAILABLE;
static SemaphoreHandle_t s_lock;
static bool s_driver_installed;
static bool s_first_mount = true;      // 只有第一次允許自動格式化
static bool s_app_mounted;
static FATFS *s_ro_fs;
static bool s_ro_mounted;

static const char *const k_names[] = {
    [STORE_MODE_UNAVAILABLE] = "unavailable",
    [STORE_MODE_APP] = "app",
    [STORE_MODE_USB] = "usb",
    [STORE_MODE_SWITCHING] = "switching",
};

// ───────────────────────── APP：自己掛 VFS ─────────────────────────

static esp_err_t open_app(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = s_first_mount,   // 全新分區才准格式化
        .max_files = 5,
    };
    const esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(APP_BASE, PART_LABEL, &cfg, &s_wl);
    s_first_mount = false;
    s_app_mounted = (err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "掛載 /data 失敗：%s", esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
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
    if (xTaskCreate(reconnect_task, "usb_reconn", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "無法建立 reconnect task");
    }
}

// ───────────────────────── 對外 ─────────────────────────

esp_err_t store_init(const esp_partition_t *part)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_part = part;

    if (part == NULL) {
        s_mode = STORE_MODE_UNAVAILABLE;
        ESP_LOGW(TAG, "沒有 data 分區 → FAT 不可用");
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_driver_installed) {
        const tinyusb_msc_driver_config_t drv_cfg = { .callback = msc_event_cb };
        const esp_err_t err = tinyusb_msc_install_driver(&drv_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MSC driver 安裝失敗：%s", esp_err_to_name(err));
            s_mode = STORE_MODE_UNAVAILABLE;
            return err;
        }
        s_driver_installed = true;
    }

    const esp_err_t err = open_app();          // 開機先留在 APP（寫出廠預置）
    s_mode = (err == ESP_OK) ? STORE_MODE_APP : STORE_MODE_UNAVAILABLE;
    ESP_LOGI(TAG, "FAT 擁有者：%s", store_mode_name());
    return err;
}

store_mode_t store_mode(void)
{
    return s_mode;
}

const char *store_mode_name(void)
{
    const store_mode_t m = s_mode;
    return (m >= 0 && m <= STORE_MODE_SWITCHING) ? k_names[m] : "?";
}

bool store_app_ready(void)
{
    return s_mode == STORE_MODE_APP && s_app_mounted;
}

const char *store_fs_base(void)
{
    if (s_mode == STORE_MODE_APP && s_app_mounted) {
        return APP_BASE;
    }
    if (s_mode == STORE_MODE_USB && s_ro_mounted) {
        return RO_BASE;
    }
    return NULL;
}

const char *store_fs_drive(void)
{
    if (s_mode == STORE_MODE_APP && s_app_mounted) {
        return APP_DRIVE;
    }
    if (s_mode == STORE_MODE_USB && s_ro_mounted) {
        return RO_DRIVE;
    }
    return NULL;
}

static esp_err_t switch_to(store_mode_t want)
{
    if (s_part == NULL || !s_driver_installed) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_mode == want) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_mode == STORE_MODE_SWITCHING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const store_mode_t prev = s_mode;
    s_mode = STORE_MODE_SWITCHING;

    esp_err_t err;
    if (want == STORE_MODE_APP) {
        ro_close();                  // 先放掉唯讀視圖，才能接管整個 FatFS
        close_usb();
        ESP_LOGW(TAG, "從 host 收回 FAT（主機端若還掛著，會看到「未正確退出」）");
        err = open_app();
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
        if (want == STORE_MODE_APP) {
            close_app();
            err = open_usb();
        } else {
            close_usb();
            err = open_app();
        }
        s_mode = (err == ESP_OK) ? prev : STORE_MODE_UNAVAILABLE;
    } else {
        s_mode = want;
        ESP_LOGI(TAG, "FAT 擁有者 → %s", store_mode_name());
        // 只在交還給 host 時重拉：claim 之後 LUN 已經拆掉，主機的 I/O 自然失敗，
        // 不需要為此把 CDC（NC4）也斷掉 —— 用戶正在用那條線下指令。
        if (want == STORE_MODE_USB) {
            usb_reconnect();
        }
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t store_claim_app(void)
{
    return switch_to(STORE_MODE_APP);
}

esp_err_t store_release_usb(void)
{
    return switch_to(STORE_MODE_USB);
}
