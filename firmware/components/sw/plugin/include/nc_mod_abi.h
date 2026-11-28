// ncm ABI v0 —— host 與 guest（wasm 模組）之間唯一的契約。
//
// 這份 header 是「安全邊界」的具體化：
//   · guest 只能存取自己的線性記憶體；這塊記憶體由 host 從 ncm.arena 切出來
//   · 兩邊唯一的資料交換 = 這份 descriptor + 參數區 + 幀緩衝
//   · host 不會把任何指標交給 guest（wasm 沒有指標，只有 offset）
//   · guest 碰不到 /data、周邊、其他模組、host heap —— 這是 wasm 結構保證，不是自律
//
// 設計來源：docs/03_notes/effect-language.md §6、ncm-loader.md §5/§10
// 變更這份 header = 改 ABI；舊模組會因為 abi 不符被拒絕載入（不是靜靜跑錯）。
//
// 2026-11-20：效果的 export 由 `fx_init`／`fx_render`／`fx_param` 改名為
// `nce_init`／`nce_render`／`nce_param`（`fx` 全面退役，status.md §7.132）。
// **記憶體佈局冇變**；舊 `.nce` 缺新名 export，載入時被拒 —— 重 build 即可，不用改 offset。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_MOD_ABI_VERSION   0
#define NC_MOD_PAGE_BYTES    65536

// ── 線性記憶體的固定佈局（全部 32-byte 對齊）──
//   +0    descriptor（28 B，補到 32）
//   +32   參數區（16 × f32 = 64 B）
//   +96   scratch（模組自用，bytes 由 manifest 宣告，預設 4 KB）
//   +96+scratch  framebuffer（px_count × 4 B，canonical R,G,B,W）
#define NC_MOD_DESC_OFF      0
#define NC_MOD_PARAM_OFF     32
#define NC_MOD_PARAM_BYTES   64
#define NC_MOD_PARAM_COUNT   (NC_MOD_PARAM_BYTES / 4)
#define NC_MOD_DATA_OFF      96
#define NC_MOD_ALIGN         32

// ── payload 的兩種格式（同一個 .nct / .nce 容器）──
//   wasm：通用，任何晶片都吃（直譯）
//   aot ：host 上用 wamrc 編好的機器碼，綁架構；target_id 必須等於執行晶片
#define NC_MOD_FMT_WASM      0
#define NC_MOD_FMT_AOT       1

// target_id（.ncm header +24）
#define NC_MOD_TARGET_WASM   0   // 通用位元組碼
#define NC_MOD_TARGET_RV32F  1   // riscv32 ilp32f（P4 沒有 D 延伸！）
#define NC_MOD_TARGET_XTENSA 2   // S3

// ── 上限（超過就拒絕載入，不會中途撇除）──
#define NC_MOD_MAX_LINEAR    (128 * 1024)   // 2 pages
// 值數上限（effect：每值 1 B）。fb 放唔放得落由 mem_bytes 同 descriptor 檢查把關。
#define NC_MOD_MAX_PIXELS    65535u

// host 寫、guest 讀。little-endian。
typedef struct {
    uint32_t px_count;      // +0   像素數
    uint32_t width;         // +4
    uint32_t height;        // +8
    uint32_t fb_off;        // +12  幀緩衝在線性記憶體裡的 offset
    uint32_t fb_bytes;      // +16  px_count * 4
    uint32_t sc_off;        // +20  scratch offset
    uint32_t sc_bytes;      // +24  scratch bytes
} nc_mod_desc_t;             // 28 B

// ── guest 必須 export（缺 nce_render → 拒絕載入）──
//   nce_init(desc_ptr)            啟動時一次；回 0 = ok
//   nce_render(frame_id, time_us) 每幀；回 0 = ok，非 0 = 模組自報錯誤
//   nce_param(i)                  可選；沒宣告 = 不吃參數
#define NC_MOD_EXPORT_INIT    "nce_init"
#define NC_MOD_EXPORT_RENDER  "nce_render"
#define NC_MOD_EXPORT_PARAM   "nce_param"

// ── host 提供給 guest 的 import ──
// 效果（v0）：零個 —— 時間與幀號由 nce_render 的參數給。
//
// **任務模組（v1，2026-11-17）：白名單 import**。任務要有「落手做嘢」的能力，
// 所以開五個 host function；仍然係白名單制（不是全部開放），而且全部只經
// host 做，guest 冇 pointer、冇 syscall、碰不到暫存器。
//
//   wasm import module = "env"
//
//   nc_print(ptr:i32, len:i32) -> i32   線性記憶體的一段文字寫去裝置 log
//   nc_print_i32(v:i32)        -> i32   一個整數寫去 log（十進位，獨立一行）
//   nc_print2(p1:i32,l1:i32,v1:i32,p2:i32,l2:i32,v2:i32) -> i32
//                                       一行過印 "<文字1><v1> <文字2><v2>"
//                                       （給「讀 GPIO 再 print」這類一行報告用）
//   nc_now_us() -> i64               開機以來微秒（等於 MPY 的 time.ticks_us()）
//   nc_gpio_read(pin:i32)      -> i32   讀 GPIO（第一次會設成輸入＋pull-up）
//   nc_gpio_write(pin:i32,v)   -> i32   設成輸出再寫（回 0／-1）
//   nc_gpio_toggle(pin:i32)    -> i32   反轉並回新電平（-1 = 失敗）
//
// 只准「安全腳位」：以下一律拒絕（回 -1）——
//   0／3／45／46（boot strap）、7／8（本板 UART）、19／20（USB OTG）、
//   26–37（SPI flash／octal PSRAM）、43／44（UART0 預設）。
//
#define NC_MOD_IMPORT_COUNT   7

// 任務模組（.nct）的 export：task_tick 必需，task_init 可選。
// 節奏 = manifest 的 fps（0 → 預設 10 ms）；budget_us 由 manifest 宣告。
#define NC_MOD_ABI_TASK_VERSION 1
#define NC_MOD_EXPORT_TASK_INIT  "task_init"
#define NC_MOD_EXPORT_TASK_TICK  "task_tick"

#define NC_MOD_IMPORT_PRINT      "nc_print"
#define NC_MOD_IMPORT_PRINT_I32  "nc_print_i32"
#define NC_MOD_IMPORT_PRINT2     "nc_print2"
#define NC_MOD_IMPORT_NOW_US     "nc_now_us"
#define NC_MOD_IMPORT_GPIO_READ  "nc_gpio_read"
#define NC_MOD_IMPORT_GPIO_WRITE "nc_gpio_write"
#define NC_MOD_IMPORT_GPIO_TOGGLE "nc_gpio_toggle"

// ── 每幀預算（host 量測；連續超支就降級）──
#define NC_MOD_FRAME_BUDGET_US   16000   // 33 ms 幀的一半
#define NC_MOD_BUDGET_STRIKES    3       // 連續 3 次超支 → 標記 bad

#ifdef __cplusplus
}
#endif
