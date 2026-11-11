// nc_hw — 硬體門面（中央目錄＋命令入口＋唯讀 snapshot）
//
// 設計（2026-11-10 定案）：
//   ① owner task = 唯一可以寫硬體的人；command queue 由 owner 消費。
//   ② 其他 core／UI 只讀 snapshot，不直接碰 GPIO／bus。
//   ③ NC4 action 只認 resource 名（例如 "gpio"），不 include driver。
//
// 這一層**不是**舊 hw_manager 的直接寫入版：
//   nc_hw_send() 只入 owner 的 queue；真正 gpio_set_level 在 owner task。
//   這樣 UI（慢）同實操（快）可以並存，唔會互相搶同一個 register。
//
// 不是熱路徑工具：command 低頻、snapshot 細（<=256 B）。
// 大資料（LED frame／音訊）用 nc_buf，不經這裡。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_HW_MAX          32
#define NC_HW_NAME_MAX     24
#define NC_HW_CMD_MAX      16
#define NC_HW_SNAP_MAX     256
#define NC_HW_CLAIM_MAX    64

typedef struct {
    uint16_t op;        // 由 owner 定義（GPIO：0=CFG 1=SET）
    uint8_t  pin;
    uint8_t  arg;
    uint8_t  len;
    uint8_t  data[8];
} nc_hw_cmd_t;

typedef enum {
    NC_HW_CLAIM_READ  = 1u << 0,
    NC_HW_CLAIM_WRITE = 1u << 1,
} nc_hw_claim_flags_t;

// 開機一次；重複呼叫是 no-op。
esp_err_t nc_hw_init(void);

// 清空（測試／重開機用；不 free queue，因為正常路徑不 destroy）。
void nc_hw_reset(void);

// owner 在 on_start 登記自己負責的資源。重名 → ESP_ERR_INVALID_STATE。
// 滿了 → ESP_ERR_NO_MEM（一定要 log，唔靜默）。
esp_err_t nc_hw_register(const char *name);

// 由 owner 呼叫：拿一筆命令（非阻塞／可等 timeout）。
bool nc_hw_recv(const char *name, nc_hw_cmd_t *cmd, uint32_t timeout_ms);

// 由任何 core 呼叫：把命令交給 owner。queue 滿 → ESP_ERR_NO_MEM。
esp_err_t nc_hw_send(const char *name, const nc_hw_cmd_t *cmd);

// owner 發佈最新狀態；reader 用 copy（唔會拿到半份）。
esp_err_t nc_hw_publish(const char *name, const void *data, size_t len);
// reader：copy 到 out；回實際 bytes（0 = 冇 snapshot／name 唔存在）。
size_t nc_hw_snapshot(const char *name, void *out, size_t cap);

// ── 開機 claim（唔做 runtime 仲裁）──
// 同一支 pin 兩個 owner 都寫 → 後者失敗；READ 可以多人共用。
esp_err_t nc_hw_claim(int pin, const char *owner, const char *label, uint32_t flags);
bool      nc_hw_reserved(int pin);       // PSRAM／flash／USB／console／strapping...
bool      nc_hw_claim_ok(void);
// 這支 pin 是否已被別的 owner claim（GPIO owner 用來擋 SD／ETH 腳）。
bool      nc_hw_pin_claimed_by_other(int pin, const char *owner);
size_t    nc_hw_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
