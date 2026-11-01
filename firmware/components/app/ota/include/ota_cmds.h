// OTA（0x22xx）— 指令表與 mp_Net-Core 的 slave/schema/ota.json 一致
//
// 已實作（第一版）：2201 BEGIN / 2202 WRITE / 2203 END / 2205 ABORT /
//                   2206-2207 VERSION / 2210-2211 CAPS / 2218-2219 PROGRESS /
//                   221A-221B STATE / 221C ERROR / 2220 APPLY
// 未實作（需要自訂 slot metadata / NVS 記錄）：2212-2213 LAST、2214-2215 PARTITION_STATUS、2216-2217 VERIFY
//
// 語義都照 ota.json：欄位順序、型別、LE、str_u16len（2B 長度前綴）。
// 錯誤一律回 OTA_ERROR_RSP 的 11 個 bool 之一，不發明新的錯誤碼。

#pragma once

#include "nc4.h"

#ifdef __cplusplus
extern "C" {
#endif

void ota_register(nc4_decoder_t *decoder);

#ifdef __cplusplus
}
#endif
