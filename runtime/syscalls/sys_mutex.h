/*
 * ps3recomp - Kernel mutex syscalls
 */

#ifndef SYS_MUTEX_H
#define SYS_MUTEX_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../platform/ps3_sync.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_MUTEX_MAX  8192

#define SYS_SYNC_FIFO            0x1
#define SYS_SYNC_PRIORITY        0x2
#define SYS_SYNC_PRIORITY_INHERIT 0x3

#define SYS_SYNC_RECURSIVE       0x10
#define SYS_SYNC_NOT_RECURSIVE   0x20

#define SYS_MUTEX_WAITERS        0x80000000u

typedef struct sys_mutex_attribute_t {
    uint32_t protocol;
    uint32_t recursive;
    uint32_t pshared;
    uint32_t adaptive;
    uint64_t key;
    int32_t  flags;
    uint32_t pad;
    char     name[8];
} sys_mutex_attribute_t;

typedef struct sys_mutex_info {
    ps3_lock          lock;
    volatile uint32_t owner;
    uint32_t          lock_count;
    uint32_t          cond_count;
    uint32_t          protocol;
    int               recursive;
    int               active;
    char              name[8];
    ps3_waitq         q;
} sys_mutex_info;

extern sys_mutex_info g_sys_mutexes[SYS_MUTEX_MAX];

sys_mutex_info* lv2_mutex_get(uint32_t id);
static inline uint32_t lv2_mutex_owner(const sys_mutex_info* m)
{
    return __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) & ~SYS_MUTEX_WAITERS;
}
void    lv2_mutex_release(sys_mutex_info* m, uint32_t self);
int     lv2_mutex_try_own(sys_mutex_info* m, ps3_waiter* w);
int64_t lv2_mutex_sleep(sys_mutex_info* m, ps3_waiter* w, uint64_t deadline_ns);

int64_t sys_mutex_create(ppu_context* ctx);
int64_t sys_mutex_destroy(ppu_context* ctx);
int64_t sys_mutex_lock(ppu_context* ctx);
int64_t sys_mutex_trylock(ppu_context* ctx);
int64_t sys_mutex_unlock(ppu_context* ctx);

void sys_mutex_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_MUTEX_H */
