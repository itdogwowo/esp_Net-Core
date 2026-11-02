// nc_cfg_patch 的 host 測試：只驗「換值但保留其他位元組」。
#include <stdio.h>
#include <string.h>

#include "nc_cfg_patch.h"

static int fails;

static void check(const char *name, const char *text, const char *path,
                  const char *value, const char *expect)
{
    char out[1024];
    size_t len = 0;
    const bool ok = nc_cfg_patch_value(text, path, value, out, sizeof(out), &len);

    if (expect == NULL) {
        if (ok) {
            printf("  FAIL %s：應該失敗但成功了（%s）\n", name, out);
            fails++;
        } else {
            printf("  PASS %s\n", name);
        }
        return;
    }
    if (!ok) {
        printf("  FAIL %s：找不到路徑\n", name);
        fails++;
        return;
    }
    if (strcmp(out, expect) != 0 || len != strlen(expect)) {
        printf("  FAIL %s\n    got: %s\n    want: %s\n", name, out, expect);
        fails++;
        return;
    }
    printf("  PASS %s\n", name);
}

int main(void)
{
    printf("[1] 根層值（cID 的實際用法）\n");
    check("空字串 → 填值",
          "{\n  \"hostname\": \"\",\n  \"cID\": \"\",\n  \"x\": 1\n}\n",
          "cID", "A57C",
          "{\n  \"hostname\": \"\",\n  \"cID\": \"A57C\",\n  \"x\": 1\n}\n");
    check("已有值 → 換值，排版不動",
          "{\"cID\":\"1234\",\"a\":2}",
          "cID", "A57C",
          "{\"cID\":\"A57C\",\"a\":2}");

    printf("[2] 巢狀物件\n");
    check("a.b",
          "{\"a\":{\"b\":\"old\",\"c\":3}}",
          "a.b", "new",
          "{\"a\":{\"b\":\"new\",\"c\":3}}");
    check("三層",
          "{\"a\":{\"b\":{\"c\":\"x\"}}}",
          "a.b.c", "y",
          "{\"a\":{\"b\":{\"c\":\"y\"}}}");

    printf("[3] 陣列索引\n");
    check("links.1.rx.decode",
          "{\"links\":[{\"rx\":{\"decode\":\"decode0\"}},{\"rx\":{\"decode\":\"decode0\"}}]}",
          "links.1.rx.decode", "decode1",
          "{\"links\":[{\"rx\":{\"decode\":\"decode0\"}},{\"rx\":{\"decode\":\"decode1\"}}]}");
    check("純量陣列 links.0.slots",
          "{\"links\":[{\"slots\":4},{\"slots\":4}]}",
          "links.0.slots", "8",
          "{\"links\":[{\"slots\":\"8\"},{\"slots\":4}]}");

    printf("[4] 跳過干擾\n");
    check("字串裡有括號與逗號",
          "{\"a\":\"}{,\",\"b\":\"\"}",
          "b", "ok",
          "{\"a\":\"}{,\",\"b\":\"ok\"}");
    check("跳過物件值",
          "{\"a\":{\"x\":1,\"y\":2},\"b\":\"\"}",
          "b", "ok",
          "{\"a\":{\"x\":1,\"y\":2},\"b\":\"ok\"}");
    check("轉義引號",
          "{\"a\":\"x\\\"y\",\"b\":\"\"}",
          "b", "ok",
          "{\"a\":\"x\\\"y\",\"b\":\"ok\"}");

    printf("[5] 找不到\n");
    check("缺鍵", "{\"a\":1}", "b", "x", NULL);
    check("路徑錯", "{\"a\":1}", "a.b", "x", NULL);
    check("索引越界", "{\"a\":[1]}", "a.5", "x", NULL);

    printf("\n%s（%d 個失敗）\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
