#ifndef PS3EMU_SPU_FALLBACK_H
#define PS3EMU_SPU_FALLBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-thread runner for an SPU thread.
 *   tid       — the synthesized SPU thread id assigned by sys_spu_thread_initialize
 *   args_ea   — guest EA of the args block passed via sys_spu_thread_set_argument
 *   args_size — size in bytes of the args block (or 0 if unknown)
 *   user      — opaque pointer chosen at group_start
 *
 * The handler runs on a host thread spawned by sys_spu_thread_group_start.
 * Return value goes into the SPU thread's exit status (read by
 * sys_spu_thread_get_exit_status / reported by sys_spu_thread_group_join).
 */
typedef int32_t (*spu_ppu_fallback_fn)(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user);

/*
 * Get a pointer to a SPU thread's virtual local store (256 KB). Lazily
 * allocated on first read/write. Returns NULL if tid is unknown.
 */
uint8_t* spu_thread_get_local_store(uint32_t tid);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_SPU_FALLBACK_H */
