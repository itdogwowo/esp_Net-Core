// NC4 出口登記表：把「link 名」對上一個送出函式。
//
// 為什麼要有這張表：路由表（system.json 的 `routes[].out`）寫的是**名字**
// （`decode0` / `UART` / `TCP` …），但送出函式活在各自的 transport 裡
// （`cdc_send`、`uart_write_bytes`、`tcp_nc4_send`…）。router 只懂名字，
// 不應該認識任何一條通道；通道也不應該互相 include。兩邊在中間相遇：
//
//   boot：transport 建立成功 → nc4_link_register("UART", write, NULL)
//   收幀：parser_dispatch → nc4_router_links() → nc4_link_send("UART", 整幀)
//
// 語義（與 tx-path-design.md 一致）：
//   · 一次呼叫 = **整幀**交出去。成功代表整幀已 copy 進目的通道的 TX 緩衝
//     （CDC 4 KB FIFO／UART driver ring／TCP sndbuf／WS async），handler 可以立即返回。
//   · 緩衝滿的時候會唔會等，由 transport 自己決定（CDC 100 ms、UART 排空、
//     TCP 對端 ACK）—— 表只如實記錄結果，不另外加一層等待。
//   · 短送／錯誤＝**drop**，計數並留住 last_err。轉寄的幀來自 parser 環，
//     handler 返回後就會被覆寫，所以失敗只能丟，不能「稍後重試」。
//
// 執行緒安全：register 只可以在建立 decode 任務之前做（開機一次性）；
// 之後表是唯讀，send 可以由兩個核同時呼叫（計數用 atomic）。
//
// 這個檔案只依賴 esp_err.h + libc，所以 host 都測得（tools/hosttest/link_test）。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 同時登記的出口數。今日最多 CDC＋UART＋TCP＋WS＋UDP = 5，留 1 格餘裕。
#define NC4_LINK_MAX       6
#define NC4_LINK_NAME_MAX  16     // 與 nc4_router 的線名上限一致

// 寫入一次：回傳實際交出去的 bytes（< len = 塞唔落／短送）；< 0 = 錯誤。
// 實作可以阻塞（例如 UART 等 ring 排空），但不可以改變 data 的內容。
typedef int (*nc4_link_write_fn)(void *arg, const uint8_t *data, size_t len);

// 登記（重名／滿了／壞名 → 錯誤，唔會靜默覆蓋）
esp_err_t nc4_link_register(const char *name, nc4_link_write_fn write, void *arg);

// 清空（host 測試用；正常開機唔需要叫）
void nc4_link_reset(void);

bool   nc4_link_has(const char *name);
size_t nc4_link_count(void);

// 轉寄／大回覆共用：把**已打包好的整幀**交給目的通道。
//   ESP_OK               → 整幀已交出（frames／bytes 增加）
//   ESP_ERR_NOT_FOUND    → 冇登記這條 link（config 有寫但通道冇建立）
//   ESP_ERR_INVALID_ARG  → frame/len 不合法
//   ESP_FAIL / ESP_ERR_TIMEOUT → 目的通道拒收／短送（drops 增加，last_err 留住）
esp_err_t nc4_link_send(const char *name, const uint8_t *frame, size_t len);

// 統計（測試指令 0xF013、STATUS 用）
void nc4_link_stats(const char *name, uint32_t *frames, uint32_t *bytes, uint32_t *drops);
void nc4_link_stats_reset(void);

// {"max":6,"links":[{"name":"CDC","frames":1,"bytes":100,"drops":0},…]}
size_t nc4_link_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
