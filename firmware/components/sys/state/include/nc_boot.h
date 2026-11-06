// 開機狀態：**一個** NVS 記錄，取代以前散落的四個旗標。
//
// 以前有：boot/fs_init（初始化過未）、boot/fs_format（要求重建）、boot/fw_fp（韌體指紋）、
//         main.c 的 static s_fs_first（fs_init 的 runtime 快取）。
// 它們其實都在答同一條問題：「這一塊 data 分區，現在處於生命週期的哪一步？」
// 所以收成一條記錄：
//
//   NVS namespace "boot", key "record"  =  nc_boot_record_t（8 B）
//
// 旗標：
//   PROVISIONED     分區初始化過、底座（config）已寫好 → 之後唔准自動格式化
//   FORMAT_PENDING  有人明確要求下次開機重建（用戶指令，或偵測到換韌體）
//   DEGRADED        上次開機有 config 讀唔到／壞掉，只用了預設（可觀察，不阻開機）
//
// 換韌體的處理（2026-11-06 定案）：
//   app_elf_sha256 變了 → 這次開機就格式化 → 重新建立底座。
//   所以「換版要清什麼」不再是一段獨立邏輯（以前是刪 4 個 config 檔），
//   而是回到生命週期的起點，跟第一次開機走同一條路。
//
// 注意：格式化會清掉整個 data 分區，包括 /data/pixel/ 的效果資產。
//       這是「換韌體 = factory reset」的必然結果；要保留資產就要另做遷移。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_BOOT_SCHEMA_VERSION  1

#define NC_BOOT_PROVISIONED     0x0001u   // 分區初始化過、底座已寫
#define NC_BOOT_FORMAT_PENDING  0x0002u   // 下次開機重建 FAT（明確請求）
#define NC_BOOT_DEGRADED        0x0004u   // 上次開機 config 有問題（只記錄）

typedef struct {
    uint16_t version;    // 記錄格式版本（不是韌體版本）
    uint16_t flags;
    uint32_t fw_hash;    // app_elf_sha256 前 4 bytes：用來偵測「換咗韌體」
} nc_boot_record_t;

// 必須在 nvs_flash_init() 之後、讀任何 boot 狀態之前呼叫。
// 內部：登記到 state manager → 讀回 NVS → 偵測換版（可能會設 FORMAT_PENDING）。
void nc_boot_init(void);

bool nc_boot_provisioned(void);
bool nc_boot_format_pending(void);
bool nc_boot_degraded(void);
bool nc_boot_fw_changed(void);       // 本次開機是否換咗韌體

// 用戶明確要求「下次開機重建」；會即刻落盤（重開機之前不能只留喺 RAM）。
void nc_boot_request_format(bool on);

// 底座寫好之後呼叫：PROVISIONED=1、FORMAT_PENDING=0（只改 RAM，之後要 commit）。
void nc_boot_mark_provisioned(void);
void nc_boot_mark_degraded(void);

// 把 RAM 的記錄寫回 NVS（只有改過才寫）。
esp_err_t nc_boot_commit(void);

// STATUS 用：{"prov":1,"fmtq":0,"degr":0,"fw_new":0}
void nc_boot_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
