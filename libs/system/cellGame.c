/*
 * ps3recomp - cellGame HLE implementation
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#include "cellGame.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32 (guest mem) */
#include "ps3emu/guest_call.h"              /* g_ps3_guest_caller (funcStat dispatch) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/statvfs.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static char s_title_id[64]  = "BLES00000";
static char s_title[256]    = "Unknown Title";
static char s_version[16]   = "01.00";
static char s_system_ver[16] = "00.0000";
static char s_app_ver[16]   = "01.00";

/* Content info / usrdir paths.
 *
 * This is the HOST directory the guest path /dev_hdd0/game maps to, and it has
 * to be the SAME directory ppu_fs.cpp resolves that mount to. cellGame creates
 * the game-data dir and hands the title a guest path; the title then opens files
 * under that path through cellFs. If the two disagree the title writes its game
 * data into one directory and reads it back from another, finds nothing, and
 * reports the data as CORRUPT rather than as missing.
 *
 * LBP does exactly that: cellGameDataCheck saw ./gamedata/.../NPEA00241 -- left
 * over from an earlier run's cellGameCreateGameData, and empty -- answered
 * CELL_OK ("installed"), and the title then read /dev_hdd0/game/NPEA00241/USRDIR,
 * which ppu_fs maps under the VFS root, got nothing, and put up
 * cellGameContentErrorDialog(BROKEN_EXIT_GAMEDATA) and exited.
 *
 * The literal below is only the fallback for a host with no VFS configured;
 * content_root() resolves the real one on first use. */
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
static char s_exit_param[256] = "";

static int  s_boot_checked = 0;
static u32  s_game_type = CELL_GAME_GAMETYPE_DISC;

/* Current content-check session. BootCheck/DataCheck open a session naming
 * some content; ContentPermit reports THAT content's paths and closes it.
 * Disc sessions must yield /dev_bdvd/PS3_GAME -- always returning the
 * game-data dir sent LBP hunting for data.farc on /dev_hdd0 (ENOENT), so
 * every resource load failed and the loader stalled polling its completion
 * semaphore forever. */
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

/* Content may not exist on first boot. Query its nearest existing ancestor
 * so installation checks use the destination volume, without creating data. */
static s32 content_free_kb(void)
{
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s", s_content_path[0] ? s_content_path : ".");
    for (;;) {
#ifdef _WIN32
        ULARGE_INTEGER available;
        if (GetDiskFreeSpaceExA(path, &available, NULL, NULL)) {
            u64 kb = available.QuadPart / 1024;
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

/* hddFreeSizeKB as the guest reads it. Games compute hddFreeSizeKB * 1024
 * in 32 bits to get bytes, so a real free space of 4 GiB or more wraps to
 * whatever is left over modulo 4 GiB: depending on how full the host disk
 * happens to be, the install check sees a few MB and fails with "not enough
 * space" (inFAMOUS 2 did, at boot, with 41.7 GB free). RPCS3 reports 40 GiB
 * - 256 KiB for every one of these calls for that reason (cellGame.cpp,
 * cellHddGameCheck): times 1024 that is 0xFFFC0000, still nearly 4 GiB
 * after the wrap. Here the measured value is kept below 4 GiB, where it
 * cannot wrap and a nearly full disk still reads as one, and RPCS3's figure
 * is reported above it. */
#define CELL_GAME_FREE_KB_WRAP     (4 * 1024 * 1024)        /* x1024 = 2^32 */
#define CELL_GAME_FREE_KB_REPORTED (40 * 1024 * 1024 - 256) /* RPCS3 */
static s32 content_free_kb_reported(void)
{
    const s32 kb = content_free_kb();
    return kb >= CELL_GAME_FREE_KB_WRAP ? CELL_GAME_FREE_KB_REPORTED : kb;
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

/* The game's display name, straight from PARAM.SFO's TITLE. The boot harness
 * uses this for the window caption -- it used to hardcode another title's name,
 * which is confusing the moment you run two ports side by side. */
const char* cellGame_get_title(void)
{
    return s_title;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * PARAM.SFO -> title id (2026-06-21): the ROBUST fix for title-id paths.
 *
 * Root cause of the /dev_hdd0/game/BLES00000 bug: s_title_id was a hardcoded
 * placeholder and cellGame_set_title_id() was never called, so EVERY title-id
 * path (cellGameContentPermit/BootCheck/DataCheck/web + cellDiscGameGetBootDiscInfo)
 * used the wrong id. Fix: read the real id from the game's PARAM.SFO once at boot
 * (main.cpp -> cellGame_init_from_paramsfo) so it's correct for ANY title.
 *
 * SFO format is LITTLE-ENDIAN: header @0 (magic "\0PSF", key_table@0x08,
 * data_table@0x0C, entries@0x10), then entries[0x10 each]: key_off(u16)@0,
 * fmt(u16)@2, data_len(u32)@4, data_max(u32)@8, data_off(u32)@0xC.
 * -----------------------------------------------------------------------*/
static int sfo_read_string(const char* sfo_path, const char* key,
                           char* out, int out_size)
{
    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 0x14 || sz > (1 << 20)) { fclose(fp); return -1; }
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    if (!d) { fclose(fp); return -1; }
    int ok = (fread(d, 1, (size_t)sz, fp) == (size_t)sz);
    fclose(fp);
    if (!ok) { free(d); return -1; }

    int ret = -1;
    if (d[0]==0x00 && d[1]==0x50 && d[2]==0x53 && d[3]==0x46) {  /* "\0PSF" */
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

/* Read title/version fields from the game's PARAM.SFO at boot. Call once
 * from main.cpp before the guest runs. Falls back to the defaults if the SFO
 * can't be read (keeps the game working without it). */
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
    /* CATEGORY decides the type cellGameBootCheck reports: "DG" is a disc game, "HG" an HDD game (a PSN
     * download, booted from /dev_hdd0/game/<id>). Titles check it and give up on a type they do not expect.
     * RPCS3 reports HDD for every category that is not a disc one; only these two are mapped here, any
     * other keeps the default. */
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

/* The title id when the booted title is an HDD game (CATEGORY "HG"), else NULL:
 * its /dev_hdd0/game/<id>/ IS the booted game directory (sys_fs.c). */
const char* cellGame_get_hdd_title_id(void)
{
    return s_game_type == CELL_GAME_GAMETYPE_HDD && s_title_id[0] ? s_title_id : NULL;
}

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName)
{
    printf("[cellGame] BootCheck()\n");

    /* Build paths based on title ID */
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

    /* The HLE bridge passes the GUEST addresses of these out-params (the PPC
     * r3-r6 values), not host pointers. Translate through vm_base and write
     * big-endian via vm_write32 -- dereferencing them directly would fault on
     * the guest stack address (e.g. 0x0FEFFB60). */
    uint32_t type_ea = (uint32_t)(uintptr_t)type;
    uint32_t attr_ea = (uint32_t)(uintptr_t)attributes;
    uint32_t size_ea = (uint32_t)(uintptr_t)size;
    uint32_t dir_ea  = (uint32_t)(uintptr_t)dirName;

    if (type_ea) vm_write32(type_ea, (uint32_t)s_game_type);
    if (attr_ea) vm_write32(attr_ea, s_is_patch ? CELL_GAME_ATTRIBUTE_PATCH : 0);

    if (size_ea) {
        vm_write32(size_ea + 0, (u32)content_free_kb_reported());
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);                           /* sysSizeKB */
    }

    if (dir_ea) {
        size_t len = strlen(s_title_id);
        if (len > CELL_GAME_DIRNAME_SIZE - 1) len = CELL_GAME_DIRNAME_SIZE - 1;
        memcpy(vm_base + dir_ea, s_title_id, len);
        vm_base[dir_ea + len] = '\0';
    }

    s_boot_checked = 1;
    /* Open a check session for the BOOT content (disc titles boot from
     * /dev_bdvd, not from the game-data dir). */
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
        /* Ensure directories exist, including the one cellFs maps the title's
         * game-data USRDIR to: a game that polls cellFsStat for it stalls
         * forever otherwise. */
        ensure_dirs(s_content_info_path);
        ensure_dirs(s_usrdir_path);
        {
            char gpath[CELL_GAME_PATH_MAX], vpath[CELL_GAME_PATH_MAX];
            snprintf(gpath, sizeof gpath, "/dev_hdd0/game/%s/USRDIR", s_check_dir);
            sys_fs_translate_path(gpath, vpath, (int)sizeof vpath);
            ensure_dirs(vpath);
        }
    }

    /* Both paths are GUEST addresses; format into a host temp, copy into the
     * guest buffer through vm_base (a raw snprintf to the guest addr faults).
     * The paths must match the content the current session checked: the disc
     * for BootCheck/DataCheck(DISC), the game-data dir otherwise. */
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
    /* dirName is a GUEST address -- translate BEFORE any use. The old code
     * printf'd the raw EA as a host char* and crashed (LBP: read fault at
     * 0x94AEA0 = the guest EA of its dir string). */
    uint32_t dir_ea = (uint32_t)(uintptr_t)dirName;   /* guest addr */
    const char* check_dir = dir_ea ? (const char*)(vm_base + dir_ea) : s_title_id;
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n", type, check_dir);

    /* Open a check session for this content; the next ContentPermit reports
     * its paths (disc -> /dev_bdvd/PS3_GAME, else the game-data dir). */
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

    /* Disc-content check (type 1 = CELL_GAME_GAMETYPE_DISC): the disc is
     * inherently present (mounted via the VFS root); a NOTFOUND here makes
     * disc titles bail out of boot. */
    if (type == 1)
        return CELL_OK;

    if (dir_exists(path)) {
        return CELL_OK;
    }

    /* Missing game-data dir is NOT an error: firmware (and RPCS3) return
     * CELL_GAME_RET_NONE (2) so the title knows to cellGameCreateGameData.
     * Returning ERROR_NOTFOUND here made LBP treat first boot as fatal and
     * exit(0) politely. sizeKB must read 0 (not NOTCALC) for the no-data case. */
    if (size_ea)
        vm_write32(size_ea + 4, 0);   /* sizeKB = 0 (no data yet) */
    return CELL_GAME_RET_NONE;
}

/* cellGameDataCheckCreate2 (NID 0xC9645C41) -- check/prepare game data, invoking the
 * title's funcStat callback (CellGameDataStatCallback) exactly like firmware so the
 * save/boot state machine advances. Struct layout + behavior cross-referenced against
 * RPCS3 cellGame.cpp. Demon's Souls's DsSaveLoadMan thread calls this; without the
 * callback firing, its game-data check never completes, the sema/SLSession chain
 * stalls, and the cellSaveDataAutoLoad2 (auto-save notice) dialog never appears.
 *
 * funcStat(cbResult, get, set): the title reads `get` (isNewData, free space, params)
 * and writes cbResult->result. We report existing data (isNewData=0) + ample space;
 * DeS's callback returns CELL_GAMEDATA_CBRESULT_OK_CANCEL (check-only) -> CELL_OK. */
#define GAMEDATA_CB_BASE   0x02520000u   /* guest scratch (distinct from savedata's) */
#define CELL_GAMEDATA_CBRESULT_OK_CANCEL  1
#define CELL_GAMEDATA_CBRESULT_OK         0

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize)
{
    printf("[cellGame] GetParamString(id=%d, bufsize=%u)\n", id, bufsize);

    /* `buf` is a GUEST address (raw r4); translate through vm_base. Strings are
     * byte data, so no byte-swap -- just write into the guest buffer. A raw
     * strncpy(buf, ...) faulted on the guest stack address. */
    uint32_t buf_ea = (uint32_t)(uintptr_t)buf;
    if (!buf_ea || bufsize == 0)
        return CELL_GAME_ERROR_PARAM;
    char* hbuf = (char*)(vm_base + buf_ea);

    const char* src;
    if (id >= CELL_GAME_PARAMID_TITLE && id <= CELL_GAME_PARAMID_TITLE_TURKISH) {
        /* Localized TITLE_## fields are not tracked separately yet. */
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

    uint32_t init_ea = (uint32_t)(uintptr_t)init;   /* guest addrs */
    if (!init_ea)
        return CELL_GAME_ERROR_PARAM;

    /* Create the directory the title ASKED for, not s_title_id.
     *
     * The install handshake is: cellGameDataCheck(type=GAMEDATA, dirName) ->
     * CELL_GAME_RET_NONE ("no data yet") -> the title calls
     * cellGameCreateGameData -> it checks again and expects to find it. This
     * created <content>/s_title_id instead, and s_title_id is the placeholder
     * "BLES00000" unless a PARAM.SFO set it -- so DataCheck kept looking for
     * <content>/NPUA80523, never found it, and Tokyo Jungle's installer span
     * DataCheck/CreateGameData forever.
     *
     * s_check_dir is the dirName from the check session that immediately
     * precedes this call, which is exactly what firmware creates. */
    const char* create_dir = s_check_dir[0] ? s_check_dir : s_title_id;

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", content_root(), create_dir);

    ensure_dirs(path);

    /* Also create USRDIR */
    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    /* Host paths are only for host filesystem access. Guest code feeds these
     * results back to cellFs, which resolves PS3 virtual paths through VFS. */
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

/* cellGameContentErrorDialog -- firmware puts a system error message on screen
 * ("the game data is corrupted", "not enough space", ...) and, for the 1xx
 * types, the title exits as soon as it returns.
 *
 * It has to exist even as a stub, and it has to SAY what it was asked to show.
 * LBP calls this as the very last thing its "bringup" thread does before
 * sys_ppu_thread_exit(0): the thread's result feeds main's startup gate, main
 * sees a false, runs its teardown and calls exit(0). Unimplemented, the whole
 * sequence reads as a clean voluntary shutdown with no cause anywhere in the
 * log -- the title looks like it simply finished. The dialog type and dirName
 * are the only place the actual complaint is named, so print them.
 *
 * Returning CELL_OK is correct: firmware returns success for "the dialog was
 * shown". It does not mean the underlying content problem went away. */
s32 cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName)
{
    /* dirName is a GUEST address; translate before use (same as DataCheck). */
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
