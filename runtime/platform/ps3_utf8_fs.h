#ifndef PS3RECOMP_UTF8_FS_H
#define PS3RECOMP_UTF8_FS_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

__attribute__((format(printf, 3, 4)))
static inline int ps3_path_format(char* out, size_t cap, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < cap;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <wchar.h>

#define PS3_HOST_PATH_MAX 4096

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

static inline wchar_t* ps3_utf8_to_wide_path(const char* path)
{
    wchar_t* w = ps3_utf8_to_wide(path, NULL, 0);
    if (!w || !wcsncmp(w, L"\\\\?\\", 4)) return w;
    const DWORD need = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t* full = need ? (wchar_t*)malloc((size_t)need * sizeof(wchar_t)) : NULL;
    const DWORD len = full ? GetFullPathNameW(w, need, full, NULL) : 0;
    free(w);
    if (!len || len >= need) {
        free(full);
        return NULL;
    }
    if (full[0] == L'\\' && full[1] == L'\\' && (full[2] == L'.' || full[2] == L'?'))
        return full;
    const int unc = full[0] == L'\\' && full[1] == L'\\';
    const wchar_t* prefix = unc ? L"\\\\?\\UNC" : L"\\\\?\\";
    const wchar_t* rest = unc ? full + 1 : full;
    const size_t plen = wcslen(prefix), rlen = wcslen(rest);
    wchar_t* out = (wchar_t*)malloc((plen + rlen + 1) * sizeof(wchar_t));
    if (out) {
        memcpy(out, prefix, plen * sizeof(wchar_t));
        memcpy(out + plen, rest, (rlen + 1) * sizeof(wchar_t));
    }
    free(full);
    return out;
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

static inline int ps3_utf8_open(const char* path, int flags, int mode)
{
    wchar_t sbuf[512];
    wchar_t* wp = ps3_utf8_to_wide(path, sbuf, 512);
    if (!wp) return -1;
    const int fd = _wopen(wp, flags, mode);
    ps3_utf8_wide_free(wp, sbuf);
    return fd;
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

#define PS3_REPLACE_FILE_ATTEMPTS 10
#define PS3_REPLACE_FILE_RETRY_MS 50

static inline int ps3_replace_file_transient(DWORD err)
{
    return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED;
}

static inline int ps3_utf8_replace_file(const char* src_path, const char* dst_path)
{
    wchar_t sbuf[512], dbuf[512];
    wchar_t* ws = ps3_utf8_to_wide(src_path, sbuf, 512);
    wchar_t* wd = ps3_utf8_to_wide(dst_path, dbuf, 512);
    int rc = -1;
    if (ws && wd) {
        for (int attempt = 1;; attempt++) {
            if (MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                rc = 0;
                break;
            }
            if (attempt == PS3_REPLACE_FILE_ATTEMPTS || !ps3_replace_file_transient(GetLastError())) break;
            Sleep(PS3_REPLACE_FILE_RETRY_MS);
        }
    }
    ps3_utf8_wide_free(ws, sbuf);
    ps3_utf8_wide_free(wd, dbuf);
    return rc;
}

static inline int ps3_file_sync(FILE* fp)
{
    if (!fp) return -1;
    if (fflush(fp) != 0) return -1;
    return _commit(_fileno(fp));
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

#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#define PS3_HOST_PATH_MAX PATH_MAX

typedef struct stat ps3_host_stat_t;

static inline FILE* ps3_utf8_fopen(const char* path, const char* mode)
{
    return fopen(path, mode);
}

static inline int ps3_utf8_open(const char* path, int flags, int mode)
{
    return open(path, flags, mode);
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

static inline void ps3_sync_parent_dir(const char* path)
{
    const char* slash = strrchr(path, '/');
    const size_t n = !slash ? 1 : slash == path ? 1 : (size_t)(slash - path);
    char* dir = (char*)malloc(n + 1);
    if (!dir) return;
    if (slash) memcpy(dir, path, n);
    else dir[0] = '.';
    dir[n] = '\0';
    const int fd = open(dir, O_RDONLY | O_DIRECTORY);
    free(dir);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
}

static inline int ps3_utf8_replace_file(const char* src_path, const char* dst_path)
{
    if (rename(src_path, dst_path) != 0) return -1;
    ps3_sync_parent_dir(dst_path);
    return 0;
}

static inline int ps3_file_sync(FILE* fp)
{
    if (!fp) return -1;
    if (fflush(fp) != 0) return -1;
    return fsync(fileno(fp));
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
