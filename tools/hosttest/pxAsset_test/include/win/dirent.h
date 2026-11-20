// 測試用 dirent.h shim（Windows／MSVC）：用 _findfirst 模擬 POSIX opendir／readdir。
//
// 只為 host test 而設（firmware 用 FatFS 的真 dirent）。行為對齊 POSIX：`.`／`..`
// 會出現，呼叫端自己跳；回 NULL = 到底。
#pragma once

#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dirent {
    char d_name[260];
};

typedef struct {
    intptr_t            h;
    struct _finddata_t  fd;
    int                 done;
    struct dirent       ent;
} DIR;

static inline DIR *opendir(const char *path)
{
    char pat[300];
    snprintf(pat, sizeof(pat), "%s/*", path);
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (d == NULL) {
        return NULL;
    }
    d->h = _findfirst(pat, &d->fd);
    if (d->h == -1) {
        free(d);
        return NULL;
    }
    d->done = 0;
    return d;
}

static inline struct dirent *readdir(DIR *d)
{
    if (d == NULL || d->done) {
        return NULL;
    }
    memcpy(d->ent.d_name, d->fd.name, sizeof(d->ent.d_name));
    if (_findnext(d->h, &d->fd) != 0) {
        d->done = 1;
    }
    return &d->ent;
}

static inline int closedir(DIR *d)
{
    if (d == NULL) {
        return -1;
    }
    if (d->h != -1) {
        _findclose(d->h);
    }
    free(d);
    return 0;
}
