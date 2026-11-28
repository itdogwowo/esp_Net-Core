// nameDict_test — pixel name dictionary：UTF-8、持久化、增量、同 id 覆蓋。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nc_px_name_dict.h"

static int g_fail;
static void check(bool cond, const char *name)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) {
        g_fail++;
    }
}

int main(void)
{
    const char *path = "nameDict_test.nd";
    remove(path);
    remove("nameDict_test.nd.part");

    nc_px_name_dict_t *d = nc_px_name_dict_create();
    check(d != NULL, "create");

    bool added = false;
    uint32_t id_red = nc_px_name_dict_intern(d, "紅色波浪", strlen("紅色波浪"), &added);
    check(added && id_red == 0, "UTF-8 中文新增 id0");
    uint32_t id_green = nc_px_name_dict_intern(d, "綠色", strlen("綠色"), &added);
    check(added && id_green == 1, "第二個 name id1");

    added = true;
    uint32_t again = nc_px_name_dict_intern(d, "紅色波浪", strlen("紅色波浪"), &added);
    check(!added && again == id_red, "重複 name 用返 id，唔加");

    added = false;
    bool updated = false;
    uint32_t same = nc_px_name_dict_intern_id(d, id_red, "紅色波浪", strlen("紅色波浪"),
                                             &added, &updated);
    check(!added && !updated && same == id_red, "同 id 同名：唔改");

    updated = false;
    same = nc_px_name_dict_intern_id(d, id_red, "紅色海洋", strlen("紅色海洋"),
                                     &added, &updated);
    check(!added && updated && same == id_red, "同 id 不同名：直接覆蓋");
    uint16_t len = 0;
    const char *nm = nc_px_name_dict_name(d, id_red, &len);
    check(nm != NULL && len == strlen("紅色海洋") &&
          memcmp(nm, "紅色海洋", len) == 0, "覆蓋後 name 正確");
    check(nc_px_name_dict_count(d) == 2, "覆蓋唔增加 count");

    check(nc_px_name_dict_dirty(d), "改過係 dirty");
    check(nc_px_name_dict_save(d, path) == ESP_OK, "save");
    check(!nc_px_name_dict_dirty(d), "save 後 clean");

    nc_px_name_dict_t *d2 = nc_px_name_dict_create();
    check(nc_px_name_dict_load(d2, path) == ESP_OK, "load");
    check(nc_px_name_dict_count(d2) == 2, "load count 2");
    nm = nc_px_name_dict_name(d2, id_red, &len);
    check(nm != NULL && len == strlen("紅色海洋") &&
          memcmp(nm, "紅色海洋", len) == 0, "load 後覆蓋名保留");
    check(nc_px_name_dict_find(d2, "紅色海洋", strlen("紅色海洋")) == id_red,
          "load 後 hash lookup");
    check(nc_px_name_dict_find(d2, "紅色波浪", strlen("紅色波浪")) ==
          NC_PX_NAME_DICT_BAD_ID, "舊名唔再存在");

    // 字典缺失 = 重建（load NOT_FOUND）
    remove(path);
    check(nc_px_name_dict_load(d2, path) == ESP_ERR_NOT_FOUND, "缺失 → NOT_FOUND");

    // 損壞 = INVALID_STATE
    FILE *f = fopen(path, "wb");
    fwrite("BAD", 1, 3, f);
    fclose(f);
    check(nc_px_name_dict_load(d2, path) == ESP_ERR_INVALID_STATE, "壞檔 → INVALID_STATE");
    remove(path);

    nc_px_name_dict_destroy(d);
    nc_px_name_dict_destroy(d2);
    printf("%s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
