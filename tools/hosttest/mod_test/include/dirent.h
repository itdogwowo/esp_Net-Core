// MSVC 沒有 <dirent.h>；用 Win32 的 FindFirstFile 做一個最小 shim。
// 只給 host 測試用（nc_mod_scan 需要 opendir / readdir / closedir）。
//
// 非 Windows（Linux／macOS）：這個檔經 -I 排在系統 include 前面，所以唔可以留空
// —— 空的話 <dirent.h> 就等於乜都冇，#nc_mod.c 會見到 undeclared DIR。
// 用 #include_next 交返畀真正的系統 <dirent.h>。
#pragma once

#ifdef _WIN32

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct dirent {
    char d_name[260];
};

typedef struct {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              first;
    struct dirent   *ent;
} DIR;

static DIR *opendir(const char *path)
{
    char pattern[512];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (d == NULL) {
        return NULL;
    }
    d->h = FindFirstFileA(pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    d->ent = (struct dirent *)calloc(1, sizeof(struct dirent));
    if (d->ent == NULL) {
        FindClose(d->h);
        free(d);
        return NULL;
    }
    return d;
}

static struct dirent *readdir(DIR *d)
{
    if (d == NULL) {
        return NULL;
    }
    if (!d->first) {
        if (!FindNextFileA(d->h, &d->fd)) {
            return NULL;
        }
    }
    d->first = 0;
    strncpy_s(d->ent->d_name, sizeof(d->ent->d_name), d->fd.cFileName, _TRUNCATE);
    return d->ent;
}

static int closedir(DIR *d)
{
    if (d == NULL) {
        return -1;
    }
    FindClose(d->h);
    free(d->ent);
    free(d);
    return 0;
}

#else  // !_WIN32

#include_next <dirent.h>

#endif
