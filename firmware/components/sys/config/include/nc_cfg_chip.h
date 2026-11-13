// 晶片 USB 能力：**編譯期事實**，runtime 只讀。
//
// 為什麼要有它：出廠預設要「每塊板都用得到原生 USB」，但各晶片的 USB 長得唔同 ——
//
//   S2           OTG FS，冇 USB-Serial-JTAG
//   S3           OTG FS ＋ USJ，兩者共用同一條 PHY（二選一）
//   P4           OTG FS ＋ OTG HS（UTMI）＋ USJ，三條獨立
//   C3/C5/C6/C61/H2  只有 USJ（它本身就是一個 CDC-ACM）
//   ESP32        冇 USB
//
// 這些差異以前散落在 `#if NC_HAS_USB` 與每個 port 的 system.json，
// 沒有一處可以回答「這顆晶片出廠應該用哪條線、rx 要幾大、有幾核」。
// 這張表就是那個答案：`nc_cfg_defaults.c` 用它砌預設，main 用它決定
// USB 服務組的運輸方式與 decoder 數量。
//
// 邊界：**只用來砌預設／回報**。用戶寫在 system.json 的 links[] 仍然是唯一真相，
// 這張表不會覆蓋任何已存在的設定。
//
// 資料來源：ESP-IDF v6.0.3 `components/soc/<target>/include/soc/soc_caps.h`
// （SOC_USB_OTG_SUPPORTED／SOC_USB_SERIAL_JTAG_SUPPORTED）＋ P4 的 SOC_USB_UTMI_PHY_NUM。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nc_cfg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char    *id;              // CONFIG_IDF_TARGET 的字串："esp32s3" …
    uint8_t        cores;           // FreeRTOS 核數；單核板唔應該出現 decode1
    bool           usb_otg;         // 有 OTG 控制器（TinyUSB CDC／MSC 的載體）
    bool           usb_otg_hs;      // P4：OTG 可以走 UTMI 的 HS PHY
    bool           usb_serial_jtag; // 有 USB-Serial-JTAG（可當 CDC-ACM，也可當 console）
    bool           usb_shared_phy;  // OTG 與 USJ 共用同一條 PHY（S3/S2 是，P4 不是）
    const char    *usb_link;        // 出廠 USB 通道名；NULL = 這顆晶片沒有 USB
    nc_link_kind_t usb_kind;        // 對應的 link kind（cdc / usb_jtag）
    uint16_t       usb_rx_region;   // USB parser 緩衝的貼身值（bytes）
} nc_cfg_chip_t;

// 找不到 → NULL（呼叫端應該退回「不知道晶片」的保守預設並記錄）。
const nc_cfg_chip_t *nc_cfg_chip_find(const char *id);

#ifdef __cplusplus
}
#endif
