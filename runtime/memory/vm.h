#ifndef VM_H
#define VM_H

#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "vm_track.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <signal.h>
  #include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VM_TOTAL_SIZE       0x100000000ull
#define VM_MAIN_MEM_BASE    0x00010000u
#define VM_MAIN_MEM_SIZE    0x10000000u
#define VM_STACK_BASE       0xD0000000u
#define VM_STACK_REGION     0x10000000u

#define VM_PPU_STACK_SIZE   0x00100000u

#define VM_CB_STACK_REGION  0x01000000u
#define VM_CB_STACK_BASE    (VM_STACK_BASE + VM_STACK_REGION - VM_CB_STACK_REGION)
#define VM_CB_STACK_SIZE    0x00040000u

#define VM_PPU_TLS_BASE     0x10F00000u
#define VM_PPU_TLS_SIZE     0x00100000u

#define VM_HLE_HEAP_BASE    0xB0000000u
#define VM_HLE_HEAP_SIZE    0x08000000u
#define VM_AUDIO_BASE       0xB8000000u
#define VM_AUDIO_PORT_SIZE  0x00200000u
#define VM_AUDIO_SIZE       0x01000000u
#define VM_SYS_VM_BASE      0xA0000000u
#define VM_SYS_VM_SIZE      0x10000000u

extern uint32_t ppu_hle_inject_base;
#define VM_HLE_INJECT_BASE  ppu_hle_inject_base
#define VM_HLE_INJECT_SIZE  0x01000000u

#define VM_PAGE_SIZE        0x00001000u

static inline uint32_t vm_host_page_size(void)
{
#ifdef _WIN32
    return VM_PAGE_SIZE;
#else
    static uint32_t s_page = 0;
    if (!s_page) {
        long v = sysconf(_SC_PAGESIZE);
        s_page = (v > 0) ? (uint32_t)v : VM_PAGE_SIZE;
    }
    return s_page;
#endif
}

#define VM_ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

extern uint8_t* vm_base;

#ifndef _WIN32
static inline int vm__mprotect_pages(uint32_t addr, uint32_t size, int prot)
{
    uint64_t pg = vm_host_page_size();
    uint64_t a0 = (uint64_t)addr & ~(pg - 1u);
    uint64_t a1 = ((uint64_t)addr + size + pg - 1u) & ~(pg - 1u);
    return mprotect(vm_base + a0, (size_t)(a1 - a0), prot);
}
#endif

extern uint32_t ppu_vm_size;
#define VM_READY (vm_base != 0 || ppu_vm_size != 0)

static inline void vm_shutdown(void);

static inline int32_t vm_init(void)
{
    if (vm_base != NULL) return CELL_OK;

#ifdef _WIN32
    for (int attempt = 0; attempt < 8 && !vm_base; attempt++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)(VM_TOTAL_SIZE * 2),
                                                MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) break;
        uint8_t* aligned = (uint8_t*)(((uintptr_t)probe + (uintptr_t)VM_TOTAL_SIZE - 1u)
                                      & ~((uintptr_t)VM_TOTAL_SIZE - 1u));
        VirtualFree(probe, 0, MEM_RELEASE);
        vm_base = (uint8_t*)VirtualAlloc(aligned, (SIZE_T)VM_TOTAL_SIZE,
                                         MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!vm_base) return CELL_ENOMEM;

    if (!VirtualAlloc(vm_base + VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE,
                      MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

    if (!VirtualAlloc(vm_base + VM_STACK_BASE, VM_STACK_REGION,
                      MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(vm_base, 0, MEM_RELEASE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

#else
    {
        size_t want = (size_t)VM_TOTAL_SIZE;
        uint8_t* raw = (uint8_t*)mmap(NULL, want * 2, PROT_NONE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                      -1, 0);
        if (raw == MAP_FAILED) {
            vm_base = NULL;
            return CELL_ENOMEM;
        }
        uint8_t* aligned = (uint8_t*)(((uintptr_t)raw + (uintptr_t)want - 1u)
                                      & ~((uintptr_t)want - 1u));
        if (aligned > raw)
            munmap(raw, (size_t)(aligned - raw));
        if (raw + want * 2 > aligned + want)
            munmap(aligned + want, (size_t)((raw + want * 2) - (aligned + want)));
        vm_base = aligned;
    }

    if (mprotect(vm_base + VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(vm_base, (size_t)VM_TOTAL_SIZE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }

    if (mprotect(vm_base + VM_STACK_BASE, VM_STACK_REGION,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(vm_base, (size_t)VM_TOTAL_SIZE);
        vm_base = NULL;
        return CELL_ENOMEM;
    }
#endif

    assert(((uintptr_t)vm_base & ((uintptr_t)VM_TOTAL_SIZE - 1u)) == 0);
    if (((uintptr_t)vm_base & ((uintptr_t)VM_TOTAL_SIZE - 1u)) != 0) {
        vm_shutdown();
        return CELL_ENOMEM;
    }

    memset(vm_base + VM_MAIN_MEM_BASE, 0, VM_MAIN_MEM_SIZE);

    return CELL_OK;
}

static inline void vm_shutdown(void)
{
    if (!vm_base) return;

#ifdef _WIN32
    VirtualFree(vm_base, 0, MEM_RELEASE);
#else
    munmap(vm_base, (size_t)VM_TOTAL_SIZE);
#endif

    vm_base = NULL;
}

static inline int32_t vm_commit(uint32_t addr, uint32_t size)
{
    if (!VM_READY) return CELL_EFAULT;
    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

#ifdef _WIN32
    if (!vm_commit_host(vm_base + addr, size))
        return CELL_ENOMEM;
#else
    if (vm__mprotect_pages(addr, size, PROT_READ | PROT_WRITE) != 0)
        return CELL_ENOMEM;
#endif

    return CELL_OK;
}

static inline int32_t vm_protect(uint32_t addr, uint32_t size, int read, int write, int exec)
{
    if (!VM_READY) return CELL_EFAULT;
    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

#ifdef _WIN32
    DWORD prot = PAGE_NOACCESS;
    if (read && write && exec) prot = PAGE_EXECUTE_READWRITE;
    else if (read && write)    prot = PAGE_READWRITE;
    else if (read && exec)     prot = PAGE_EXECUTE_READ;
    else if (read)             prot = PAGE_READONLY;
    else if (exec)             prot = PAGE_EXECUTE;

    DWORD old;
    if (!VirtualProtect(vm_base + addr, size, prot, &old))
        return CELL_EFAULT;
#else
    int prot = PROT_NONE;
    if (read)  prot |= PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec)  prot |= PROT_EXEC;

    if (vm__mprotect_pages(addr, size, prot) != 0)
        return CELL_EFAULT;
#endif

    return CELL_OK;
}

typedef struct vm_stack_alloc {
    uint32_t next_addr;
    uint32_t region_end;
} vm_stack_alloc;

static inline void vm_stack_alloc_init(vm_stack_alloc* sa)
{
    sa->next_addr  = VM_STACK_BASE;
    sa->region_end = VM_CB_STACK_BASE;
}

static inline uint32_t vm_stack_allocate(vm_stack_alloc* sa, uint32_t stack_size)
{
    uint32_t pg = vm_host_page_size();
    stack_size = VM_ALIGN_UP(stack_size, pg);

    uint32_t total = stack_size + pg;

    if (sa->next_addr + total > sa->region_end)
        return 0;

    uint32_t base = sa->next_addr;
    sa->next_addr += total;

    vm_protect(base, pg, 0, 0, 0);

    return base + pg;
}

static inline void* vm_to_host(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

#ifdef __cplusplus
}
#endif

#endif
