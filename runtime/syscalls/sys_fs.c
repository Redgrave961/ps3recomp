#include "sys_fs.h"
#include "../memory/vm.h"
#include "../platform/ps3_dev_flash.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define stat _stat64
#else
  #include <unistd.h>
  #include <time.h>
#endif

sys_fs_fd_info  g_sys_fs_fds[SYS_FS_FD_MAX];
sys_fs_dir_info g_sys_fs_dirs[SYS_FS_DIR_MAX];

#ifdef _WIN32
static SRWLOCK s_fs_slot_lock = SRWLOCK_INIT;
static void fs_slot_lock(void)   { AcquireSRWLockExclusive(&s_fs_slot_lock); }
static void fs_slot_unlock(void) { ReleaseSRWLockExclusive(&s_fs_slot_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_fs_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static void fs_slot_lock(void)   { pthread_mutex_lock(&s_fs_slot_lock); }
static void fs_slot_unlock(void) { pthread_mutex_unlock(&s_fs_slot_lock); }
#endif

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

const char* ppu_vfs_root = ".";

static void fs_normalize_sep(char* p) {
#ifdef _WIN32
    for (char* c = p; *c; c++) if (*c == '/') *c = '\\';
#else
    (void)p;
#endif
}

static void fs_forward_slashes(char* p)
{
    for (; *p; p++) if (*p == '\\') *p = '/';
}

extern __attribute__((weak)) const char* cellGame_get_hdd_title_id(void);

static void ps3_vfs_ps3game_fallback(char* path, size_t cap)
{
    ps3_vstat st;
    if (!path || !*path || ps3_vfs_stat(path, &st) == 0)
        return;
    {
        const char* id = cellGame_get_hdd_title_id ? cellGame_get_hdd_title_id() : NULL;
        char key[48];
        char* g;
        if (id && snprintf(key, sizeof key, "/game/%s/", id) < (int)sizeof key
            && ((g = strstr(path, key)) != NULL
                || (snprintf(key, sizeof key, "\\game\\%s\\", id), g = strstr(path, key)) != NULL)) {
            char alt[1024];
            const size_t head = (size_t)(g - path);
            if (head + 10 + strlen(g + strlen(key)) < sizeof alt) {
                memcpy(alt, path, head);
                snprintf(alt + head, sizeof alt - head, "/PS3_GAME/%s", g + strlen(key));
                fs_normalize_sep(alt);
                if (ps3_vfs_stat(alt, &st) == 0) { snprintf(path, cap, "%s", alt); return; }
            }
        }
    }
    char* p = strstr(path, "/PS3_GAME/");
    if (!p) {
        char* u = strstr(path, "/USRDIR/");
        if (!u) u = strstr(path, "\\USRDIR\\");
        if (!u) return;
        char alt[1024];
        size_t head = (size_t)(u - path);
        if (head + 10 + strlen(u) >= sizeof alt) return;
        memcpy(alt, path, head);
        snprintf(alt + head, sizeof alt - head, "/PS3_GAME%s", u);
        fs_normalize_sep(alt);
        if (ps3_vfs_stat(alt, &st) == 0) snprintf(path, cap, "%s", alt);
        return;
    }
    char alt[1024];
    size_t head = (size_t)(p - path);
    if (head + 1 >= sizeof alt)
        return;
    memcpy(alt, path, head);
    snprintf(alt + head, sizeof alt - head, "/%s", p + 10);
    if (ps3_vfs_stat(alt, &st) != 0)
        return;
    snprintf(path, cap, "%s", alt);
}

void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size)
{
    const size_t cap = (size_t)host_path_size;

    static const char* hdd0_root = NULL; static int hdd0_init = 0;
    if (!hdd0_init) { hdd0_root = getenv("PS3_HDD0_ROOT"); hdd0_init = 1; }
    if (hdd0_root && strncmp(ps3_path, "/dev_hdd0/", 10) == 0) {
        snprintf(host_path, cap, "%s/%s", hdd0_root, ps3_path + 10);
        fs_forward_slashes(host_path);
        return;
    }

    static const char* hdd1_root = NULL; static int hdd1_init = 0;
    if (!hdd1_init) { hdd1_root = getenv("PS3_HDD1_ROOT"); hdd1_init = 1; }
    if (hdd1_root && *hdd1_root && strncmp(ps3_path, "/dev_hdd1/", 10) == 0) {
        snprintf(host_path, cap, "%s/%s", hdd1_root, ps3_path + 10);
        fs_forward_slashes(host_path);
        return;
    }

    if (strncmp(ps3_path, "/dev_flash/", 11) == 0) {
        const char* fw = getenv("PS3_DEV_FLASH");
        if (!fw || !*fw) fw = "D:/recomp/tools/rpcs3/dev_flash";
        ps3_dev_flash_path(host_path, cap, fw, ps3_path + 11);
        fs_forward_slashes(host_path);
        return;
    }

    static const char* const mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    const char* rel = ps3_path;
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(ps3_path, mounts[i], n) == 0) { rel = ps3_path + n; break; }
        if (n > 1 && strncmp(ps3_path, mounts[i], n - 1) == 0 && ps3_path[n - 1] == '\0') {
            rel = ps3_path + n - 1;
            break;
        }
    }
    if (rel == ps3_path && ps3_path[0] == '/') rel = ps3_path + 1;
    snprintf(host_path, cap, "%s/%s", ppu_vfs_root, rel);
    fs_forward_slashes(host_path);
    ps3_vfs_ps3game_fallback(host_path, cap);
}

int64_t sys_fs_open(ppu_context* ctx)
{
    uint32_t path_addr  = LV2_ARG_PTR(ctx, 0);
    int32_t  flags      = LV2_ARG_S32(ctx, 1);
    uint32_t fd_out     = LV2_ARG_PTR(ctx, 2);

    if (path_addr == 0 || fd_out == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    const char* mode;
    int access = flags & CELL_FS_O_ACCMODE;

    if (flags & CELL_FS_O_CREAT) {
        if (flags & CELL_FS_O_TRUNC) {
            if (access == CELL_FS_O_RDWR)
                mode = "w+b";
            else
                mode = "wb";
        } else if (flags & CELL_FS_O_APPEND) {
            if (access == CELL_FS_O_RDWR)
                mode = "a+b";
            else
                mode = "ab";
        } else {
            if (access == CELL_FS_O_RDWR)
                mode = "r+b";
            else if (access == CELL_FS_O_WRONLY)
                mode = "r+b";
            else
                mode = "rb";
        }
    } else if (flags & CELL_FS_O_TRUNC) {
        if (access == CELL_FS_O_RDWR)
            mode = "w+b";
        else
            mode = "wb";
    } else if (flags & CELL_FS_O_APPEND) {
        if (access == CELL_FS_O_RDWR)
            mode = "a+b";
        else
            mode = "ab";
    } else {
        if (access == CELL_FS_O_RDWR)
            mode = "r+b";
        else if (access == CELL_FS_O_WRONLY)
            mode = "r+b";
        else
            mode = "rb";
    }

    if ((flags & CELL_FS_O_CREAT) && (flags & CELL_FS_O_EXCL)) {
        struct stat _sx;
        if (stat(host_path, &_sx) == 0)
            return (int64_t)(int32_t)CELL_EEXIST;
    }

    ps3_vfile* fp = ps3_vfile_open(host_path, mode);
    const int open_errno = errno;

    if (!fp && open_errno != ENOENT &&
        !(flags & (CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC))) {
        for (int _r = 0; !fp && _r < 3000; _r++) {
#ifdef _WIN32
            Sleep(1);
#else
            { struct timespec _d = { 0, 1000 * 1000 }; nanosleep(&_d, NULL); }
#endif
            fp = ps3_vfile_open(host_path, mode);
        }
        if (fp) fprintf(stderr, "[sys_fs] open recovered after retry: %s\n", host_path);
    }

    if (!fp && (flags & CELL_FS_O_CREAT) && open_errno == ENOENT) {
        fp = ps3_vfile_open(host_path, "w+b");
    }

    if (!fp) {
        ps3_vstat _sd;
        if (ps3_vfs_stat(host_path, &_sd) == 0 && _sd.is_dir) {
            fprintf(stderr, "[sys_fs] open -> EISDIR (directory): %s\n", host_path);
            return (int64_t)(int32_t)CELL_EISDIR;
        }
    }

    if (!fp) {
        { ps3_vstat _s2; int _ex = (ps3_vfs_stat(host_path, &_s2) == 0);
          fprintf(stderr, "[sys_fs] open FAILED: %s (errno=%d %s, exists=%d, size=%lld)\n",
                  host_path, errno, strerror(errno), _ex, _ex ? (long long)_s2.size : -1LL); }
        return (int64_t)(int32_t)CELL_ENOENT;
    }

    fs_slot_lock();
    int slot = -1;
    for (int i = 0; i < SYS_FS_FD_MAX; i++) {
        if (!g_sys_fs_fds[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        fs_slot_unlock();
        ps3_vfile_close(fp);
        return (int64_t)(int32_t)CELL_EMFILE;
    }
    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    f->fp     = fp;
    f->flags  = flags;
    strncpy(f->path, host_path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';
    f->active = 1;
    fs_slot_unlock();

    fprintf(stderr, "[sys_fs] open OK: %s\n", host_path);

    int32_t fd = slot + 3;
    write_be32(fd_out, (uint32_t)fd);

    return CELL_OK;
}

int64_t sys_fs_read(ppu_context* ctx)
{
    int32_t  fd         = LV2_ARG_S32(ctx, 0);
    uint32_t buf_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t size       = LV2_ARG_U64(ctx, 2);
    uint32_t nread_addr = LV2_ARG_PTR(ctx, 3);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

    size_t nread = ps3_vfile_read(vm_to_host(buf_addr), (size_t)size, f->fp);

    if (nread_addr != 0) {
        write_be64(nread_addr, (uint64_t)nread);
    }

    return CELL_OK;
}

int64_t sys_fs_write(ppu_context* ctx)
{
    int32_t  fd           = LV2_ARG_S32(ctx, 0);
    uint32_t buf_addr     = LV2_ARG_PTR(ctx, 1);
    uint64_t size         = LV2_ARG_U64(ctx, 2);
    uint32_t nwritten_addr = LV2_ARG_PTR(ctx, 3);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX) {
        if (nwritten_addr != 0)
            write_be64(nwritten_addr, size);
        return CELL_OK;
    }

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp) {
        if (nwritten_addr != 0)
            write_be64(nwritten_addr, size);
        return CELL_OK;
    }

    const void* buf = vm_to_host(buf_addr);
    size_t nwritten = ps3_vfile_write(buf, (size_t)size, f->fp);
    if (ps3_vfile_host(f->fp)) fflush(ps3_vfile_host(f->fp));

    if (nwritten_addr != 0) {
        write_be64(nwritten_addr, (uint64_t)nwritten);
    }

    return CELL_OK;
}

int64_t sys_fs_close(ppu_context* ctx)
{
    int32_t fd = LV2_ARG_S32(ctx, 0);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active)
        return (int64_t)(int32_t)CELL_EBADF;

    if (f->fp)
        ps3_vfile_close(f->fp);
    fs_slot_lock();
    f->fp = NULL;
    f->active = 0;
    fs_slot_unlock();

    return CELL_OK;
}

int64_t sys_fs_lseek(ppu_context* ctx)
{
    int32_t  fd        = LV2_ARG_S32(ctx, 0);
    int64_t  offset    = LV2_ARG_S64(ctx, 1);
    int32_t  whence    = LV2_ARG_S32(ctx, 2);
    uint32_t pos_addr  = LV2_ARG_PTR(ctx, 3);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX) {
        if (pos_addr)
            write_be64(pos_addr, 0);
        return 0;
    }

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp) {
        if (pos_addr)
            write_be64(pos_addr, 0);
        return 0;
    }

    int origin;
    switch (whence) {
        case CELL_FS_SEEK_SET: origin = SEEK_SET; break;
        case CELL_FS_SEEK_CUR: origin = SEEK_CUR; break;
        case CELL_FS_SEEK_END: origin = SEEK_END; break;
        default: return (int64_t)(int32_t)CELL_EINVAL;
    }

    const int64_t before = ps3_vfile_tell(f->fp);
    if (ps3_vfile_seek(f->fp, offset, origin) != 0) {
        if (before >= 0) ps3_vfile_seek(f->fp, before, SEEK_SET);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    int64_t pos = ps3_vfile_tell(f->fp);

    if (pos_addr != 0) {
        write_be64(pos_addr, (uint64_t)pos);
    }

    return CELL_OK;
}

static void fill_cell_stat(uint32_t stat_addr, const ps3_vstat* st)
{
    uint32_t mode = CELL_FS_S_IRUSR | CELL_FS_S_IRGRP | CELL_FS_S_IROTH | CELL_FS_S_IWUSR;
    mode |= st->is_dir ? CELL_FS_S_IFDIR | CELL_FS_S_IXUSR : CELL_FS_S_IFREG;

    write_be32(stat_addr + 0x00, mode);
    write_be32(stat_addr + 0x04, 0);
    write_be32(stat_addr + 0x08, 0);
    write_be64(stat_addr + 0x0C, (uint64_t)st->atime);
    write_be64(stat_addr + 0x14, (uint64_t)st->mtime);
    write_be64(stat_addr + 0x1C, (uint64_t)st->ctime);
    write_be64(stat_addr + 0x24, st->size);
    write_be64(stat_addr + 0x2C, 4096ULL);
}

int64_t sys_fs_stat(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t stat_addr = LV2_ARG_PTR(ctx, 1);

    if (path_addr == 0 || stat_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    ps3_vstat st;
    if (ps3_vfs_stat(host_path, &st) != 0)
        return (int64_t)(int32_t)CELL_ENOENT;

    fill_cell_stat(stat_addr, &st);
    return CELL_OK;
}

int64_t sys_fs_fstat(ppu_context* ctx)
{
    int32_t  fd        = LV2_ARG_S32(ctx, 0);
    uint32_t stat_addr = LV2_ARG_PTR(ctx, 1);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

    ps3_vstat st;
    ps3_vfile_fstat(f->fp, &st);
    fill_cell_stat(stat_addr, &st);
    return CELL_OK;
}

int64_t sys_fs_opendir(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t fd_out    = LV2_ARG_PTR(ctx, 1);

    if (path_addr == 0 || fd_out == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    ps3_vdir* dir = ps3_vdir_open(host_path);
    if (!dir)
        return (int64_t)(int32_t)CELL_ENOENT;

    fs_slot_lock();
    int slot = -1;
    for (int i = 0; i < SYS_FS_DIR_MAX; i++) {
        if (!g_sys_fs_dirs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        fs_slot_unlock();
        ps3_vdir_close(dir);
        return (int64_t)(int32_t)CELL_EMFILE;
    }

    sys_fs_dir_info* d = &g_sys_fs_dirs[slot];
    memset(d, 0, sizeof(*d));
    d->dir = dir;
    d->active = 1;
    fs_slot_unlock();

    int32_t dir_fd = slot + 1;
    write_be32(fd_out, (uint32_t)dir_fd);

    return CELL_OK;
}

int64_t sys_fs_readdir(ppu_context* ctx)
{
    int32_t  dir_fd     = LV2_ARG_S32(ctx, 0);
    uint32_t dirent_addr = LV2_ARG_PTR(ctx, 1);
    uint32_t nread_addr  = LV2_ARG_PTR(ctx, 2);

    if (dir_fd <= 0 || dir_fd > SYS_FS_DIR_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_dir_info* d = &g_sys_fs_dirs[dir_fd - 1];
    if (!d->active)
        return (int64_t)(int32_t)CELL_EBADF;

    int is_dir = 0;
    const char* name = ps3_vdir_next(d->dir, &is_dir);
    if (!name) {
        if (nread_addr != 0) write_be64(nread_addr, 0);
        return CELL_OK;
    }

    if (dirent_addr != 0 && name != NULL) {
        uint8_t* out = (uint8_t*)vm_to_host(dirent_addr);
        uint8_t namlen = (uint8_t)strlen(name);
        if (namlen > 255) namlen = 255;

        out[0] = is_dir ? 1 : 2;
        out[1] = namlen;
        memcpy(out + 2, name, namlen);
        out[2 + namlen] = '\0';
    }

    if (nread_addr != 0) {
        write_be64(nread_addr, (uint64_t)(name ? 0x102 : 0));
    }

    return CELL_OK;
}

int64_t sys_fs_closedir(ppu_context* ctx)
{
    int32_t dir_fd = LV2_ARG_S32(ctx, 0);

    if (dir_fd <= 0 || dir_fd > SYS_FS_DIR_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_dir_info* d = &g_sys_fs_dirs[dir_fd - 1];
    if (!d->active)
        return (int64_t)(int32_t)CELL_EBADF;

    ps3_vdir_close(d->dir);

    fs_slot_lock();
    d->active = 0;
    fs_slot_unlock();
    return CELL_OK;
}

int64_t sys_fs_mkdir(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);

    if (path_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

#ifdef _WIN32
    int rc = _mkdir(host_path);
#else
    int rc = mkdir(host_path, 0755);
#endif

    if (rc != 0) {
        struct stat st;
#ifdef _WIN32
        if (_stat64(host_path, (struct _stat64*)&st) == 0)
#else
        if (stat(host_path, &st) == 0)
#endif
            return (int64_t)(int32_t)CELL_EEXIST;
        return (int64_t)(int32_t)CELL_ENOENT;
    }

    return CELL_OK;
}

int64_t sys_fs_rename(ppu_context* ctx)
{
    uint32_t old_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t new_addr = LV2_ARG_PTR(ctx, 1);

    if (old_addr == 0 || new_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* old_ps3 = (const char*)vm_to_host(old_addr);
    const char* new_ps3 = (const char*)vm_to_host(new_addr);

    char old_host[1024], new_host[1024];
    sys_fs_translate_path(old_ps3, old_host, sizeof(old_host));
    sys_fs_translate_path(new_ps3, new_host, sizeof(new_host));

    if (rename(old_host, new_host) != 0)
        return (int64_t)(int32_t)CELL_ENOENT;

    return CELL_OK;
}

int64_t sys_fs_unlink(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);

    if (path_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

#ifdef _WIN32
    if (_unlink(host_path) != 0)
#else
    if (unlink(host_path) != 0)
#endif
    {
        const int e = errno;
        struct stat st;
        if (stat(host_path, &st) != 0)
            return (int64_t)(int32_t)CELL_ENOENT;
        if (S_ISDIR(st.st_mode))
            return (int64_t)(int32_t)CELL_EISDIR;
        fprintf(stderr, "[sys_fs] unlink '%s' failed (host errno %d) -> EIO\n", host_path, e);
        return (int64_t)(int32_t)CELL_EIO;
    }

    return CELL_OK;
}

int64_t sys_fs_rmdir(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);

    if (path_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

#ifdef _WIN32
    if (_rmdir(host_path) != 0)
#else
    if (rmdir(host_path) != 0)
#endif
    {
        const int e = errno;
        struct stat st;
        if (stat(host_path, &st) != 0)
            return (int64_t)(int32_t)CELL_ENOENT;
        if (!S_ISDIR(st.st_mode))
            return (int64_t)(int32_t)CELL_ENOTDIR;
        if (e == ENOTEMPTY || e == EEXIST)
            return (int64_t)(int32_t)CELL_ENOTEMPTY;
        return (int64_t)(int32_t)CELL_EIO;
    }

    return CELL_OK;
}

int64_t sys_fs_ftruncate(ppu_context* ctx)
{
    int32_t  fd   = LV2_ARG_S32(ctx, 0);
    uint64_t size = LV2_ARG_U64(ctx, 1);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

    FILE* host = ps3_vfile_host(f->fp);
    if (!host)
        return (int64_t)(int32_t)CELL_EINVAL;
    fflush(host);

#ifdef _WIN32
    int fno = _fileno(host);
    if (_chsize_s(fno, (long long)size) != 0)
        return (int64_t)(int32_t)CELL_EINVAL;
#else
    int fno = fileno(host);
    if (ftruncate(fno, (off_t)size) != 0)
        return (int64_t)(int32_t)CELL_EINVAL;
#endif

    return CELL_OK;
}

void sys_fs_init(lv2_syscall_table* tbl)
{
    memset(g_sys_fs_fds,  0, sizeof(g_sys_fs_fds));
    memset(g_sys_fs_dirs, 0, sizeof(g_sys_fs_dirs));

    lv2_syscall_register(tbl, SYS_FS_OPEN,      sys_fs_open);
    lv2_syscall_register(tbl, SYS_FS_READ,       sys_fs_read);
    lv2_syscall_register(tbl, SYS_FS_WRITE,      sys_fs_write);
    lv2_syscall_register(tbl, SYS_FS_CLOSE,      sys_fs_close);
    lv2_syscall_register(tbl, SYS_FS_OPENDIR,    sys_fs_opendir);
    lv2_syscall_register(tbl, SYS_FS_READDIR,    sys_fs_readdir);
    lv2_syscall_register(tbl, SYS_FS_CLOSEDIR,   sys_fs_closedir);
    lv2_syscall_register(tbl, SYS_FS_STAT,       sys_fs_stat);
    lv2_syscall_register(tbl, SYS_FS_FSTAT,      sys_fs_fstat);
    lv2_syscall_register(tbl, SYS_FS_MKDIR,      sys_fs_mkdir);
    lv2_syscall_register(tbl, SYS_FS_RENAME,     sys_fs_rename);
    lv2_syscall_register(tbl, SYS_FS_RMDIR,      sys_fs_rmdir);
    lv2_syscall_register(tbl, SYS_FS_UNLINK,     sys_fs_unlink);
    lv2_syscall_register(tbl, SYS_FS_LSEEK,      sys_fs_lseek);
    lv2_syscall_register(tbl, SYS_FS_FTRUNCATE,  sys_fs_ftruncate);
}
