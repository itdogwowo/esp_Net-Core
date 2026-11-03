// 極簡 cJSON 替身（只給 host test 用，不是真的 cJSON）
//
// 為什麼要自己寫一個：`nc_task.c` 讀 tasks.json 會用到 cJSON，但 host test
// 沒有 IDF、也不該為了跑一支測試把整個 cJSON 拉進來。這裡只實作 nc_task.c
// 真的會呼叫的六個函式，node 由 cfg_stub.c 直接組出來（不解析文字）。
#pragma once

#include <stddef.h>

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *child;
    char *string;        // 物件成員的鍵（陣列元素是 NULL）
    int type;
    char *valuestring;
    int valueint;
} cJSON;

#define cJSON_Array  32
#define cJSON_Object 64
#define cJSON_String 16
#define cJSON_Number 8

const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *key);
const cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
int cJSON_GetArraySize(const cJSON *array);
int cJSON_IsArray(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);

#define cJSON_ArrayForEach(element, array) \
    for ((element) = (array) ? (array)->child : NULL; (element) != NULL; (element) = (element)->next)
