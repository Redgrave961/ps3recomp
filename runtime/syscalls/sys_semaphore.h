#ifndef SYS_SEMAPHORE_H
#define SYS_SEMAPHORE_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../platform/ps3_sync.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_SEMAPHORE_MAX  256

typedef struct sys_semaphore_info {
    ps3_lock         lock;
    int              active;
    char             name[8];
    uint32_t         protocol;
    volatile int32_t value;
    int32_t          max_value;
    ps3_waitq        q;
} __attribute__((aligned(64))) sys_semaphore_info;

extern sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

int64_t sys_semaphore_create(ppu_context* ctx);
int64_t sys_semaphore_destroy(ppu_context* ctx);
int64_t sys_semaphore_wait(ppu_context* ctx);
int64_t sys_semaphore_trywait(ppu_context* ctx);
int64_t sys_semaphore_post(ppu_context* ctx);
int64_t sys_semaphore_get_value(ppu_context* ctx);

void sys_semaphore_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
