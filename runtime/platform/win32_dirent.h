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
    char d_name[MAX_PATH * 3];
};

typedef struct DIR {
    HANDLE           h;
    WIN32_FIND_DATAW fd;
    int              pending;
    struct dirent    ent;
} DIR;

static inline DIR* opendir(const char* path)
{
    if (!path || !*path) return NULL;

    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wn <= 0) return NULL;
    wchar_t* pattern = (wchar_t*)malloc((size_t)(wn + 3) * sizeof(wchar_t));
    if (!pattern) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, pattern, wn);
    size_t n = (size_t)(wn - 1);
    if (n && (pattern[n - 1] == L'/' || pattern[n - 1] == L'\\')) n--;
    pattern[n]     = L'\\';
    pattern[n + 1] = L'*';
    pattern[n + 2] = L'\0';

    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) { free(pattern); return NULL; }

    d->h = FindFirstFileW(pattern, &d->fd);
    free(pattern);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }

    d->pending = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d)
{
    if (!d || d->h == INVALID_HANDLE_VALUE) return NULL;

    if (d->pending) d->pending = 0;
    else if (!FindNextFileW(d->h, &d->fd)) return NULL;

    int got = WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->ent.d_name, (int)sizeof d->ent.d_name, NULL, NULL);
    if (got <= 0) d->ent.d_name[0] = '\0';
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
