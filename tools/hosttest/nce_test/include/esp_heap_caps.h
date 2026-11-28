// host 測試 shim：heap_caps → malloc（唔需要 IDF）
#pragma once
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_8BIT     1
#define MALLOC_CAP_INTERNAL 2

static inline void *heap_caps_malloc(size_t n, int caps)
{
    (void)caps;
    return malloc(n);
}

static inline void heap_caps_free(void *p)
{
    free(p);
}
