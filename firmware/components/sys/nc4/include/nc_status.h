// nc_status.h — STATUS_GET 的外掛式區塊（元件自己現身，main 唔識佢哋）。
//
// 為什麼要這個：STATUS_GET 的區塊本來全部寫死在 main.c。測試能力（例如 ncm）
// 一搬出 main 就冇辦法出現在 status 裡，除非 main 再 include 佢 —— 那就白搬了。
//
// 契約：
//   · provider 回傳「值」的 JSON（例如 `{"arena":…}`），**唔包 key**；
//     外層 `,"ncm":` 由這裡補，同 main 其他區塊一致。
//   · 註冊只喺開機做一次（constructor／on_start）；讀取可能喺 decode task，
//     所以註冊表一旦寫入就唯讀。
//   · 區塊排喺 main 內建區塊之後；2,600 B 上限照舊（放唔落就截，主機要分批問）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_STATUS_MAX 8

// 寫入 `buf`（長度 `cap`，含結尾 NUL），回實際寫入長度。
typedef size_t (*nc_status_fn_t)(char *buf, size_t cap);

// 註冊一個區塊。key 全系統唯一（重名 → 第二個被拒並回錯誤）。
esp_err_t nc_status_register(const char *key, nc_status_fn_t fn);
int       nc_status_count(void);

// 依 keys（逗號分隔；空／"all" = 全部）把已註冊的區塊接到 JSON 尾巴。
// `*off`／`*first` 同 main 的 status_add 同一套語義。
void nc_status_append(const char *keys, char *buf, size_t cap, size_t *off, bool *first);

#ifdef __cplusplus
}
#endif
