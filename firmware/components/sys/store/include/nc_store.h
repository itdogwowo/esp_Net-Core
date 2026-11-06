// FAT 分區的擁有者仲裁。
//
// 為什麼需要這一層：
//   data 分區只有一個 FAT，而且只能有一個主人 ——
//     交給 USB host  => 主機看到隨身碟，裝置看不到 /data
//     留在 APP       => 裝置可以 fopen("/data/…")，主機看不到碟
//
// 讀寫分離：主機擁有 FAT 時，裝置仍掛一份唯讀視圖（nc_store_fs_base() = "/ro"），
// 所以 FILE_QUERY 任何時候都答得到（可能過期）；寫入才需要接管。
//
// 為什麼不能只呼叫 tinyusb_msc_set_storage_mount_point()：
//   那個 API 只換「FatFS 掛在哪」，MSC 的 READ10/WRITE10 完全不看 mount_point，
//   只有 TEST UNIT READY 會回 not-ready（主機 mount 之後就不再問了）。
//   實測：切到 APP 後 host 寫 64 KiB 仍然真的落到 flash（7.3 秒）—— 兩邊同時寫同一塊。
//
// 所以這裡的做法是「有 LUN 才算 USB」：
//   APP 模式：不建立任何 MSC storage（LUN 不存在 → 主機寫不進來），
//             自己用 esp_vfs_fat_spiflash_mount_rw_wl() 掛 /data
//   USB 模式：拆掉 VFS，自己 wl_mount()，再建 MOUNT_USB 的 storage
// 切換 = 拆掉一邊、建立另一邊，兩者永遠不會同時存在。
//
// 使用規則：
//   · 要讀寫 /data 之前先問 nc_store_app_ready()
//   · 切換期間是 NC_STORE_MODE_SWITCHING，其他呼叫者拿到 ESP_ERR_INVALID_STATE
//
// 已知限制：從 USB 收回 FAT，對主機等於「直接拔線」。
//   主機快取裡未落盤的寫入會掉，可能出現「未正確退出」警告 —— 單一 FAT 的物理限制。
//   要安全就先在 host 端 eject，再送 STATUS_SET 把 System.file_mode 設成 1。
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_STORE_MODE_UNAVAILABLE = 0,   // 沒有 data 分區 / 掛載失敗
    NC_STORE_MODE_APP,               // 裝置擁有 FAT（/data 可用，主機看不到碟）
    NC_STORE_MODE_USB,               // 交給 host（隨身碟）
    NC_STORE_MODE_SWITCHING,         // 切換中
} nc_nc_store_mode_t;

// part = type data / subtype fat 的分區。store 負責 WL 與 FatFS 的整個生命週期。
esp_err_t nc_store_usb_driver(void);   // MSC driver 安裝（USB 組，開機 init）
uint16_t  nc_store_reconn_hwm(void);  // usb_reconn 臨終前的 stack 剩餘（0 = 未跑過）

// allow_format = 這次初始化可否在掛載失敗時格式化。
// **只有「第一次初始化／重建」傳 true**（呼叫端由 state manager 的 boot record 判斷）；
// 之後的開機一律傳 false：掛唔上就原樣保留，不會自動清掉使用者的資料。
esp_err_t nc_store_init(const esp_partition_t *part, bool allow_format);

// 明確的「重建 FAT」：抹除整個 data 分區 → 重新建立。
// **會清掉使用者全部檔案**，只應該在第一次初始化失敗、或使用者明確要求 factory reset 時呼叫。
// 它一定會用 allow_format=true 重新 init（因為分區已經空白）。
//
// 「什麼時候該重建」不是 store 的事 —— 那個決策在 state manager（nc_boot）。
esp_err_t nc_store_format(const esp_partition_t *part);

nc_nc_store_mode_t nc_store_mode(void);
const char *nc_store_mode_name(void);

// FAT 現在歸裝置？只有 true 時才能「寫」。
bool nc_store_app_ready(void);

// 可讀取的根路徑與對應的 FatFS drive：
//   APP 模式 → "/data" / "0:"（可讀可寫）
//   USB 模式 → "/ro"   / "1:"（唯讀視圖，主機仍在用；內容可能過期或半截）
//   NULL = 目前讀不到
const char *nc_store_fs_base(void);
const char *nc_store_fs_drive(void);

// 切換所有權。已在目標狀態時是 no-op。
esp_err_t nc_store_claim_app(void);     // USB  → APP（主機的碟會消失）
esp_err_t nc_store_release_usb(void);   // APP  → USB

#ifdef __cplusplus
}
#endif
