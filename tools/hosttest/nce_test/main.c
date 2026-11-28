// nce_test — `.nce` 值流信箱（雙緩衝／序號／截短／0–255→12-bit）
//
// 驗的是契約（見 nc_px_nce.h）：
//   · 未 publish → latest() 回 false；
//   · publish 之後 latest() 拿到完整一份，seq 遞增；
//   · publish 大過信箱 → 截短，唔會寫爆；
//   · 雙緩衝：兩次 publish 之後，舊 pointer 仍然係完整內容（唔會撕裂）；
//   · to_vals：0–255 → 0–4080（<<4），之後配對層 >>4 復原。
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nc_px_nce.h"

static int s_fail;
static int s_case;

static void check_impl(int cond, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  %s  ", cond ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    if (!cond) {
        printf("   (%s:%d)", file, line);
        s_fail++;
    }
    printf("\n");
    va_end(ap);
}

#define CHECK(cond, ...) check_impl((cond), __FILE__, __LINE__, __VA_ARGS__)

static void case_begin(const char *name)
{
    s_case++;
    printf("[%d] %s\n", s_case, name);
}

int main(void)
{
    const uint32_t px = 16;
    const uint32_t bytes = px * NC_PX_NCE_BPP;      // 每值 1 B；16 值 = 16 B

    case_begin("未初始化／未 publish");
    nc_px_nce_deinit();
    nc_px_nce_frame_t f;
    CHECK(!nc_px_nce_latest(&f), "未 init → latest() 回 false");
    CHECK(!nc_px_nce_publish((const uint8_t *)"x", 1), "未 init → publish 回 false");

    case_begin("初始化（冪等）＋第一次 publish");
    CHECK(nc_px_nce_init(bytes), "init %u B 成功", (unsigned)bytes);
    CHECK(nc_px_nce_bytes() == bytes, "bytes=%u", (unsigned)nc_px_nce_bytes());
    CHECK(nc_px_nce_init(bytes), "同一個大小再 init 都成功（唔會重複要記憶體）");
    CHECK(!nc_px_nce_latest(&f), "未 publish 過 → false");

    uint8_t a[32];
    for (uint32_t i = 0; i < sizeof(a); i++) {
        a[i] = (uint8_t)(i * 3u + 1u);
    }
    CHECK(nc_px_nce_publish(a, bytes), "publish 成功");
    CHECK(nc_px_nce_latest(&f), "latest() 有值");
    CHECK(f.bytes == bytes && f.seq == 1, "bytes=%u seq=%u", (unsigned)f.bytes, (unsigned)f.seq);
    CHECK(memcmp(f.data, a, bytes) == 0, "內容一致");

    case_begin("第二次 publish → 換手，舊 pointer 仍然完整");
    const uint8_t *old = f.data;
    uint8_t b[32];
    memset(b, 0xAA, sizeof(b));
    CHECK(nc_px_nce_publish(b, bytes), "第二次 publish 成功");
    CHECK(nc_px_nce_latest(&f) && f.seq == 2, "seq=2");
    CHECK(f.data != old, "換咗另一塊 buffer");
    CHECK(memcmp(f.data, b, bytes) == 0, "新內容正確");
    CHECK(memcmp(old, a, bytes) == 0, "舊 pointer 冇被覆蓋（雙緩衝有效）");

    case_begin("publish 大過信箱 → 截短，唔會寫爆");
    uint8_t big[64];
    memset(big, 0x5A, sizeof(big));
    CHECK(nc_px_nce_publish(big, sizeof(big)), "過大仍然 publish（截短）");
    CHECK(nc_px_nce_latest(&f) && f.bytes == bytes, "latest 仍然係信箱大小");

    case_begin("0–255 → 12-bit（<<4），之後 >>4 復原");
    uint8_t src[8] = { 0, 1, 15, 16, 127, 128, 254, 255 };
    uint16_t dst[8] = { 0 };
    CHECK(nc_px_nce_to_vals(src, 8, dst, 8) == 8, "轉換 8 個");
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        ok = ok && (dst[i] == (uint16_t)(src[i] << 4)) && ((uint8_t)(dst[i] >> 4) == src[i]);
    }
    CHECK(ok, "逐一對得上（0/255 邊界）");
    CHECK(nc_px_nce_to_vals(src, 8, dst, 4) == 4, "dst 位數不足 → 只轉 4 個");

    case_begin("deinit 之後回到未初始化");
    nc_px_nce_deinit();
    CHECK(!nc_px_nce_latest(&f), "deinit → latest() false");
    CHECK(nc_px_nce_bytes() == 0, "bytes=0");

    printf("\n%s（%d 個失敗）\n", s_fail ? "FAIL" : "ALL PASS", s_fail);
    return s_fail ? 1 : 0;
}
