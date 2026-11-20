#ifndef PS3RECOMP_WIN32_DIRENT_H
#define PS3RECOMP_WIN32_DIRENT_H

#ifndef _WIN32

#include <dirent.h>

#else

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              pending;
    struct dirent    ent;
} DIR;

static inline DIR* opendir(const char* path)
{
    if (!path || !*path) return NULL;

    size_t n = strlen(path);
    char*  pattern = (char*)malloc(n + 3);
    if (!pattern) return NULL;
    memcpy(pattern, path, n);
    if (n && (pattern[n - 1] == '/' || pattern[n - 1] == '\\')) n--;
    pattern[n]     = '\\';
    pattern[n + 1] = '*';
    pattern[n + 2] = '\0';

    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) { free(pattern); return NULL; }

    d->h = FindFirstFileA(pattern, &d->fd);
    free(pattern);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }

    d->pending = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d)
{
    if (!d || d->h == INVALID_HANDLE_VALUE) return NULL;

    if (d->pending) d->pending = 0;
    else if (!FindNextFileA(d->h, &d->fd)) return NULL;

    size_t n = strlen(d->fd.cFileName);
    if (n >= sizeof d->ent.d_name) n = sizeof d->ent.d_name - 1;
    memcpy(d->ent.d_name, d->fd.cFileName, n);
    d->ent.d_name[n] = '\0';
    return &d->ent;
}

static inline int closedir(DIR* d)
{
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

#endif
#endif
