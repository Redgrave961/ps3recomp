#ifndef SYS_MEMORY_H
#define SYS_MEMORY_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../memory/vm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_MEMORY_PAGE_SIZE_1M   0x400

#define SYS_MEM_USER_TOTAL        (213u * 1024u * 1024u)
#define SYS_MEMORY_ALLOC_MAX      (SYS_MEM_USER_TOTAL / 0x10000u)
#define SYS_MEMORY_CONTAINER_MAX  16
#define SYS_MMAPPER_SHARED_MAX    64

typedef struct sys_mem_alloc_info {
    uint32_t addr;
    uint32_t size;
    int32_t  container_id;
    uint32_t page_size;
} sys_mem_alloc_info;

typedef struct sys_mem_container_info {
    int      active;
    uint32_t total_size;
    uint32_t used_size;
} sys_mem_container_info;

typedef struct sys_mmapper_shared_info {
    int      active;
    uint32_t size;
    uint32_t addr;
    uint64_t key;
} sys_mmapper_shared_info;

extern sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
extern sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
extern sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];
extern uint32_t                 g_sys_mem_bump_ptr;

int64_t sys_memory_allocate(ppu_context* ctx);
int64_t sys_memory_free(ppu_context* ctx);
int64_t sys_memory_get_user_memory_size(ppu_context* ctx);
int64_t sys_memory_get_page_attribute(ppu_context* ctx);
int64_t sys_memory_container_create(ppu_context* ctx);
int64_t sys_memory_container_destroy(ppu_context* ctx);
int64_t sys_memory_container_get_size(ppu_context* ctx);
int64_t sys_memory_allocate_from_container(ppu_context* ctx);
int64_t sys_mmapper_allocate_address(ppu_context* ctx);
int64_t sys_mmapper_search_map_address(uint32_t start, uint32_t size, uint32_t* out);
int64_t sys_mmapper_free_address(ppu_context* ctx);
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx);
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx);

void sys_memory_set_image_query(uint64_t (*query)(uint64_t addr, uint64_t end));

void sys_memory_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
