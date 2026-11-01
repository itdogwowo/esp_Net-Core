#pragma once
#include <stddef.h>

void plan_host_reset(size_t dma, size_t internal, size_t psram);
void *plan_host_alloc(size_t bytes, size_t align, int cls);
size_t plan_host_used(int cls);
int plan_host_allocs(void);
void plan_host_log(const char *fmt, ...);
