#ifndef SYS_FS_H
#define SYS_FS_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../../libs/filesystem/ps3_vfs.h"
#include "../platform/ps3_utf8_fs.h"

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/stat.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_FS_FD_MAX   256
#define SYS_FS_DIR_MAX  64
#define SYS_FS_HOST_PATH_MAX PS3_HOST_PATH_MAX

#define CELL_FS_S_IFDIR   0040000
#define CELL_FS_S_IFREG   0100000
#define CELL_FS_S_IRUSR   0000400
#define CELL_FS_S_IWUSR   0000200
#define CELL_FS_S_IXUSR   0000100
#define CELL_FS_S_IRGRP   0000040
#define CELL_FS_S_IROTH   0000004

#define CELL_FS_O_WRONLY   0000001
#define CELL_FS_O_RDWR     0000002
#define CELL_FS_O_ACCMODE  0000003
#define CELL_FS_O_CREAT    0000100
#define CELL_FS_O_EXCL     0000200
#define CELL_FS_O_TRUNC    0001000
#define CELL_FS_O_APPEND   0002000

#define CELL_FS_SEEK_SET   0
#define CELL_FS_SEEK_CUR   1
#define CELL_FS_SEEK_END   2

typedef struct sys_fs_fd_info {
    int    active;
    ps3_vfile* fp;
    int    flags;
} sys_fs_fd_info;

typedef struct sys_fs_dir_info {
    int    active;
    ps3_vdir* dir;
} sys_fs_dir_info;

extern sys_fs_fd_info  g_sys_fs_fds[SYS_FS_FD_MAX];
extern sys_fs_dir_info g_sys_fs_dirs[SYS_FS_DIR_MAX];

extern const char* ppu_vfs_root;

int sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

int64_t sys_fs_open(ppu_context* ctx);
int64_t sys_fs_read(ppu_context* ctx);
int64_t sys_fs_write(ppu_context* ctx);
int64_t sys_fs_close(ppu_context* ctx);
int64_t sys_fs_lseek(ppu_context* ctx);
int64_t sys_fs_stat(ppu_context* ctx);
int64_t sys_fs_fstat(ppu_context* ctx);
int64_t sys_fs_opendir(ppu_context* ctx);
int64_t sys_fs_readdir(ppu_context* ctx);
int64_t sys_fs_closedir(ppu_context* ctx);
int64_t sys_fs_mkdir(ppu_context* ctx);
int64_t sys_fs_rename(ppu_context* ctx);
int64_t sys_fs_unlink(ppu_context* ctx);
int64_t sys_fs_rmdir(ppu_context* ctx);
int64_t sys_fs_ftruncate(ppu_context* ctx);

void sys_fs_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
