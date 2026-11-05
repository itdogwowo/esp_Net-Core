// fx ABI v0 —— host 與 guest（wasm 效果模組）之間唯一的契約。
//
// 這份 header 是「安全邊界」的具體化：
//   · guest 只能存取自己的線性記憶體；這塊記憶體由 host 從 fx.arena 切出來
//   · 兩邊唯一的資料交換 = 這份 descriptor + 參數區 + 幀緩衝
//   · host 不會把任何指標交給 guest（wasm 沒有指標，只有 offset）
//   · guest 碰不到 /data、周邊、其他模組、host heap —— 這是 wasm 結構保證，不是自律
//
// 設計來源：docs/03_notes/effect-language.md §6、fx-loader.md §5/§10
// 變更這份 header = 改 ABI；舊模組會因為 abi 不符被拒絕載入（不是靜靜跑錯）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_FX_ABI_VERSION   0
#define NC_FX_PAGE_BYTES    65536

// ── 線性記憶體的固定佈局（全部 32-byte 對齊）──
//   +0    descriptor（28 B，補到 32）
//   +32   參數區（16 × f32 = 64 B）
//   +96   scratch（模組自用，bytes 由 manifest 宣告，預設 4 KB）
//   +96+scratch  framebuffer（px_count × 4 B，canonical R,G,B,W）
#define NC_FX_DESC_OFF      0
#define NC_FX_PARAM_OFF     32
#define NC_FX_PARAM_BYTES   64
#define NC_FX_PARAM_COUNT   (NC_FX_PARAM_BYTES / 4)
#define NC_FX_DATA_OFF      96
#define NC_FX_ALIGN         32

// ── payload 的兩種格式（同一個 .fxe 容器）──
//   wasm：通用，任何晶片都吃（直譯）
//   aot ：host 上用 wamrc 編好的機器碼，綁架構；target_id 必須等於執行晶片
#define NC_FX_FMT_WASM      0
#define NC_FX_FMT_AOT       1

// target_id（.fxe header +24）
#define NC_FX_TARGET_WASM   0   // 通用位元組碼
#define NC_FX_TARGET_RV32F  1   // riscv32 ilp32f（P4 沒有 D 延伸！）
#define NC_FX_TARGET_XTENSA 2   // S3

// ── 上限（超過就拒絕載入，不會中途撇除）──
#define NC_FX_MAX_LINEAR    (128 * 1024)   // 2 pages
#define NC_FX_MAX_PIXELS    4096           // fb 最大 16 KB

// host 寫、guest 讀。little-endian。
typedef struct {
    uint32_t px_count;      // +0   像素數
    uint32_t width;         // +4
    uint32_t height;        // +8
    uint32_t fb_off;        // +12  幀緩衝在線性記憶體裡的 offset
    uint32_t fb_bytes;      // +16  px_count * 4
    uint32_t sc_off;        // +20  scratch offset
    uint32_t sc_bytes;      // +24  scratch bytes
} nc_fx_desc_t;             // 28 B

// ── guest 必須 export（缺 fx_render → 拒絕載入）──
//   fx_init(desc_ptr)            啟動時一次；回 0 = ok
//   fx_render(frame_id, time_us) 每幀；回 0 = ok，非 0 = 模組自報錯誤
//   fx_param(i)                  可選；沒宣告 = 不吃參數
#define NC_FX_EXPORT_INIT    "fx_init"
#define NC_FX_EXPORT_RENDER  "fx_render"
#define NC_FX_EXPORT_PARAM   "fx_param"

// ── host 提供給 guest 的 import ──
// v0 刻意是**零個**：模組不能印字、不能讀時間、不能碰檔案。
// 時間與幀號由 fx_render 的參數給；要更多能力必須升 abi，舊模組不受影響。
#define NC_FX_IMPORT_COUNT   0

// ── 每幀預算（host 量測；連續超支就降級）──
#define NC_FX_FRAME_BUDGET_US   16000   // 33 ms 幀的一半
#define NC_FX_BUDGET_STRIKES    3       // 連續 3 次超支 → 標記 bad

#ifdef __cplusplus
}
#endif
