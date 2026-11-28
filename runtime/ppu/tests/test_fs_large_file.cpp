#include "../../../libs/filesystem/ps3_vfs.c"
#include "ppu_fs.cpp"
#ifdef _WIN32
#  include <windows.h>
#  include <winioctl.h>
#endif

extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
unsigned long long ps3_ms_now(void) { return 0; }
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
const char* ppu_vfs_root = ".";
int sys_fs_translate_path(const char* g, char* h, int n)
{
    const char* r = strncmp(g, "/dev_hdd1", 9) == 0 ? g + 9 : g;
    return snprintf(h, (size_t)n, "%s%s%s", ppu_vfs_root, *r == '/' ? "" : "/", r) < n;
}
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
int vm_commit_host(const void*, size_t) { return 1; }
void vm_io_begin(const void*, size_t) {}
void vm_io_end(const void*, size_t) {}
}
#include "../../platform/ps3_dev_flash.c"
#include "../../../libs/filesystem/npdrm.h"
int npdrm_is_npd(ps3_vfile* f) { (void)f; return 0; }
npdrm_file* npdrm_open(ps3_vfile* f) { (void)f; return 0; }
int64_t npdrm_read(npdrm_file* d, uint64_t o, void* p, uint64_t n) { (void)d; (void)o; (void)p; (void)n; return -1; }
uint64_t npdrm_size(const npdrm_file* d) { (void)d; return 0; }
void npdrm_close(npdrm_file* d) { (void)d; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static const uint64_t FILE_SIZE = (5ULL << 30) + 16;
static const uint64_t MARK_OFF  = (5ULL << 30) + 8;

static int make_sparse(const char* path)
{
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL)) { CloseHandle(h); return -2; }
    LARGE_INTEGER at; at.QuadPart = (LONGLONG)MARK_OFF;
    DWORD wr = 0;
    int ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && WriteFile(h, "MARKtail", 8, &wr, NULL) && wr == 8;
    CloseHandle(h);
    return ok ? 0 : -1;
#else
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fseeko(f, (off_t)MARK_OFF, SEEK_SET) == 0 && fwrite("MARKtail", 1, 8, f) == 8;
    return (fclose(f) == 0 && ok) ? 0 : -1;
#endif
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[1 << 16];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : NULL;
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (!dir) { DWORD n = GetTempPathA(sizeof tmp, tmp); if (n && n < sizeof tmp) { tmp[n - 1] = 0; dir = tmp; } }
#endif
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_large_file.bin", dir);
    int mk = make_sparse(host);
    if (mk == -2) { remove(host); printf("SKIP: the filesystem under %s cannot hold a sparse file\n", dir); return 0; }
    if (mk != 0) { remove(host); printf("FAIL: could not create %s\n", host); return 1; }

    ppu_vfs_root = dir;
    const char* gpath = "/dev_hdd1/ps3recomp_fs_large_file.bin";
    strcpy((char*)mem + 0x100, gpath);
    ppu_context ctx;

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0x400;
    cellFsStat(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsStat returns CELL_OK (0x%08X)", (uint32_t)ctx.gpr[3]);
    CHECK(rd64(0x400 + 0x24) == FILE_SIZE, "cellFsStat size 0x%llX (want 0x%llX)",
          (unsigned long long)rd64(0x400 + 0x24), (unsigned long long)FILE_SIZE);

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0; ctx.gpr[5] = 0x500;
    cellFsOpen(&ctx);
    uint32_t fd = rd32(0x500);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] == 0) {
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = MARK_OFF; ctx.gpr[5] = 0; ctx.gpr[6] = 0x508;
        cellFsLseek(&ctx);
        CHECK(rd64(0x508) == MARK_OFF, "cellFsLseek(SET) position 0x%llX (want 0x%llX)",
              (unsigned long long)rd64(0x508), (unsigned long long)MARK_OFF);

        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x600; ctx.gpr[5] = 4; ctx.gpr[6] = 0x510;
        cellFsRead(&ctx);
        CHECK(rd64(0x510) == 4 && memcmp(mem + 0x600, "MARK", 4) == 0, "cellFsRead after the seek reads the marker");

        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0; ctx.gpr[5] = 2; ctx.gpr[6] = 0x518;
        cellFsLseek(&ctx);
        CHECK(rd64(0x518) == FILE_SIZE, "cellFsLseek(END) position 0x%llX (want 0x%llX)",
              (unsigned long long)rd64(0x518), (unsigned long long)FILE_SIZE);

        memset(mem + 0x700, 0, 0x34);
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x700;
        cellFsFstat(&ctx);
        CHECK((uint32_t)ctx.gpr[3] == 0 && rd64(0x700 + 0x24) == FILE_SIZE, "cellFsFstat size 0x%llX (want 0x%llX)",
              (unsigned long long)rd64(0x700 + 0x24), (unsigned long long)FILE_SIZE);

        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
        cellFsClose(&ctx);
    }
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
