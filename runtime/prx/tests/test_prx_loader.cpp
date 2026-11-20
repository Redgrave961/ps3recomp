#include "ppu_recomp.h"
#include "prx_loader.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <windows.h>

extern "C" const func_entry libsre_function_table[];
extern "C" const uint64_t    libsre_function_table_count;

extern "C" const prx_export libsre_exports[];
extern "C" const uint32_t    libsre_export_count;

extern "C" uint8_t* vm_base = nullptr;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;

static inline uint64_t guest(uint64_t a) { return (uint64_t)(uintptr_t)(vm_base + a); }
extern "C" uint8_t  vm_read8 (uint64_t a){ return *(uint8_t*) (vm_base + a); }
extern "C" uint16_t vm_read16(uint64_t a){ uint16_t v; memcpy(&v, vm_base+a, 2); return v; }
extern "C" uint32_t vm_read32(uint64_t a){ uint32_t v; memcpy(&v, vm_base+a, 4); return v; }
extern "C" uint64_t vm_read64(uint64_t a){ uint64_t v; memcpy(&v, vm_base+a, 8); return v; }
extern "C" void vm_write8 (uint64_t a, uint8_t  v){ *(uint8_t*)(vm_base+a)=v; }
extern "C" void vm_write16(uint64_t a, uint16_t v){ memcpy(vm_base+a,&v,2); }
extern "C" void vm_write32(uint64_t a, uint32_t v){ memcpy(vm_base+a,&v,4); }
extern "C" void vm_write64(uint64_t a, uint64_t v){ memcpy(vm_base+a,&v,8); }
extern "C" void lv2_syscall(ppu_context*) {}
extern "C" void ps3_indirect_call(ppu_context*) {}

struct Reg { uint32_t addr; void (*fn)(void*); };
static Reg  g_reg[4096];
static int  g_reg_n = 0;
static void mock_register(uint32_t addr, void (*fn)(void*)) {
    if (g_reg_n < (int)(sizeof(g_reg)/sizeof(g_reg[0])))
        g_reg[g_reg_n++] = { addr, fn };
}
static void (*reg_lookup(uint32_t addr))(void*) {
    for (int i = 0; i < g_reg_n; i++) if (g_reg[i].addr == addr) return g_reg[i].fn;
    return nullptr;
}

int main(int argc, char** argv) {
    const char* img_path = argc > 1 ? argv[1]
                                    : "D:/recomp/ps3games/flow/prx/libsre.linked.bin";
    const uint32_t BASE = 0x30000000;

    const size_t span = (size_t)BASE + 0x00100000;
    vm_base = (uint8_t*)VirtualAlloc(nullptr, span, MEM_RESERVE, PAGE_NOACCESS);
    if (!vm_base) { fprintf(stderr, "VirtualAlloc reserve failed\n"); return 2; }
    if (!VirtualAlloc(vm_base + BASE, 0x00080000, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "VirtualAlloc commit failed\n"); return 2;
    }

    uint32_t img_size = 0;
    uint8_t* img = prx_image_load_file(img_path, &img_size);
    if (!img) { fprintf(stderr, "could not load %s\n", img_path); return 2; }
    printf("  loaded image: %u bytes from %s\n", img_size, img_path);

    prx_module m;
    memset(&m, 0, sizeof(m));
    m.name       = "libsre";
    m.base       = BASE;
    m.image      = img;
    m.image_size = img_size;
    m.funcs        = (const prx_func_entry*)libsre_function_table;
    m.func_count   = libsre_function_table_count;
    m.exports      = libsre_exports;
    m.export_count = libsre_export_count;

    prx_load_result r = prx_load_module(&m, mock_register);

    int ok = 1;

    printf("  [LOAD      ] ok=%d base=0x%08X size=%u                 %s\n",
           r.ok, r.base, r.image_size, r.ok ? "OK" : "FAIL");
    ok &= r.ok;

    uint32_t raw = vm_read32(BASE + 0x1DB38);
    uint32_t got = _byteswap_ulong(raw);
    uint32_t exp = BASE + 0x1DCA4;
    int img_ok = (got == exp);
    printf("  [IMAGE@0x30...1DB38] = 0x%08X  expect 0x%08X          %s\n",
           got, exp, img_ok ? "OK" : "FAIL");
    ok &= img_ok;

    int count_ok = (r.funcs_registered == libsre_function_table_count) &&
                   (g_reg_n == (int)libsre_function_table_count);
    printf("  [REGISTER  ] registered=%u table=%llu mock=%d           %s\n",
           r.funcs_registered,
           (unsigned long long)libsre_function_table_count, g_reg_n,
           count_ok ? "OK" : "FAIL");
    ok &= count_ok;

    uint32_t f0_addr = (uint32_t)libsre_function_table[0].addr;
    void (*f0_tbl)(void*) = (void(*)(void*))libsre_function_table[0].func;
    void (*f0_reg)(void*) = reg_lookup(f0_addr);
    int map_ok = (f0_reg != nullptr) && (f0_reg == f0_tbl) &&
                 (f0_addr == BASE);
    printf("  [DISPATCH  ] 0x%08X -> %p (table %p)                %s\n",
           f0_addr, (void*)f0_reg, (void*)f0_tbl, map_ok ? "OK" : "FAIL");
    ok &= map_ok;

    const uint32_t NID_cellSpursInitialize = 0xACFC8DBC;
    uint32_t opd = prx_resolve_export(NID_cellSpursInitialize);
    uint32_t want_opd = BASE + 0x03127C;
    int reg_count_ok = (prx_export_registry_count() == libsre_export_count);
    int resolve_ok = (opd == want_opd);
    printf("  [EXPORT    ] registry=%u expect=%u                       %s\n",
           prx_export_registry_count(), libsre_export_count,
           reg_count_ok ? "OK" : "FAIL");
    printf("  [RESOLVE   ] cellSpursInitialize NID 0x%08X -> 0x%08X  %s\n",
           NID_cellSpursInitialize, opd, resolve_ok ? "OK" : "FAIL");
    ok &= reg_count_ok & resolve_ok;

    if (resolve_ok) {
        uint32_t code = _byteswap_ulong(vm_read32(opd));
        void (*fn)(void*) = reg_lookup(code);
        int opd_ok = (fn != nullptr);
        printf("  [OPD WALK  ] *0x%08X = code 0x%08X -> host %p          %s\n",
               opd, code, (void*)fn, opd_ok ? "OK" : "FAIL");
        ok &= opd_ok;
    }

    printf("\n  %s\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    free(img);
    return ok ? 0 : 1;
}
