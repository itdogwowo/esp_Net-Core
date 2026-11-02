// NC4 over WebSocket（第一版）
//
// 為什麼 WS 比 TCP 簡單：
//   WS 本身就是「訊息導向」——一則 WS 訊息 = 一個 NC4 幀，
//   所以不需要插槽、不需要 RX task、不需要 hub：直接在 httpd 給的緩衝上解析。
//
// 限制（第一版，明確寫在這裡）：
//   · 一次只服務一條 WS（第二條連線會被拒絕）——跟 TCP 那條的規則一致
//   · 一則訊息最多 NC4_MAX_FRAME（8205 B）
//   · handler 跑在 httpd 的 task 上（不是 decode engine），所以它是第三個執行點；
//     nc4 的 dispatch mutex 會保護共用狀態，但「慢 handler 會佔住 httpd」是真的
//   · 尚未支援 subprotocol
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "nc4.h"

#ifdef __cplusplus
extern "C" {
#endif

// 底層的 httpd（給 /config.json 之類的端點共用同一個 server）
httpd_handle_t nc4_ws_httpd(void);

// 啟動 WS server（uri = /nc4）。decoder 由呼叫端提供，parser 在這裡建。
esp_err_t nc4_ws_start(nc4_decoder_t *decoder, uint16_t port);

// 停止並釋放
void nc4_ws_stop(void);

// 狀態（STATUS 用）：{"port":…,"fd":…,"rx_msgs":…,"rx_bytes":…,"tx_frames":…,"bad":…}
size_t nc4_ws_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
