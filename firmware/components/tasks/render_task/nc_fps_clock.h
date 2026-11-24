// nc_fps_clock.h — deadline 時鐘＋「時間銀行」（slack），純 C，可 host 測試。
//
// 目標：平均 fps 準（唔漂移），而且可以吸收臨時尖峰：
//   · 早完成 → 把多餘時間存入 slack（有上限 cap）。
//   · 遲到   → 先花 slack 頂住，deadline 仍然留在原本的 grid（下一輪可以追返）。
//   · 遲到超過 slack → 放棄追趕，deadline 重設為 now + period（避免一次過爆追）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t  period_us;
    int64_t  cap_us;        // slack 上限
    int64_t  deadline_us;   // grid 上的下一個 deadline
    int64_t  slack_us;      // 已存時間（0..cap）
    uint32_t catchup_n;     // 用 slack 頂住的次數
    uint32_t overrun_n;     // 遲到超過 slack、被迫重排的次數
    uint32_t late_n;        // 任何遲到的次數
} nc_fps_clock_t;

void    nc_fps_clock_init(nc_fps_clock_t *c, int64_t now_us, int64_t period_us, int64_t cap_us);
int64_t nc_fps_clock_wait(const nc_fps_clock_t *c, int64_t now_us);  // 回要等幾久（0 = 即刻做）
void    nc_fps_clock_done(nc_fps_clock_t *c, int64_t now_us);        // 一輪完成後更新

#ifdef __cplusplus
}
#endif
