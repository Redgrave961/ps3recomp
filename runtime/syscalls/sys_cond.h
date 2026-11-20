#ifndef SYS_COND_H
#define SYS_COND_H

#include "lv2_syscall_table.h"
#include "sys_mutex.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../platform/ps3_sync.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_COND_MAX  8192

typedef struct sys_cond_info {
    ps3_lock        lock;
    int             active;
    uint32_t        mutex_id;
    sys_mutex_info* mutex;
    char            name[8];
    ps3_waitq       q;
} sys_cond_info;

extern sys_cond_info g_sys_conds[SYS_COND_MAX];

int64_t sys_cond_create(ppu_context* ctx);
int64_t sys_cond_destroy(ppu_context* ctx);
int64_t sys_cond_wait(ppu_context* ctx);
int64_t sys_cond_signal(ppu_context* ctx);
int64_t sys_cond_signal_all(ppu_context* ctx);
int64_t sys_cond_signal_to(ppu_context* ctx);

void sys_cond_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
