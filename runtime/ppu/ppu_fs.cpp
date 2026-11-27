#define PPU_NO_INLINE_STORES
#include "ppu_recomp.h"
#include "../../libs/filesystem/npdrm.h"
#include "../../libs/filesystem/ps3_vfs.h"
#include "../platform/ps3_utf8_fs.h"
#include "ps3emu/nid.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <mutex>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_vm_size;
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" void     sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);
extern "C" unsigned long long ps3_ms_now(void);
#include "ps3emu/log_first.h"

#define CELL_OK              0
#define CELL_FS_ENOENT       (-2147418106)
#define CELL_FS_EISDIR       (-2147418094)
#define CELL_FS_EIO          (-2147418069)
#define CELL_FS_EFSSPECIFIC  (-2147418056)
#define CELL_FS_EBADF        (-2147418070)
#define CELL_FS_EMFILE       (-2147418068)
#define CELL_FS_EINVAL       (-2147418110)
#define CELL_FS_ENOSYS       (-2147418109)
#define CELL_FS_EFAULT       ((int32_t)0x8001000Du)
#define CELL_FS_EEXIST       ((int32_t)0x80010014u)
#define CELL_FS_EROFS        ((int32_t)0x80010026u)
#define CELL_FS_ENOTDIR      ((int32_t)0x8001002Eu)
#define CELL_FS_EXDEV        ((int32_t)0x80010030u)
#define CELL_FS_ENOTMOUNTED  ((int32_t)0x8001003Au)
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
#define CELL_FS_O_CREAT      0x0040
#define CELL_FS_O_TRUNC      0x0200
#define CELL_FS_O_APPEND     0x0400
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

static void guest_strcpy(char* dst, uint32_t gaddr, size_t cap)
{
    size_t i = 0;
    for (; i < cap - 1; i++) {
        if (ppu_vm_size && gaddr + i >= ppu_vm_size) break;
        char c = (char)vm_base[gaddr + i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = 0;
}

static void host_path(char* out, size_t cap, const char* guest)
{
    sys_fs_translate_path(guest, out, (int)cap);
}

#define FS_MAX 256
static ps3_vfile* g_files[FS_MAX];
static npdrm_file* g_drm[FS_MAX];
static uint64_t g_drm_pos[FS_MAX];
static std::mutex g_drm_lock;
static ps3_vdir* g_dirs[FS_MAX];
static uint8_t g_fd_dir[FS_MAX];
static void fs_set_dir(int fd, ps3_vfile* f, uint8_t dir)
{
    if (g_fd_dir[fd] && g_fd_dir[fd] != dir) ps3_vfile_seek(f, 0, SEEK_CUR);
    g_fd_dir[fd] = dir;
}

static std::mutex g_fd_lock;
static int fd_alloc_file(ps3_vfile* f)
{
    std::lock_guard<std::mutex> hold(g_fd_lock);
    for (int i = 3; i < FS_MAX; i++)
        if (!g_files[i] && !g_dirs[i]) {
            g_fd_dir[i] = 0;
            __atomic_store_n(&g_files[i], f, __ATOMIC_RELEASE);
            return i;
        }
    return -1;
}
static int fd_alloc_dir(ps3_vdir* d)
{
    std::lock_guard<std::mutex> hold(g_fd_lock);
    for (int i = 3; i < FS_MAX; i++)
        if (!g_files[i] && !g_dirs[i]) { __atomic_store_n(&g_dirs[i], d, __ATOMIC_RELEASE); return i; }
    return -1;
}
static int64_t fs_drm_read(int fd, uint64_t offset, void* dst, uint64_t size)
{
    std::lock_guard<std::mutex> hold(g_drm_lock);
    return npdrm_read(g_drm[fd], offset, dst, size);
}

static int fs_open_npd(ps3_vfile* f, npdrm_file** drm)
{
    *drm = nullptr;
    if (!npdrm_is_npd(f)) return 0;
    *drm = npdrm_open(f);
    return *drm ? 0 : -1;
}

static ps3_vfile* fd_file(int fd)
{
    return fd >= 0 && fd < FS_MAX ? __atomic_load_n(&g_files[fd], __ATOMIC_ACQUIRE) : nullptr;
}
static ps3_vdir* fd_dir(int fd)
{
    return fd >= 0 && fd < FS_MAX ? __atomic_load_n(&g_dirs[fd], __ATOMIC_ACQUIRE) : nullptr;
}

static void (*g_fs_open_cb)(const char*) = nullptr;

extern "C" void ppu_fs_set_open_callback(void (*cb)(const char*))
{
    g_fs_open_cb = cb;
}

static void cellFsOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    int acc = flags & 0x3;
    int oflags = (acc == CELL_FS_O_RDWR)   ? O_RDWR
               : (acc == CELL_FS_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & CELL_FS_O_CREAT)  oflags |= O_CREAT;
    if (flags & CELL_FS_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflags |= O_APPEND;

    ps3_vstat vst;
    if (ps3_vfs_stat(hpath, &vst) == 0 && vst.is_dir) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EISDIR;
        return;
    }
    ps3_vfile* f = nullptr;
    int hfd = open(hpath, oflags | O_BINARY, 0666);
    if (hfd >= 0) {
        const char* fmode = (acc == CELL_FS_O_RDWR)
                            ? ((flags & CELL_FS_O_APPEND) ? "ab+" : "rb+")
                            : (acc == CELL_FS_O_WRONLY)
                              ? ((flags & CELL_FS_O_APPEND) ? "ab" : "wb")
                              : "rb";
        FILE* hf = fdopen(hfd, fmode);
        if (!hf) {
            close(hfd);
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
        }
        f = ps3_vfile_adopt(hf);
    } else if (oflags == O_RDONLY) {
        f = ps3_vfile_open(hpath, "rb");
    }
    if (!f) {
        fprintf(stderr, "[fs] open FAIL '%s' -> '%s' (flags=0x%X)\n", gpath, hpath, flags);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    if (g_fs_open_cb) g_fs_open_cb(gpath);
    npdrm_file* drm = nullptr;
    if (acc != CELL_FS_O_WRONLY && acc != CELL_FS_O_RDWR && fs_open_npd(f, &drm) < 0) {
        ps3_vfile_close(f);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EFSSPECIFIC; return;
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { if (drm) npdrm_close(drm); else ps3_vfile_close(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    g_drm[fd] = drm;
    g_drm_pos[fd] = 0;
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    fprintf(stderr, "[fs] %llu ms open '%s' -> fd %d\n", ps3_ms_now(), gpath, fd);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsSdataOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);
    ps3_vfile* f = ps3_vfile_open(hpath, "rb");
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    npdrm_file* drm = nullptr;
    if (fs_open_npd(f, &drm) < 0) { ps3_vfile_close(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EFSSPECIFIC; return; }
    int fd = fd_alloc_file(f);
    if (fd < 0) { if (drm) npdrm_close(drm); else ps3_vfile_close(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    g_drm[fd] = drm;
    g_drm_pos[fd] = 0;
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    ps3_vfile* f = nullptr;
    npdrm_file* drm = nullptr;
    if (fd >= 0 && fd < FS_MAX) {
        std::lock_guard<std::mutex> hold(g_fd_lock);
        f = g_files[fd];
        drm = g_drm[fd];
        g_drm[fd] = nullptr;
        if (f) {
            __atomic_store_n(&g_files[fd], (ps3_vfile*)nullptr, __ATOMIC_RELEASE);
            g_fd_dir[fd] = 0;
        }
    }
    if (drm) npdrm_close(drm);
    else if (f) ps3_vfile_close(f);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsRead(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    ps3_vfile* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    if (g_drm[fd]) {
        if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
        const int64_t got = fs_drm_read(fd, g_drm_pos[fd], vm_base + buf, nbytes);
        if (got < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
        g_drm_pos[fd] += (uint64_t)got;
        if (nread_ptr) vm_write64(nread_ptr, (uint64_t)got);
        ctx->gpr[3] = CELL_OK;
        return;
    }
    if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
    fs_set_dir(fd, f, 1);
    size_t n = ps3_vfile_read(vm_base + buf, (size_t)nbytes, f);
    { static uint64_t tot=0; static int _n=0;
      static uint64_t per_fd[64]; static uint32_t cnt_fd[64];
      tot+=n;
      if (fd>=0 && fd<64) { per_fd[fd]+=n; cnt_fd[fd]++; }
      int first_for_fd = (fd>=0 && fd<64 && cnt_fd[fd]==1);
      if(PS3_LOG_FIRST(_n, 50) || first_for_fd)
        fprintf(stderr,"[fs] read fd=%d nbytes=%llu -> %zu (magic=%02X%02X%02X%02X, total=%llu)%s\n",
                fd,(unsigned long long)nbytes,n,vm_base[buf],vm_base[buf+1],vm_base[buf+2],vm_base[buf+3],
                (unsigned long long)tot, first_for_fd?"  <= FIRST READ ON THIS FD":"");
      if ((_n % 2000)==0) { fprintf(stderr,"[fs] read summary after %d reads:",_n);
          for (int i=0;i<64;i++) if (cnt_fd[i]) fprintf(stderr," fd%d=%ux/%lluB",i,cnt_fd[i],(unsigned long long)per_fd[i]);
          fprintf(stderr,"\n"); } }
    if (nread_ptr) vm_write64(nread_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsWrite(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nwr_ptr = (uint32_t)ctx->gpr[6];
    ps3_vfile* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    fs_set_dir(fd, f, 2);
    size_t n = ps3_vfile_write(vm_base + buf, (size_t)nbytes, f);
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsLseek(ppu_context* ctx)
{
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    ps3_vfile* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    if (wh > CELL_FS_SEEK_END) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EINVAL; return; }
    if (npdrm_file* drm = g_drm[fd]) {
        const int64_t base = wh == CELL_FS_SEEK_END ? (int64_t)npdrm_size(drm) : wh == CELL_FS_SEEK_CUR ? (int64_t)g_drm_pos[fd] : 0;
        if (base + off < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EINVAL; return; }
        g_drm_pos[fd] = (uint64_t)(base + off);
        if (pos_ptr) vm_write64(pos_ptr, g_drm_pos[fd]);
        ctx->gpr[3] = CELL_OK;
        return;
    }
    int worigin = (wh == CELL_FS_SEEK_END) ? SEEK_END : (wh == CELL_FS_SEEK_CUR) ? SEEK_CUR : SEEK_SET;
    const int64_t before = ps3_vfile_tell(f);
    if (ps3_vfile_seek(f, off, worigin) != 0) {
        if (before >= 0) ps3_vfile_seek(f, before, SEEK_SET);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EINVAL; return;
    }
    int64_t p = ps3_vfile_tell(f);
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

static void write_stat(uint32_t sb, uint32_t mode, uint64_t size,
                       int64_t atime = 0, int64_t mtime = 0, int64_t ctime = 0)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);
    vm_write32(sb + 0x08, 0);
    vm_write64(sb + 0x0C, (uint64_t)atime);
    vm_write64(sb + 0x14, (uint64_t)mtime);
    vm_write64(sb + 0x1C, (uint64_t)ctime);
    vm_write64(sb + 0x24, size);
    vm_write64(sb + 0x2C, 0x200);
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    ps3_vstat st;
    if (ps3_vfs_stat(hpath, &st) != 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    uint32_t mode = st.is_dir ? (CELL_FS_S_IFDIR | 0x1FF) : (CELL_FS_S_IFREG | 0x1B6);
    if (sb) write_stat(sb, mode, st.size, st.atime, st.mtime, st.ctime);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    ps3_vfile* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    ps3_vstat st;
    ps3_vfile_fstat(f, &st);
    const int64_t sz = g_drm[fd] ? (int64_t)npdrm_size(g_drm[fd]) : (int64_t)st.size;
    if (sb) write_stat(sb, CELL_FS_S_IFREG | 0x1B6, (uint64_t)sz, st.atime, st.mtime, st.ctime);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    ps3_vdir* d = ps3_vdir_open(hpath);
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    int fd = fd_alloc_dir(d);
    if (fd < 0) { ps3_vdir_close(d); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsReaddir(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    ps3_vdir* d = fd_dir(fd);
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    int is_dir = 0;
    const char* name = ps3_vdir_next(d, &is_dir);
    if (!name) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
    size_t nl = strlen(name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = is_dir ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClosedir(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    ps3_vdir* d = nullptr;
    if (fd >= 0 && fd < FS_MAX) {
        std::lock_guard<std::mutex> hold(g_fd_lock);
        d = g_dirs[fd];
        __atomic_store_n(&g_dirs[fd], (ps3_vdir*)nullptr, __ATOMIC_RELEASE);
    }
    if (d) ps3_vdir_close(d);
    ctx->gpr[3] = CELL_OK;
}

#define HOST_MKDIR(p) ps3_utf8_mkdir(p)

static int host_mkdir_p(const char* path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len-1] == '/' || tmp[len-1] == '\\')) tmp[--len] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; HOST_MKDIR(tmp); *p = c; }
    }
    int r = HOST_MKDIR(tmp);
    if (r != 0) { ps3_host_stat_t st; if (ps3_utf8_stat64(tmp, &st) == 0 && (st.st_mode & S_IFDIR)) r = 0; }
    return r;
}

static void cellFsMkdir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    host_mkdir_p(hpath);
    ctx->gpr[3] = CELL_OK;
}
static size_t fs_mount_len(const char* p)
{
    if (p[0] != '/') return 0;
    const char* e = strchr(p + 1, '/');
    return e ? (size_t)(e - p) : strlen(p);
}

static void cellFsRename(ppu_context* ctx)
{
    const uint32_t from_ea = (uint32_t)ctx->gpr[3], to_ea = (uint32_t)ctx->gpr[4];
    if (!from_ea || !to_ea) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EFAULT; return; }
    char gfrom[1024], gto[1024], hfrom[1100], hto[1100];
    guest_strcpy(gfrom, from_ea, sizeof gfrom);
    guest_strcpy(gto, to_ea, sizeof gto);
    const size_t mf = fs_mount_len(gfrom), mt = fs_mount_len(gto);
    if (!mf || !mt) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOTMOUNTED; return; }
    if (mf != mt || strncmp(gfrom, gto, mf) != 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EXDEV; return; }
    if (strncmp(gfrom, "/dev_bdvd", mf) == 0 || strncmp(gfrom, "/dev_flash", mf) == 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EROFS; return;
    }
    host_path(hfrom, sizeof hfrom, gfrom);
    host_path(hto, sizeof hto, gto);
    ps3_vstat st;
    if (ps3_vfs_stat(hfrom, &st) != 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    if (ps3_vfs_stat(hto, &st) == 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EEXIST; return; }
    if (ps3_utf8_rename(hfrom, hto) != 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)(errno == ENOTDIR ? CELL_FS_ENOTDIR : CELL_FS_ENOENT);
        return;
    }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsRmdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsUnlink(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
static void cellFsFsync(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (ps3_vfile* f = fd_file(fd))
        if (FILE* h = ps3_vfile_host(f)) fflush(h);
    ctx->gpr[3] = CELL_OK;
}
static void cellFsGetFreeSize(ppu_context* ctx)
{
    uint32_t bs = (uint32_t)ctx->gpr[4], fb = (uint32_t)ctx->gpr[5];
    if (bs) vm_write32(bs, 4096);
    if (fb) vm_write64(fb, (32ULL << 30) / 4096);
    ctx->gpr[3] = CELL_OK;
}
static void cellFsUtime(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
static void cellFsSdataOpenByFd(ppu_context* ctx)
{
    static int n;
    if (PS3_LOG_FIRST(n, 4)) fprintf(stderr, "[fs] cellFsSdataOpenByFd(fd=%d) not implemented -> CELL_ENOSYS\n",
                         (int)(uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOSYS;
}

extern "C" void ppu_fs_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpen"),     "cellFsOpen",     cellFsOpen);
    ps3_hle_register_ctx(0xB1840B53u, "cellFsSdataOpen", cellFsSdataOpen);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClose"),    "cellFsClose",    cellFsClose);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRead"),     "cellFsRead",     cellFsRead);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWrite"),    "cellFsWrite",    cellFsWrite);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsLseek"),    "cellFsLseek",    cellFsLseek);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsStat"),     "cellFsStat",     cellFsStat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFstat"),    "cellFsFstat",    cellFsFstat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpendir"),  "cellFsOpendir",  cellFsOpendir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReaddir"),  "cellFsReaddir",  cellFsReaddir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClosedir"), "cellFsClosedir", cellFsClosedir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsMkdir"),    "cellFsMkdir",    cellFsMkdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRename"),   "cellFsRename",   cellFsRename);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRmdir"),    "cellFsRmdir",    cellFsRmdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUnlink"),   "cellFsUnlink",   cellFsUnlink);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFsync"),    "cellFsFsync",    cellFsFsync);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetFreeSize"),   "cellFsGetFreeSize",   cellFsGetFreeSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUtime"),         "cellFsUtime",         cellFsUtime);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsSdataOpenByFd"), "cellFsSdataOpenByFd", cellFsSdataOpenByFd);
}
