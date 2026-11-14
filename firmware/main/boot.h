// 開機：**契約**在這裏，實作在 main.c。
//
// 三層分工
// --------
//   boot.h  契約：一個階段長什麼樣（name／needs／gives／fn）、跨階段要傳什麼（ctx）
//   boot.c  執行器：驗依賴 → 呼叫 → 計時 → 記錄 → 印摘要
//   main.c  實作：每個階段真正做什麼（NVS、storage、capability、USB、通道…）
//
// 為什麼要有 needs／gives
// ----------------------
// 原本「順序就是資料」只答了「先後」，冇答「為什麼」。加一步時你仍然要記住
// 它要等邊個跑完 —— 而那些知識只寫在註解裡，改次序時冇人會提醒你。
//
// 現在每個階段自己講清楚：
//   needs = 進入前必須已具備（執行器驗，缺了就 SKIP 並講出缺什麼）
//   gives = 成功後提供什麼（執行器標記，供後面的階段驗）
//
// 好處是「一個階段失敗」的後果變成集中的、可讀的：以前 USB 沒起來 / config 沒讀到，
// 下游十幾個地方各自壞、各自印，症狀散開；現在下游直接 SKIP 並寫明「缺 usb」。
//
// 跨階段資料只准經 ctx
// --------------------
// ctx 是「這次開機的決定」：幾多核、USB 用邊條、link 叫什麼……
// 階段之間**不要**再用 main.c 的 static 互相傳 —— 那些是實作細節，
// 它們的壽命、誰先寫誰先讀，全靠閱讀順序去猜。
//
// 現在 ctx 只收得落「核數 + USB（人格＋組）」六個欄位（2026-11-13）。其餘階段**刻意未搬**：
// 每搬一個欄位 = 一個階段從「static 互傳」變成「契約的一部份」。
// 搬的時候只改三處：ctx 加欄位、階段寫它、讀者改讀它 —— 一次一個。
//
// 現階段 main.c 的 ph_* 只負責「填 ctx」與「呼叫模組」；模組本身唔知開機次序。
// 將來的形狀是 ph_* 也可以搬去自己的檔案，main.c 只剩一張表。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ───────────────────────── 能力：USB 模式 ─────────────────────────
// 由 capability 階段決定一次（編譯期能力 + 執行期 config 的合成結果）。
// 這個 enum 屬於契約而不是 main.c 的細節：STATUS、log、測試都會用同一組字。
typedef enum {
    NC_USB_NONE = 0,      // 這次開機沒有 USB 線（或編譯期關掉）
    NC_USB_JTAG,          // 這條線走 USB-Serial-JTAG（USJ）：C3 類晶片，或 S3／P4 改 config
    NC_USB_OTG,           // 這條線走 USB-OTG：TinyUSB CDC（＋可選 MSC）；S2／S3／P4
} nc_usb_mode_t;

// "none" / "jtag" / "otg"（boot 與 STATUS 共用同一個字串，避免兩處寫法不同）
const char *nc_usb_mode_name(nc_usb_mode_t mode);

// ───────────────────────── 跨階段的資料 ─────────────────────────
// 只收「後面的階段要讀」的決定。實作細節留在各自的模組裡。
typedef struct {
    // ① 核數（NC_CAP_CORES）：唯讀鏡像，真相在 nc_core_count()
    int cores;

    // ② USB（NC_CAP_USB）：模式 + NC4 那條 link 的名字 + 兩個執行期閘
    nc_usb_mode_t usb_mode;
    const char *usb_link;   // "CDC" / "usb_jtag" / NULL
    bool usb_log;           // 要不要開 log 管道
    bool usb_msc;           // 要不要把 FAT 交給 host（只有 OTG 有）

    // ③ USB 這條線屬於哪一組（NC_CAP_ROUTER）：B05 建完「線 → 組」表之後寫入。
    //    -1 = 這條線未路由（config 的 routes[] 沒有它）。之後 B07（回覆幀）、
    //    B10（TinyUSB／log）、B11（bus）全部讀這裡 —— 唔再各自去問 router，
    //    否則同一個決定會問四次，仲各自帶 fallback。
    int usb_core;

    // 帳：目前已具備什麼（執行器維護，階段只讀）
    uint16_t ready;
} nc_boot_ctx_t;

// 單例。階段與讀者都經它拿 ctx —— 唔好再自己開 static 副本。
nc_boot_ctx_t *nc_boot_ctx(void);

// ───────────────────── 能力位（唔係「階段」）─────────────────────
// 一個位 = 一件事「已經準備好」。階段用 needs／gives 講自己站在哪個位之上。
// 名字刻意用 CAP 而唔係 STAGE：位元唔係階段，只係階段留下的痕跡。
#define NC_CAP_NVS       0x0001u   // 日誌／NVS 可用
#define NC_CAP_STORAGE   0x0002u   // /data 可用（APP 模式）
#define NC_CAP_CONFIG    0x0004u   // config 三層已載入（system／hardware／network）
#define NC_CAP_CORES     0x0008u   // 核數已探測（nc_core_count() 有答案）
#define NC_CAP_USB       0x0010u   // USB 模式／link 名已定
#define NC_CAP_ROUTER    0x0020u   // 線 → 核 的路由表已定
#define NC_CAP_DECLARED  0x0040u   // 記憶體已向 plan 宣告（未 commit）
#define NC_CAP_PLAN      0x0080u   // 記憶體已 commit（可以派）
#define NC_CAP_TASKS     0x0100u   // 任務已 start（資源已派、loop 未開）
#define NC_CAP_CHANNELS  0x0200u   // 通道（bus）已建立

// 給 log 用的名字（"config|usb" 這種）；缺依賴時要講得出缺什麼。
const char *nc_cap_name(uint16_t bit);

// ───────────────────────── 識別碼（ID）─────────────────────────
// 問題：「storage+cfg」係一個**階段**，定係 storage 同 cfg 兩個**功能**？
// 答案：兩樣都要有自己嘅名字，而且講嘅時候要分得開。所以每個階段有一個 ID。
//
// 命名空間（全專案共用；文件、log、圖、commit message 都用同一個 token）：
//
//   B01…   開機階段（下表；順序執行，整輩子一次）      B03 ＝ storage+cfg
//          B06（pool）已於 2026-11-14 移除 → B06 跳號，B07–B12 不重編號
//   T.xxx  任務（nc_task_def.name；有棧、有核）        T.decode0、T.cdc.rx
//   R.xxx  plan 資源／緩衝（nc_task_res_t.name）       R.usb.rx、R.reply1
//   L.xxx  通道／線（link 名；NC4 的線名）             L.CDC、L.UART
//   C.xxxx 協議指令（16-bit）                          C.0x1101
//   M.xxx  模塊／功能（component 或一組行為）          M.store、M.config
//
// 規則
//   ① ID 係**契約**：一旦寫落文件／log／圖就唔改。要改就係「換一個 ID」，
//      並且要一齊改引用它的地方（見 tools/id_check.py）。
//   ② 講嘢要帶前綴。「B03 慢」同「M.store 慢」唔同結論：一個係開機次序問題，
//      一個係模塊本身問題。
//   ③ ID **唔反映次序**以外嘅意思：B 的數字細唔代表重要，只代表先後。
//
// ───────────────────────── 一個階段 ─────────────────────────
typedef struct {
    const char *id;                   // 穩定 ID（"B01"）；log 同文件都用它
    const char *name;                 // 人讀的描述（開機 log 會印；可以改）
    uint16_t needs;                   // 進入前必須已具備（缺 → SKIP）
    uint16_t gives;                   // 成功後標記為已具備
    esp_err_t (*fn)(void);            // 實作；NULL 當成成功（預留位）
    bool optional;                    // 失敗唔算 boot 失敗（只記錄）
} nc_boot_stage_t;

#define NC_BOOT_COUNT(_arr) (sizeof(_arr) / sizeof((_arr)[0]))

// ───────────────────────── 結果（給 STATUS／診斷）─────────────────────────
typedef enum {
    NC_BOOT_UNRUN = 0,
    NC_BOOT_OK,
    NC_BOOT_SKIP,      // 缺依賴，冇跑
    NC_BOOT_FAIL,
    NC_BOOT_WARN,      // optional 階段失敗
} nc_boot_state_t;

typedef struct {
    const char *id;                   // "B07"
    const char *name;
    nc_boot_state_t state;
    esp_err_t err;
    uint32_t us;                      // 這個階段用了幾 µs
} nc_boot_result_t;

// 依序執行全部階段（**不會因為失敗而停** —— 失敗只會被記錄，
// 後面的階段靠 needs 自己決定要不要跑）。
// 回傳第一個「非 optional」失敗的階段索引；全部成功／都已 SKIP 回 -1。
int nc_boot_run(const nc_boot_stage_t *stages, size_t n);

// 上一次 run 的逐階段結果（唯讀；n 可為 NULL）。最多記 NC_BOOT_MAX_STAGES 個。
#define NC_BOOT_MAX_STAGES 16
const nc_boot_result_t *nc_boot_results(size_t *n);

#ifdef __cplusplus
}
#endif
