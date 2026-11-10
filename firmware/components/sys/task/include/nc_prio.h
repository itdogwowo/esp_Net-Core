// 優先級保留區：NC 核心（收 → 解 → 回）獨佔 7–15
//
// 為什麼要有這條：核心的收／解／回是整個系統的節拍來源，任何「未來會有的實作」
// （燈效、UI、HTTP、新 transport、測試任務）都只可以排在它下面。沒有具名常數的話，
// 下一個人會直接寫 `prio = 8`，然後核心就被擠掉 —— 而且症狀是「偶爾慢」，極難查。
//
// 2026-11-10 實測支撐（S3）：
//   · producer(5) > decode(4) → push→decode 接手平均 478 µs、最大 1.77 ms，
//     而且插槽環被讀爆（slot_full +122）。倒過來之後變成 62 / 250 µs、slot_full = 0。
//   · 所以「decode 一定要高過同核 producer」是硬規則，不是偏好。
//
// 階梯（由高到低）：
//   24  ipc1（IDF 系統）
//   22  esp_timer（IDF 系統）
//   15–7  **NC 核心保留**：decode 8、producer 7，其餘留白
//    6   緩衝區（留給 TinyUSB 5 之上的安全距離）
//    5   TinyUSB（IDF）
//    2   fx / nc_log
//    1   app_main / hello / bench
//    0   IDLE
#pragma once

// decode0 / decode1（nc4 的兩個解碼迴圈）
#define NC_PRIO_DECODE      8
// bus producer：cdc.rx / uart.rx / 將來的 espnow.rx、spi.rx …（一定要低過 decode）
#define NC_PRIO_PRODUCER    7
// 保留區的下界：任何非 NC 核心的任務唔可以用 >= 這個值的優先級
#define NC_PRIO_BAND_MIN    7
#define NC_PRIO_BAND_MAX   15
// 其他任務的上限（TinyUSB 5 之上留一格緩衝）
#define NC_PRIO_OTHERS_MAX  6
