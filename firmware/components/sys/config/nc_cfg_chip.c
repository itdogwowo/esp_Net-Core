// 晶片 USB 能力表（定義見 nc_cfg_chip.h 的說明）。
//
// 數值全部來自 ESP-IDF v6.0.3 的 soc_caps.h，不是憑記憶：
//   S2：SOC_USB_OTG_SUPPORTED=1、冇 USJ
//   S3：OTG=1、USJ=1（共用 FSLS PHY）
//   P4：OTG=1、USJ=1、UTMI PHY ×1（HS）
//   C3/C5/C6/C61/H2：只有 USJ=1
//   ESP32：兩者皆無
// 核數是 FreeRTOS 的 portNUM_PROCESSORS。
//
// 出廠預設：**有 USJ 就用 USJ**（ROM 自帶 CDC-ACM，開機就有、唔靠 TinyUSB，
// 而且 esptool／console 都喺同一條線 → 最適合第一次 bring-up）。
// 只有 OTG、冇 USJ 的晶片（S2）才預設 "CDC"。
// 兩者的 link 名同 kind 不同；用戶改 config 的 kind 就是「換 personality」。
//
// 呢張表要同 tools/board.py 的 CHIP_DIR 保持同一批 target；新增晶片時兩邊一齊加。
#include "nc_cfg_chip.h"

#include <stddef.h>
#include <string.h>

static const nc_cfg_chip_t k_chips[] = {
    { .id = "esp32s3", .cores = 2, .usb_otg = true, .usb_serial_jtag = true,
      .usb_shared_phy = true, .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32s2", .cores = 1, .usb_otg = true,
      .usb_link = "CDC", .usb_kind = NC_LINK_CDC,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32p4", .cores = 2, .usb_otg = true, .usb_otg_hs = true,
      .usb_serial_jtag = true, .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32c3", .cores = 1, .usb_serial_jtag = true,
      .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32c5", .cores = 1, .usb_serial_jtag = true,
      .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32c6", .cores = 1, .usb_serial_jtag = true,
      .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32c61", .cores = 1, .usb_serial_jtag = true,
      .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    { .id = "esp32h2", .cores = 1, .usb_serial_jtag = true,
      .usb_link = "usb_jtag", .usb_kind = NC_LINK_USBJTAG,
      .usb_rx_region = NC_CFG_USB_RX_REGION },
    // 冇 USB 的晶片也要有一行：預設會因此只剩 UART，而不是硬砌一條永遠起不來的 CDC。
    { .id = "esp32", .cores = 2 },
};

const nc_cfg_chip_t *nc_cfg_chip_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_chips) / sizeof(k_chips[0]); i++) {
        if (strcmp(k_chips[i].id, id) == 0) {
            return &k_chips[i];
        }
    }
    return NULL;
}
