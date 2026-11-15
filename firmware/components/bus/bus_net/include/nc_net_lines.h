// 統一網絡線路層：Interface → Line（iface + transport + port + service）
//
// 只處理「一條線」：listener、session、該線自己的 RX buffer、core 與 send。
// 不處理 web UI 的內容，也不把 WS 綁去 httpd；WS 的 handshake／frame 由這層自己做。
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "nc4.h"

#ifdef __cplusplus
extern "C" {
#endif

// main 提供「core → decoder」的查詢；module 不擁有 decoder。
typedef nc4_decoder_t *(*nc_net_decoder_fn)(int core);

// config 內有 managed 的網絡線（iface/service）才啟用這層。
bool nc_net_lines_enabled(void);

// B08：為每條 NC4 線建立 bus／parser 緩衝；HTTP 線只記錄設定。
esp_err_t nc_net_lines_declare(nc_net_decoder_fn get_decoder);

// B11：開 listener／httpd；idempotent。
esp_err_t nc_net_lines_start(void);

#ifdef __cplusplus
}
#endif
