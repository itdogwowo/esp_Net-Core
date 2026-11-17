// config 的**格式**：struct 就是 schema。
//
// 規則：
//   · 純資料，不懂 JSON、不含 cJSON、沒有指標、沒有動態配置。
//   · 字串一律固定大小 + NUL；長度不夠就當「不合法」用預設（不截斷）。
//   · 每個結構第一欄是 version —— 泛型檢查與將來遷移都靠它。
//   · 陣列有固定上限，超出 = 不合法（fail-visible，不靜默丟掉）。
//
// 為什麼不放指標：
//   `router_from_cfg()` 以前把 cJSON 的字串指標交給 router，之後才 nc_cfg_close()
//   —— 那是 use-after-free。字串進 struct 之後，壽命天然等於設定本身。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_CFG_SCHEMA_VERSION   1

#define NC_CFG_USB_PORT_NONE 0xFF

#define NC_CFG_MAX_LINKS        8
#define NC_CFG_MAX_ROUTES       8
#define NC_CFG_MAX_OUT          4
#define NC_CFG_MAX_TASKS        20     // 與 NC_TASK_MAX 相同
#define NC_CFG_NAME_MAX         16     // 通道名（CDC / UART / TCP …）
#define NC_CFG_TASK_NAME_MAX    24
#define NC_CFG_CID_MAX          8      // "F5F9" + 餘裕
#define NC_CFG_REGION_MAX       65535  // network line 可能要收多幀；slot/region 用 uint16
// USB（TinyUSB CDC 與 USB-Serial-JTAG 共用）的 parser 緩衝貼身值。
//   最大 FILE_CHUNK 4,096 ＋ NC4 header 9 ＋ chunk header 6 ＋ CRC 4 = 4,115 B
//   向上 32 對齊 → 4,128（留 13 B 餘裕）
// 呢個係「今次 host 實際會送幾大」的答案，唔係協議上限：主機送更大的 chunk 時，
// FILE_BEGIN 應該即時拒絕（未實作；見 docs/03_notes/usb-config.md §2.5）。
// 以前的 4,192 係 4 KB + 96，純粹係手寫整數；4,128 一樣放得落 4 KB 檔，
// 每條 USB link 省 64 B。
#define NC_CFG_USB_RX_REGION    4128
#define NC_CFG_GPIO_MAX         54     // P4 最大 GPIO54（S3 只到 48；超出的值留給 driver 判錯）
#define NC_CFG_PHY_NAME_MAX     12     // "IP101" / "LAN8720" …

typedef enum {
    NC_LINK_CDC = 0,
    NC_LINK_UART,
    NC_LINK_TCP,
    NC_LINK_WS,
    NC_LINK_UDP,
    NC_LINK_HTTP,   // 這條線交給 web service（httpd），不進 NC4 parser
    // USB-Serial-JTAG 的資料通道（C3／C6／H2：沒有 OTG，但同一顆 peripheral
    // 本身就是一個 CDC-ACM，RX/TX 都是硬件 FIFO）。kind 名 = "usb_jtag"。
    NC_LINK_USBJTAG,
    NC_LINK_KIND_MAX,
} nc_link_kind_t;

// ── 字典 → 數值 ────────────────────────────────────────────────
// 這些欄位以前是 char[8]／char[6]，每個 link 就多 22 B，而且每次比較都要
// strcmp。JSON 仍然寫 "eth"／"nc4"／"sram"（人類可讀），進來 struct 就只剩
// 一個 byte；要印 log 再用 name() 轉回去。
typedef enum {
    NC_CFG_IFACE_ANY = 0,   // 空／未指定
    NC_CFG_IFACE_ETH,
    NC_CFG_IFACE_WIFI,
} nc_iface_t;

typedef enum {
    NC_CFG_SVC_AUTO = 0,    // 由 kind 推導：http→web，其餘→nc4
    NC_CFG_SVC_NC4,
    NC_CFG_SVC_WEB,
} nc_service_t;

typedef enum {
    NC_CFG_MEM_AUTO = 0,    // 先 PSRAM，配不到再 internal
    NC_CFG_MEM_SRAM,
    NC_CFG_MEM_PSRAM,
} nc_mem_t;

typedef enum {
    NC_CFG_PHY_UNKNOWN = 0,
    NC_CFG_PHY_IP101,
    NC_CFG_PHY_LAN8720,
    NC_CFG_PHY_DP83848,
    NC_CFG_PHY_RTL8201,
    NC_CFG_PHY_GENERIC,
} nc_phy_type_t;

// 一條 link 的 rx 參數。rx_mask 記錄「config 真的寫了邊幾項」，
// 這樣 STATUS 的 cfg_src 才是真話（而不是「套完之後看起來都一樣」）。
#define NC_CFG_RX_REGION     0x01u
#define NC_CFG_RX_CHUNK      0x02u
#define NC_CFG_RX_BUDGET     0x04u
#define NC_CFG_RX_SLOTS      0x08u
#define NC_CFG_RX_SLOT_SIZE  0x10u

typedef struct {
    uint16_t region;      // parser 緩衝（bytes）
    uint16_t chunk;       // 一次 read 上限
    uint16_t slot_size;   // PUSH 每格大小
    uint8_t  slots;       // PUSH 格數
    uint8_t  budget;      // 一輪最多讀幾次
} nc_link_rx_t;

typedef struct {
    char           name[NC_CFG_NAME_MAX];
    uint8_t        index;       // 同類線編號；name 省略時用來生成 eth_tcp0
    uint8_t        usb_port;    // USB 線 → hardware.json USB.list index；0xFF = 未指定
    int8_t         msc;         // USB link 的 MSC 開關：-1 = 未寫（用 global）、0/1
    nc_link_kind_t kind;
    bool           enable;
    uint16_t       port;        // tcp / ws / udp / http
    uint8_t        iface;       // nc_iface_t（any / eth / wifi）
    uint8_t        service;     // nc_service_t（auto / nc4 / web）
    uint8_t        mem;         // nc_mem_t（auto / sram / psram）
    bool           managed;     // config 有寫 iface/service → 交給 nc_net_lines
    uint8_t        rx_mask;     // config 實際寫了哪些 rx 欄位
    nc_link_rx_t   rx;
} nc_link_cfg_t;

// 一列路由：in = 來源（link 名 / nctx / outbound），out = 目的地。
// 目的地可以是 link（轉發）或 decodeN（本地執行），所以存字串，語意跟 JSON 一致。
typedef struct {
    char    in[NC_CFG_NAME_MAX];
    uint8_t n_out;
    char    out[NC_CFG_MAX_OUT][NC_CFG_NAME_MAX];
} nc_route_cfg_t;

#define NC_CFG_MOUNT_MAX  16

// ── hardware 層（hardware.json）：實體匯流排 ──
// 一個層一個檔，跟 system/tasks 同一個生命周期（seed → load → struct）。
// 已型別化：ETH、SDIO。其他段（PIN/SPI/I2C/I80/UART/I2S/PWM/ENC/XL9555）
// 只會被計入 unknown —— **改它們不會生效**，等有 driver 用到再逐個加型別。
#define NC_CFG_MAX_SDIO 2
#define NC_CFG_MAX_PWM  8
#define NC_CFG_MAX_UART 3
#define NC_CFG_MAX_PIN  16
#define NC_CFG_PIN_LABEL_MAX 16
#define NC_CFG_PWM_LABEL_MAX 16

// ETH（RMII）：跟 SDIO 一樣只出現一次。
// present 不是 enable 的別名，兩者語意不同：
//   present=0（config 冇寫 ETH 段）→ 呼叫端用**編譯期內建腳位**
//       —— hardware.json 缺檔／壞掉時板子仍然上得了網（failure-policy）；
//   present=1, enable=0            → 用戶明確關閉，唔起 netif。
typedef enum {
    NC_ETH_DRIVER_RMII = 0,    // 內建 EMAC + 外部 PHY（P4/S3）
    NC_ETH_DRIVER_SPI  = 1,    // SPI 接 PHY（W5500 類）；driver 未實作
} nc_eth_driver_t;

typedef struct {
    bool    present;
    bool    enable;
    uint8_t driver;            // nc_eth_driver_t
    int8_t  mdc;
    int8_t  mdio;
    int8_t  ref_clk;
    int8_t  spi_idx;           // SPI PHY：claim spi_list 的哪一條（index）
    int8_t  cs;
    int8_t  rst;
    int8_t  int_pin;
    int8_t  phy_addr;          // RMII：0–31；SPI 未用 = -1
    uint8_t phy_type;          // nc_phy_type_t（IP101 / LAN8720 …）；只影響 log
} nc_eth_cfg_t;

typedef struct {
    bool     enable;
    uint32_t freq_hz;      // 40000000
    uint8_t  width;        // 1 / 4
    int8_t   clk;          // SD 的 sck
    int8_t   cmd;
    int8_t   d[4];         // D0..D3（-1 = 未用）
} nc_sdio_cfg_t;

// PIN：舊專案 {GPIO,label,mode,pull,initial}；由 hw_gpio owner 在 on_start 套用。
typedef struct {
    int8_t   pin;
    uint8_t  mode;       // 0=OUT 1=IN
    uint8_t  pull;       // 0=none 1=up 2=down
    uint8_t  initial;    // OUT 初始電平
    char     label[NC_CFG_PIN_LABEL_MAX];
} nc_pin_cfg_t;

// PWM（LEDC）：舊專案只有 GPIO；freq／resolution／duty 係新增可選欄位。
typedef struct {
    int8_t   pin;              // -1 = 未設定
    uint32_t freq_hz;
    uint8_t  resolution_bits;  // 1..14
    uint16_t duty;             // 0..(2^bits-1)
    uint16_t d_stay;           // 停止／歸零時回到的 12-bit 中性值（舊專案 dStay）
    char     label[NC_CFG_PWM_LABEL_MAX];
} nc_pwm_cfg_t;

// UART：照舊專案 { id, baudrate, rxbuf, txbuf, en_settle_ms, GPIO{tx,rx,en} }。
typedef struct {
    bool     enable;
    uint8_t  id;               // UART port
    uint32_t baud;
    uint32_t rxbuf;
    uint32_t txbuf;
    int8_t   tx;
    int8_t   rx;
    int8_t   en;               // RS485 DE/RE；-1 = 純 UART
    uint16_t en_settle_ms;
} nc_uart_cfg_t;

typedef struct {
    uint16_t      version;
    nc_eth_cfg_t  eth;
    uint8_t       n_sdio;
    nc_sdio_cfg_t sdio[NC_CFG_MAX_SDIO];
    uint8_t       n_pin;
    nc_pin_cfg_t  pin[NC_CFG_MAX_PIN];
    uint8_t       n_pwm;
    nc_pwm_cfg_t  pwm[NC_CFG_MAX_PWM];
    uint8_t       n_uart;
    nc_uart_cfg_t uart[NC_CFG_MAX_UART];
} nc_hardware_cfg_t;

typedef struct {
    uint16_t       version;
    char           cid[NC_CFG_CID_MAX];    // "F5F9"；空 = 由 MAC 推導
    uint16_t       frame_interval_ms;
    uint16_t       num_pixels;
    uint8_t        n_links;
    nc_link_cfg_t  links[NC_CFG_MAX_LINKS];
    uint8_t        n_routes;
    nc_route_cfg_t routes[NC_CFG_MAX_ROUTES];
    uint8_t        log_debug_mode;  // 0 = 正常（2 KB、E/W/I、由 USB 起來開始記）｜1 = 除錯（8 KB、全部、連開機）
    // ── USB（執行期）。編譯期由 CONFIG_NETCORE_USB_OTG 決定有沒有這組；
    //    這裡決定要不要真的用，改完即時生效（唔使重燒）──
    uint8_t        usb_msc;         // 0 = 唔交碟（/data 留在 APP）｜1 = 開機交俾 host
    uint8_t        usb_cdc_log;     // 0 = log 只走 console｜1 = 開 log 管道（CDC）
} nc_system_cfg_t;

typedef struct {
    char   name[NC_CFG_TASK_NAME_MAX];
    bool   has_level;    // false = config 冇寫 level（不是 0！）→ 用固件宣告
    int8_t level;        // -1 = 停用
    bool   aff[2];       // [core0, core1]
} nc_task_cfg_t;

typedef struct {
    uint16_t      version;
    uint8_t       n_tasks;
    nc_task_cfg_t tasks[NC_CFG_MAX_TASKS];
} nc_tasks_cfg_t;

// 一層的載入結果（給 STATUS 看，不影響行為）
typedef struct {
    uint32_t missing;      // 檔不存在（1/0）
    uint32_t bad_json;     // 整份壞掉（1/0）
    uint32_t bad_version;  // version 不符（1/0）
    uint32_t defaulted;    // 缺欄位 → 用預設
    uint32_t invalid;      // 型別錯／超範圍 → 用預設
    uint32_t unknown;      // 不認識的鍵（只計數，向前兼容）
} nc_cfg_stat_t;

#ifdef __cplusplus
}
#endif
