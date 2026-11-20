#ifndef PS3RECOMP_PRX_LOADER_H
#define PS3RECOMP_PRX_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t    addr;
    void      (*func)(void* ctx);
    const char* name;
} prx_func_entry;

typedef struct {
    uint32_t nid;
    uint32_t vaddr;
} prx_export;

typedef struct {
    const char*           name;
    uint32_t              base;
    const uint8_t*        image;
    uint32_t              image_size;
    const prx_func_entry* funcs;
    uint64_t              func_count;
    const prx_export*     exports;
    uint32_t              export_count;
} prx_module;

typedef void (*prx_register_fn)(uint32_t guest_addr, void (*host)(void*));

typedef struct {
    int      ok;
    uint32_t funcs_registered;
    uint32_t base;
    uint32_t image_size;
} prx_load_result;

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg);

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size);

uint32_t prx_resolve_export(uint32_t nid);

uint32_t prx_export_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif
