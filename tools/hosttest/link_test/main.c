// NC4 出口登記表 host 測試（唔需要板子、唔需要 IDF）
//
// 驗嘅係「link 名 → 送出函式」這張表同 nc4_link_send 的三種結果：
//   1. 整幀交出去（回 len）→ frames/bytes 增加
//   2. 短送（回 < len）     → drops 增加、回 ESP_ERR_TIMEOUT
//   3. 錯誤（回 < 0）       → drops 增加、回 ESP_FAIL
// 亦驗：重名/壞名/滿格、查唔到、統計、JSON 截斷安全。
//
// 呢個測試同時係「link 表唔靠 IDF」嘅證明：nc4_link.c 只 include esp_err.h 同 libc。
#include <stdio.h>
#include <string.h>

#include "nc4_link.h"

static int s_fail;
static int s_case;

static void check_impl(int cond, int line, const char *what)
{
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        printf("        (%s:%d)\n", __FILE__, line);
        s_fail++;
    }
}

#define CHECK(cond, what) check_impl((cond), __LINE__, what)

static void case_begin(const char *name)
{
    s_case++;
    printf("[%d] %s\n", s_case, name);
}

// ── 假的目的通道 ──
static uint8_t s_sink[64];
static size_t  s_sink_len;
static int     s_sink_calls;
static int     s_sink_mode;      // 0 = 成功、>0 = 短送（回這個長度）、<0 = 錯誤

static int sink_write(void *arg, const uint8_t *data, size_t len)
{
    s_sink_calls++;
    if (arg != NULL) {
        (*(int *)arg)++;
    }
    s_sink_len = len;
    if (len <= sizeof(s_sink)) {
        memcpy(s_sink, data, len);
    }
    if (s_sink_mode < 0) {
        return -1;
    }
    if (s_sink_mode > 0) {
        return (len < (size_t)s_sink_mode) ? (int)len : s_sink_mode;
    }
    return (int)len;
}

static void sink_reset(void)
{
    memset(s_sink, 0, sizeof(s_sink));
    s_sink_len = 0;
    s_sink_calls = 0;
    s_sink_mode = 0;
}

int main(void)
{
    printf("nc4_link host test\n");

    // ── 1. 空表 ──
    case_begin("空表：冇出口、查唔到、send 回 NOT_FOUND");
    nc4_link_reset();
    CHECK(nc4_link_count() == 0, "count == 0");
    CHECK(!nc4_link_has("CDC"), "CDC 未登記");
    CHECK(nc4_link_send("CDC", (const uint8_t *)"x", 1) == ESP_ERR_NOT_FOUND,
          "未登記 → NOT_FOUND");
    CHECK(nc4_link_send(NULL, (const uint8_t *)"x", 1) == ESP_ERR_NOT_FOUND,
          "NULL 名 → NOT_FOUND");

    // ── 2. 登記 ──
    case_begin("登記：CDC / UART，重名與壞輸入要拒絕");
    {
        int calls = 0;
        CHECK(nc4_link_register("CDC", sink_write, &calls) == ESP_OK, "CDC 登記");
        CHECK(nc4_link_register("UART", sink_write, NULL) == ESP_OK, "UART 登記");
        CHECK(nc4_link_count() == 2, "count == 2");
        CHECK(nc4_link_has("CDC") && nc4_link_has("UART"), "兩條都查得到");
        CHECK(!nc4_link_has("TCP"), "TCP 未登記");
        CHECK(nc4_link_has("CD") == false, "只做全名比對");
        CHECK(nc4_link_register("CDC", sink_write, NULL) == ESP_ERR_INVALID_STATE,
              "重名 → INVALID_STATE（唔會蓋掉舊的）");
        CHECK(nc4_link_register("", sink_write, NULL) == ESP_ERR_INVALID_ARG, "空名 → INVALID_ARG");
        CHECK(nc4_link_register(NULL, sink_write, NULL) == ESP_ERR_INVALID_ARG, "NULL 名 → INVALID_ARG");
        CHECK(nc4_link_register("X", NULL, NULL) == ESP_ERR_INVALID_ARG, "冇 write → INVALID_ARG");
        CHECK(nc4_link_register("0123456789abcdef", sink_write, NULL) == ESP_ERR_INVALID_ARG,
              "名字太長（= NC4_LINK_NAME_MAX）→ INVALID_ARG");
        CHECK(nc4_link_register("0123456789abcde", sink_write, NULL) == ESP_OK,
              "名字剛好 NC4_LINK_NAME_MAX-1 可以");
        CHECK(nc4_link_count() == 3, "count == 3");
        nc4_link_reset();
    }

    // ── 3. 成功送出 ──
    case_begin("整幀送出：回 len → 內容原樣、frames/bytes 增加");
    {
        sink_reset();
        int calls = 0;
        nc4_link_reset();
        nc4_link_register("CDC", sink_write, &calls);
        const uint8_t frame[] = {'N', 'C', 4, 0, 0, 1, 0, 2, 0, 0xAA, 0xBB};
        CHECK(nc4_link_send("CDC", frame, sizeof(frame)) == ESP_OK, "send OK");
        CHECK(calls == 1, "write 被叫一次");
        CHECK(s_sink_len == sizeof(frame), "長度一致");
        CHECK(memcmp(s_sink, frame, sizeof(frame)) == 0, "內容逐 byte 一致（冇被改）");
        uint32_t f = 0, b = 0, d = 0;
        nc4_link_stats("CDC", &f, &b, &d);
        CHECK(f == 1, "frames == 1");
        CHECK(b == sizeof(frame), "bytes == 幀長");
        CHECK(d == 0, "drops == 0");
        CHECK(nc4_link_send("CDC", frame, 0) == ESP_ERR_INVALID_ARG, "len 0 → INVALID_ARG");
        CHECK(nc4_link_send("CDC", NULL, 5) == ESP_ERR_INVALID_ARG, "NULL frame → INVALID_ARG");
        nc4_link_stats("CDC", &f, &b, &d);
        CHECK(f == 1, "失敗的 send 唔會計 frames");
    }

    // ── 4. 短送 ──
    case_begin("短送：回 < len → drops＋last_err=TIMEOUT");
    {
        sink_reset();
        nc4_link_reset();
        nc4_link_register("UART", sink_write, NULL);
        s_sink_mode = 3;                       // 只收 3 B
        const uint8_t frame[10] = {0};
        CHECK(nc4_link_send("UART", frame, sizeof(frame)) == ESP_ERR_TIMEOUT,
              "短送 → ESP_ERR_TIMEOUT");
        uint32_t f = 0, b = 0, d = 0;
        nc4_link_stats("UART", &f, &b, &d);
        CHECK(f == 0, "frames 唔增加");
        CHECK(b == 0, "bytes 唔增加");
        CHECK(d == 1, "drops == 1");
    }

    // ── 5. 錯誤 ──
    case_begin("錯誤：write 回 -1 → ESP_FAIL＋drops");
    {
        sink_reset();
        nc4_link_reset();
        nc4_link_register("TCP", sink_write, NULL);
        s_sink_mode = -1;
        CHECK(nc4_link_send("TCP", (const uint8_t *)"abc", 3) == ESP_FAIL, "錯誤 → ESP_FAIL");
        uint32_t f = 0, b = 0, d = 0;
        nc4_link_stats("TCP", &f, &b, &d);
        CHECK(d == 1 && f == 0 && b == 0, "只計 drop");
    }

    // ── 6. 滿格 ──
    case_begin("滿格：第 NC4_LINK_MAX+1 條 → NO_MEM（唔會越界）");
    {
        nc4_link_reset();
        char name[NC4_LINK_NAME_MAX];
        int ok = 0;
        for (int i = 0; i < NC4_LINK_MAX; i++) {
            snprintf(name, sizeof(name), "L%d", i);
            if (nc4_link_register(name, sink_write, NULL) == ESP_OK) {
                ok++;
            }
        }
        CHECK(ok == NC4_LINK_MAX, "全部登記成功");
        CHECK(nc4_link_count() == NC4_LINK_MAX, "count == MAX");
        CHECK(nc4_link_register("EXTRA", sink_write, NULL) == ESP_ERR_NO_MEM, "再多一條 → NO_MEM");
        CHECK(!nc4_link_has("EXTRA"), "拒絕的唔會留低");
        nc4_link_reset();
    }

    // ── 7. 統計 reset ──
    case_begin("stats reset：清走 frames/bytes/drops");
    {
        sink_reset();
        nc4_link_reset();
        nc4_link_register("CDC", sink_write, NULL);
        nc4_link_send("CDC", (const uint8_t *)"12345", 5);
        s_sink_mode = -1;
        nc4_link_send("CDC", (const uint8_t *)"12345", 5);
        uint32_t f = 0, b = 0, d = 0;
        nc4_link_stats("CDC", &f, &b, &d);
        CHECK(f == 1 && b == 5 && d == 1, "前後計數正確");
        nc4_link_stats_reset();
        nc4_link_stats("CDC", &f, &b, &d);
        CHECK(f == 0 && b == 0 && d == 0, "reset 之後全部 0");
        nc4_link_stats("NOPE", &f, &b, &d);      // 查唔到：唔可以爆
        CHECK(f == 0 && b == 0 && d == 0, "查唔到回 0");
    }

    // ── 8. JSON ──
    case_begin("JSON：內容正確、任何 cap 都唔越界");
    {
        sink_reset();
        nc4_link_reset();
        nc4_link_register("CDC", sink_write, NULL);
        nc4_link_register("UART", sink_write, NULL);
        nc4_link_send("CDC", (const uint8_t *)"1234567890", 10);
        char buf[256];
        const size_t n = nc4_link_json(buf, sizeof(buf));
        CHECK(n > 0 && n < sizeof(buf), "有寫入而且冇滿");
        CHECK(strstr(buf, "\"CDC\"") != NULL, "有 CDC");
        CHECK(strstr(buf, "\"UART\"") != NULL, "有 UART");
        CHECK(strstr(buf, "\"frames\":1") != NULL, "frames:1");
        CHECK(strstr(buf, "\"bytes\":10") != NULL, "bytes:10");
        CHECK(buf[n] == '\0', "有 NUL 結尾");

        for (size_t cap = 1; cap <= 48; cap++) {
            char small[64];
            memset(small, 0x7F, sizeof(small));
            const size_t w = nc4_link_json(small, cap);
            const int nul_ok = (cap > 0) && (small[cap - 1] == '\0' || w == 0);
            if (!nul_ok) {
                printf("        cap=%u 冇 NUL（w=%u）\n", (unsigned)cap, (unsigned)w);
            }
            CHECK(w < cap, "回傳值 < cap");
            CHECK(nul_ok, "cap 之內有 NUL");
        }
        CHECK(nc4_link_json(NULL, 10) == 0, "NULL 緩衝 → 0");
        CHECK(nc4_link_json(buf, 0) == 0, "cap=0 → 0");
    }

    printf("\n%s：%d 個 case，%d 個失敗\n", s_fail ? "FAIL" : "ALL PASS", s_case, s_fail);
    return s_fail ? 1 : 0;
}
