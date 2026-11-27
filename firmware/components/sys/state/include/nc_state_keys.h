// NCSTATE key id／unit id：全專案唯一 source of truth。
//
// **本檔由 tools/gen_state_keys.py 生成，唔好手改**：改 schema/state-keys.json 再跑工具。
// run_all.sh 會用 --check 驗同步；命名規矩：
//   key id(u32) = domain(8b) | unit(8b) | index(8b) | field(8b)
//   例：0x00070005 = domain 0（hw）／unit 7（i80）／index 0／field 5（freq）
//   index 用嚟分通道／lane／pin 編號（0..255），所以 PIN 48 支、P4 多 bus 都放得落。
#pragma once

#include <stdint.h>

#define NC_STATE_KEYS_VERSION 2u

// unit = 能力群組；key id 的 unit byte 一定要等於呢個值（生成器已驗）。
enum {
    NC_UNIT_SYS = 0,  /* 0x0 系統層（system.json） */
    NC_UNIT_NET = 1,  /* 0x1 網絡層（network.json wifi） */
    NC_UNIT_LINK = 2,  /* 0x2 NC4 通道（system.json nc4.links） */
    NC_UNIT_ROUTE = 3,  /* 0x3 路由表（system.json nc4.routes） */
    NC_UNIT_RMT = 5,  /* 0x5 RMT（遙控／WS2812／IR；S3/P4 8 ch、C3 4 ch） */
    NC_UNIT_PCNT = 6,  /* 0x6 PCNT 計數器（S3/P4 4 unit；C3 冇） */
    NC_UNIT_I80 = 7,  /* 0x7 I80／LCD_CAM 匯流排（hardware.json） */
    NC_UNIT_SPI = 8,  /* 0x8 SPI 匯流排（hardware.json） */
    NC_UNIT_ADC = 9,  /* 0x9 ADC（S3 20 ch／P4 14／C3 6） */
    NC_UNIT_TWAI = 10,  /* 0xa TWAI／CAN（S3 1／P4 3／C3 1；要外加收發器） */
    NC_UNIT_MCPWM = 11,  /* 0xb MCPWM 馬達 PWM（S3/P4 2 unit；C3 冇） */
    NC_UNIT_LED = 12,  /* 0xc 燈鏈（driver.json 的 LED 段） */
    NC_UNIT_PARLIO = 13,  /* 0xd PARLIO 並列輸出（P4 專有，≤16-bit） */
    NC_UNIT_LCD = 14,  /* 0xe LCD RGB 面板（S3 8–16-bit／P4 8–24-bit） */
    NC_UNIT_TOUCH = 15,  /* 0xf 電容觸摸（S3/P4 14 ch；C3 冇） */
    NC_UNIT_CAM = 16,  /* 0x10 DVP／CSI 相機（S3 8–16-bit／P4 8/16） */
};

enum {
    /* 0x00000001 16-bit cID；0 = 由 MAC 推導（system.json:cID） */
    NC_KEY_SYS_CID = 0x00000001u,
    /* 0x00000002 節拍間隔 ms（system.json:frame_interval_ms） */
    NC_KEY_SYS_FRAME_MS = 0x00000002u,
    /* 0x00000003 總像素數（資訊用）（system.json:num_pixels） */
    NC_KEY_SYS_NUM_PIXELS = 0x00000003u,
    /* 0x00000004 log 除錯模式（system.json:log_debug_mode） */
    NC_KEY_SYS_LOG_DEBUG = 0x00000004u,
    /* 0x00010001 WiFi 啟用（network.json:wifi.enable） */
    NC_KEY_NET_WIFI_ENABLE = 0x00010001u,
    /* 0x00010002 連線 timeout 秒（network.json:wifi.timeout_s） */
    NC_KEY_NET_WIFI_TIMEOUT = 0x00010002u,
    /* 0x00010003 sta／ap（network.json:wifi.mode） */
    NC_KEY_NET_WIFI_MODE = 0x00010003u,
    /* 0x00020001 通道啟用（system.json:nc4.links[0].enable） */
    NC_KEY_LINK0_ENABLE = 0x00020001u,
    /* 0x00020002 通道類型（複合名用 iface）（system.json:nc4.links[0].kind） */
    NC_KEY_LINK0_KIND = 0x00020002u,
    /* 0x00020003 同類線編號（system.json:nc4.links[0].index） */
    NC_KEY_LINK0_INDEX = 0x00020003u,
    /* 0x00020004 USB MSC（0/1；-1 未寫）（system.json:nc4.links[0].msc） */
    NC_KEY_LINK0_MSC = 0x00020004u,
    /* 0x00020005 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[0].port） */
    NC_KEY_LINK0_PORT = 0x00020005u,
    /* 0x00020006 綁哪個網絡介面（system.json:nc4.links[0].iface） */
    NC_KEY_LINK0_IFACE = 0x00020006u,
    /* 0x00020007 auto／nc4／web（system.json:nc4.links[0].service） */
    NC_KEY_LINK0_SVC = 0x00020007u,
    /* 0x00020008 緩衝位置 auto／sram／psram（system.json:nc4.links[0].mem） */
    NC_KEY_LINK0_MEM = 0x00020008u,
    /* 0x00020101 通道啟用（system.json:nc4.links[1].enable） */
    NC_KEY_LINK1_ENABLE = 0x00020101u,
    /* 0x00020102 通道類型（複合名用 iface）（system.json:nc4.links[1].kind） */
    NC_KEY_LINK1_KIND = 0x00020102u,
    /* 0x00020103 同類線編號（system.json:nc4.links[1].index） */
    NC_KEY_LINK1_INDEX = 0x00020103u,
    /* 0x00020104 USB MSC（0/1；-1 未寫）（system.json:nc4.links[1].msc） */
    NC_KEY_LINK1_MSC = 0x00020104u,
    /* 0x00020105 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[1].port） */
    NC_KEY_LINK1_PORT = 0x00020105u,
    /* 0x00020106 綁哪個網絡介面（system.json:nc4.links[1].iface） */
    NC_KEY_LINK1_IFACE = 0x00020106u,
    /* 0x00020107 auto／nc4／web（system.json:nc4.links[1].service） */
    NC_KEY_LINK1_SVC = 0x00020107u,
    /* 0x00020108 緩衝位置 auto／sram／psram（system.json:nc4.links[1].mem） */
    NC_KEY_LINK1_MEM = 0x00020108u,
    /* 0x00020201 通道啟用（system.json:nc4.links[2].enable） */
    NC_KEY_LINK2_ENABLE = 0x00020201u,
    /* 0x00020202 通道類型（複合名用 iface）（system.json:nc4.links[2].kind） */
    NC_KEY_LINK2_KIND = 0x00020202u,
    /* 0x00020203 同類線編號（system.json:nc4.links[2].index） */
    NC_KEY_LINK2_INDEX = 0x00020203u,
    /* 0x00020204 USB MSC（0/1；-1 未寫）（system.json:nc4.links[2].msc） */
    NC_KEY_LINK2_MSC = 0x00020204u,
    /* 0x00020205 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[2].port） */
    NC_KEY_LINK2_PORT = 0x00020205u,
    /* 0x00020206 綁哪個網絡介面（system.json:nc4.links[2].iface） */
    NC_KEY_LINK2_IFACE = 0x00020206u,
    /* 0x00020207 auto／nc4／web（system.json:nc4.links[2].service） */
    NC_KEY_LINK2_SVC = 0x00020207u,
    /* 0x00020208 緩衝位置 auto／sram／psram（system.json:nc4.links[2].mem） */
    NC_KEY_LINK2_MEM = 0x00020208u,
    /* 0x00020301 通道啟用（system.json:nc4.links[3].enable） */
    NC_KEY_LINK3_ENABLE = 0x00020301u,
    /* 0x00020302 通道類型（複合名用 iface）（system.json:nc4.links[3].kind） */
    NC_KEY_LINK3_KIND = 0x00020302u,
    /* 0x00020303 同類線編號（system.json:nc4.links[3].index） */
    NC_KEY_LINK3_INDEX = 0x00020303u,
    /* 0x00020304 USB MSC（0/1；-1 未寫）（system.json:nc4.links[3].msc） */
    NC_KEY_LINK3_MSC = 0x00020304u,
    /* 0x00020305 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[3].port） */
    NC_KEY_LINK3_PORT = 0x00020305u,
    /* 0x00020306 綁哪個網絡介面（system.json:nc4.links[3].iface） */
    NC_KEY_LINK3_IFACE = 0x00020306u,
    /* 0x00020307 auto／nc4／web（system.json:nc4.links[3].service） */
    NC_KEY_LINK3_SVC = 0x00020307u,
    /* 0x00020308 緩衝位置 auto／sram／psram（system.json:nc4.links[3].mem） */
    NC_KEY_LINK3_MEM = 0x00020308u,
    /* 0x00020401 通道啟用（system.json:nc4.links[4].enable） */
    NC_KEY_LINK4_ENABLE = 0x00020401u,
    /* 0x00020402 通道類型（複合名用 iface）（system.json:nc4.links[4].kind） */
    NC_KEY_LINK4_KIND = 0x00020402u,
    /* 0x00020403 同類線編號（system.json:nc4.links[4].index） */
    NC_KEY_LINK4_INDEX = 0x00020403u,
    /* 0x00020404 USB MSC（0/1；-1 未寫）（system.json:nc4.links[4].msc） */
    NC_KEY_LINK4_MSC = 0x00020404u,
    /* 0x00020405 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[4].port） */
    NC_KEY_LINK4_PORT = 0x00020405u,
    /* 0x00020406 綁哪個網絡介面（system.json:nc4.links[4].iface） */
    NC_KEY_LINK4_IFACE = 0x00020406u,
    /* 0x00020407 auto／nc4／web（system.json:nc4.links[4].service） */
    NC_KEY_LINK4_SVC = 0x00020407u,
    /* 0x00020408 緩衝位置 auto／sram／psram（system.json:nc4.links[4].mem） */
    NC_KEY_LINK4_MEM = 0x00020408u,
    /* 0x00020501 通道啟用（system.json:nc4.links[5].enable） */
    NC_KEY_LINK5_ENABLE = 0x00020501u,
    /* 0x00020502 通道類型（複合名用 iface）（system.json:nc4.links[5].kind） */
    NC_KEY_LINK5_KIND = 0x00020502u,
    /* 0x00020503 同類線編號（system.json:nc4.links[5].index） */
    NC_KEY_LINK5_INDEX = 0x00020503u,
    /* 0x00020504 USB MSC（0/1；-1 未寫）（system.json:nc4.links[5].msc） */
    NC_KEY_LINK5_MSC = 0x00020504u,
    /* 0x00020505 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[5].port） */
    NC_KEY_LINK5_PORT = 0x00020505u,
    /* 0x00020506 綁哪個網絡介面（system.json:nc4.links[5].iface） */
    NC_KEY_LINK5_IFACE = 0x00020506u,
    /* 0x00020507 auto／nc4／web（system.json:nc4.links[5].service） */
    NC_KEY_LINK5_SVC = 0x00020507u,
    /* 0x00020508 緩衝位置 auto／sram／psram（system.json:nc4.links[5].mem） */
    NC_KEY_LINK5_MEM = 0x00020508u,
    /* 0x00020601 通道啟用（system.json:nc4.links[6].enable） */
    NC_KEY_LINK6_ENABLE = 0x00020601u,
    /* 0x00020602 通道類型（複合名用 iface）（system.json:nc4.links[6].kind） */
    NC_KEY_LINK6_KIND = 0x00020602u,
    /* 0x00020603 同類線編號（system.json:nc4.links[6].index） */
    NC_KEY_LINK6_INDEX = 0x00020603u,
    /* 0x00020604 USB MSC（0/1；-1 未寫）（system.json:nc4.links[6].msc） */
    NC_KEY_LINK6_MSC = 0x00020604u,
    /* 0x00020605 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[6].port） */
    NC_KEY_LINK6_PORT = 0x00020605u,
    /* 0x00020606 綁哪個網絡介面（system.json:nc4.links[6].iface） */
    NC_KEY_LINK6_IFACE = 0x00020606u,
    /* 0x00020607 auto／nc4／web（system.json:nc4.links[6].service） */
    NC_KEY_LINK6_SVC = 0x00020607u,
    /* 0x00020608 緩衝位置 auto／sram／psram（system.json:nc4.links[6].mem） */
    NC_KEY_LINK6_MEM = 0x00020608u,
    /* 0x00020701 通道啟用（system.json:nc4.links[7].enable） */
    NC_KEY_LINK7_ENABLE = 0x00020701u,
    /* 0x00020702 通道類型（複合名用 iface）（system.json:nc4.links[7].kind） */
    NC_KEY_LINK7_KIND = 0x00020702u,
    /* 0x00020703 同類線編號（system.json:nc4.links[7].index） */
    NC_KEY_LINK7_INDEX = 0x00020703u,
    /* 0x00020704 USB MSC（0/1；-1 未寫）（system.json:nc4.links[7].msc） */
    NC_KEY_LINK7_MSC = 0x00020704u,
    /* 0x00020705 TCP/UDP/WS/HTTP 端口（system.json:nc4.links[7].port） */
    NC_KEY_LINK7_PORT = 0x00020705u,
    /* 0x00020706 綁哪個網絡介面（system.json:nc4.links[7].iface） */
    NC_KEY_LINK7_IFACE = 0x00020706u,
    /* 0x00020707 auto／nc4／web（system.json:nc4.links[7].service） */
    NC_KEY_LINK7_SVC = 0x00020707u,
    /* 0x00020708 緩衝位置 auto／sram／psram（system.json:nc4.links[7].mem） */
    NC_KEY_LINK7_MEM = 0x00020708u,
    /* 0x00030001 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[0].out） */
    NC_KEY_ROUTE0_MASK = 0x00030001u,
    /* 0x00030101 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[1].out） */
    NC_KEY_ROUTE1_MASK = 0x00030101u,
    /* 0x00030201 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[2].out） */
    NC_KEY_ROUTE2_MASK = 0x00030201u,
    /* 0x00030301 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[3].out） */
    NC_KEY_ROUTE3_MASK = 0x00030301u,
    /* 0x00030401 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[4].out） */
    NC_KEY_ROUTE4_MASK = 0x00030401u,
    /* 0x00030501 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[5].out） */
    NC_KEY_ROUTE5_MASK = 0x00030501u,
    /* 0x00030601 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[6].out） */
    NC_KEY_ROUTE6_MASK = 0x00030601u,
    /* 0x00030701 decode 目標 bitmask（bit0=decode0；0=只轉發）（system.json:nc4.routes[7].out） */
    NC_KEY_ROUTE7_MASK = 0x00030701u,
    /* 0x00050001 RMT 通道啟用（hardware.json:RMT.list[0].enable） */
    NC_KEY_RMT0_ENABLE = 0x00050001u,
    /* 0x00050002 訊號腳（hardware.json:RMT.list[0].gpio） */
    NC_KEY_RMT0_GPIO = 0x00050002u,
    /* 0x00050003 tx／rx（hardware.json:RMT.list[0].mode） */
    NC_KEY_RMT0_MODE = 0x00050003u,
    /* 0x00050004 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[0].dma） */
    NC_KEY_RMT0_DMA = 0x00050004u,
    /* 0x00050101 RMT 通道啟用（hardware.json:RMT.list[1].enable） */
    NC_KEY_RMT1_ENABLE = 0x00050101u,
    /* 0x00050102 訊號腳（hardware.json:RMT.list[1].gpio） */
    NC_KEY_RMT1_GPIO = 0x00050102u,
    /* 0x00050103 tx／rx（hardware.json:RMT.list[1].mode） */
    NC_KEY_RMT1_MODE = 0x00050103u,
    /* 0x00050104 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[1].dma） */
    NC_KEY_RMT1_DMA = 0x00050104u,
    /* 0x00050201 RMT 通道啟用（hardware.json:RMT.list[2].enable） */
    NC_KEY_RMT2_ENABLE = 0x00050201u,
    /* 0x00050202 訊號腳（hardware.json:RMT.list[2].gpio） */
    NC_KEY_RMT2_GPIO = 0x00050202u,
    /* 0x00050203 tx／rx（hardware.json:RMT.list[2].mode） */
    NC_KEY_RMT2_MODE = 0x00050203u,
    /* 0x00050204 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[2].dma） */
    NC_KEY_RMT2_DMA = 0x00050204u,
    /* 0x00050301 RMT 通道啟用（hardware.json:RMT.list[3].enable） */
    NC_KEY_RMT3_ENABLE = 0x00050301u,
    /* 0x00050302 訊號腳（hardware.json:RMT.list[3].gpio） */
    NC_KEY_RMT3_GPIO = 0x00050302u,
    /* 0x00050303 tx／rx（hardware.json:RMT.list[3].mode） */
    NC_KEY_RMT3_MODE = 0x00050303u,
    /* 0x00050304 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[3].dma） */
    NC_KEY_RMT3_DMA = 0x00050304u,
    /* 0x00050401 RMT 通道啟用（hardware.json:RMT.list[4].enable） */
    NC_KEY_RMT4_ENABLE = 0x00050401u,
    /* 0x00050402 訊號腳（hardware.json:RMT.list[4].gpio） */
    NC_KEY_RMT4_GPIO = 0x00050402u,
    /* 0x00050403 tx／rx（hardware.json:RMT.list[4].mode） */
    NC_KEY_RMT4_MODE = 0x00050403u,
    /* 0x00050404 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[4].dma） */
    NC_KEY_RMT4_DMA = 0x00050404u,
    /* 0x00050501 RMT 通道啟用（hardware.json:RMT.list[5].enable） */
    NC_KEY_RMT5_ENABLE = 0x00050501u,
    /* 0x00050502 訊號腳（hardware.json:RMT.list[5].gpio） */
    NC_KEY_RMT5_GPIO = 0x00050502u,
    /* 0x00050503 tx／rx（hardware.json:RMT.list[5].mode） */
    NC_KEY_RMT5_MODE = 0x00050503u,
    /* 0x00050504 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[5].dma） */
    NC_KEY_RMT5_DMA = 0x00050504u,
    /* 0x00050601 RMT 通道啟用（hardware.json:RMT.list[6].enable） */
    NC_KEY_RMT6_ENABLE = 0x00050601u,
    /* 0x00050602 訊號腳（hardware.json:RMT.list[6].gpio） */
    NC_KEY_RMT6_GPIO = 0x00050602u,
    /* 0x00050603 tx／rx（hardware.json:RMT.list[6].mode） */
    NC_KEY_RMT6_MODE = 0x00050603u,
    /* 0x00050604 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[6].dma） */
    NC_KEY_RMT6_DMA = 0x00050604u,
    /* 0x00050701 RMT 通道啟用（hardware.json:RMT.list[7].enable） */
    NC_KEY_RMT7_ENABLE = 0x00050701u,
    /* 0x00050702 訊號腳（hardware.json:RMT.list[7].gpio） */
    NC_KEY_RMT7_GPIO = 0x00050702u,
    /* 0x00050703 tx／rx（hardware.json:RMT.list[7].mode） */
    NC_KEY_RMT7_MODE = 0x00050703u,
    /* 0x00050704 用 GDMA（S3/P4 TX3／RX7；C3 冇）（hardware.json:RMT.list[7].dma） */
    NC_KEY_RMT7_DMA = 0x00050704u,
    /* 0x00060001 PCNT 單元啟用（hardware.json:PCNT.list[0].enable） */
    NC_KEY_PCNT0_ENABLE = 0x00060001u,
    /* 0x00060002 A 相腳（hardware.json:PCNT.list[0].gpio_a） */
    NC_KEY_PCNT0_GPIO_A = 0x00060002u,
    /* 0x00060003 B 相腳（-1=單相）（hardware.json:PCNT.list[0].gpio_b） */
    NC_KEY_PCNT0_GPIO_B = 0x00060003u,
    /* 0x00060101 PCNT 單元啟用（hardware.json:PCNT.list[1].enable） */
    NC_KEY_PCNT1_ENABLE = 0x00060101u,
    /* 0x00060102 A 相腳（hardware.json:PCNT.list[1].gpio_a） */
    NC_KEY_PCNT1_GPIO_A = 0x00060102u,
    /* 0x00060103 B 相腳（-1=單相）（hardware.json:PCNT.list[1].gpio_b） */
    NC_KEY_PCNT1_GPIO_B = 0x00060103u,
    /* 0x00060201 PCNT 單元啟用（hardware.json:PCNT.list[2].enable） */
    NC_KEY_PCNT2_ENABLE = 0x00060201u,
    /* 0x00060202 A 相腳（hardware.json:PCNT.list[2].gpio_a） */
    NC_KEY_PCNT2_GPIO_A = 0x00060202u,
    /* 0x00060203 B 相腳（-1=單相）（hardware.json:PCNT.list[2].gpio_b） */
    NC_KEY_PCNT2_GPIO_B = 0x00060203u,
    /* 0x00060301 PCNT 單元啟用（hardware.json:PCNT.list[3].enable） */
    NC_KEY_PCNT3_ENABLE = 0x00060301u,
    /* 0x00060302 A 相腳（hardware.json:PCNT.list[3].gpio_a） */
    NC_KEY_PCNT3_GPIO_A = 0x00060302u,
    /* 0x00060303 B 相腳（-1=單相）（hardware.json:PCNT.list[3].gpio_b） */
    NC_KEY_PCNT3_GPIO_B = 0x00060303u,
    /* 0x00070001 真正接燈的資料腳數（稀疏非 -1）（hardware.json:I80.list[0].data） */
    NC_KEY_I80_COUNT = 0x00070001u,
    /* 0x00070002 WR pin（-1 = 未用）（hardware.json:I80.list[0].wr） */
    NC_KEY_I80_WR = 0x00070002u,
    /* 0x00070003 DC pin（-1 = 未用）（hardware.json:I80.list[0].dc） */
    NC_KEY_I80_DC = 0x00070003u,
    /* 0x00070004 CS pin（-1 = 未用）（hardware.json:I80.list[0].cs） */
    NC_KEY_I80_CS = 0x00070004u,
    /* 0x00070005 LCD PCLK Hz（hardware.json:I80.list[0].freq） */
    NC_KEY_I80_FREQ = 0x00070005u,
    /* 0x00080001 SPI 是否啟用（hardware.json:SPI.enable） */
    NC_KEY_SPI_ENABLE = 0x00080001u,
    /* 0x00080002 SPI 時鐘（hardware.json:SPI.list[0].baudrate） */
    NC_KEY_SPI_BAUD = 0x00080002u,
    /* 0x00080003 phase（0/1）（hardware.json:SPI.list[0].phase） */
    NC_KEY_SPI_PHASE = 0x00080003u,
    /* 0x00080004 polarity（0/1）（hardware.json:SPI.list[0].polarity） */
    NC_KEY_SPI_POL = 0x00080004u,
    /* 0x00080005 SCLK pin（hardware.json:SPI.list[0].GPIO.sck） */
    NC_KEY_SPI_SCK = 0x00080005u,
    /* 0x00080006 MOSI（單線 / data0）（hardware.json:SPI.list[0].GPIO.mosi） */
    NC_KEY_SPI_MOSI = 0x00080006u,
    /* 0x00080007 MISO（-1 = 冇）（hardware.json:SPI.list[0].GPIO.miso） */
    NC_KEY_SPI_MISO = 0x00080007u,
    /* 0x00090001 ADC 通道啟用（hardware.json:ADC.list[0].enable） */
    NC_KEY_ADC0_ENABLE = 0x00090001u,
    /* 0x00090002 adc1／adc2（hardware.json:ADC.list[0].unit） */
    NC_KEY_ADC0_UNIT = 0x00090002u,
    /* 0x00090003 衰減（hardware.json:ADC.list[0].atten） */
    NC_KEY_ADC0_ATTEN = 0x00090003u,
    /* 0x00090101 ADC 通道啟用（hardware.json:ADC.list[1].enable） */
    NC_KEY_ADC1_ENABLE = 0x00090101u,
    /* 0x00090102 adc1／adc2（hardware.json:ADC.list[1].unit） */
    NC_KEY_ADC1_UNIT = 0x00090102u,
    /* 0x00090103 衰減（hardware.json:ADC.list[1].atten） */
    NC_KEY_ADC1_ATTEN = 0x00090103u,
    /* 0x00090201 ADC 通道啟用（hardware.json:ADC.list[2].enable） */
    NC_KEY_ADC2_ENABLE = 0x00090201u,
    /* 0x00090202 adc1／adc2（hardware.json:ADC.list[2].unit） */
    NC_KEY_ADC2_UNIT = 0x00090202u,
    /* 0x00090203 衰減（hardware.json:ADC.list[2].atten） */
    NC_KEY_ADC2_ATTEN = 0x00090203u,
    /* 0x00090301 ADC 通道啟用（hardware.json:ADC.list[3].enable） */
    NC_KEY_ADC3_ENABLE = 0x00090301u,
    /* 0x00090302 adc1／adc2（hardware.json:ADC.list[3].unit） */
    NC_KEY_ADC3_UNIT = 0x00090302u,
    /* 0x00090303 衰減（hardware.json:ADC.list[3].atten） */
    NC_KEY_ADC3_ATTEN = 0x00090303u,
    /* 0x00090401 ADC 通道啟用（hardware.json:ADC.list[4].enable） */
    NC_KEY_ADC4_ENABLE = 0x00090401u,
    /* 0x00090402 adc1／adc2（hardware.json:ADC.list[4].unit） */
    NC_KEY_ADC4_UNIT = 0x00090402u,
    /* 0x00090403 衰減（hardware.json:ADC.list[4].atten） */
    NC_KEY_ADC4_ATTEN = 0x00090403u,
    /* 0x00090501 ADC 通道啟用（hardware.json:ADC.list[5].enable） */
    NC_KEY_ADC5_ENABLE = 0x00090501u,
    /* 0x00090502 adc1／adc2（hardware.json:ADC.list[5].unit） */
    NC_KEY_ADC5_UNIT = 0x00090502u,
    /* 0x00090503 衰減（hardware.json:ADC.list[5].atten） */
    NC_KEY_ADC5_ATTEN = 0x00090503u,
    /* 0x00090601 ADC 通道啟用（hardware.json:ADC.list[6].enable） */
    NC_KEY_ADC6_ENABLE = 0x00090601u,
    /* 0x00090602 adc1／adc2（hardware.json:ADC.list[6].unit） */
    NC_KEY_ADC6_UNIT = 0x00090602u,
    /* 0x00090603 衰減（hardware.json:ADC.list[6].atten） */
    NC_KEY_ADC6_ATTEN = 0x00090603u,
    /* 0x00090701 ADC 通道啟用（hardware.json:ADC.list[7].enable） */
    NC_KEY_ADC7_ENABLE = 0x00090701u,
    /* 0x00090702 adc1／adc2（hardware.json:ADC.list[7].unit） */
    NC_KEY_ADC7_UNIT = 0x00090702u,
    /* 0x00090703 衰減（hardware.json:ADC.list[7].atten） */
    NC_KEY_ADC7_ATTEN = 0x00090703u,
    /* 0x00090801 ADC 通道啟用（hardware.json:ADC.list[8].enable） */
    NC_KEY_ADC8_ENABLE = 0x00090801u,
    /* 0x00090802 adc1／adc2（hardware.json:ADC.list[8].unit） */
    NC_KEY_ADC8_UNIT = 0x00090802u,
    /* 0x00090803 衰減（hardware.json:ADC.list[8].atten） */
    NC_KEY_ADC8_ATTEN = 0x00090803u,
    /* 0x00090901 ADC 通道啟用（hardware.json:ADC.list[9].enable） */
    NC_KEY_ADC9_ENABLE = 0x00090901u,
    /* 0x00090902 adc1／adc2（hardware.json:ADC.list[9].unit） */
    NC_KEY_ADC9_UNIT = 0x00090902u,
    /* 0x00090903 衰減（hardware.json:ADC.list[9].atten） */
    NC_KEY_ADC9_ATTEN = 0x00090903u,
    /* 0x00090a01 ADC 通道啟用（hardware.json:ADC.list[10].enable） */
    NC_KEY_ADC10_ENABLE = 0x00090a01u,
    /* 0x00090a02 adc1／adc2（hardware.json:ADC.list[10].unit） */
    NC_KEY_ADC10_UNIT = 0x00090a02u,
    /* 0x00090a03 衰減（hardware.json:ADC.list[10].atten） */
    NC_KEY_ADC10_ATTEN = 0x00090a03u,
    /* 0x00090b01 ADC 通道啟用（hardware.json:ADC.list[11].enable） */
    NC_KEY_ADC11_ENABLE = 0x00090b01u,
    /* 0x00090b02 adc1／adc2（hardware.json:ADC.list[11].unit） */
    NC_KEY_ADC11_UNIT = 0x00090b02u,
    /* 0x00090b03 衰減（hardware.json:ADC.list[11].atten） */
    NC_KEY_ADC11_ATTEN = 0x00090b03u,
    /* 0x00090c01 ADC 通道啟用（hardware.json:ADC.list[12].enable） */
    NC_KEY_ADC12_ENABLE = 0x00090c01u,
    /* 0x00090c02 adc1／adc2（hardware.json:ADC.list[12].unit） */
    NC_KEY_ADC12_UNIT = 0x00090c02u,
    /* 0x00090c03 衰減（hardware.json:ADC.list[12].atten） */
    NC_KEY_ADC12_ATTEN = 0x00090c03u,
    /* 0x00090d01 ADC 通道啟用（hardware.json:ADC.list[13].enable） */
    NC_KEY_ADC13_ENABLE = 0x00090d01u,
    /* 0x00090d02 adc1／adc2（hardware.json:ADC.list[13].unit） */
    NC_KEY_ADC13_UNIT = 0x00090d02u,
    /* 0x00090d03 衰減（hardware.json:ADC.list[13].atten） */
    NC_KEY_ADC13_ATTEN = 0x00090d03u,
    /* 0x00090e01 ADC 通道啟用（hardware.json:ADC.list[14].enable） */
    NC_KEY_ADC14_ENABLE = 0x00090e01u,
    /* 0x00090e02 adc1／adc2（hardware.json:ADC.list[14].unit） */
    NC_KEY_ADC14_UNIT = 0x00090e02u,
    /* 0x00090e03 衰減（hardware.json:ADC.list[14].atten） */
    NC_KEY_ADC14_ATTEN = 0x00090e03u,
    /* 0x00090f01 ADC 通道啟用（hardware.json:ADC.list[15].enable） */
    NC_KEY_ADC15_ENABLE = 0x00090f01u,
    /* 0x00090f02 adc1／adc2（hardware.json:ADC.list[15].unit） */
    NC_KEY_ADC15_UNIT = 0x00090f02u,
    /* 0x00090f03 衰減（hardware.json:ADC.list[15].atten） */
    NC_KEY_ADC15_ATTEN = 0x00090f03u,
    /* 0x00091001 ADC 通道啟用（hardware.json:ADC.list[16].enable） */
    NC_KEY_ADC16_ENABLE = 0x00091001u,
    /* 0x00091002 adc1／adc2（hardware.json:ADC.list[16].unit） */
    NC_KEY_ADC16_UNIT = 0x00091002u,
    /* 0x00091003 衰減（hardware.json:ADC.list[16].atten） */
    NC_KEY_ADC16_ATTEN = 0x00091003u,
    /* 0x00091101 ADC 通道啟用（hardware.json:ADC.list[17].enable） */
    NC_KEY_ADC17_ENABLE = 0x00091101u,
    /* 0x00091102 adc1／adc2（hardware.json:ADC.list[17].unit） */
    NC_KEY_ADC17_UNIT = 0x00091102u,
    /* 0x00091103 衰減（hardware.json:ADC.list[17].atten） */
    NC_KEY_ADC17_ATTEN = 0x00091103u,
    /* 0x00091201 ADC 通道啟用（hardware.json:ADC.list[18].enable） */
    NC_KEY_ADC18_ENABLE = 0x00091201u,
    /* 0x00091202 adc1／adc2（hardware.json:ADC.list[18].unit） */
    NC_KEY_ADC18_UNIT = 0x00091202u,
    /* 0x00091203 衰減（hardware.json:ADC.list[18].atten） */
    NC_KEY_ADC18_ATTEN = 0x00091203u,
    /* 0x00091301 ADC 通道啟用（hardware.json:ADC.list[19].enable） */
    NC_KEY_ADC19_ENABLE = 0x00091301u,
    /* 0x00091302 adc1／adc2（hardware.json:ADC.list[19].unit） */
    NC_KEY_ADC19_UNIT = 0x00091302u,
    /* 0x00091303 衰減（hardware.json:ADC.list[19].atten） */
    NC_KEY_ADC19_ATTEN = 0x00091303u,
    /* 0x000a0001 CAN 啟用（hardware.json:TWAI.list[0].enable） */
    NC_KEY_TWAI0_ENABLE = 0x000a0001u,
    /* 0x000a0002 TX 腳（hardware.json:TWAI.list[0].tx） */
    NC_KEY_TWAI0_TX = 0x000a0002u,
    /* 0x000a0003 RX 腳（hardware.json:TWAI.list[0].rx） */
    NC_KEY_TWAI0_RX = 0x000a0003u,
    /* 0x000a0004 位元率（bps）（hardware.json:TWAI.list[0].bitrate） */
    NC_KEY_TWAI0_BITRATE = 0x000a0004u,
    /* 0x000a0005 normal／listen（hardware.json:TWAI.list[0].mode） */
    NC_KEY_TWAI0_MODE = 0x000a0005u,
    /* 0x000a0101 CAN 啟用（hardware.json:TWAI.list[1].enable） */
    NC_KEY_TWAI1_ENABLE = 0x000a0101u,
    /* 0x000a0102 TX 腳（hardware.json:TWAI.list[1].tx） */
    NC_KEY_TWAI1_TX = 0x000a0102u,
    /* 0x000a0103 RX 腳（hardware.json:TWAI.list[1].rx） */
    NC_KEY_TWAI1_RX = 0x000a0103u,
    /* 0x000a0104 位元率（bps）（hardware.json:TWAI.list[1].bitrate） */
    NC_KEY_TWAI1_BITRATE = 0x000a0104u,
    /* 0x000a0105 normal／listen（hardware.json:TWAI.list[1].mode） */
    NC_KEY_TWAI1_MODE = 0x000a0105u,
    /* 0x000a0201 CAN 啟用（hardware.json:TWAI.list[2].enable） */
    NC_KEY_TWAI2_ENABLE = 0x000a0201u,
    /* 0x000a0202 TX 腳（hardware.json:TWAI.list[2].tx） */
    NC_KEY_TWAI2_TX = 0x000a0202u,
    /* 0x000a0203 RX 腳（hardware.json:TWAI.list[2].rx） */
    NC_KEY_TWAI2_RX = 0x000a0203u,
    /* 0x000a0204 位元率（bps）（hardware.json:TWAI.list[2].bitrate） */
    NC_KEY_TWAI2_BITRATE = 0x000a0204u,
    /* 0x000a0205 normal／listen（hardware.json:TWAI.list[2].mode） */
    NC_KEY_TWAI2_MODE = 0x000a0205u,
    /* 0x000b0001 MCPWM 啟用（hardware.json:MCPWM.list[0].enable） */
    NC_KEY_MCPWM0_ENABLE = 0x000b0001u,
    /* 0x000b0002 timer 0..2（hardware.json:MCPWM.list[0].timer） */
    NC_KEY_MCPWM0_TIMER = 0x000b0002u,
    /* 0x000b0003 PWMxA 腳（hardware.json:MCPWM.list[0].gpio_a） */
    NC_KEY_MCPWM0_GPIO_A = 0x000b0003u,
    /* 0x000b0004 PWMxB 腳（hardware.json:MCPWM.list[0].gpio_b） */
    NC_KEY_MCPWM0_GPIO_B = 0x000b0004u,
    /* 0x000b0005 頻率 Hz（hardware.json:MCPWM.list[0].freq） */
    NC_KEY_MCPWM0_FREQ = 0x000b0005u,
    /* 0x000b0006 初始 duty（0..1000‰）（hardware.json:MCPWM.list[0].duty） */
    NC_KEY_MCPWM0_DUTY = 0x000b0006u,
    /* 0x000b0101 MCPWM 啟用（hardware.json:MCPWM.list[1].enable） */
    NC_KEY_MCPWM1_ENABLE = 0x000b0101u,
    /* 0x000b0102 timer 0..2（hardware.json:MCPWM.list[1].timer） */
    NC_KEY_MCPWM1_TIMER = 0x000b0102u,
    /* 0x000b0103 PWMxA 腳（hardware.json:MCPWM.list[1].gpio_a） */
    NC_KEY_MCPWM1_GPIO_A = 0x000b0103u,
    /* 0x000b0104 PWMxB 腳（hardware.json:MCPWM.list[1].gpio_b） */
    NC_KEY_MCPWM1_GPIO_B = 0x000b0104u,
    /* 0x000b0105 頻率 Hz（hardware.json:MCPWM.list[1].freq） */
    NC_KEY_MCPWM1_FREQ = 0x000b0105u,
    /* 0x000b0106 初始 duty（0..1000‰）（hardware.json:MCPWM.list[1].duty） */
    NC_KEY_MCPWM1_DUTY = 0x000b0106u,
    /* 0x000c0001 燈鏈啟用（driver.json:LED.enable） */
    NC_KEY_LED_ENABLE = 0x000c0001u,
    /* 0x000c0002 nc_led_backend_t（driver.json:LED.backend） */
    NC_KEY_LED_BACKEND = 0x000c0002u,
    /* 0x000c0003 引用 hardware 同類 list 的 index（driver.json:LED.bus.index） */
    NC_KEY_LED_BUS = 0x000c0003u,
    /* 0x000c0004 真的接燈的 lane 數（driver.json:LED.lanes） */
    NC_KEY_LED_LANES = 0x000c0004u,
    /* 0x000c0005 每條 lane 幾顆（driver.json:LED.px） */
    NC_KEY_LED_PX = 0x000c0005u,
    /* 0x000c0006 線材格式（= nc_px_format_t 的 index）（driver.json:LED.format） */
    NC_KEY_LED_FMT = 0x000c0006u,
    /* 0x000c0007 I80 PCLK / SPI 時鐘（driver.json:LED.timing.pclk_hz） */
    NC_KEY_LED_CLK = 0x000c0007u,
    /* 0x000c0008 每 bit 幾個 cell（driver.json:LED.timing.bit_cells） */
    NC_KEY_LED_BIT_CELLS = 0x000c0008u,
    /* 0x000c0009 0 的高電平 cell 數（driver.json:LED.timing.t0h） */
    NC_KEY_LED_T0H = 0x000c0009u,
    /* 0x000c000a 1 的高電平 cell 數（driver.json:LED.timing.t1h） */
    NC_KEY_LED_T1H = 0x000c000au,
    /* 0x000d0001 PARLIO 啟用（P4 專有）（hardware.json:PARLIO.list[0].enable） */
    NC_KEY_PARLIO0_ENABLE = 0x000d0001u,
    /* 0x000d0002 1/2/4/8/16 bit（hardware.json:PARLIO.list[0].data_width） */
    NC_KEY_PARLIO0_WIDTH = 0x000d0002u,
    /* 0x000d0003 時鐘腳（hardware.json:PARLIO.list[0].clk） */
    NC_KEY_PARLIO0_CLK = 0x000d0003u,
    /* 0x000d0004 D0（hardware.json:PARLIO.list[0].data[0]） */
    NC_KEY_PARLIO0_D0 = 0x000d0004u,
    /* 0x000d0005 IO clock Hz（≤40 MHz）（hardware.json:PARLIO.list[0].freq） */
    NC_KEY_PARLIO0_FREQ = 0x000d0005u,
    /* 0x000d0006 tx／rx（hardware.json:PARLIO.list[0].mode） */
    NC_KEY_PARLIO0_MODE = 0x000d0006u,
    /* 0x000e0001 RGB 面板啟用（hardware.json:LCD.list[0].enable） */
    NC_KEY_LCD0_ENABLE = 0x000e0001u,
    /* 0x000e0002 8/16/24 bit（hardware.json:LCD.list[0].data_width） */
    NC_KEY_LCD0_WIDTH = 0x000e0002u,
    /* 0x000e0003 PCLK 腳（hardware.json:LCD.list[0].pclk） */
    NC_KEY_LCD0_PCLK = 0x000e0003u,
    /* 0x000e0004 HSYNC（hardware.json:LCD.list[0].hsync） */
    NC_KEY_LCD0_HSYNC = 0x000e0004u,
    /* 0x000e0005 VSYNC（hardware.json:LCD.list[0].vsync） */
    NC_KEY_LCD0_VSYNC = 0x000e0005u,
    /* 0x000e0006 DE（hardware.json:LCD.list[0].de） */
    NC_KEY_LCD0_DE = 0x000e0006u,
    /* 0x000e0007 PCLK Hz（8-bit<80M／16-bit<40M）（hardware.json:LCD.list[0].freq） */
    NC_KEY_LCD0_FREQ = 0x000e0007u,
    /* 0x000e0008 水平像素（hardware.json:LCD.list[0].h_res） */
    NC_KEY_LCD0_HPX = 0x000e0008u,
    /* 0x000e0009 垂直像素（hardware.json:LCD.list[0].v_res） */
    NC_KEY_LCD0_VPX = 0x000e0009u,
    /* 0x000e000a HSYNC 寬度（hardware.json:LCD.list[0].hsync_pw） */
    NC_KEY_LCD0_HSYNC_PW = 0x000e000au,
    /* 0x000e000b VSYNC 寬度（hardware.json:LCD.list[0].vsync_pw） */
    NC_KEY_LCD0_VSYNC_PW = 0x000e000bu,
    /* 0x000e000c RGB 順序（hardware.json:LCD.list[0].big_endian） */
    NC_KEY_LCD0_BIG_ENDIAN = 0x000e000cu,
    /* 0x000f0001 觸摸通道啟用（hardware.json:TOUCH.list[0].enable） */
    NC_KEY_TOUCH0_ENABLE = 0x000f0001u,
    /* 0x000f0002 觸發門檻（hardware.json:TOUCH.list[0].threshold） */
    NC_KEY_TOUCH0_THRESHOLD = 0x000f0002u,
    /* 0x000f0101 觸摸通道啟用（hardware.json:TOUCH.list[1].enable） */
    NC_KEY_TOUCH1_ENABLE = 0x000f0101u,
    /* 0x000f0102 觸發門檻（hardware.json:TOUCH.list[1].threshold） */
    NC_KEY_TOUCH1_THRESHOLD = 0x000f0102u,
    /* 0x000f0201 觸摸通道啟用（hardware.json:TOUCH.list[2].enable） */
    NC_KEY_TOUCH2_ENABLE = 0x000f0201u,
    /* 0x000f0202 觸發門檻（hardware.json:TOUCH.list[2].threshold） */
    NC_KEY_TOUCH2_THRESHOLD = 0x000f0202u,
    /* 0x000f0301 觸摸通道啟用（hardware.json:TOUCH.list[3].enable） */
    NC_KEY_TOUCH3_ENABLE = 0x000f0301u,
    /* 0x000f0302 觸發門檻（hardware.json:TOUCH.list[3].threshold） */
    NC_KEY_TOUCH3_THRESHOLD = 0x000f0302u,
    /* 0x000f0401 觸摸通道啟用（hardware.json:TOUCH.list[4].enable） */
    NC_KEY_TOUCH4_ENABLE = 0x000f0401u,
    /* 0x000f0402 觸發門檻（hardware.json:TOUCH.list[4].threshold） */
    NC_KEY_TOUCH4_THRESHOLD = 0x000f0402u,
    /* 0x000f0501 觸摸通道啟用（hardware.json:TOUCH.list[5].enable） */
    NC_KEY_TOUCH5_ENABLE = 0x000f0501u,
    /* 0x000f0502 觸發門檻（hardware.json:TOUCH.list[5].threshold） */
    NC_KEY_TOUCH5_THRESHOLD = 0x000f0502u,
    /* 0x000f0601 觸摸通道啟用（hardware.json:TOUCH.list[6].enable） */
    NC_KEY_TOUCH6_ENABLE = 0x000f0601u,
    /* 0x000f0602 觸發門檻（hardware.json:TOUCH.list[6].threshold） */
    NC_KEY_TOUCH6_THRESHOLD = 0x000f0602u,
    /* 0x000f0701 觸摸通道啟用（hardware.json:TOUCH.list[7].enable） */
    NC_KEY_TOUCH7_ENABLE = 0x000f0701u,
    /* 0x000f0702 觸發門檻（hardware.json:TOUCH.list[7].threshold） */
    NC_KEY_TOUCH7_THRESHOLD = 0x000f0702u,
    /* 0x000f0801 觸摸通道啟用（hardware.json:TOUCH.list[8].enable） */
    NC_KEY_TOUCH8_ENABLE = 0x000f0801u,
    /* 0x000f0802 觸發門檻（hardware.json:TOUCH.list[8].threshold） */
    NC_KEY_TOUCH8_THRESHOLD = 0x000f0802u,
    /* 0x000f0901 觸摸通道啟用（hardware.json:TOUCH.list[9].enable） */
    NC_KEY_TOUCH9_ENABLE = 0x000f0901u,
    /* 0x000f0902 觸發門檻（hardware.json:TOUCH.list[9].threshold） */
    NC_KEY_TOUCH9_THRESHOLD = 0x000f0902u,
    /* 0x000f0a01 觸摸通道啟用（hardware.json:TOUCH.list[10].enable） */
    NC_KEY_TOUCH10_ENABLE = 0x000f0a01u,
    /* 0x000f0a02 觸發門檻（hardware.json:TOUCH.list[10].threshold） */
    NC_KEY_TOUCH10_THRESHOLD = 0x000f0a02u,
    /* 0x000f0b01 觸摸通道啟用（hardware.json:TOUCH.list[11].enable） */
    NC_KEY_TOUCH11_ENABLE = 0x000f0b01u,
    /* 0x000f0b02 觸發門檻（hardware.json:TOUCH.list[11].threshold） */
    NC_KEY_TOUCH11_THRESHOLD = 0x000f0b02u,
    /* 0x000f0c01 觸摸通道啟用（hardware.json:TOUCH.list[12].enable） */
    NC_KEY_TOUCH12_ENABLE = 0x000f0c01u,
    /* 0x000f0c02 觸發門檻（hardware.json:TOUCH.list[12].threshold） */
    NC_KEY_TOUCH12_THRESHOLD = 0x000f0c02u,
    /* 0x000f0d01 觸摸通道啟用（hardware.json:TOUCH.list[13].enable） */
    NC_KEY_TOUCH13_ENABLE = 0x000f0d01u,
    /* 0x000f0d02 觸發門檻（hardware.json:TOUCH.list[13].threshold） */
    NC_KEY_TOUCH13_THRESHOLD = 0x000f0d02u,
    /* 0x00100001 相機啟用（hardware.json:CAM.list[0].enable） */
    NC_KEY_CAM0_ENABLE = 0x00100001u,
    /* 0x00100002 8/16 bit（hardware.json:CAM.list[0].data_width） */
    NC_KEY_CAM0_WIDTH = 0x00100002u,
    /* 0x00100003 PCLK（hardware.json:CAM.list[0].pclk） */
    NC_KEY_CAM0_PCLK = 0x00100003u,
    /* 0x00100004 VSYNC（hardware.json:CAM.list[0].vsync） */
    NC_KEY_CAM0_VSYNC = 0x00100004u,
    /* 0x00100005 HREF（hardware.json:CAM.list[0].href） */
    NC_KEY_CAM0_HREF = 0x00100005u,
    /* 0x00100006 XCLK（hardware.json:CAM.list[0].xclk） */
    NC_KEY_CAM0_XCLK = 0x00100006u,
    /* 0x00100007 XCLK Hz（hardware.json:CAM.list[0].xclk_freq） */
    NC_KEY_CAM0_XCLK_FREQ = 0x00100007u,
    /* 0x00100008 SCCB 位址（hardware.json:CAM.list[0].sccb_addr） */
    NC_KEY_CAM0_SCCB_ADDR = 0x00100008u,
};
