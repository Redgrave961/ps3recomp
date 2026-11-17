/*
 * ps3recomp - cellFs VFS (sys_fs HLE)
 *
 * Backs the game's file I/O with the real host filesystem so it can load its
 * data/config/assets. Guest PS3 paths (/dev_bdvd/..., /app_home/..., /dev_hdd0,
 * etc.) are translated to a host root (the game directory that contains
 * PS3_GAME), set by the boot harness from the EBOOT path (or $PS3_VFS_ROOT).
 *
 * Context-aware HLE: guest pointers (path, buffers, out params) are read/written
 * through vm_base in big-endian.
 */
#define PPU_NO_INLINE_STORES   /* declares its own out-of-line vm_write32/64 below */
#include "ppu_recomp.h"      /* ppu_context */
#include "../../libs/filesystem/npdrm.h"
#include "ps3emu/nid.h"      /* ps3_compute_nid */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <mutex>
#include <sys/stat.h>
#include "../platform/win32_dirent.h"   /* <dirent.h>, or a Win32 stand-in */
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>          /* open/close on MinGW */
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0       /* POSIX has no text/binary distinction */
#endif

/* 64-bit file offsets and sizes. Windows is LLP64: `long` is 32 bits, so
 * fseek(long)/ftell and `struct stat` (st_size is a 32-bit _off_t on MinGW, and
 * stat() is stat64i32) cannot express a file past 2 GiB. Disc archives are
 * routinely bigger: Lseek truncated the offset, Fstat's size came from ftell's
 * overflow result and Stat's from the 32-bit wrapper, and what those return
 * differs between CRTs: MinGW-w64's own stat() wrapper (msvcrt builds) fails
 * with EOVERFLOW, UCRT's _stat64i32 (llvm-mingw) returns the size truncated, so
 * the same title took different paths depending on the toolchain. POSIX LP64
 * is unaffected. */
#ifdef _WIN32
#  define FS_SEEK64(f, o, w) _fseeki64((f), (int64_t)(o), (w))
#  define FS_TELL64(f)       ((int64_t)_ftelli64(f))
typedef struct _stat64 fs_stat64_t;
/* _stat64 rejects a trailing slash (MinGW's stat wrapper stripped it). */

static int FS_STAT64(const char* p, fs_stat64_t* s)
{
    char t[1100]; size_t n = strlen(p);
    if (n >= sizeof t) return _stat64(p, s);
    memcpy(t, p, n + 1);
    while (n > 1 && (t[n - 1] == '/' || t[n - 1] == '\\') && t[n - 2] != ':') t[--n] = 0;
    return _stat64(t, s);
}
#else
#  define FS_SEEK64(f, o, w) fseeko((f), (off_t)(o), (w))
#  define FS_TELL64(f)       ((int64_t)ftello(f))
#  define FS_STAT64(p, s)    stat((p), (s))
typedef struct stat fs_stat64_t;
#endif

extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_vm_size;
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" void     sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);
extern "C" unsigned long long ps3_ms_now(void);
#include "ps3emu/log_first.h"
#include "ps3emu/ps3_env_once.h"   /* ps3_env_once: each switch looked up once */

/* CELL_FS return / mode / flag constants. */
#define CELL_OK              0
#define CELL_FS_ENOENT       (-2147418106)   /* 0x80010006 -- was -2147418090, which is 0x80010016 = CELL_ENOTCONN, not ENOENT */
#define CELL_FS_EISDIR       (-2147418094)   /* 0x80010012 */
/* 0x8001002B. This was -2147418111 = 0x80010001, which is CELL_EAGAIN ("try
 * again"), not EIO: a title that retries on EAGAIN spun on a hard I/O failure. */
#define CELL_FS_EIO          (-2147418069)
#define CELL_FS_EFSSPECIFIC  (-2147418056)
#define CELL_FS_EBADF        (-2147418070)   /* 0x8001002A: not an open fd */
#define CELL_FS_EMFILE       (-2147418068)   /* 0x8001002C: fd table full */
#define CELL_FS_EINVAL       (-2147418110)   /* 0x80010002 */
#define CELL_FS_ENOSYS       (-2147418109)   /* 0x80010003 */
#define CELL_FS_EFAULT       ((int32_t)0x8001000Du)
#define CELL_FS_EEXIST       ((int32_t)0x80010014u)
#define CELL_FS_EROFS        ((int32_t)0x80010026u)
#define CELL_FS_ENOTDIR      ((int32_t)0x8001002Eu)
#define CELL_FS_EXDEV        ((int32_t)0x80010030u)
#define CELL_FS_ENOTMOUNTED  ((int32_t)0x8001003Au)
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_RDONLY     0
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
/* Open flags: the PS3 SDK values (octal). The hex guesses that used to be here
 * (O_CREAT 0x0200, O_TRUNC 0x0400, O_APPEND 0x0100) put the right numbers on
 * the wrong names -- 0x200 and 0x400 are TRUNC and APPEND, not CREAT and
 * TRUNC -- so a title opening a file with O_RDWR|O_CREAT (flags 0x42 =
 * 000002 | 000100) hit an unnamed bit and got a plain read-only open; the file
 * was never created. */
#define CELL_FS_O_CREAT      0x0040   /* 000100 */
#define CELL_FS_O_TRUNC      0x0200   /* 001000 */
#define CELL_FS_O_APPEND     0x0400   /* 002000 */
#define CELL_FS_O_EXCL       0x0080   /* 000200 */
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

/* ---- guest memory string helpers ---- */
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

/* ---- fd / dir handle tables ---- */
#define FS_MAX 256
static FILE* g_files[FS_MAX];
static npdrm_file* g_drm[FS_MAX];
static uint64_t g_drm_pos[FS_MAX];
static std::mutex g_drm_lock;
static DIR*  g_dirs[FS_MAX];
/* Last transfer direction on each fd's stream: 0 none, 1 read, 2 write.
 *
 * C allows a read after a write on one stream only once it has been flushed or
 * repositioned, and a write after a read only once it has been repositioned;
 * otherwise the result is undefined. cellFs has no such rule, so a title may
 * write and then read on one fd with nothing in between. FIOS does exactly
 * that with its HDD cache: it writes a 512-byte header into a new cache.idx
 * and reads on. On the MSVC CRT that read returned 3584 bytes of stale buffer
 * and the header never reached the file (size still 0), so the index never
 * validated and FIOS rebuilt it for good -- ICO stayed on a black screen.
 * fs_set_dir() repositions in place (SEEK_CUR 0) when the direction changes. */
static uint8_t g_fd_dir[FS_MAX];
static void fs_set_dir(int fd, FILE* f, uint8_t dir)
{
    if (g_fd_dir[fd] && g_fd_dir[fd] != dir) FS_SEEK64(f, 0, SEEK_CUR);
    g_fd_dir[fd] = dir;
}
static char  g_dir_path[FS_MAX][1024];   /* host path per open dir (for readdir stat) */

/* Guest threads open, read and close concurrently (FIOS media threads, audio
 * streamers). Allocation and release take this lock, so two opens can no longer
 * claim the same slot, and every handler reads its slot once (fd_file/fd_dir)
 * rather than re-reading a pointer another thread's close may have cleared. */
static std::mutex g_fd_lock;
static int fd_alloc_file(FILE* f)
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
static int fd_alloc_dir(DIR* d)
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

static int fs_open_npd(FILE* f, npdrm_file** drm)
{
    *drm = nullptr;
    if (!npdrm_is_npd(f)) return 0;
    *drm = npdrm_open(f, nullptr);
    return *drm ? 0 : -1;
}

static FILE* fd_file(int fd)
{
    return fd >= 0 && fd < FS_MAX ? __atomic_load_n(&g_files[fd], __ATOMIC_ACQUIRE) : nullptr;
}
static DIR* fd_dir(int fd)
{
    return fd >= 0 && fd < FS_MAX ? __atomic_load_n(&g_dirs[fd], __ATOMIC_ACQUIRE) : nullptr;
}

/* ---- handlers ---- */
static void cellFsOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* fopen() mode strings can't express the PS3/POSIX open semantics (e.g.
     * O_WRONLY without create+truncate, or O_CREAT without O_TRUNC), so build
     * real open() flags from the access mode (low 2 bits) + the modifiers, then
     * wrap the fd in a FILE* with fdopen() (which neither creates nor truncates
     * -- that was already decided by open()). */
    int acc = flags & 0x3;
    int oflags = (acc == CELL_FS_O_RDWR)   ? O_RDWR
               : (acc == CELL_FS_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & CELL_FS_O_CREAT)  oflags |= O_CREAT;
    if (flags & CELL_FS_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflags |= O_APPEND;

    /* A bare device root ("/dev_bdvd") resolves to a DIRECTORY, and open() on a
     * directory fails on Windows. Real cellFsOpen answers EISDIR there, and a
     * title reads that as "the medium is mounted" -- YDKJ probes the disc this
     * way before loading its Scaleform UI and FMOD banks, so the ENOENT it got
     * instead read as "no disc" and it drew nothing but a clear colour.
     * S_IFMT/S_IFDIR rather than S_ISDIR: MSVC's CRT does not define the macro. */
    {
        struct stat _dst;
        if (stat(hpath, &_dst) == 0 && (_dst.st_mode & S_IFMT) == S_IFDIR) {
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EISDIR;
            return;
        }
    }
    int hfd = open(hpath, oflags | O_BINARY, 0666);
    if (hfd < 0) {
        fprintf(stderr, "[fs] open FAIL '%s' -> '%s' (flags=0x%X)\n", gpath, hpath, flags);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    const char* fmode = (acc == CELL_FS_O_RDWR)
                        ? ((flags & CELL_FS_O_APPEND) ? "ab+" : "rb+")
                        : (acc == CELL_FS_O_WRONLY)
                          ? ((flags & CELL_FS_O_APPEND) ? "ab" : "wb")
                          : "rb";
    FILE* f = fdopen(hfd, fmode);
    if (!f) {
        close(hfd);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
    }
    npdrm_file* drm = nullptr;
    if (acc != CELL_FS_O_WRONLY && acc != CELL_FS_O_RDWR && fs_open_npd(f, &drm) < 0) {
        fclose(f);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EFSSPECIFIC; return;
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { if (drm) npdrm_close(drm); else fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
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
    FILE* f = fopen(hpath, "rb");
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    npdrm_file* drm = nullptr;
    if (fs_open_npd(f, &drm) < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EFSSPECIFIC; return; }
    int fd = fd_alloc_file(f);
    if (fd < 0) { if (drm) npdrm_close(drm); else fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    g_drm[fd] = drm;
    g_drm_pos[fd] = 0;
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    FILE* f = nullptr;
    npdrm_file* drm = nullptr;
    if (fd >= 0 && fd < FS_MAX) {
        std::lock_guard<std::mutex> hold(g_fd_lock);
        f = g_files[fd];
        drm = g_drm[fd];
        g_drm[fd] = nullptr;
        if (f) {
            __atomic_store_n(&g_files[fd], (FILE*)nullptr, __ATOMIC_RELEASE);
            g_fd_dir[fd] = 0;
        }
    }
    if (drm) npdrm_close(drm);
    else if (f) fclose(f);
    ctx->gpr[3] = CELL_OK;
}

/* Pre-fault a guest range so the kernel can write into it.
 *
 * The flat VM is MEM_RESERVEd and each page is committed on FIRST ACCESS by a
 * vectored exception handler. That covers CPU access from lifted code, but
 * fread/fwrite move data through the kernel, and a kernel write to a reserved
 * page does not raise a user-mode exception -- the I/O just fails, returning 0
 * with ferror set. Twisted Metal hit this on every read whose destination the
 * guest had not touched yet: read 1 of a file succeeded, read 2 into a fresh
 * page came back n=0 eof=0 err=1, and the title logged its own
 * "Short read ... Possible reasons include disc eject" and gave up.
 *
 * Touch each page read-then-write so the fault happens here, in user mode,
 * where the handler can commit it. Read-then-write rather than a plain store so
 * nothing already in the buffer is disturbed. */
static inline void fs_prefault(uint32_t buf, uint64_t len)
{
    if (!vm_base || !len) return;
    volatile uint8_t* p = (volatile uint8_t*)(vm_base + buf);
    for (uint64_t o = 0; o < len; o += 0x1000) p[o] = p[o];
    p[len - 1] = p[len - 1];
}

static void cellFsRead(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    FILE* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    if (g_drm[fd]) {
        if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
        fs_prefault(buf, nbytes);
        const int64_t got = fs_drm_read(fd, g_drm_pos[fd], vm_base + buf, nbytes);
        if (got < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
        g_drm_pos[fd] += (uint64_t)got;
        if (nread_ptr) vm_write64(nread_ptr, (uint64_t)got);
        ctx->gpr[3] = CELL_OK;
        return;
    }
    if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
    fs_prefault(buf, nbytes);
    fs_set_dir(fd, f, 1);
    size_t n = fread(vm_base + buf, 1, (size_t)nbytes, f);   /* raw bytes, no swap */
    /* Per-fd totals, not just the first 50 lines: a per-fd first-read line
     * plus a periodic summary tells "opened and never read" apart from reads
     * that fell off the end of the log. */
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
    FILE* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    fs_prefault(buf, nbytes);   /* kernel READS the buffer; same reserved-page trap */
    fs_set_dir(fd, f, 2);
    size_t n = fwrite(vm_base + buf, 1, (size_t)nbytes, f);
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsLseek(ppu_context* ctx)
{
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    FILE* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    /* An unknown whence, or a seek that would land before the start of the
     * file, is CELL_EINVAL and leaves the offset where it was. fseek failing
     * is not harmless on the MSVC CRT: it drops the stream's buffer and the
     * stream then reports the OS position, which is past the buffered data,
     * so the old position is put back explicitly. */
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
    const int64_t before = FS_TELL64(f);
    if (FS_SEEK64(f, off, worigin) != 0) {
        if (before >= 0) FS_SEEK64(f, before, SEEK_SET);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EINVAL; return;
    }
    int64_t p = FS_TELL64(f);
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsStat is 0x34 (52) bytes, 4-byte aligned -- the s64/u64 members are
 * be_t<...,4> so there is NO 4-byte pad after gid (verified vs RPCS3:
 * CHECK_SIZE_ALIGN(CellFsStat, 52, 4)). Laying it out 8-byte-aligned (0x38,
 * pad@0x0C) shifts size/blksize +4 and overruns the struct by 4 bytes -- games
 * that embed a CellFsStat inside a larger object (e.g. Dantelion's
 * DLFileDeviceStream, stat@obj+0xD8) then have the trailing blksize clobber the
 * field right after the stat (the fd at obj+0x10c), which later fails lseek.
 * Layout: mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C. */
static void write_stat(uint32_t sb, uint32_t mode, uint64_t size,
                       int64_t atime = 0, int64_t mtime = 0, int64_t ctime = 0)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);            /* uid */
    vm_write32(sb + 0x08, 0);            /* gid */
    vm_write64(sb + 0x0C, (uint64_t)atime);   /* seconds since the epoch */
    vm_write64(sb + 0x14, (uint64_t)mtime);
    vm_write64(sb + 0x1C, (uint64_t)ctime);
    vm_write64(sb + 0x24, size);         /* size */
    vm_write64(sb + 0x2C, 0x200);        /* blksize */
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    fs_stat64_t st;
    if (FS_STAT64(hpath, &st) != 0) {
        if (ps3_env_once("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> ENOENT\n", gpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    if (ps3_env_once("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> OK (size=%lld)\n", gpath, (long long)st.st_size);
    uint32_t mode = (st.st_mode & S_IFDIR) ? (CELL_FS_S_IFDIR | 0x1FF)
                                           : (CELL_FS_S_IFREG | 0x1B6);
    /* The file's own times; they used to be written as 0 (1970), and a title
     * that lists a directory and shows or compares file dates saw all of them
     * equal. */
    if (sb) write_stat(sb, mode, (uint64_t)st.st_size,
                       (int64_t)st.st_atime, (int64_t)st.st_mtime, (int64_t)st.st_ctime);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    FILE* f = fd_file(fd);
    if (!f) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    int64_t sz;
    if (g_drm[fd]) {
        sz = (int64_t)npdrm_size(g_drm[fd]);
    } else {
        int64_t cur = FS_TELL64(f);
        FS_SEEK64(f, 0, SEEK_END);
        sz = FS_TELL64(f);
        FS_SEEK64(f, cur, SEEK_SET);
    }
    if (sb) {
        /* Times only from the host: the size above also counts unflushed writes. */
        fs_stat64_t fst;   /* 64-bit: a 32-bit fstat fails on a file past 2 GiB */
#ifdef _WIN32
        const int have_t = _fstat64(_fileno(f), &fst) == 0;
#else
        const int have_t = fstat(fileno(f), &fst) == 0;
#endif
        write_stat(sb, CELL_FS_S_IFREG | 0x1B6, (uint64_t)sz,
                   have_t ? (int64_t)fst.st_atime : 0, have_t ? (int64_t)fst.st_mtime : 0,
                   have_t ? (int64_t)fst.st_ctime : 0);
    }
    if (ps3_env_once("PS3_FSLOG")) { static int _n=0; if(PS3_LOG_FIRST(_n, 12)) fprintf(stderr,"[FSDBG] cellFsFstat(fd=%d) -> size=0x%llX\n",fd,(unsigned long long)sz); }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    DIR* d = opendir(hpath);
    if (ps3_env_once("PS3_FSLOG")) fprintf(stderr, "[fs] opendir '%s' -> %s\n", gpath, d ? "OK" : "ENOENT");
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    int fd = fd_alloc_dir(d);
    if (fd < 0) { closedir(d); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EMFILE; return; }
    strncpy(g_dir_path[fd], hpath, sizeof g_dir_path[fd] - 1);
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirent: d_type(1) d_namlen(1) d_name[256]; total 0x102. */
static void cellFsReaddir(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    DIR* d = fd_dir(fd);
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EBADF; return; }
    struct dirent* e = readdir(d);
    if (!e) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
    char full[1300]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", g_dir_path[fd], e->d_name);
    /* "." and ".." are always directories: a title that walks a tree skips the
     * dot entries by type and name, and one typed as a file would be handed
     * to its file handler. Not left to stat("<dir>//.") resolving. */
    const int dot = e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]));
    uint8_t type = (dot || (stat(full, &st) == 0 && (st.st_mode & S_IFDIR)))
                   ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    size_t nl = strlen(e->d_name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = type;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)e->d_name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClosedir(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    DIR* d = nullptr;
    if (fd >= 0 && fd < FS_MAX) {
        std::lock_guard<std::mutex> hold(g_fd_lock);
        d = g_dirs[fd];
        __atomic_store_n(&g_dirs[fd], (DIR*)nullptr, __ATOMIC_RELEASE);
    }
    if (d) closedir(d);
    ctx->gpr[3] = CELL_OK;
}

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

/* Create `path` and any missing parent dirs on the host. Returns 0 on
 * success or if the directory already exists. */
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
    if (r != 0) { struct stat st; if (stat(tmp, &st) == 0 && (st.st_mode & S_IFDIR)) r = 0; }
    return r;
}

/* cellFsMkdir(path, mode): create the guest directory on the host. A no-op stub
 * here silently breaks games that create then poll a dir (e.g. LBP's boot waits
 * for /dev_hdd0/game/<title>/USRDIR to appear). Create parents too. */
static void cellFsMkdir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    int r = host_mkdir_p(hpath);
    if (ps3_env_once("PS3_FSLOG")) fprintf(stderr, "[fs] mkdir '%s' -> '%s' (%s)\n", gpath, hpath, r == 0 ? "OK" : "FAIL");
    ctx->gpr[3] = CELL_OK;   /* existing dir is benign for boot */
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
    fs_stat64_t st;
    if (FS_STAT64(hfrom, &st) != 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    if (FS_STAT64(hto, &st) == 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EEXIST; return; }
    if (rename(hfrom, hto) != 0) {
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
    if (FILE* f = fd_file(fd)) fflush(f);
    ctx->gpr[3] = CELL_OK;
}
/* Free space for (path, u32* block_size, u64* free_block_count). Unregistered,
 * the call took the unresolved-NID path: CELL_OK with both out-params left as
 * unwritten, so a title sizing a cache read whatever was there. Report a
 * 4 KiB block and 32 GiB free: enough for any cache, and the same on every host. */
static void cellFsGetFreeSize(ppu_context* ctx)
{
    uint32_t bs = (uint32_t)ctx->gpr[4], fb = (uint32_t)ctx->gpr[5];
    if (bs) vm_write32(bs, 4096);
    if (fb) vm_write64(fb, (32ULL << 30) / 4096);
    ctx->gpr[3] = CELL_OK;
}
static void cellFsUtime(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }   /* timestamps are not modelled */
/* SDATA opened from an already-open fd is not implemented: fail visibly rather
 * than return CELL_OK with *sdata_fd never written. */
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
    /* Register by the literal import NID: LBP imports 0xB1840B53 for
     * cellFsSdataOpen, which ps3_compute_nid("cellFsSdataOpen") does NOT match
     * (the real exported symbol name differs from the friendly name). */
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
