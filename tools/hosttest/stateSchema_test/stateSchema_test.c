// stateSchema_test — schema registry（schema/state-keys.json → 生成表）host test。
//
// 驗的是「一個地方」的契約：
//   1. 查表：id ↔ name ↔ unit，表同 header 同步（每個 key 都查得到）
//   2. 型別：schema type 對應 blob type（uint／sint／bool／bytes／enum）
//   3. 範圍：min/max、str 長度、enum 值
//   4. check：boot／set 共用同一條驗證規則（未知 key／型別／範圍）
//   5. 讀寫 API：schema 型別把關、set 驗範圍、dirty 計唔同 key
#include <stdio.h>
#include <string.h>

#include "nc_state_api.h"
#include "nc_state_keys.h"
#include "nc_state_schema.h"
#include "nc_state_store.h"

// ── boot 的 store stub（呢支測試唔拉 config／FAT） ──────────────
static nc_state_store_t s_stub;
static bool s_ready;

const nc_state_store_t *nc_state_boot_store(void)
{
    return (s_ready && s_stub.n > 0) ? &s_stub : NULL;
}

nc_state_store_t *nc_state_boot_store_mut(void)
{
    return s_ready ? &s_stub : NULL;
}

static int g_fail;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            g_fail++;                                                           \
        }                                                                       \
    } while (0)

int main(void)
{
    printf("[1] 查表：id／name／unit 一致、每個 key 有 path\n");
    CHECK(nc_state_key_count() == NC_STATE_KEY_COUNT, "count=%u", nc_state_key_count());
    CHECK(nc_state_unit_count() == NC_STATE_UNIT_COUNT, "units=%u", nc_state_unit_count());
    for (uint32_t i = 0; i < nc_state_key_count(); i++) {
        const nc_state_keydef_t *d = nc_state_key_at(i);
        CHECK(d != NULL, "key_at(%u) NULL", i);
        if (d == NULL) {
            continue;
        }
        CHECK(nc_state_key(d->id) == d, "0x%08x 用 id 查唔返", d->id);
        CHECK(nc_state_key_name(d->name) == d, "%s 用名查唔返", d->name);
        CHECK(nc_state_unit(d->unit) != NULL, "%s 的 unit %u 唔存在", d->name, d->unit);
        CHECK(((d->id >> 16) & 0xFF) == d->unit,
              "0x%08x 的 unit byte 同 unit %u 唔一致", d->id, d->unit);
        CHECK(d->path[0] != '\0', "%s 冇 path", d->name);
        CHECK(d->min <= d->max, "%s min %d > max %d", d->name, d->min, d->max);
    }
    CHECK(nc_state_key(0xFFFFFFFFu) == NULL, "未知 id 應該 NULL");
    CHECK(nc_state_key_name("NC_KEY_NOPE") == NULL, "未知名應該 NULL");
    CHECK(nc_state_key_name(NULL) == NULL, "NULL 名應該 NULL");
    CHECK(nc_state_key_at(NC_STATE_KEY_COUNT) == NULL, "越界應該 NULL");
    CHECK(nc_state_unit(250) == NULL, "未知 unit 應該 NULL");

    printf("[2] 型別：schema type → blob type\n");
    const nc_state_keydef_t *k;
    k = nc_state_key(NC_KEY_SYS_FRAME_MS);
    CHECK(k != NULL && k->type == NC_STATE_T_UINT && k->unit == NC_UNIT_SYS, "frame_ms");
    k = nc_state_key(NC_KEY_I80_WR);
    CHECK(k != NULL && k->type == NC_STATE_T_SINT && k->min == -1, "i80 wr");
    k = nc_state_key(NC_KEY_LED_ENABLE);
    CHECK(k != NULL && k->type == NC_STATE_T_BOOL, "led enable");
    k = nc_state_key(NC_KEY_LED_FMT);
    CHECK(k != NULL && k->type == NC_STATE_T_UINT && k->n_enum == 5, "led fmt（enum）");
    CHECK(k != NULL && strcmp(nc_state_enum_name(k, 0), "ws2812") == 0, "fmt enum 0");
    CHECK(k != NULL && nc_state_enum_id(k, "apa102") == 4, "fmt 名 → index");
    CHECK(k != NULL && nc_state_enum_id(k, "nope") == -1, "未知 fmt 名 → -1");
    k = nc_state_key(NC_KEY_LED_BACKEND);
    CHECK(k != NULL && k->type == NC_STATE_T_UINT && k->n_enum == 7, "led backend enum");
    CHECK(k != NULL && strcmp(nc_state_enum_name(k, 1), "i80") == 0, "enum 1 = i80");
    CHECK(k != NULL && nc_state_enum_name(k, 7) == NULL, "enum 7 應該 NULL");
    CHECK(nc_state_type_name(NC_STATE_T_SINT) != NULL, "type name");

    printf("[2b] class：value 才入 store；struct 係字典登記\n");
    CHECK(NC_STATE_KEY_VALUE + NC_STATE_KEY_STRUCT == NC_STATE_KEY_COUNT, "value+struct 要等於總數");
    CHECK(NC_STATE_KEY_STRUCT > 0 && NC_STATE_KEY_VALUE > 0, "兩類都要有");
    k = nc_state_key(NC_KEY_SYS_CID);
    CHECK(k != NULL && k->cls == NC_STATE_CLS_VALUE, "cID 應該係 value");
    k = nc_state_key(NC_KEY_LINK0_ENABLE);
    CHECK(k != NULL && k->cls == NC_STATE_CLS_STRUCT, "link 應該係 struct");
    k = nc_state_key(NC_KEY_TASK0_LEVEL);
    CHECK(k != NULL && k->cls == NC_STATE_CLS_STRUCT, "task 應該係 struct");
    for (uint32_t i = 0; i < nc_state_key_count(); i++) {
        const nc_state_keydef_t *d = nc_state_key_at(i);
        CHECK(d != NULL && (d->cls == NC_STATE_CLS_VALUE || d->cls == NC_STATE_CLS_STRUCT),
              "key %u 的 class 唔合法（%u）", i, d ? d->cls : 9u);
        CHECK(d != NULL && d->path[0] != '\0', "key %u 冇 path", i);
    }

    printf("[3] 範圍：min／max／enum／str\n");
    k = nc_state_key(NC_KEY_SYS_FRAME_MS);
    CHECK(!nc_state_key_in_range(k, 0), "frame_ms 0 應該超範圍");
    CHECK(nc_state_key_in_range(k, 1), "frame_ms 1 應該 OK");
    CHECK(nc_state_key_in_range(k, 1000), "frame_ms 1000 應該 OK");
    CHECK(!nc_state_key_in_range(k, 1001), "frame_ms 1001 應該超範圍");
    k = nc_state_key(NC_KEY_I80_WR);
    CHECK(nc_state_key_in_range(k, -1), "wr -1 應該 OK");
    CHECK(!nc_state_key_in_range(k, -2), "wr -2 應該超範圍");
    CHECK(!nc_state_key_in_range(k, 55), "wr 55 應該超範圍");
    k = nc_state_key(NC_KEY_LED_FMT);
    CHECK(nc_state_key_in_range(k, 4), "enum 4 應該 OK");
    CHECK(!nc_state_key_in_range(k, 5), "enum 5 應該超範圍");
    CHECK(!nc_state_key_str_ok(k, 5), "enum key 用 str 規則應該 false");
    CHECK(!nc_state_key_str_ok(nc_state_key(NC_KEY_LED_PX), 1), "非 str key 應該 false");

    printf("[4] check：boot／set 共用的驗證\n");
    nc_state_rec_t r = { .key = NC_KEY_LED_PX, .type = NC_STATE_T_UINT, .u = 64 };
    CHECK(nc_state_schema_check(&r) == NC_STATE_CHK_OK, "正常 record 應該 OK");
    r.key = 0x7FFF;
    CHECK(nc_state_schema_check(&r) == NC_STATE_CHK_UNKNOWN_KEY, "未知 key");
    r.key = NC_KEY_LED_PX;
    r.type = NC_STATE_T_SINT;
    CHECK(nc_state_schema_check(&r) == NC_STATE_CHK_TYPE, "型別唔啱");
    r.type = NC_STATE_T_UINT;
    r.u = 0;
    CHECK(nc_state_schema_check(&r) == NC_STATE_CHK_RANGE, "px 0 應該超範圍");
    nc_state_rec_t rs = { .key = NC_KEY_LED_FMT, .type = NC_STATE_T_UINT, .u = 5 };
    CHECK(nc_state_schema_check(&rs) == NC_STATE_CHK_RANGE, "fmt enum 5 超範圍");
    CHECK(strcmp(nc_state_schema_check_name(NC_STATE_CHK_TYPE), "type") == 0, "check name");

    printf("[5] 讀：schema 型別把關、sparse 回 false\n");
    s_ready = true;
    nc_state_store_clear(&s_stub);
    nc_state_dirty_reset();
    uint32_t u = 0;
    int32_t iv = 0;
    bool bv = false;
    CHECK(!nc_state_get_u32(NC_KEY_LED_PX, &u), "空 store 應該 false");
    nc_state_rec_t recs[] = {
        { .key = NC_KEY_LED_PX, .type = NC_STATE_T_UINT, .u = 64 },
        { .key = NC_KEY_I80_WR, .type = NC_STATE_T_SINT, .i = -1 },
        { .key = NC_KEY_LED_ENABLE, .type = NC_STATE_T_BOOL, .u = 1 },
        { .key = NC_KEY_LED_BACKEND, .type = NC_STATE_T_UINT, .u = 1 },
    };
    for (size_t i = 0; i < sizeof(recs) / sizeof(recs[0]); i++) {
        CHECK(nc_state_store_put(&s_stub, &recs[i]), "put[%zu]", i);
    }
    CHECK(nc_state_get_u32(NC_KEY_LED_PX, &u) && u == 64, "get px");
    CHECK(nc_state_get_i32(NC_KEY_I80_WR, &iv) && iv == -1, "get wr");
    CHECK(nc_state_get_bool(NC_KEY_LED_ENABLE, &bv) && bv, "get enable");
    CHECK(nc_state_get_u32(NC_KEY_LED_BACKEND, &u) && u == 1, "get backend（enum 都係 uint）");
    CHECK(!nc_state_get_u32(NC_KEY_I80_WR, &u), "sint key 用 u32 讀應該 false");
    CHECK(!nc_state_get_i32(NC_KEY_LED_PX, &iv), "uint key 用 i32 讀應該 false");
    CHECK(!nc_state_get_bool(NC_KEY_LED_PX, &bv), "uint key 用 bool 讀應該 false");

    printf("[6] 寫：schema 驗範圍、明示失敗、dirty 計唔同 key\n");
    nc_state_dirty_reset();
    CHECK(nc_state_set_u32(NC_KEY_LED_PX, 128) == NC_STATE_SET_OK, "set px 128");
    CHECK(nc_state_get_u32(NC_KEY_LED_PX, &u) && u == 128, "set 後讀返 128");
    CHECK(nc_state_set_u32(NC_KEY_LED_PX, 0) == NC_STATE_SET_OUT_OF_RANGE, "px 0 應該拒絕");
    CHECK(nc_state_get_u32(NC_KEY_LED_PX, &u) && u == 128, "拒絕後值唔應該變");
    CHECK(nc_state_set_u32(0x7FFF, 1) == NC_STATE_SET_UNKNOWN_KEY, "未知 key");
    CHECK(nc_state_set_i32(NC_KEY_LED_PX, 1) == NC_STATE_SET_WRONG_TYPE, "型別唔啱");
    CHECK(nc_state_set_i32(NC_KEY_I80_WR, -5) == NC_STATE_SET_OUT_OF_RANGE, "wr -5 拒絕");
    CHECK(nc_state_set_bool(NC_KEY_LED_ENABLE, false) == NC_STATE_SET_OK, "set enable");
    CHECK(nc_state_set_u32(NC_KEY_LED_FMT, 0) == NC_STATE_SET_OK, "set fmt enum 0");
    CHECK(nc_state_set_u32(NC_KEY_LED_FMT, 5) == NC_STATE_SET_OUT_OF_RANGE,
          "fmt enum 5 應該拒絕");
    {
        uint32_t v = 9;
        CHECK(nc_state_get_u32(NC_KEY_LED_FMT, &v) && v == 0, "get fmt");
        CHECK(strcmp(nc_state_enum_name(nc_state_key(NC_KEY_LED_FMT), v), "ws2812") == 0,
              "fmt index → 名");
    }
    CHECK(nc_state_dirty_count() == 3, "dirty=%u（px／enable／fmt）", nc_state_dirty_count());
    CHECK(nc_state_set_u32(NC_KEY_LED_PX, 256) == NC_STATE_SET_OK, "再 set px");
    CHECK(nc_state_dirty_count() == 3, "同一個 key 再寫唔應該加 dirty=%u", nc_state_dirty_count());

    printf("[6b] activation：未啟動 unit → not_active（S4）\n");
    nc_state_active_units_set((uint16_t)(1u << NC_UNIT_SYS));
    CHECK(!nc_state_unit_active(NC_UNIT_LED), "LED 應該標成未啟動");
    CHECK(nc_state_unit_active(NC_UNIT_SYS), "SYS 永遠啟動");
    CHECK(nc_state_set_u32(NC_KEY_LED_PX, 128) == NC_STATE_SET_NOT_ACTIVE,
          "未啟動 unit 的 set 要回 not_active");
    CHECK(nc_state_set_u32(NC_KEY_SYS_FRAME_MS, 20) == NC_STATE_SET_OK,
          "已啟動 unit 照常可寫");
    nc_state_active_units_set(0xFFFFu);

    printf("[7] 未 sync（store 未 ready）→ not_ready\n");
    s_ready = false;
    CHECK(nc_state_set_u32(NC_KEY_LED_PX, 64) == NC_STATE_SET_NOT_READY, "not ready");
    CHECK(!nc_state_get_u32(NC_KEY_LED_PX, &u), "未 ready 讀都係 false");
    s_ready = true;

    printf("[8] schema 版本／CRC 存在\n");
    CHECK(nc_state_schema_version() == NC_STATE_SCHEMA_VERSION, "version");
    CHECK(nc_state_schema_crc() != 0, "crc");

    if (g_fail == 0) {
        printf("stateSchema_test OK\n");
        return 0;
    }
    printf("stateSchema_test FAIL（%d 項）\n", g_fail);
    return 1;
}
