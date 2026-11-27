// NCSTATE 讀寫 API：**driver／工具讀寫設定的唯一入口**。
//
// 讀：型別由 schema 決定（唔係由呼叫端猜）。schema 話 i8 就只可以 nc_state_get_i32；
//     用錯函式／store 冇呢個 key（sparse）→ false。false 唔代表錯誤，呼叫端保留自己的
//     預設值（同以前 nc_state_read_* 的語義一樣，只係搬去一個地方）。
//
// 寫：先經 schema 驗型別＋範圍（min／max／str 長度／enum 值），失敗回明示原因，
//     唔會靜默截斷或夾值。成功就寫入 RAM store，並計入 dirty（status 見得到）。
//
// **寫入語義（重要）**：只改 RAM store，唔會自己寫落 state.ncst／JSON。
//   重開機時 boot 仍然以 /data/config/*.json（L1，最高權威）重算；
//   所以「永久改」要改 JSON（見 docs/03_notes/config-layers.md）。呢個 API 係
//   runtime 調參（例：0xF0xx 臨時改 px／fps）同測試的基礎，避免多一條半權威路徑。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── 讀（schema 型別對唔上／sparse 冇值 → false） ─────────────────
bool nc_state_get_u32(uint32_t key, uint32_t *out);   // schema type: u8／u16／u32／enum
bool nc_state_get_i32(uint32_t key, int32_t *out);    // schema type: i8／i32
bool nc_state_get_bool(uint32_t key, bool *out);      // schema type: bool

// ── 寫（schema 驗證；只改 RAM） ──────────────────────────────────
typedef enum {
    NC_STATE_SET_OK = 0,
    NC_STATE_SET_UNKNOWN_KEY,    // schema 冇呢個 key
    NC_STATE_SET_WRONG_TYPE,     // 呼叫的 setter 同 schema 型別唔同
    NC_STATE_SET_OUT_OF_RANGE,   // 超出 schema min/max／str 太長／enum 冇呢個值
    NC_STATE_SET_NOT_READY,      // state 未 sync（store 未建立）
    NC_STATE_SET_NO_SPACE,       // store 滿／字串超出 store 容量
    NC_STATE_SET_NOT_ACTIVE,     // 呢個 unit 未啟動（改 config＋reboot）
    NC_STATE_SET_INVALID_ARG,    // 參數唔合法（空名／空指針）
} nc_state_set_res_t;

// ── 動態 key（任務／效果；**唔入字典**）────────────────────────
// 為什麼：任務管理器會熱插拔（.nct 隨時載入）、效果器亦會（.nce）；
// 靜態 index 一插就錯位，字典一定失效。所以動態 key 用「名的 hash」做身份：
//   key = 0xF0<<24 ｜ hash24(name)
// 名係穩定身份（改載入次序／插新任務都唔會令舊值對錯人）。
// 碰撞：24-bit（1/16M）；管理員載入時用 nc_state_key_is_dyn／同名重算自查，
// 撞到要大聲 log（唔好靜默蓋值）。
#define NC_STATE_DYN_DOMAIN 0xF0u

uint32_t nc_state_name_hash(const char *name);      // FNV-1a 32（同一個名永遠同值）
bool     nc_state_key_is_dyn(uint32_t key);
// 動態寫／讀：唔查 schema（呼叫者＝任務管理器／效果管理器，範圍自己驗）。
nc_state_set_res_t nc_state_dyn_set_s32(const char *name, int32_t v);
bool     nc_state_dyn_get_s32(const char *name, int32_t *v);

// ── 熱路徑綁定位址（唔使每次查 key） ───────────────────────────
// store 係固定陣列：同一個 key 之後再 set 都係改同一個位置，pointer 一直有效，
// 直到程序結束。適合 render／driver 在 init 時綁一次，loop 內直接讀。
// 未 sync／schema 冇此 key／型別唔對 → false（唔會回假 pointer）。
bool nc_state_bind_u32(uint32_t key, const uint32_t **out);
bool nc_state_bind_i32(uint32_t key, const int32_t **out);

// ── activation（S4）─────────────────────────────────────────────
// boot 由 cfg 算出「邊啲 unit 啟動」，寫入呢個 mask（bit = unit id）。
// 未啟動的 unit：唔會造 record，set 會回 NC_STATE_SET_NOT_ACTIVE。
void     nc_state_active_units_set(uint16_t mask);
uint16_t nc_state_active_units(void);
bool     nc_state_unit_active(uint8_t unit);


nc_state_set_res_t nc_state_set_u32(uint32_t key, uint32_t v);
nc_state_set_res_t nc_state_set_i32(uint32_t key, int32_t v);
nc_state_set_res_t nc_state_set_bool(uint32_t key, bool v);

// 失敗原因的字串（log／status 用）。
const char *nc_state_set_res_name(nc_state_set_res_t r);

// 今次開機由 nc_state_set_* 改過幾個「唔同的 key」（唔係呼叫次數）。
uint16_t nc_state_dirty_count(void);
void nc_state_dirty_reset(void);

#ifdef __cplusplus
}
#endif
