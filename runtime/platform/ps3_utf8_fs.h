#ifndef PS3RECOMP_UTF8_FS_H
#define PS3RECOMP_UTF8_FS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <wchar.h>

typedef struct __stat64 ps3_host_stat_t;

static inline wchar_t* ps3_utf8_to_wide(const char* path, wchar_t* stack_buf, size_t stack_cap)
{
    if (!path) return NULL;
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wn <= 0) return NULL;
    wchar_t* out = ((size_t)wn <= stack_cap) ? stack_buf : (wchar_t*)malloc((size_t)wn * sizeof(wchar_t));
    if (!out) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, out, wn)) {
        if (out != stack_buf) free(out);
        return NULL;
    }
    return out;
}

static inline void ps3_utf8_wide_free(wchar_t* p, wchar_t* stack_buf)
{
    if (p && p != stack_buf) free(p);
}

static inline FILE* ps3_utf8_fopen(const char* path, const char* mode)
{
    if (!path || !mode) return NULL;
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return NULL;
    wchar_t wm[16];
    size_t i = 0;
    for (; mode[i] && i < 15; i++) wm[i] = (wchar_t)(unsigned char)mode[i];
    wm[i] = 0;
    FILE* f = _wfopen(wp, wm);
    ps3_utf8_wide_free(wp, sbuf);
    return f;
}

static inline int ps3_utf8_mkdir(const char* path)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return -1;
    int rc = _wmkdir(wp);
    ps3_utf8_wide_free(wp, sbuf);
    return rc;
}

static inline int ps3_utf8_stat64(const char* path, ps3_host_stat_t* st)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return -1;
    size_t n = wcslen(wp);
    while (n > 1 && (wp[n - 1] == L'/' || wp[n - 1] == L'\\') && wp[n - 2] != L':')
        wp[--n] = 0;
    int rc = _wstat64(wp, st);
    ps3_utf8_wide_free(wp, sbuf);
    return rc;
}

static inline int ps3_utf8_rename(const char* old_path, const char* new_path)
{
    wchar_t sobuf[512], snbuf[512];
    wchar_t* wo = ps3_utf8_to_wide(old_path, sobuf, 512);
    wchar_t* wn = ps3_utf8_to_wide(new_path, snbuf, 512);
    int rc = (wo && wn) ? _wrename(wo, wn) : -1;
    ps3_utf8_wide_free(wo, sobuf);
    ps3_utf8_wide_free(wn, snbuf);
    return rc;
}

static inline int ps3_utf8_remove(const char* path)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return -1;
    int rc = _wremove(wp);
    ps3_utf8_wide_free(wp, sbuf);
    return rc;
}

static inline int ps3_utf8_unlink(const char* path)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return -1;
    int rc = _wunlink(wp);
    ps3_utf8_wide_free(wp, sbuf);
    return rc;
}

static inline int ps3_utf8_rmdir(const char* path)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return -1;
    int rc = _wrmdir(wp);
    ps3_utf8_wide_free(wp, sbuf);
    return rc;
}

static inline int ps3_utf8_disk_free_bytes(const char* path, uint64_t* out_avail)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return 0;
    ULARGE_INTEGER available;
    BOOL ok = GetDiskFreeSpaceExW(wp, &available, NULL, NULL);
    ps3_utf8_wide_free(wp, sbuf);
    if (ok && out_avail) *out_avail = (uint64_t)available.QuadPart;
    return ok ? 1 : 0;
}

static inline DWORD ps3_utf8_file_attributes(const char* path)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return INVALID_FILE_ATTRIBUTES;
    DWORD attr = GetFileAttributesW(wp);
    ps3_utf8_wide_free(wp, sbuf);
    return attr;
}

#else

#include <unistd.h>

typedef struct stat ps3_host_stat_t;

static inline FILE* ps3_utf8_fopen(const char* path, const char* mode)
{
    return fopen(path, mode);
}

static inline int ps3_utf8_mkdir(const char* path)
{
    return mkdir(path, 0755);
}

static inline int ps3_utf8_stat64(const char* path, ps3_host_stat_t* st)
{
    return stat(path, st);
}

static inline int ps3_utf8_rename(const char* old_path, const char* new_path)
{
    return rename(old_path, new_path);
}

static inline int ps3_utf8_remove(const char* path)
{
    return remove(path);
}

static inline int ps3_utf8_unlink(const char* path)
{
    return unlink(path);
}

static inline int ps3_utf8_rmdir(const char* path)
{
    return rmdir(path);
}

#endif

#endif
