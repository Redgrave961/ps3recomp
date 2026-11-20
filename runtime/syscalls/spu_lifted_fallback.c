#include "../spu/spu_lifted_job.h"
#include "../spu/spu_workload.h"
#include <stdint.h>

extern uint8_t* spu_thread_get_local_store(uint32_t tid);
extern uint32_t spu_thread_get_group_id(uint32_t tid);
extern uint32_t spu_thread_take_pending_inmbox(uint32_t tid);

int32_t spu_registry_fallback(uint32_t tid, uint32_t args_ea,
                              uint32_t args_size, void* user)
{
    (void)args_size;
    int image_id = 0;
    spu_lifted_entry_fn fn =
        spu_workload_find_img((uint64_t)(uintptr_t)user, &image_id);
    if (!fn) return -1;
    spu_run_opts opts;
    opts.inmbox_val    = spu_thread_take_pending_inmbox(tid);
    opts.park_on_empty = 0;
    opts.spu_id        = tid;
    opts.group_id      = spu_thread_get_group_id(tid);
    return spu_run_lifted_job_abi(fn, spu_thread_get_local_store(tid), args_ea,
                                  image_id, &opts);
}
