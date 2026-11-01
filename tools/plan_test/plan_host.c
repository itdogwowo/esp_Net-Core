#include "plan_host.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t cap[3];
static int allocs;
static size_t used[3];
static uint8_t *base;

void plan_host_reset(size_t dma, size_t internal, size_t psram)
{
    cap[0] = dma; cap[1] = internal; cap[2] = psram;
    memset(used, 0, sizeof(used));
    allocs = 0;
    free(base);
    size_t total = dma + internal + psram + 4096;
    base = malloc(total);
}

void *plan_host_alloc(size_t bytes, size_t align, int cls)
{
    allocs++;
    if (cls < 0 || cls > 2 || align == 0) return NULL;
    size_t off = (used[cls] + align - 1) & ~(align - 1);
    if (off + bytes > cap[cls]) return NULL;   // 模擬「配置不到」
    used[cls] = off + bytes;
    return base + cls * 1024 * 1024 + off;
}

int plan_host_allocs(void) { return allocs; }

size_t plan_host_used(int cls) { return cls >= 0 && cls <= 2 ? used[cls] : 0; }

void plan_host_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
