// nc_px_nce.h — `.nce`（WASM 效果）交去配對層的**值流信箱**。
//
// 契約（用家 2026-11-28 定案，對照舊項目 mp_Net-Core 的 scatter）：
//   · `.nce` 只交一串 **0–255**；buffer **唔描述自己嘅格式**。
//   · 格式（邊個通道、每顆幾多值）由**配對層**的 `write` 決定（r/g/b/w/rgb/rgbw…）。
//   · 每值 **1 B（0–255）**；buffer 長度＝效果自己輸出幾個值（例如 led_n × 通道數），
//     由效果參數決定 —— 唔係固定 4 B/px。
//   · host 讀到之後 `<<4` 入 12-bit 值流（`>>4` 復原無損），之後所有 write 模式照舊。
//
// 分工：
//   writer ＝ ncm task（單一）——每 tick 由插件拿 framebuffer 再 publish；
//   reader ＝ pixel_effect_task（單一）——每幀 latest() 拿最新一份。
// 雙緩衝＋原子換手：writer 寫另一塊再 flip，reader 唔會讀到寫到一半的 buffer。
// （reader 拿到的指標最多被下一次 publish 覆蓋；effect task 喺同一幀內讀完，可以接受。）
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 每值 1 B（0–255）；buffer 長度＝值數，由效果（manifest 的 px／led_n）決定。
#define NC_PX_NCE_BPP 1u

typedef struct {
    const uint8_t *data;    // 指向其中一塊雙緩衝（唯讀；下一次 publish 可能覆蓋）
    uint32_t       bytes;
    uint32_t       seq;      // 每次 publish +1（0 = 未 publish 過）
} nc_px_nce_frame_t;

// 一次配置（開機／reload）。`bytes` = 值數（每值 1 B）。冪等：同一個大小唔會重新要記憶體。
// 回 false = 要唔到記憶體（PSRAM 優先，唔夠退 internal）→ 呼叫者要明示 skip，唔好靜默。
bool nc_px_nce_init(uint32_t bytes);
void nc_px_nce_deinit(void);
uint32_t nc_px_nce_bytes(void);
// 診斷：publish 次數／把最新一份頭 n 個值複製出來（回實際複製數）
uint32_t nc_px_nce_seq(void);
uint32_t nc_px_nce_last(uint8_t *out, uint32_t n);

// writer：ncm task 每 tick 交一份。src 可以係插件 linear memory 的指標（會即刻複製）。
bool nc_px_nce_publish(const uint8_t *src, uint32_t bytes);

// reader：拿最新一份。未 publish 過 → false。
bool nc_px_nce_latest(nc_px_nce_frame_t *out);

// 0–255 → 12-bit 值流（<<4）。回實際轉換的數量（= min(n, bytes)）。
// 純函式，host 可測；配對層之後照舊 >>4 出 8-bit。
uint32_t nc_px_nce_to_vals(const uint8_t *src, uint32_t bytes, uint16_t *dst, uint32_t n);

#ifdef __cplusplus
}
#endif
