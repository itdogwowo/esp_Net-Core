// 統一線路（TCP／UDP／WS）→ nc4_bus
//
// 目的：三條線在 NC4 層用同一組 read／send 介面，不再各自寫一份 parser/task：
//   nc4_bus（PUSH）→ slots → parser → decoder
//   reply/forward → send() → 同一條線
//
// 三條 adapter 只負責「怎麼讀一個 byte stream／一則訊息」與「怎麼送」，不碰 NC4 幀。
// 目前用 S3 的 WiFi；P4 可重用同一層（socket 介面一樣）。
#pragma once

#include "esp_err.h"
#include "nc4.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在 task 啟動前建立 3 條 nc4_bus（read 在 socket 未開時回 0）。
esp_err_t nc4_line_net_declare(nc4_decoder_t *decoder, int core);

// 開 socket：TCP 6000、UDP 6001、WS 8080。可重複呼叫（已開就 no-op）。
esp_err_t nc4_line_net_start(void);

// WS 的 httpd（未開 WS 時 NULL）；給 /config.json、/ota、/ 共用同一個 server。
httpd_handle_t nc4_line_net_httpd(void);

// 狀態：{"tcp":{"fd":..,"port":..},"udp":...,"ws":...}
size_t nc4_line_net_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
