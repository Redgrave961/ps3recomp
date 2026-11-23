#include "cellSaveData.h"
#include "ps3_psf.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#ifdef st_atime
#  undef st_atime
#endif
#ifdef st_mtime
#  undef st_mtime
#endif
#ifdef st_ctime
#  undef st_ctime
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

static char s_save_root[1024] = "./gamedata/dev_hdd0/home/00000001/savedata";
static ps3_savedata_load_cb s_on_save_load = NULL;

void cellSaveData_set_load_callback(ps3_savedata_load_cb cb)
{
    s_on_save_load = cb;
}

static void adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done) return;
    s_done = 1;
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (env && *env)
        snprintf(s_save_root, sizeof(s_save_root), "%s/dev_hdd0/home/00000001/savedata", env);
}

static void ensure_dirs(const char* path)
{
    char tmp[1024];
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

static void build_save_path(char* buf, size_t buf_size, const char* dirName)
{
    adopt_gamedata_root();
    snprintf(buf, buf_size, "%s/%s", s_save_root, dirName);
#ifdef _WIN32
    for (char* p = buf; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif
}

static const char* savedata_host_str(const char* p)
{
    uintptr_t v = (uintptr_t)p;
    if (v && v < 0x100000000ull)
        return (const char*)(vm_base + (uint32_t)v);
    return p;
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

static int dir_has_save(const char* path)
{
    if (!dir_exists(path)) return 0;
    char sfo[1024];
    snprintf(sfo, sizeof(sfo), "%s/PARAM.SFO", path);
    HOST_STAT_T st;
    return HOST_STAT(sfo, &st) == 0;
}

#define SAVEDATA_SCRATCH_BASE  0x024E0000u
#define SAVEDATA_SCRATCH_SIZE  0x00020000u

static uint32_t s_scratch_base = SAVEDATA_SCRATCH_BASE;
static uint32_t s_scratch_size = SAVEDATA_SCRATCH_SIZE;
static uint32_t s_scratch_next = SAVEDATA_SCRATCH_BASE;

static void scratch_reset(void) { s_scratch_next = s_scratch_base; }

static uint32_t scratch_alloc(uint32_t size)
{
    size = (size + 0xF) & ~0xFu;
    if ((u64)s_scratch_next + size > (u64)s_scratch_base + s_scratch_size)
        return 0;
    uint32_t addr = s_scratch_next;
    s_scratch_next += size;
    memset(vm_base + addr, 0, size);
    return addr;
}

static void marshal_cbresult_init(uint32_t addr, s32 result, uint32_t userdata_ea)
{
    vm_write32(addr + 0,  (uint32_t)result);
    vm_write32(addr + 4,  0);
    vm_write32(addr + 8,  0);
    vm_write32(addr + 12, 0);
    vm_write32(addr + 16, userdata_ea);
}

static s32 marshal_cbresult_read_result(uint32_t addr)
{
    return (s32)vm_read32(addr + 0);
}

#define SAVEDATA_STATGET_SIZE   1704u
#define SAVEDATA_STATSET_SIZE   16u
#define SAVEDATA_CBRESULT_SIZE  20u

#define SAVEDATA_FILEGET_SIZE   0x44u
#define SAVEDATA_FILESET_SIZE   0x30u

static s32 dispatch_func_file(uint32_t func_opd, uint32_t* userdata_ea,
                              u32 exc_size, CellSaveDataFileSet* out)
{
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    scratch_reset();
    u32 cb_ea = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    u32 get_ea = scratch_alloc(SAVEDATA_FILEGET_SIZE);
    u32 set_ea = scratch_alloc(SAVEDATA_FILESET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, *userdata_ea);
    vm_write32(get_ea, exc_size);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);
    *userdata_ea = vm_read32(cb_ea + 16);
    memset(out, 0, sizeof(*out));
    out->fileOperation = vm_read32(set_ea);
    out->fileType = vm_read32(set_ea + 8);
    memcpy(out->secureFileId, vm_base + set_ea + 12, 16);
    u32 name = vm_read32(set_ea + 28), buffer = vm_read32(set_ea + 44);
    out->fileName = name ? (char*)vm_base + name : NULL;
    out->fileOffset = vm_read32(set_ea + 32);
    out->fileSize = vm_read32(set_ea + 36);
    out->fileBufSize = vm_read32(set_ea + 40);
    out->fileBuf = buffer ? vm_base + buffer : NULL;
    if (buffer && out->fileBufSize) vm_commit(buffer, out->fileBufSize);
    return marshal_cbresult_read_result(cb_ea);
}

static void marshal_statget_init(uint32_t addr, int is_new, const char* dirName,
                                  s32 sizeKB, u32 fileNum)
{
    vm_write32(addr + 0,    1024 * 1024);
    vm_write32(addr + 4,    is_new ? 1 : 0);
    vm_write64(addr + 8,    0);
    vm_write64(addr + 16,   0);
    vm_write64(addr + 24,   0);
    if (dirName) {
        size_t len = strnlen(dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);
        memcpy(vm_base + addr + 32, dirName, len);
        ((char*)vm_base)[addr + 32 + len] = 0;
    }
    vm_write32(addr + 1616, 0);
    vm_write32(addr + 1620, (uint32_t)sizeKB);
    vm_write32(addr + 1624, 0);
    vm_write32(addr + 1628, fileNum);
    vm_write32(addr + 1632, 0);
    vm_write32(addr + 1636, 0);
}

static s32 dispatch_func_stat_full(uint32_t func_opd, int is_new, const char* dirName,
                                   uint32_t* userdata_ea, const CellSaveDataStatGet* input,
                                   CellSaveDataStatSet* output)
{
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;

    scratch_reset();
    uint32_t cb_ea  = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_STATGET_SIZE);
    uint32_t set_ea = scratch_alloc(SAVEDATA_STATSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) {
        printf("[cellSaveData] scratch alloc failed\n");
        return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    }

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, *userdata_ea);
    marshal_statget_init(get_ea, is_new, dirName, 0, 0);
    if (input) {
        vm_write32(get_ea, (u32)input->hddFreeSizeKB);
        vm_write64(get_ea + 8, (u64)input->dir.st_atime);
        vm_write64(get_ea + 16, (u64)input->dir.st_mtime);
        vm_write64(get_ea + 24, (u64)input->dir.st_ctime);
        memcpy(vm_base + get_ea + 64, &input->getParam, 1552);
        vm_write32(get_ea + 64 + 1280, input->getParam.attribute);
        vm_write32(get_ea + 1616, input->bind);
        vm_write32(get_ea + 1620, (u32)input->sizeKB);
        vm_write32(get_ea + 1624, (u32)input->sysSizeKB);
        vm_write32(get_ea + 1628, input->fileNum);
        vm_write32(get_ea + 1632, input->fileListNum);
        u32 files = input->fileListNum ? scratch_alloc(input->fileListNum * 56) : 0;
        if (input->fileListNum && !files) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
        vm_write32(get_ea + 1636, files);
        for (u32 i = 0; i < input->fileListNum; i++) {
            const CellSaveDataFileStat* f = input->fileList + i;
            u32 dst = files + i * 56;
            vm_write32(dst, f->fileType); vm_write64(dst + 8, f->st_size);
            vm_write64(dst + 16, (u64)f->st_atime);
            vm_write64(dst + 24, (u64)f->st_mtime);
            vm_write64(dst + 32, (u64)f->st_ctime);
            memcpy(vm_base + dst + 40, f->fileName, CELL_SAVEDATA_FILENAME_SIZE);
        }
    }

    printf("[cellSaveData] dispatching funcStat OPD=0x%08X (cb=0x%X get=0x%X set=0x%X, isNew=%d, userdata=0x%08X)\n",
           func_opd, cb_ea, get_ea, set_ea, is_new, *userdata_ea);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);

    *userdata_ea = vm_read32(cb_ea + 16);
    if (output) {
        u32 params = vm_read32(set_ea);
        output->setParam = params ? (CellSaveDataSystemFileParam*)(vm_base + params) : NULL;
        output->reCreateMode = vm_read32(set_ea + 4);
        output->indicator = NULL;
    }
    s32 result = marshal_cbresult_read_result(cb_ea);
    printf("[cellSaveData] funcStat returned cbResult.result=%d\n", result);
    return result;
}

static int savedata_system_file(const char* name)
{
    return !strcmp(name, "PARAM.SFO") || !strcmp(name, "PARAM.PFD");
}

static u32 savedata_content_type(const char* name)
{
    if (!strcmp(name, "ICON0.PNG")) return CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
    if (!strcmp(name, "ICON1.PAM")) return CELL_SAVEDATA_FILETYPE_CONTENT_ICON1;
    if (!strcmp(name, "PIC1.PNG")) return CELL_SAVEDATA_FILETYPE_CONTENT_PIC1;
    if (!strcmp(name, "SND0.AT3")) return CELL_SAVEDATA_FILETYPE_CONTENT_SND0;
    return CELL_SAVEDATA_FILETYPE_NORMALFILE;
}

static void pfd_path(const char* save_path, char* out, size_t n)
{
    snprintf(out, n, "%s/PARAM.PFD", save_path);
#ifdef _WIN32
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
#endif
}

static int pfd_has(const char* save_path, const char* name)
{
    char path[1024], line[64];
    pfd_path(save_path, path, sizeof(path));
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        found = !strcmp(line, name);
    }
    fclose(fp);
    return found;
}

static void pfd_add(const char* save_path, const char* name)
{
    if (pfd_has(save_path, name)) return;
    char path[1024];
    pfd_path(save_path, path, sizeof(path));
    FILE* fp = fopen(path, "ab");
    if (fp) { fprintf(fp, "%s\n", name); fclose(fp); }
}

static u32 savedata_file_type(const char* save_path, const char* name)
{
    u32 t = savedata_content_type(name);
    return t == CELL_SAVEDATA_FILETYPE_NORMALFILE && pfd_has(save_path, name)
         ? CELL_SAVEDATA_FILETYPE_SECUREFILE : t;
}

static u32 enumerate_save_files(const char* save_path,
                                 CellSaveDataFileStat* fileList, u32 max)
{
    u32 count = 0;

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", save_path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || savedata_system_file(fd.cFileName))
                continue;
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = savedata_file_type(save_path, fd.cFileName);
                strncpy(fileList[count].fileName, fd.cFileName,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                fileList[count].st_size = (u64)sz.QuadPart;
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(save_path);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.' || savedata_system_file(de->d_name))
                continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", save_path, de->d_name);
            HOST_STAT_T st;
            if (HOST_STAT(full, &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                continue;
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = savedata_file_type(save_path, de->d_name);
                strncpy(fileList[count].fileName, de->d_name,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                fileList[count].st_size = (u64)st.st_size;
#ifdef _WIN32
                fileList[count].st_atime = (s64)st.st_atime;
                fileList[count].st_mtime = (s64)st.st_mtime;
                fileList[count].st_ctime = (s64)st.st_ctime;
#elif defined(__APPLE__)
                fileList[count].st_atime = (s64)st.st_atimespec.tv_sec;
                fileList[count].st_mtime = (s64)st.st_mtimespec.tv_sec;
                fileList[count].st_ctime = (s64)st.st_ctimespec.tv_sec;
#else
                fileList[count].st_atime = (s64)st.st_atim.tv_sec;
                fileList[count].st_mtime = (s64)st.st_mtim.tv_sec;
                fileList[count].st_ctime = (s64)st.st_ctim.tv_sec;
#endif
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

static s32 process_file_op(const char* save_path, CellSaveDataFileSet* set)
{
    if (!set) return CELL_SAVEDATA_ERROR_PARAM;
    const char* name = set->fileName;
    switch (set->fileType) {
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0: name = "ICON0.PNG"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1: name = "ICON1.PAM"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1: name = "PIC1.PNG"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_SND0: name = "SND0.AT3"; break;
    case CELL_SAVEDATA_FILETYPE_SECUREFILE:
    case CELL_SAVEDATA_FILETYPE_NORMALFILE: break;
    default: return CELL_SAVEDATA_ERROR_PARAM;
    }
    if (!name || !name[0] || strnlen(name, CELL_SAVEDATA_FILENAME_SIZE) == CELL_SAVEDATA_FILENAME_SIZE ||
        strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") || !strcmp(name, "..") ||
        (set->fileOperation != CELL_SAVEDATA_FILEOP_DELETE && !set->fileBuf))
        return CELL_SAVEDATA_ERROR_PARAM;
    char file_path[1024];
    snprintf(file_path, sizeof(file_path), "%s/%s", save_path, name);
#ifdef _WIN32
    for (char* p = file_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    switch (set->fileOperation) {
    case CELL_SAVEDATA_FILEOP_READ: {
        FILE* fp = fopen(file_path, "rb");
        if (!fp) {
            printf("[cellSaveData] file read: cannot open '%s'\n", file_path);
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        if (set->fileOffset > 0) {
#ifdef _WIN32
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t read_size = (size_t)set->fileSize;
        if (read_size > set->fileBufSize) read_size = set->fileBufSize;
        vm_io_begin(set->fileBuf, read_size);
        size_t got = fread(set->fileBuf, 1, read_size, fp);
        vm_io_end(set->fileBuf, read_size);
        int failed = ferror(fp);
        if (fclose(fp) != 0) failed = 1;
        if (!failed && got > 0 && s_on_save_load) s_on_save_load();
        return failed ? CELL_SAVEDATA_ERROR_ACCESS_ERROR : (s32)got;
    }

    case CELL_SAVEDATA_FILEOP_WRITE:
    case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: {
        ensure_dirs(save_path);
        const char* mode;
        if (set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE && set->fileOffset == 0) {
            mode = "wb";
        } else {
            mode = "r+b";
        }
        FILE* fp = fopen(file_path, mode);
        if (!fp) {
            fp = fopen(file_path, "wb");
        }
        if (!fp) {
            printf("[cellSaveData] file write: cannot open '%s'\n", file_path);
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        if (set->fileOffset > 0) {
#ifdef _WIN32
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t write_size = (size_t)set->fileSize;
        if (write_size > set->fileBufSize) write_size = set->fileBufSize;
        size_t wrote = fwrite(set->fileBuf, 1, write_size, fp);
        int failed = wrote != write_size;
        if (fclose(fp) != 0) failed = 1;
        if (!failed && set->fileType == CELL_SAVEDATA_FILETYPE_SECUREFILE)
            pfd_add(save_path, name);
        return failed ? CELL_SAVEDATA_ERROR_ACCESS_ERROR : (s32)wrote;
    }

    case CELL_SAVEDATA_FILEOP_DELETE:
        remove(file_path);
        return 0;

    default:
        return 0;
    }
}

static void write_param_sfo(const char* save_path, const CellSaveDataSystemFileParam* param)
{
    if (!param) return;

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    const char* dir = save_path;
    for (const char* q = save_path; *q; q++)
        if ((*q == '/' || *q == '\\') && q[1]) dir = q + 1;

    char title[CELL_SAVEDATA_SYSP_TITLE_SIZE], sub[CELL_SAVEDATA_SYSP_SUBTITLE_SIZE];
    char detail[CELL_SAVEDATA_SYSP_DETAIL_SIZE], lp[CELL_SAVEDATA_SYSP_LPARAM_SIZE];
    snprintf(title, sizeof title, "%.*s", (int)sizeof title - 1, param->title);
    snprintf(sub, sizeof sub, "%.*s", (int)sizeof sub - 1, param->subTitle);
    snprintf(detail, sizeof detail, "%.*s", (int)sizeof detail - 1, param->detail);
    snprintf(lp, sizeof lp, "%.*s", (int)sizeof lp - 1, param->listParam);

    uint8_t buf[4096];
    size_t n = ps3_psf_build(buf, sizeof buf, dir, title, sub, detail, param->attribute, lp);
    if (!n) return;

    FILE* fp = fopen(sfo_path, "wb");
    if (!fp) return;
    fwrite(buf, 1, n, fp);
    fclose(fp);
}

static void read_param_sfo(const char* save_path, CellSaveDataSystemFileParam* param)
{
    if (!param) return;
    memset(param, 0, sizeof(CellSaveDataSystemFileParam));

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return;

    uint8_t buf[8192];
    size_t got = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    if (got >= 4 && buf[0] == 0 && buf[1] == 'P' && buf[2] == 'S' && buf[3] == 'F') {
        uint32_t attr = 0;
        if (ps3_psf_parse(buf, got, param->title, sizeof param->title, param->subTitle, sizeof param->subTitle,
                          param->detail, sizeof param->detail, &attr, param->listParam, sizeof param->listParam))
            param->attribute = attr;
        return;
    }

    char text[sizeof buf + 1];
    memcpy(text, buf, got); text[got] = 0;
    for (char* line = text; *line; ) {
        char* nl = line; while (*nl && *nl != '\n') nl++;
        char saved = *nl; *nl = 0;
        size_t len = strlen(line);
        while (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        if (strncmp(line, "TITLE=", 6) == 0)
            strncpy(param->title, line + 6, CELL_SAVEDATA_SYSP_TITLE_SIZE - 1);
        else if (strncmp(line, "SUB_TITLE=", 10) == 0)
            strncpy(param->subTitle, line + 10, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE - 1);
        else if (strncmp(line, "DETAIL=", 7) == 0)
            strncpy(param->detail, line + 7, CELL_SAVEDATA_SYSP_DETAIL_SIZE - 1);
        else if (strncmp(line, "ATTRIBUTE=", 10) == 0)
            param->attribute = (u32)atoi(line + 10);
        else if (strncmp(line, "LIST_PARAM=", 11) == 0)
            strncpy(param->listParam, line + 11, CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1);
        if (!saved) break;
        line = nl + 1;
    }
}

static s32 savedata_execute(const char* dirName, int is_save,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            void* userdata)
{
    if (!dirName || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);

    int is_new = !dir_has_save(save_path);

    printf("[cellSaveData] %s dir='%s' (new=%d)\n",
           is_save ? "SAVE" : "LOAD", dirName, is_new);

    u32 file_list_max = setBuf ? vm_read32((u32)(uintptr_t)setBuf + 4) : 64;
    if (file_list_max > CELL_SAVEDATA_LISTITEM_MAX) return CELL_SAVEDATA_ERROR_PARAM;
    CellSaveDataFileStat* fileList = NULL;
    u32 fileNum = 0;

    if (file_list_max > 0) {
        fileList = (CellSaveDataFileStat*)calloc(file_list_max, sizeof(CellSaveDataFileStat));
        if (!fileList) return CELL_SAVEDATA_ERROR_INTERNAL;
        if (!is_new) {
            fileNum = enumerate_save_files(save_path, fileList, file_list_max);
        }
    }

    CellSaveDataStatGet statGet;
    memset(&statGet, 0, sizeof(statGet));
    statGet.hddFreeSizeKB = 1024 * 1024;
    statGet.isNewData = is_new ? 1 : 0;
    strncpy(statGet.dir.dirName, dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);

    if (!is_new) {
        HOST_STAT_T hst;
        if (HOST_STAT(save_path, &hst) == 0) {
#ifdef _WIN32
            statGet.dir.st_atime = (s64)hst.st_atime;
            statGet.dir.st_mtime = (s64)hst.st_mtime;
            statGet.dir.st_ctime = (s64)hst.st_ctime;
#elif defined(__APPLE__)
            statGet.dir.st_atime = (s64)hst.st_atimespec.tv_sec;
            statGet.dir.st_mtime = (s64)hst.st_mtimespec.tv_sec;
            statGet.dir.st_ctime = (s64)hst.st_ctimespec.tv_sec;
#else
            statGet.dir.st_atime = (s64)hst.st_atim.tv_sec;
            statGet.dir.st_mtime = (s64)hst.st_mtim.tv_sec;
            statGet.dir.st_ctime = (s64)hst.st_ctim.tv_sec;
#endif
        }
        read_param_sfo(save_path, &statGet.getParam);
    }

    statGet.fileNum = fileNum;
    statGet.fileListNum = fileNum < file_list_max ? fileNum : file_list_max;
    statGet.fileList = fileList;
    statGet.bind = 0;
    statGet.sizeKB = 0;
    statGet.sysSizeKB = 0;

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataStatSet statSet;
    memset(&statSet, 0, sizeof(statSet));

    u32 callback_userdata = (u32)(uintptr_t)userdata;
    cbResult.result = dispatch_func_stat_full((uint32_t)(uintptr_t)funcStat, is_new,
                         dirName, &callback_userdata, &statGet, &statSet);

    if (cbResult.result < 0) {
        printf("[cellSaveData] stat callback returned error %d\n", cbResult.result);
        free(fileList);
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    if (is_save && statSet.setParam) {
        ensure_dirs(save_path);
        CellSaveDataSystemFileParam params;
        memcpy(&params, statSet.setParam, sizeof(params));
        params.attribute = ps3_bswap32(params.attribute);
        write_param_sfo(save_path, &params);
    }

    if (funcFile && cbResult.result == CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        u32 exc_size = 0;
        while (1) {
            CellSaveDataFileSet fileSet;
            cbResult.result = dispatch_func_file((u32)(uintptr_t)funcFile,
                                                 &callback_userdata, exc_size, &fileSet);
            if (cbResult.result != CELL_SAVEDATA_CBRESULT_OK_NEXT) break;
            s32 exc = process_file_op(save_path, &fileSet);
            printf("[cellSaveData] file op=%u type=%u name='%s' off=%u size=%u buf=%u -> %d\n",
                   fileSet.fileOperation, fileSet.fileType,
                   fileSet.fileName ? fileSet.fileName : "", fileSet.fileOffset,
                   fileSet.fileSize, fileSet.fileBufSize, exc);
            if (exc < 0) { free(fileList); return exc; }
            exc_size = (u32)exc;
        }
    }

    free(fileList);

    if (cbResult.result < 0) {
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    printf("[cellSaveData] %s complete for '%s'\n",
           is_save ? "SAVE" : "LOAD", dirName);
    return CELL_OK;
}

s32 cellSaveDataAutoLoad2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    (void)version; (void)errDialog; (void)setBuf; (void)funcFile;
    (void)container;
    printf("[cellSaveData] AutoLoad2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_has_save(save_path);

    (void)is_new;
    s32 r = savedata_execute(dirName, 0, setBuf, funcStat, funcFile, userdata);
    return r == CELL_SAVEDATA_ERROR_NODATA ? CELL_OK : r;
}

s32 cellSaveDataDelete2(u32 container)
{
    printf("[cellSaveData] Delete2(container=%u)\n", container);

    return CELL_OK;
}

s32 cellSaveDataAutoSave(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    (void)errDialog; (void)funcFile; (void)container;
    printf("[cellSaveData] AutoSave(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_has_save(save_path);

    (void)is_new;
    return savedata_execute(dirName, 1, setBuf, funcStat, funcFile, userdata);
}

s32 cellSaveDataAutoLoad(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    (void)version; (void)errDialog; (void)setBuf; (void)funcFile;
    (void)container;
    printf("[cellSaveData] AutoLoad(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_has_save(save_path);

    (void)is_new;
    s32 r = savedata_execute(dirName, 0, setBuf, funcStat, funcFile, userdata);
    return r == CELL_SAVEDATA_ERROR_NODATA ? CELL_OK : r;
}

#include "../../runtime/ppu/ppu_context.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

#define SAVEDATA_ARG9_SP_OFF 112u

static uint32_t savedata_arg9(ppu_context* ctx)
{
    uint32_t sp = (uint32_t)ctx->gpr[1];
    return (uint32_t)vm_read64(sp + SAVEDATA_ARG9_SP_OFF);
}

static void hle_cellSaveDataUserAutoLoad(ppu_context* ctx)
{
    u32   version   = (u32)ctx->gpr[3];
    u32   userId    = (u32)ctx->gpr[4];
    const char* dir = (const char*)(uintptr_t)(u32)ctx->gpr[5];
    u32   errDialog = (u32)ctx->gpr[6];
    void* setBuf    = (void*)(uintptr_t)(u32)ctx->gpr[7];
    void* funcStat  = (void*)(uintptr_t)(u32)ctx->gpr[8];
    void* funcFile  = (void*)(uintptr_t)(u32)ctx->gpr[9];
    u32   container = (u32)ctx->gpr[10];
    uint32_t userdata = savedata_arg9(ctx);

    printf("[cellSaveData] UserAutoLoad(user=%u, userdata=0x%08X)\n", userId, userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)
        cellSaveDataAutoLoad2(version, dir, errDialog,
                              (CellSaveDataSetBuf*)setBuf,
                              (CellSaveDataStatCallback)funcStat,
                              (CellSaveDataFileCallback)funcFile,
                              container, (void*)(uintptr_t)userdata);
}

static void hle_cellSaveDataUserAutoSave(ppu_context* ctx)
{
    u32   version   = (u32)ctx->gpr[3];
    u32   userId    = (u32)ctx->gpr[4];
    const char* dir = (const char*)(uintptr_t)(u32)ctx->gpr[5];
    u32   errDialog = (u32)ctx->gpr[6];
    void* setBuf    = (void*)(uintptr_t)(u32)ctx->gpr[7];
    void* funcStat  = (void*)(uintptr_t)(u32)ctx->gpr[8];
    void* funcFile  = (void*)(uintptr_t)(u32)ctx->gpr[9];
    u32   container = (u32)ctx->gpr[10];
    uint32_t userdata = savedata_arg9(ctx);

    printf("[cellSaveData] UserAutoSave(user=%u, userdata=0x%08X)\n", userId, userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)
        cellSaveDataAutoSave(version, dir, errDialog,
                             (CellSaveDataSetBuf*)setBuf,
                             (CellSaveDataStatCallback)funcStat,
                             (CellSaveDataFileCallback)funcFile,
                             container, (void*)(uintptr_t)userdata);
}

void cellSaveData_register_ctx_handlers(void)
{
    ps3_hle_register_ctx(0xCDC6AEFDu, "cellSaveDataUserAutoLoad",
                         hle_cellSaveDataUserAutoLoad);
    ps3_hle_register_ctx(0x52AAC4FAu, "cellSaveDataUserAutoSave",
                         hle_cellSaveDataUserAutoSave);
}

s32 cellSaveDataUserAutoLoad(u32 version, u32 userId, const char* dirName,
                             u32 errDialog, CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile, u32 container)
{
    (void)userId;
    printf("[cellSaveData] UserAutoLoad(user=%u) [8-arg path: userdata unavailable]\n", userId);
    return cellSaveDataAutoLoad(version, dirName, errDialog, setBuf,
                                funcStat, funcFile, container, NULL);
}
