#ifndef SPU_LIFTED_THREAD_H
#define SPU_LIFTED_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

typedef struct spu_lifted_thread_desc {
    uint32_t tid;
    uint32_t group_id;
    uint32_t entry;
    uint32_t img_ea;
    uint64_t args[4];
    int      image_id;
} spu_lifted_thread_desc;

typedef struct spu_lifted_thread_result {
    int32_t exit_status;
    int     group_exit;
    int32_t group_status;
    int     faulted;
} spu_lifted_thread_result;

int spu_lifted_thread_available(uint32_t entry);

void spu_lifted_thread_setup(struct spu_context* ctx,
                             const spu_lifted_thread_desc* d);

void spu_lifted_thread_run(struct spu_context* ctx,
                           spu_lifted_thread_result* out);

#ifdef __cplusplus
}
#endif

#endif
