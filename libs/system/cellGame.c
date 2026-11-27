#include "cellGame.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "ps3emu/guest_call.h"
#include "../filesystem/ps3_vfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>

#include "../../runtime/platform/ps3_utf8_fs.h"
#ifndef _WIN32
#  include <sys/types.h>
#  include <sys/statvfs.h>
#endif
#define HOST_MKDIR(p) ps3_utf8_mkdir(p)
#define HOST_STAT     ps3_utf8_stat64
#define HOST_STAT_T   ps3_host_stat_t

static char s_title_id[64]  = "BLES00000";
static char s_title[256]    = "Unknown Title";
static char s_version[16]   = "01.00";
static char s_system_ver[16] = "00.0000";
static char s_app_ver[16]   = "01.00";

static char s_content_path[CELL_GAME_PATH_MAX] = "./gamedata/dev_hdd0/game";
static int  s_content_path_resolved = 0;

void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

static const char* content_root(void)
{
    if (!s_content_path_resolved) {
        s_content_path_resolved = 1;
        sys_fs_translate_path("/dev_hdd0/game", s_content_path, (int)sizeof s_content_path);
        printf("[cellGame] content root (host /dev_hdd0/game): %s\n", s_content_path);
    }
    return s_content_path;
}
static char s_content_info_path[CELL_GAME_PATH_MAX] = "";
static char s_usrdir_path[CELL_GAME_PATH_MAX] = "";
static char s_tmp_path[CELL_GAME_PATH_MAX] = "";

static u32  s_game_type = CELL_GAME_GAMETYPE_DISC;

static int  s_check_is_disc = 1;
static char s_check_dir[64] = "";

static void ensure_dirs(const char* path)
{
    char tmp[CELL_GAME_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    HOST_MKDIR(tmp);
}

static int dir_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static s32 content_free_kb(void)
{
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s", s_content_path[0] ? s_content_path : ".");
    for (;;) {
#ifdef _WIN32
        uint64_t avail = 0;
        if (ps3_utf8_disk_free_bytes(path, &avail)) {
            u64 kb = avail / 1024;
            return kb > INT32_MAX ? INT32_MAX : (s32)kb;
        }
        DWORD error = GetLastError();
        if (error != ERROR_PATH_NOT_FOUND && error != ERROR_FILE_NOT_FOUND) break;
#else
        struct statvfs info;
        if (statvfs(path, &info) == 0) {
            u64 block = info.f_frsize ? info.f_frsize : info.f_bsize;
            u64 count = info.f_bavail;
            if (!block) return 0;
            if (count > ((u64)INT32_MAX * 1024) / block) return INT32_MAX;
            return (s32)((count * block) / 1024);
        }
        if (errno != ENOENT && errno != ENOTDIR) break;
#endif
        size_t len = strlen(path);
#ifdef _WIN32
        if (len == 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) break;
#endif
        while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
            path[--len] = 0;
        char* slash = strrchr(path, '/');
#ifdef _WIN32
        char* backslash = strrchr(path, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        if (slash == path + 2 && path[1] == ':') {
            if (!slash[1]) break;
            slash[1] = 0;
            continue;
        }
#endif
        if (!slash) {
            if (strcmp(path, ".") == 0) break;
            strcpy(path, ".");
        } else if (slash == path) {
            if (!slash[1]) break;
            slash[1] = 0;
        } else {
            *slash = 0;
        }
    }
    return 0;
}

#define CELL_GAME_FREE_KB_WRAP     (4 * 1024 * 1024)
#define CELL_GAME_FREE_KB_REPORTED (40 * 1024 * 1024 - 256)
static s32 content_free_kb_reported(void)
{
    const s32 kb = content_free_kb();
    return kb >= CELL_GAME_FREE_KB_WRAP ? CELL_GAME_FREE_KB_REPORTED : kb;
}

const char* cellGame_get_title(void)
{
    return s_title;
}

static int sfo_read_string(const char* sfo_path, const char* key,
                           char* out, int out_size)
{
    ps3_vfile* fp = ps3_vfile_open(sfo_path, "rb");
    if (!fp) return -1;
    ps3_vstat st;
    ps3_vfile_fstat(fp, &st);
    const long sz = (long)st.size;
    if (sz < 0x14 || sz > (1 << 20)) { ps3_vfile_close(fp); return -1; }
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    if (!d) { ps3_vfile_close(fp); return -1; }
    int ok = (ps3_vfile_read(d, (size_t)sz, fp) == (size_t)sz);
    ps3_vfile_close(fp);
    if (!ok) { free(d); return -1; }

    int ret = -1;
    if (d[0]==0x00 && d[1]==0x50 && d[2]==0x53 && d[3]==0x46) {
        #define SFO_RD32(o) ((u32)d[o] | ((u32)d[(o)+1]<<8) | ((u32)d[(o)+2]<<16) | ((u32)d[(o)+3]<<24))
        #define SFO_RD16(o) ((u16)d[o] | ((u16)d[(o)+1]<<8))
        u32 key_tab  = SFO_RD32(0x08);
        u32 data_tab = SFO_RD32(0x0C);
        u32 n        = SFO_RD32(0x10);
        for (u32 i = 0; i < n; i++) {
            u32 e = 0x14 + i * 0x10;
            if (e + 0x10 > (u32)sz) break;
            u32 key_off  = SFO_RD16(e + 0x00);
            u32 data_len = SFO_RD32(e + 0x04);
            u32 data_off = SFO_RD32(e + 0x0C);
            if (key_tab + key_off >= (u32)sz) continue;
            if (strcmp((const char*)(d + key_tab + key_off), key) != 0) continue;
            u32 src = data_tab + data_off;
            if (src >= (u32)sz) break;
            int copy = (int)data_len;
            if (copy > (int)((u32)sz - src)) copy = (int)((u32)sz - src);
            if (copy >= out_size) copy = out_size - 1;
            if (copy < 0) copy = 0;
            memcpy(out, d + src, (size_t)copy);
            out[copy] = '\0';
            ret = 0;
            break;
        }
        #undef SFO_RD32
        #undef SFO_RD16
    }
    free(d);
    return ret;
}

void cellGame_init_from_paramsfo(const char* sfo_path)
{
    char tmp[256];
    if (sfo_read_string(sfo_path, "TITLE_ID", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_title_id, tmp, sizeof(s_title_id) - 1);
        s_title_id[sizeof(s_title_id) - 1] = '\0';
        printf("[cellGame] title id from PARAM.SFO ('%s'): '%s'\n", sfo_path, s_title_id);
    } else {
        printf("[cellGame] PARAM.SFO not read ('%s'); keeping title id '%s'\n",
               sfo_path, s_title_id);
    }
    if (sfo_read_string(sfo_path, "CATEGORY", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        if (strcmp(tmp, "HG") == 0)      s_game_type = CELL_GAME_GAMETYPE_HDD;
        else if (strcmp(tmp, "DG") == 0) s_game_type = CELL_GAME_GAMETYPE_DISC;
        printf("[cellGame] CATEGORY '%s' -> game type %u\n", tmp, (unsigned)s_game_type);
    }
    if (sfo_read_string(sfo_path, "TITLE", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_title, tmp, sizeof(s_title) - 1); s_title[sizeof(s_title) - 1] = '\0';
    }
    if (sfo_read_string(sfo_path, "VERSION", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_version, tmp, sizeof(s_version) - 1); s_version[sizeof(s_version) - 1] = '\0';
    }
    if (sfo_read_string(sfo_path, "PS3_SYSTEM_VER", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_system_ver, tmp, sizeof(s_system_ver) - 1);
        s_system_ver[sizeof(s_system_ver) - 1] = '\0';
    }
    if (sfo_read_string(sfo_path, "APP_VER", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_app_ver, tmp, sizeof(s_app_ver) - 1); s_app_ver[sizeof(s_app_ver) - 1] = '\0';
    }
}

static int  s_is_patch = 0;
static char s_boot_path[CELL_GAME_PATH_MAX] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";

static int file_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) == 0;
#else
    return !S_ISDIR(st.st_mode);
#endif
}

static void sfo_copy(const char* sfo_path, const char* key, char* dst, size_t cap)
{
    char tmp[256];
    if (sfo_read_string(sfo_path, key, tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(dst, tmp, cap - 1);
        dst[cap - 1] = '\0';
    }
}

void cellGame_init_boot(const char* vfs_root)
{
    static const char* const layouts[] = {
        "%s/PS3_GAME/PARAM.SFO",
        "%s/../PARAM.SFO",
        "%s/PARAM.SFO",
    };
    const size_t n_layouts = sizeof(layouts) / sizeof(layouts[0]);
    char sfo[1100];
    size_t i;
    for (i = 0; i < n_layouts; i++) {
        snprintf(sfo, sizeof(sfo), layouts[i], vfs_root);
        if (file_exists(sfo))
            break;
    }
    if (i == n_layouts)
        snprintf(sfo, sizeof(sfo), layouts[0], vfs_root);
    cellGame_init_from_paramsfo(sfo);

    if (s_game_type != CELL_GAME_GAMETYPE_DISC)
        return;

    char patch_sfo[1100], patch_eboot[1100], tmp[256];
    snprintf(patch_sfo, sizeof(patch_sfo), "%s/%s/PARAM.SFO", content_root(), s_title_id);
    snprintf(patch_eboot, sizeof(patch_eboot), "%s/%s/USRDIR/EBOOT.BIN", content_root(), s_title_id);
    if (!file_exists(patch_eboot))
        return;
    if (sfo_read_string(patch_sfo, "TITLE_ID", tmp, sizeof(tmp)) != 0 || strcmp(tmp, s_title_id) != 0)
        return;
    if (sfo_read_string(patch_sfo, "CATEGORY", tmp, sizeof(tmp)) != 0 || strcmp(tmp, "GD") != 0)
        return;

    s_is_patch = 1;
    sfo_copy(patch_sfo, "TITLE", s_title, sizeof(s_title));
    sfo_copy(patch_sfo, "VERSION", s_version, sizeof(s_version));
    sfo_copy(patch_sfo, "PS3_SYSTEM_VER", s_system_ver, sizeof(s_system_ver));
    sfo_copy(patch_sfo, "APP_VER", s_app_ver, sizeof(s_app_ver));
    snprintf(s_boot_path, sizeof(s_boot_path), "/dev_hdd0/game/%s/USRDIR/EBOOT.BIN", s_title_id);
    printf("[cellGame] update APP_VER %s installed at %s/%s; boot path %s\n",
           s_app_ver, content_root(), s_title_id, s_boot_path);
}

const char* cellGame_get_boot_path(void) { return s_boot_path; }

const char* cellGame_get_hdd_title_id(void)
{
    return s_game_type == CELL_GAME_GAMETYPE_HDD && s_title_id[0] ? s_title_id : NULL;
}

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName)
{
    printf("[cellGame] BootCheck()\n");

    snprintf(s_content_info_path, sizeof(s_content_info_path),
             "%s/%s", content_root(), s_title_id);
    snprintf(s_usrdir_path, sizeof(s_usrdir_path),
             "%s/%s/USRDIR", content_root(), s_title_id);
    snprintf(s_tmp_path, sizeof(s_tmp_path),
             "%s/%s_TMP", content_root(), s_title_id);

#ifdef _WIN32
    for (char* p = s_content_info_path; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = s_usrdir_path; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = s_tmp_path; *p; p++) if (*p == '/') *p = '\\';
#endif

    uint32_t type_ea = (uint32_t)(uintptr_t)type;
    uint32_t attr_ea = (uint32_t)(uintptr_t)attributes;
    uint32_t size_ea = (uint32_t)(uintptr_t)size;
    uint32_t dir_ea  = (uint32_t)(uintptr_t)dirName;

    if (type_ea) vm_write32(type_ea, (uint32_t)s_game_type);
    if (attr_ea) vm_write32(attr_ea, s_is_patch ? CELL_GAME_ATTRIBUTE_PATCH : 0);

    if (size_ea) {
        vm_write32(size_ea + 0, (u32)content_free_kb_reported());
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);
    }

    if (dir_ea) {
        size_t len = strlen(s_title_id);
        if (len > CELL_GAME_DIRNAME_SIZE - 1) len = CELL_GAME_DIRNAME_SIZE - 1;
        memcpy(vm_base + dir_ea, s_title_id, len);
        vm_base[dir_ea + len] = '\0';
    }

    s_check_is_disc = (s_game_type == CELL_GAME_GAMETYPE_DISC);
    strncpy(s_check_dir, s_title_id, sizeof(s_check_dir) - 1);
    s_check_dir[sizeof(s_check_dir) - 1] = '\0';
    printf("[cellGame] BootCheck: type=%u, titleId='%s'\n", s_game_type, s_title_id);
    return CELL_OK;
}

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath)
{
    printf("[cellGame] ContentPermit() -> %s\n",
           s_check_is_disc ? "/dev_bdvd/PS3_GAME" : "game-data dir");

    if (!s_check_is_disc) {
        ensure_dirs(s_content_info_path);
        ensure_dirs(s_usrdir_path);
        {
            char gpath[CELL_GAME_PATH_MAX], vpath[CELL_GAME_PATH_MAX];
            snprintf(gpath, sizeof gpath, "/dev_hdd0/game/%s/USRDIR", s_check_dir);
            sys_fs_translate_path(gpath, vpath, (int)sizeof vpath);
            ensure_dirs(vpath);
        }
    }

    char tmp[CELL_GAME_PATH_MAX];
    uint32_t cip_ea = (uint32_t)(uintptr_t)contentInfoPath;
    uint32_t usr_ea = (uint32_t)(uintptr_t)usrdirPath;
    if (cip_ea) {
        int n = s_check_is_disc
              ? snprintf(tmp, sizeof tmp, "/dev_bdvd/PS3_GAME")
              : snprintf(tmp, sizeof tmp, "/dev_hdd0/game/%s", s_check_dir);
        memcpy(vm_base + cip_ea, tmp, (size_t)n + 1);
    }
    if (usr_ea) {
        int n = s_check_is_disc
              ? snprintf(tmp, sizeof tmp, "/dev_bdvd/PS3_GAME/USRDIR")
              : snprintf(tmp, sizeof tmp, "/dev_hdd0/game/%s/USRDIR", s_check_dir);
        memcpy(vm_base + usr_ea, tmp, (size_t)n + 1);
    }

    return CELL_OK;
}

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
{
    uint32_t dir_ea = (uint32_t)(uintptr_t)dirName;
    const char* check_dir = dir_ea ? (const char*)(vm_base + dir_ea) : s_title_id;
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n", type, check_dir);

    s_check_is_disc = (type == CELL_GAME_GAMETYPE_DISC);
    strncpy(s_check_dir, check_dir, sizeof(s_check_dir) - 1);
    s_check_dir[sizeof(s_check_dir) - 1] = '\0';

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", content_root(), check_dir);

    uint32_t size_ea = (uint32_t)(uintptr_t)size;
    if (size_ea) {
        vm_write32(size_ea + 0, (u32)content_free_kb_reported());
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);
    }

    if (type == 1)
        return CELL_OK;

    if (dir_exists(path)) {
        return CELL_OK;
    }

    if (size_ea)
        vm_write32(size_ea + 4, 0);
    return CELL_GAME_RET_NONE;
}

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize)
{
    printf("[cellGame] GetParamString(id=%d, bufsize=%u)\n", id, bufsize);

    uint32_t buf_ea = (uint32_t)(uintptr_t)buf;
    if (!buf_ea || bufsize == 0)
        return CELL_GAME_ERROR_PARAM;
    char* hbuf = (char*)(vm_base + buf_ea);

    const char* src;
    if (id >= CELL_GAME_PARAMID_TITLE && id <= CELL_GAME_PARAMID_TITLE_TURKISH) {
        src = s_title;
    } else {
        switch (id) {
        case CELL_GAME_PARAMID_TITLE_ID:       src = s_title_id;   break;
        case CELL_GAME_PARAMID_VERSION:        src = s_version;    break;
        case CELL_GAME_PARAMID_PS3_SYSTEM_VER: src = s_system_ver; break;
        case CELL_GAME_PARAMID_APP_VER:        src = s_app_ver;    break;
        default:
            printf("[cellGame] WARNING: unknown param string id %d\n", id);
            src = "";
            break;
        }
    }
    strncpy(hbuf, src, bufsize - 1);
    hbuf[bufsize - 1] = '\0';
    return CELL_OK;
}

s32 cellGameCreateGameData(CellGameSetInitParams* init, char* tmp_contentInfoPath,
                            char* tmp_usrdirPath)
{
    printf("[cellGame] CreateGameData()\n");

    uint32_t init_ea = (uint32_t)(uintptr_t)init;
    if (!init_ea)
        return CELL_GAME_ERROR_PARAM;

    const char* create_dir = s_check_dir[0] ? s_check_dir : s_title_id;

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", content_root(), create_dir);

    ensure_dirs(path);

    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    uint32_t cip_ea = (uint32_t)(uintptr_t)tmp_contentInfoPath;
    uint32_t usr_ea = (uint32_t)(uintptr_t)tmp_usrdirPath;
    char guest_path[CELL_GAME_PATH_MAX];
    if (cip_ea) {
        snprintf(guest_path, sizeof(guest_path), "/dev_hdd0/game/%s", create_dir);
        memcpy(vm_base + cip_ea, guest_path, strlen(guest_path) + 1);
    }
    if (usr_ea) {
        snprintf(guest_path, sizeof(guest_path), "/dev_hdd0/game/%s/USRDIR", create_dir);
        memcpy(vm_base + usr_ea, guest_path, strlen(guest_path) + 1);
    }

    return CELL_OK;
}

s32 cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName)
{
    uint32_t dir_ea = (uint32_t)(uintptr_t)dirName;
    const char* dir = dir_ea ? (const char*)(vm_base + dir_ea) : "<null>";

    const char* what;
    switch (type) {
        case 0:   what = "BROKEN_GAMEDATA";           break;
        case 1:   what = "BROKEN_HDDGAME";            break;
        case 2:   what = "NOSPACE";                   break;
        case 100: what = "BROKEN_EXIT_GAMEDATA";      break;
        case 101: what = "BROKEN_EXIT_HDDGAME";       break;
        case 102: what = "NOSPACE_EXIT";              break;
        default:  what = "<unknown type>";            break;
    }

    printf("[cellGame] *** ContentErrorDialog: %s (type=%d) dir='%s' needSizeKB=%d\n",
           what, type, dir, errNeedSizeKB);
    if (type >= 100)
        printf("[cellGame] *** this is an EXIT dialog -- the title quits after this\n");
    fflush(stdout);

    return CELL_OK;
}
