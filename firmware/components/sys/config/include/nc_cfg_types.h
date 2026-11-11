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

#define NC_CFG_MAX_LINKS        8
#define NC_CFG_MAX_ROUTES       8
#define NC_CFG_MAX_OUT          4
#define NC_CFG_MAX_TASKS        20     // 與 NC_TASK_MAX 相同
#define NC_CFG_MAX_POOLS        4      // nc4.pool：small／bulk／forward（+1 spare）
#define NC_CFG_NAME_MAX         16     // 通道名（CDC / UART / TCP …）
#define NC_CFG_TASK_NAME_MAX    24
#define NC_CFG_CID_MAX          8      // "F5F9" + 餘裕
#define NC_CFG_REGION_MAX       8300   // = NC4_MAX_FRAME（8205）向上取整；不直接 include nc4
#define NC_CFG_GPIO_MAX         54     // P4 最大 GPIO54（S3 只到 48；超出的值留給 driver 判錯）
#define NC_CFG_PHY_NAME_MAX     12     // "IP101" / "LAN8720" …

typedef enum {
    NC_LINK_CDC = 0,
    NC_LINK_UART,
    NC_LINK_TCP,
    NC_LINK_WS,
    NC_LINK_UDP,
    // USB-Serial-JTAG 的資料通道（C3／C6／H2：沒有 OTG，但同一顆 peripheral
    // 本身就是一個 CDC-ACM，RX/TX 都是硬件 FIFO）。kind 名 = "usb_jtag"。
    NC_LINK_USBJTAG,
    NC_LINK_KIND_MAX,
} nc_link_kind_t;

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
    nc_link_kind_t kind;
    bool           enable;
    uint16_t       port;        // tcp / ws / udp
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

// nc4.pool 的一列：固定 size class 的緩衝池。
// where: 0 = internal，1 = PSRAM（PSRAM 不可給 GDMA 直接用；forward/UART 用 internal）。
typedef struct {
    char     name[NC_CFG_NAME_MAX];
    uint16_t size;        // 每格 bytes
    uint8_t  count;       // 幾格
    uint8_t  where;       // 0 internal / 1 psram
    bool     enable;
} nc_cfg_pool_t;

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
typedef struct {
    bool    present;
    bool    enable;
    int8_t  mdc;
    int8_t  mdio;
    int8_t  ref_clk;
    uint8_t phy_addr;
    char    phy_type[NC_CFG_PHY_NAME_MAX];   // 目前只影響 log；IDF 6 只有 generic PHY 驅動
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
    uint8_t        n_pools;
    nc_cfg_pool_t  pools[NC_CFG_MAX_POOLS];
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
