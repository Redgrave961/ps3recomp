#ifndef SYS_VM_H
#define SYS_VM_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../memory/vm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_VM_MEMORY_MAP            300
#define SYS_VM_UNMAP                 301
#define SYS_VM_APPEND_MEMORY         302
#define SYS_VM_RETURN_MEMORY         303
#define SYS_VM_LOCK                  304
#define SYS_VM_UNLOCK                305
#define SYS_VM_TOUCH                 306
#define SYS_VM_FLUSH                 307
#define SYS_VM_INVALIDATE            308
#define SYS_VM_STORE                 309
#define SYS_VM_SYNC                  310
#define SYS_VM_MEMORY_MAP_DIFFERENT  313

#define SYS_VM_REGION_BASE  VM_SYS_VM_BASE
#define SYS_VM_REGION_END   (VM_SYS_VM_BASE + VM_SYS_VM_SIZE)

#define SYS_VM_VSIZE_ALIGN  0x02000000u
#define SYS_VM_PSIZE_ALIGN  0x00010000u

#define SYS_VM_MAX  8

typedef struct sys_vm_map_info {
    int      active;
    uint32_t addr;
    uint32_t vsize;
    uint32_t psize;
} sys_vm_map_info;

extern sys_vm_map_info g_sys_vm_maps[SYS_VM_MAX];

int64_t sys_vm_memory_map(ppu_context* ctx);
int64_t sys_vm_unmap(ppu_context* ctx);
int64_t sys_vm_append_memory(ppu_context* ctx);
int64_t sys_vm_return_memory(ppu_context* ctx);
int64_t sys_vm_advisory_nop(ppu_context* ctx);

void sys_vm_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
