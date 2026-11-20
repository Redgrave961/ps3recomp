#ifndef SPU_WORKLOAD_H
#define SPU_WORKLOAD_H

#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_entry_fn)(spu_context*);

uint64_t spu_workload_fingerprint(const void* data, size_t n);

void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name);

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint);

spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out);

void spu_workload_register_img_masked(uint64_t fingerprint, spu_lifted_entry_fn fn,
                                      int image_id, const char* name,
                                      uint32_t size, uint32_t var_off,
                                      uint32_t var_len);

spu_lifted_entry_fn spu_workload_find_img_masked(const void* data, size_t n,
                                                 uint64_t* fp_out,
                                                 int* image_id_out);

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out);

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail);

unsigned spu_workload_count(void);

#ifdef __cplusplus
}
#endif

#endif
