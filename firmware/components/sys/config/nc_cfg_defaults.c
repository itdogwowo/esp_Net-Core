// 「最基礎的形狀」：一份 const 預設。
//
// 它有三個角色（順序很重要）：
//   1. 結構的初始值 —— 解析 JSON 之前先鋪好，缺欄位就留在這裡。
//   2. 底座（seed）的內容 —— 分區空白時，用它產生第一份 JSON。
//   3. 最後保底 —— 用戶把檔寫壞，解析失敗就整層回到這裡。
//
// **它不是 runtime 的覆蓋層**：寫完底座之後，JSON 就是唯一真相；
// 預設只在使用者沒寫、或檔案壞掉時才出現，而且**不會**順手寫回用戶的檔。
//
// 數值來源：跟現有 code 的預設一致（CDC 4192/1024/8/4×256、UART 1024/512/8/4×1024、
// TCP 16410/8192/8/8×8205）。改這裡等於改「新板子第一次開機長什麼樣」。
#include <string.h>

#include "nc_cfg.h"
#include "nc_cfg_types.h"

// 依 kind 給 rx 預設：config 只寫 `name/kind/enable` 時，rx 要有一個合理的起點。
void nc_cfg_rx_default(nc_link_kind_t kind, nc_link_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    switch (kind) {
    case NC_LINK_CDC:
        *rx = (nc_link_rx_t){ .region = 4192, .chunk = 1024, .slot_size = 256,
                              .slots = 4, .budget = 8 };
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

static const nc_system_cfg_t k_system = {
    .version = NC_CFG_SCHEMA_VERSION,
    .cid = "",
    .frame_interval_ms = 20,
    .num_pixels = 0,
    .n_links = 2,
    .links = {
        {
            .name = "CDC", .kind = NC_LINK_CDC, .enable = true, .port = 0,
            .rx = { .region = 4192, .chunk = 1024, .slot_size = 256, .slots = 4, .budget = 8 },
        },
        {
            .name = "UART", .kind = NC_LINK_UART, .enable = true, .port = 0,
            .rx = { .region = 1024, .chunk = 256, .slot_size = 256, .slots = 4, .budget = 8 },
        },
    },
    .n_routes = 4,
    .routes = {
        { .in = "nctx",     .n_out = 1, .out = { "decode0" } },
        { .in = "outbound", .n_out = 0 },
        { .in = "CDC",      .n_out = 1, .out = { "decode0" } },
        { .in = "UART",     .n_out = 1, .out = { "decode1" } },
    },
    // 2026-11-09：共用緩衝池（small／bulk／forward）。
    // 預設全部 internal；forward 要給 UART/TCP 送，不可放 PSRAM。
    .n_pools = 3,
    .pools = {
        { .name = "small",   .size = 1024, .count = 8, .where = 0, .enable = true },
        { .name = "bulk",    .size = 4112, .count = 3, .where = 0, .enable = true },
        { .name = "forward", .size = 4112, .count = 2, .where = 0, .enable = true },
    },
};

static const nc_tasks_cfg_t k_tasks = {
    .version = NC_CFG_SCHEMA_VERSION,
    .n_tasks = 0,       // 空 = 全部用固件宣告（level 照 C、租戶 affinity (0,0)）
};

const nc_system_cfg_t *nc_cfg_defaults_system(void)
{
    return &k_system;
}

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
};

const nc_hardware_cfg_t *nc_cfg_defaults_hardware(void)
{
    return &k_hw;
}

const nc_tasks_cfg_t *nc_cfg_defaults_tasks(void)
{
    return &k_tasks;
}
