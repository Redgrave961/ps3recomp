#ifndef SYS_PPU_THREAD_H
#define SYS_PPU_THREAD_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../memory/vm.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sched.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PPU_THREAD_MAX  64

#define PPU_THREAD_STATE_FREE       0
#define PPU_THREAD_STATE_RUNNING    1
#define PPU_THREAD_STATE_FINISHED   2
#define PPU_THREAD_STATE_DETACHED   3

typedef void (*ppu_thread_entry_fn)(ppu_context* ctx);

typedef struct ppu_thread_info {
    int          state;
    ppu_context  ctx;
    char         name[64];
    int32_t      priority;
    int64_t      exit_status;
    int          joinable;
    int          joining;
    uint32_t     stack_addr;
    uint32_t     stack_size;
    uint64_t     entry_addr;
    int          blocked;
} ppu_thread_info;

extern ppu_thread_info g_ppu_threads[PPU_THREAD_MAX];

extern vm_stack_alloc g_vm_stack_alloc;

extern ppu_thread_entry_fn g_ppu_thread_entry_trampoline;

extern uint32_t g_ppu_main_stack_addr;
extern uint32_t g_ppu_main_stack_size;

int64_t sys_ppu_thread_create(ppu_context* ctx);

uint64_t ppu_thread_register_main(void);
ppu_context* ppu_thread_adopt(const char* name, uint32_t stack_size, int32_t priority);

void ppu_thread_note_block(uint64_t tid);
int32_t ppu_thread_priority(uint64_t tid);
int ppu_thread_exists(uint64_t tid);
uint64_t ppu_thread_register_main_priority(int32_t priority);
int64_t sys_ppu_thread_exit(ppu_context* ctx);
int64_t sys_ppu_thread_join(ppu_context* ctx);
int64_t sys_ppu_thread_detach(ppu_context* ctx);
int64_t sys_ppu_thread_yield(ppu_context* ctx);
int64_t sys_ppu_thread_get_priority(ppu_context* ctx);
int64_t sys_ppu_thread_set_priority(ppu_context* ctx);
int64_t sys_ppu_thread_rename(ppu_context* ctx);
int64_t sys_ppu_thread_get_join_state(ppu_context* ctx);
int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx);

void sys_ppu_thread_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
