// NC4 over UDP（datagram = 1..N 個 NC4 幀）
//
// 為什麼 UDP 可能最快：它沒有「TCP 視窗 ÷ RTT」的天花板。
// 代價是沒有重傳/排序——每幀有 CRC，壞的丟掉；掉包就是那一幀沒了。
//
// 這個版本刻意做得很薄：
//   · 一個 socket、一個 task（blocking recvfrom，不輪詢）
//   · 收到的一則 datagram 直接在 buffer 上逐幀解析（零應用層複製）
//   · 回覆送到「最後一則 datagram 的來源」（單一 peer，與 TCP/WS 同規則）
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nc4.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t udp_nc4_start(nc4_decoder_t *decoder, uint16_t port);
void udp_nc4_stop(void);

// {"port":…,"peer":"a.b.c.d:port","msgs":…,"bytes":…,"tx":…,"bad":…,"trunc":…}
size_t udp_nc4_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
