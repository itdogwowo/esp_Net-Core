// 「最基礎的形狀」：預設由**晶片能力**砌出來，不是一份固定常數。
//
// 它有三個角色（順序很重要）：
//   1. 結構的初始值 —— 解析 JSON 之前先鋪好，缺欄位就留在這裡。
//   2. 底座（seed）的內容 —— 分區空白時，用它產生第一份 JSON。
//   3. 最後保底 —— 用戶把檔寫壞，解析失敗就整層回到這裡。
//
// **它不是 runtime 的覆蓋層**：寫完底座之後，JSON 就是唯一真相；
// 預設只在使用者沒寫、或檔案壞掉時才出現，而且**不會**順手寫回用戶的檔。
//
// 2026-11-13：出廠預設改成「CDC 先」——
//   · 有 OTG 的晶片（S2/S3/P4）  → link "CDC"（kind=cdc，TinyUSB）
//   · 只有 USJ 的（C3/C5/C6/C61/H2）→ link "usb_jtag"（kind=usb_jtag）
//   · 連 USB 都沒有的（ESP32）    → 只剩 UART
//   · usb.msc 預設 0：開機唔把 FAT 交出去，檔案一律走 CDC；
//     「插線見到 D:」變成用戶的明確選擇，而不是所有板子的副作用。
//   · USB 的 rx.region = 4,128（4,096 的 FILE_CHUNK → 幀 4,115，向上 32 對齊），
//     這是必要且貼身的值，用戶唔應該需要調；要塞更大的 chunk 才改 config。
//
// 為什麼預設要看得懂晶片：C3 沒有 OTG，寫 "CDC" 也不會建立任何東西；
// 而「第一次開機就要用原生 USB 收得到指令」是 bring-up 的第一步。
// 這張表在 nc_cfg_chip.c，數據來自 IDF 的 soc_caps.h。
//
// 其餘數值來源（跟現有 code／量測一致）：
//   UART 1024/256/8/4×256、TCP 16410/8192/8/8×8205。
// 改這裡等於改「新板子第一次開機長什麼樣」。
#include <stdio.h>
#include <string.h>

#include "nc_cfg.h"
#include "nc_cfg_types.h"

// 現在砌預設時要看的晶片能力；NULL = 未知（保守當成「CDC + 2 核」）。
static const nc_cfg_chip_t *s_chip;
static nc_system_cfg_t s_system;
static bool s_system_ready;

void nc_cfg_defaults_set_chip(const nc_cfg_chip_t *chip)
{
    s_chip = chip;
    s_system_ready = false;   // 下次取預設時重砌
}

static uint16_t usb_region_default(void)
{
    if (s_chip != NULL && s_chip->usb_rx_region != 0) {
        return s_chip->usb_rx_region;
    }
    return NC_CFG_USB_RX_REGION;
}

// 依 kind 給 rx 預設：config 只寫 `name/kind/enable` 時，rx 要有一個合理的起點。
void nc_cfg_rx_default(nc_link_kind_t kind, nc_link_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    switch (kind) {
    case NC_LINK_CDC:
        // 4,128 放得落一個 4 KB 的 FILE_CHUNK（幀 4,115）。
        // chunk 1,024 是 TinyUSB 一次 read 的上限；PUSH 每格 256。
        *rx = (nc_link_rx_t){ .region = usb_region_default(), .chunk = 1024,
                              .slot_size = 256, .slots = 4, .budget = 8 };
        break;
    case NC_LINK_USBJTAG:
        // C3／C6／H2 沒有 OTG，USB-Serial-JTAG 就是**唯一**的 CDC（NC4）通道。
        // region 一定要放得落 FILE_CHUNK 最大包：9 header + 6 chunk header +
        // 4,096 data + 4 CRC = 4,115 B。以前跟 default 拿 4,096 → 每次 4 KB
        // 上傳都差 19 B，parser 當「幀過大」跳過，主機只見到 timeout。
        // 這個預設與 TinyUSB CDC 的 4,128 對齊，兩種 transport 的檔案語義才一樣。
        // chunk 256：LL FIFO 一次讀走的量；PUSH 模式每格也是 256。
        *rx = (nc_link_rx_t){ .region = usb_region_default(), .chunk = 256,
                              .slot_size = 256, .slots = 4, .budget = 8 };
        break;
    case NC_LINK_UART:
        // 實測（921600 baud 迴路）：單次最多 242 B、slot 峰值 1 → 4×256 貼身。
        *rx = (nc_link_rx_t){ .region = 1024, .chunk = 256, .slot_size = 256,
                              .slots = 4, .budget = 8 };
        break;
    case NC_LINK_TCP:
        *rx = (nc_link_rx_t){ .region = 16410, .chunk = 8192, .slot_size = 8205,
                              .slots = 8, .budget = 8 };
        break;
    case NC_LINK_WS:
    case NC_LINK_UDP:
    default:
        *rx = (nc_link_rx_t){ .region = 4096, .chunk = 1024, .slot_size = 1024,
                              .slots = 4, .budget = 8 };
        break;
    }
}

static nc_link_cfg_t *add_link(nc_system_cfg_t *s, const char *name, nc_link_kind_t kind)
{
    if (s->n_links >= NC_CFG_MAX_LINKS) {
        return NULL;
    }
    nc_link_cfg_t *l = &s->links[s->n_links++];
    memset(l, 0, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->kind = kind;
    l->enable = true;
    nc_cfg_rx_default(kind, &l->rx);
    return l;
}

static void add_route(nc_system_cfg_t *s, const char *in, const char *out)
{
    if (s->n_routes >= NC_CFG_MAX_ROUTES) {
        return;
    }
    nc_route_cfg_t *r = &s->routes[s->n_routes++];
    memset(r, 0, sizeof(*r));
    snprintf(r->in, sizeof(r->in), "%s", in);
    if (out != NULL) {
        snprintf(r->out[0], sizeof(r->out[0]), "%s", out);
        r->n_out = 1;
    }
}

static void system_build(void)
{
    nc_system_cfg_t *s = &s_system;
    memset(s, 0, sizeof(*s));
    s->version = NC_CFG_SCHEMA_VERSION;
    s->frame_interval_ms = 20;

    // 出廠 = USJ 先（所有人的預設）：有 USB-Serial-JTAG 就用 usb_jtag，
    // 冇 USJ 才用 CDC（S2）；這由晶片表（nc_cfg_chip.c）決定。
    // 未知晶片由 main 用編譯期能力砌一個臨時表；真的兩樣都冇 → 沒 USB。
    // 開機唔交 FAT（msc=0），log 走 USB（有 USB 的話）。
    s->usb_msc = 0;
    s->usb_cdc_log = 1;

    // 未知晶片（s_chip == NULL）→ 跟舊行為：當它有 OTG，砌一條 CDC。
    // 已知但沒有 USB（s_chip->usb_link == NULL）→ 一條 USB link 都唔砌。
    const char *usb_name = NULL;
    nc_link_kind_t usb_kind = NC_LINK_CDC;
    uint8_t cores = 2;
    if (s_chip == NULL) {
        usb_name = "CDC";
    } else {
        cores = s_chip->cores;
        if (s_chip->usb_link != NULL) {
            usb_name = s_chip->usb_link;
            usb_kind = s_chip->usb_kind;
        }
    }
    if (usb_name == NULL) {
        s->usb_cdc_log = 0;   // 冇 USB：這個開關沒有意義
    }

    // 次序就是「誰先被建立」：USB 先，UART 後。
    if (usb_name != NULL) {
        (void)add_link(s, usb_name, usb_kind);
    }
    (void)add_link(s, "UART", NC_LINK_UART);

    // nctx / outbound 兩列永遠排最前（契約：見 system.json 的 _comment）。
    add_route(s, "nctx", "decode0");
    add_route(s, "outbound", NULL);
    if (usb_name != NULL) {
        add_route(s, usb_name, "decode0");
    }
    // 單核板只可以 decode0：寫 decode1 會在單核上建立 core1 任務（實測 assert）。
    add_route(s, "UART", (cores > 1) ? "decode1" : "decode0");
    // 預設就把 USB 的幀同時轉去 ESPNOW：沒有 SD 設定的板子也可以做兩板互轉。
    add_route(s, "ESPNOW", "decode0");

}

const nc_system_cfg_t *nc_cfg_defaults_system(void)
{
    if (!s_system_ready) {
        system_build();
        s_system_ready = true;
    }
    return &s_system;
}

static const nc_tasks_cfg_t k_tasks = {
    .version = NC_CFG_SCHEMA_VERSION,
    .n_tasks = 0,       // 空 = 全部用固件宣告（level 照 C、租戶 affinity (0,0)）
};

static const nc_hardware_cfg_t k_hw = {
    .version = NC_CFG_SCHEMA_VERSION,
    // ETH：預設「config 冇寫」（present=0）→ 呼叫端用編譯期內建腳位；
    // 這裡填的是「連內建值都沒有」時的形狀（全 -1、不啟用）。
    .eth = {
        .present = false, .enable = false,
        .mdc = -1, .mdio = -1, .ref_clk = -1,
        .phy_addr = 1, .phy_type = "IP101",
    },
    .n_sdio = 1,
    .sdio = {
        { .enable = false, .freq_hz = 40000000, .width = 4,
          .clk = -1, .cmd = -1, .d = { -1, -1, -1, -1 } },
    },
    .n_pin = 0,
    .n_pwm = 0,
    .n_uart = 0,
};

const nc_hardware_cfg_t *nc_cfg_defaults_hardware(void)
{
    return &k_hw;
}

const nc_tasks_cfg_t *nc_cfg_defaults_tasks(void)
{
    return &k_tasks;
}
