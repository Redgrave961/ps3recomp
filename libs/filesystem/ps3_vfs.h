#ifndef PS3RECOMP_PS3_VFS_H
#define PS3RECOMP_PS3_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_vfile ps3_vfile;
typedef struct ps3_vdir ps3_vdir;

typedef struct {
    uint64_t size;
    int is_dir;
    int64_t atime, mtime, ctime;
} ps3_vstat;

enum { PS3_VFS_OK = 0, PS3_VFS_EOPEN = -1, PS3_VFS_EFORMAT = -2 };

int ps3_vfs_mount_iso(const char* host_dir, const char* iso_path);
int ps3_vfs_mount_pkg(const char* game_dir, const char* pkg_path, char content_id[37]);
int ps3_vfs_pkg_info(const char* pkg_path, char content_id[37], int* has_eboot);
int ps3_vfs_enum_last_mount(void (*cb)(const char* full_path, void* user), void* user);
void ps3_vfs_unmount_all(void);

int ps3_vfs_stat(const char* path, ps3_vstat* st);

ps3_vfile* ps3_vfile_open(const char* path, const char* mode);
ps3_vfile* ps3_vfile_adopt(FILE* f);
size_t     ps3_vfile_read(void* dst, size_t n, ps3_vfile* f);
size_t     ps3_vfile_write(const void* src, size_t n, ps3_vfile* f);
int        ps3_vfile_seek(ps3_vfile* f, int64_t offset, int whence);
int64_t    ps3_vfile_tell(ps3_vfile* f);
int        ps3_vfile_fstat(ps3_vfile* f, ps3_vstat* st);
FILE*      ps3_vfile_host(ps3_vfile* f);
void       ps3_vfile_close(ps3_vfile* f);

ps3_vdir*   ps3_vdir_open(const char* path);
const char* ps3_vdir_next(ps3_vdir* d, int* is_dir);
void        ps3_vdir_close(ps3_vdir* d);

#ifdef __cplusplus
}
#endif

#endif
