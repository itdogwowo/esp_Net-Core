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
    NC_UNIT_TASK = 4,  /* 0x4 任務（tasks.json） */
    NC_UNIT_I80 = 7,  /* 0x7 I80／LCD_CAM 匯流排（hardware.json） */
    NC_UNIT_SPI = 8,  /* 0x8 SPI 匯流排（hardware.json） */
    NC_UNIT_LED = 12,  /* 0xc 燈鏈（driver.json 的 LED 段） */
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
    /* 0x00040001 -1=停用；0..15 層（tasks.json:tasks[0].level） */
    NC_KEY_TASK0_LEVEL = 0x00040001u,
    /* 0x00040002 bit0=core0、bit1=core1（tasks.json:tasks[0].affinity） */
    NC_KEY_TASK0_AFFINITY = 0x00040002u,
    /* 0x00040003 1..6（租戶上限）（tasks.json:tasks[0].prio） */
    NC_KEY_TASK0_PRIO = 0x00040003u,
    /* 0x00040101 -1=停用；0..15 層（tasks.json:tasks[1].level） */
    NC_KEY_TASK1_LEVEL = 0x00040101u,
    /* 0x00040102 bit0=core0、bit1=core1（tasks.json:tasks[1].affinity） */
    NC_KEY_TASK1_AFFINITY = 0x00040102u,
    /* 0x00040103 1..6（租戶上限）（tasks.json:tasks[1].prio） */
    NC_KEY_TASK1_PRIO = 0x00040103u,
    /* 0x00040201 -1=停用；0..15 層（tasks.json:tasks[2].level） */
    NC_KEY_TASK2_LEVEL = 0x00040201u,
    /* 0x00040202 bit0=core0、bit1=core1（tasks.json:tasks[2].affinity） */
    NC_KEY_TASK2_AFFINITY = 0x00040202u,
    /* 0x00040203 1..6（租戶上限）（tasks.json:tasks[2].prio） */
    NC_KEY_TASK2_PRIO = 0x00040203u,
    /* 0x00040301 -1=停用；0..15 層（tasks.json:tasks[3].level） */
    NC_KEY_TASK3_LEVEL = 0x00040301u,
    /* 0x00040302 bit0=core0、bit1=core1（tasks.json:tasks[3].affinity） */
    NC_KEY_TASK3_AFFINITY = 0x00040302u,
    /* 0x00040303 1..6（租戶上限）（tasks.json:tasks[3].prio） */
    NC_KEY_TASK3_PRIO = 0x00040303u,
    /* 0x00040401 -1=停用；0..15 層（tasks.json:tasks[4].level） */
    NC_KEY_TASK4_LEVEL = 0x00040401u,
    /* 0x00040402 bit0=core0、bit1=core1（tasks.json:tasks[4].affinity） */
    NC_KEY_TASK4_AFFINITY = 0x00040402u,
    /* 0x00040403 1..6（租戶上限）（tasks.json:tasks[4].prio） */
    NC_KEY_TASK4_PRIO = 0x00040403u,
    /* 0x00040501 -1=停用；0..15 層（tasks.json:tasks[5].level） */
    NC_KEY_TASK5_LEVEL = 0x00040501u,
    /* 0x00040502 bit0=core0、bit1=core1（tasks.json:tasks[5].affinity） */
    NC_KEY_TASK5_AFFINITY = 0x00040502u,
    /* 0x00040503 1..6（租戶上限）（tasks.json:tasks[5].prio） */
    NC_KEY_TASK5_PRIO = 0x00040503u,
    /* 0x00040601 -1=停用；0..15 層（tasks.json:tasks[6].level） */
    NC_KEY_TASK6_LEVEL = 0x00040601u,
    /* 0x00040602 bit0=core0、bit1=core1（tasks.json:tasks[6].affinity） */
    NC_KEY_TASK6_AFFINITY = 0x00040602u,
    /* 0x00040603 1..6（租戶上限）（tasks.json:tasks[6].prio） */
    NC_KEY_TASK6_PRIO = 0x00040603u,
    /* 0x00040701 -1=停用；0..15 層（tasks.json:tasks[7].level） */
    NC_KEY_TASK7_LEVEL = 0x00040701u,
    /* 0x00040702 bit0=core0、bit1=core1（tasks.json:tasks[7].affinity） */
    NC_KEY_TASK7_AFFINITY = 0x00040702u,
    /* 0x00040703 1..6（租戶上限）（tasks.json:tasks[7].prio） */
    NC_KEY_TASK7_PRIO = 0x00040703u,
    /* 0x00040801 -1=停用；0..15 層（tasks.json:tasks[8].level） */
    NC_KEY_TASK8_LEVEL = 0x00040801u,
    /* 0x00040802 bit0=core0、bit1=core1（tasks.json:tasks[8].affinity） */
    NC_KEY_TASK8_AFFINITY = 0x00040802u,
    /* 0x00040803 1..6（租戶上限）（tasks.json:tasks[8].prio） */
    NC_KEY_TASK8_PRIO = 0x00040803u,
    /* 0x00040901 -1=停用；0..15 層（tasks.json:tasks[9].level） */
    NC_KEY_TASK9_LEVEL = 0x00040901u,
    /* 0x00040902 bit0=core0、bit1=core1（tasks.json:tasks[9].affinity） */
    NC_KEY_TASK9_AFFINITY = 0x00040902u,
    /* 0x00040903 1..6（租戶上限）（tasks.json:tasks[9].prio） */
    NC_KEY_TASK9_PRIO = 0x00040903u,
    /* 0x00040a01 -1=停用；0..15 層（tasks.json:tasks[10].level） */
    NC_KEY_TASK10_LEVEL = 0x00040a01u,
    /* 0x00040a02 bit0=core0、bit1=core1（tasks.json:tasks[10].affinity） */
    NC_KEY_TASK10_AFFINITY = 0x00040a02u,
    /* 0x00040a03 1..6（租戶上限）（tasks.json:tasks[10].prio） */
    NC_KEY_TASK10_PRIO = 0x00040a03u,
    /* 0x00040b01 -1=停用；0..15 層（tasks.json:tasks[11].level） */
    NC_KEY_TASK11_LEVEL = 0x00040b01u,
    /* 0x00040b02 bit0=core0、bit1=core1（tasks.json:tasks[11].affinity） */
    NC_KEY_TASK11_AFFINITY = 0x00040b02u,
    /* 0x00040b03 1..6（租戶上限）（tasks.json:tasks[11].prio） */
    NC_KEY_TASK11_PRIO = 0x00040b03u,
    /* 0x00040c01 -1=停用；0..15 層（tasks.json:tasks[12].level） */
    NC_KEY_TASK12_LEVEL = 0x00040c01u,
    /* 0x00040c02 bit0=core0、bit1=core1（tasks.json:tasks[12].affinity） */
    NC_KEY_TASK12_AFFINITY = 0x00040c02u,
    /* 0x00040c03 1..6（租戶上限）（tasks.json:tasks[12].prio） */
    NC_KEY_TASK12_PRIO = 0x00040c03u,
    /* 0x00040d01 -1=停用；0..15 層（tasks.json:tasks[13].level） */
    NC_KEY_TASK13_LEVEL = 0x00040d01u,
    /* 0x00040d02 bit0=core0、bit1=core1（tasks.json:tasks[13].affinity） */
    NC_KEY_TASK13_AFFINITY = 0x00040d02u,
    /* 0x00040d03 1..6（租戶上限）（tasks.json:tasks[13].prio） */
    NC_KEY_TASK13_PRIO = 0x00040d03u,
    /* 0x00040e01 -1=停用；0..15 層（tasks.json:tasks[14].level） */
    NC_KEY_TASK14_LEVEL = 0x00040e01u,
    /* 0x00040e02 bit0=core0、bit1=core1（tasks.json:tasks[14].affinity） */
    NC_KEY_TASK14_AFFINITY = 0x00040e02u,
    /* 0x00040e03 1..6（租戶上限）（tasks.json:tasks[14].prio） */
    NC_KEY_TASK14_PRIO = 0x00040e03u,
    /* 0x00040f01 -1=停用；0..15 層（tasks.json:tasks[15].level） */
    NC_KEY_TASK15_LEVEL = 0x00040f01u,
    /* 0x00040f02 bit0=core0、bit1=core1（tasks.json:tasks[15].affinity） */
    NC_KEY_TASK15_AFFINITY = 0x00040f02u,
    /* 0x00040f03 1..6（租戶上限）（tasks.json:tasks[15].prio） */
    NC_KEY_TASK15_PRIO = 0x00040f03u,
    /* 0x00041001 -1=停用；0..15 層（tasks.json:tasks[16].level） */
    NC_KEY_TASK16_LEVEL = 0x00041001u,
    /* 0x00041002 bit0=core0、bit1=core1（tasks.json:tasks[16].affinity） */
    NC_KEY_TASK16_AFFINITY = 0x00041002u,
    /* 0x00041003 1..6（租戶上限）（tasks.json:tasks[16].prio） */
    NC_KEY_TASK16_PRIO = 0x00041003u,
    /* 0x00041101 -1=停用；0..15 層（tasks.json:tasks[17].level） */
    NC_KEY_TASK17_LEVEL = 0x00041101u,
    /* 0x00041102 bit0=core0、bit1=core1（tasks.json:tasks[17].affinity） */
    NC_KEY_TASK17_AFFINITY = 0x00041102u,
    /* 0x00041103 1..6（租戶上限）（tasks.json:tasks[17].prio） */
    NC_KEY_TASK17_PRIO = 0x00041103u,
    /* 0x00041201 -1=停用；0..15 層（tasks.json:tasks[18].level） */
    NC_KEY_TASK18_LEVEL = 0x00041201u,
    /* 0x00041202 bit0=core0、bit1=core1（tasks.json:tasks[18].affinity） */
    NC_KEY_TASK18_AFFINITY = 0x00041202u,
    /* 0x00041203 1..6（租戶上限）（tasks.json:tasks[18].prio） */
    NC_KEY_TASK18_PRIO = 0x00041203u,
    /* 0x00041301 -1=停用；0..15 層（tasks.json:tasks[19].level） */
    NC_KEY_TASK19_LEVEL = 0x00041301u,
    /* 0x00041302 bit0=core0、bit1=core1（tasks.json:tasks[19].affinity） */
    NC_KEY_TASK19_AFFINITY = 0x00041302u,
    /* 0x00041303 1..6（租戶上限）（tasks.json:tasks[19].prio） */
    NC_KEY_TASK19_PRIO = 0x00041303u,
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
};
