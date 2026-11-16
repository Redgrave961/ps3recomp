/* spu_lifted_fallback.c — lv2 glue: run a LIFTED SPU thread image as a PPU-fallback. */
#include "../spu/spu_lifted_job.h"
#include "../spu/spu_workload.h"
#include <stdint.h>

/* defined in lv2_register.c — the SPU thread's 256 KB local store */
extern uint8_t* spu_thread_get_local_store(uint32_t tid);
extern uint32_t spu_thread_get_group_id(uint32_t tid);
extern uint32_t spu_thread_take_pending_inmbox(uint32_t tid);

/* The lifted entry is found at run time by CONTENT FINGERPRINT rather than by
 * image entry point. Two registries existed and the raw sys_spu_thread_group
 * path only ever consulted the entry-point one, so a title that starts a plain
 * SPU thread group reported "no fallback" with its image lifted and registered
 * the whole time.
 *
 * `user` carries the fingerprint. A raw SPU thread is entered with its
 * argument EA in r3. */
int32_t spu_registry_fallback(uint32_t tid, uint32_t args_ea,
                              uint32_t args_size, void* user)
{
    (void)args_size;
    int image_id = 0;
    spu_lifted_entry_fn fn =
        spu_workload_find_img((uint64_t)(uintptr_t)user, &image_id);
    if (!fn) return -1;
    /* Raw SPU thread: it is a persistent worker. Hand it any command the PPU
     * wrote with sys_spu_thread_write_spu_mb, and identify the context so its
     * reply can be routed back to the connected event queue. */
    spu_run_opts opts;
    opts.inmbox_val    = spu_thread_take_pending_inmbox(tid);
    opts.park_on_empty = 0;
    opts.spu_id        = tid;
    opts.group_id      = spu_thread_get_group_id(tid);
    return spu_run_lifted_job_abi(fn, spu_thread_get_local_store(tid), args_ea,
                                  image_id, &opts);
}
