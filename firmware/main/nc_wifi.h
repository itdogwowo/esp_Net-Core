// Wi-Fi 通道：先問「會不會用」，再決定要不要初始化。
//
// 兩層開關，兩層都開才會佔記憶體：
//   ① 編譯期 CONFIG_NETCORE_WIFI
//        唔開 → esp_wifi 完全唔 link（0 B 靜態、0 B flash）
//   ② 執行期 network.json 的 wifi.enable + ssid
//        唔滿足 → 唔呼叫 esp_wifi_init（0 B 執行期 heap，約 90–120 KB）
//
// 為什麼要兩層：① 影響靜態 BSS 同 flash；② 影響執行期 heap。
// 只做②仍然會為一個唔用嘅功能付靜態 RAM 同幾百 KB flash。
//
// 呼叫端**唔需要** #if：未編入時本模組仍然提供同樣的符號，回 off_compile。
// （保持「低耦合」：main 只呼叫 nc_wifi_boot() / nc_wifi_json()。）
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NC_WIFI_OFF_COMPILE = 0,   // 未編入（CONFIG_NETCORE_WIFI=n）
    NC_WIFI_OFF_CONFIG,        // wifi.enable = 0（或讀不到 network.json）
    NC_WIFI_OFF_NO_CRED,       // enable=1 但 ssid 空白
    NC_WIFI_OFF_UNSUPPORTED,   // mode 不是 sta（ap / apsta 未實作）
    NC_WIFI_ON,                // 已初始化並啟動
    NC_WIFI_ERROR,             // 初始化失敗（已回收，不留記憶體）
} nc_wifi_state_t;

// 決定要不要用 Wi-Fi；只有真的要用的時候才會配置記憶體。
// 可以重複呼叫（已啟動就回報現狀）。
nc_wifi_state_t nc_wifi_boot(void);

// 目前是否真的在使用（= 已初始化）。
bool nc_wifi_used(void);

// STATUS_GET 的 "wifi" 區塊；不含密碼。
size_t nc_wifi_json(char *buf, size_t cap);

const char *nc_wifi_state_name(nc_wifi_state_t s);

#ifdef __cplusplus
}
#endif
