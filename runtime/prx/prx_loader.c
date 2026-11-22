#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;

typedef struct { uint32_t nid; uint32_t guest_addr; } prx_export_reg_entry;
#define PRX_EXPORT_REG_CAP 4096u
#define PRX_EXPORT_REG_MASK (PRX_EXPORT_REG_CAP - 1u)
static prx_export_reg_entry s_export_reg[PRX_EXPORT_REG_CAP];
static uint32_t             s_export_reg_n = 0;

static inline uint32_t prx_export_slot(uint32_t nid)
{
    return (nid * 2654435761u) >> 20;
}

static void prx_export_register(uint32_t nid, uint32_t guest_addr)
{
    if (!nid) return;
    uint32_t i = prx_export_slot(nid);
    for (uint32_t probe = 0; probe < PRX_EXPORT_REG_CAP; probe++, i = (i + 1u) & PRX_EXPORT_REG_MASK) {
        if (s_export_reg[i].nid == nid) {
            s_export_reg[i].guest_addr = guest_addr;
            return;
        }
        if (s_export_reg[i].nid == 0) {
            s_export_reg[i] = (prx_export_reg_entry){ nid, guest_addr };
            s_export_reg_n++;
            return;
        }
    }
    fprintf(stderr, "[prx] export registry full, dropping NID 0x%08X\n", nid);
}

uint32_t prx_resolve_export(uint32_t nid)
{
    if (!nid || !s_export_reg_n) return 0;
    uint32_t i = prx_export_slot(nid);
    for (uint32_t probe = 0; probe < PRX_EXPORT_REG_CAP; probe++, i = (i + 1u) & PRX_EXPORT_REG_MASK) {
        const uint32_t k = s_export_reg[i].nid;
        if (!k) return 0;
        if (k == nid) return s_export_reg[i].guest_addr;
    }
    return 0;
}

uint32_t prx_export_registry_count(void) { return s_export_reg_n; }

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg)
{
    prx_load_result r;
    memset(&r, 0, sizeof(r));

    if (!m || !m->image || m->image_size == 0) {
        fprintf(stderr, "[prx] load failed: null/empty module\n");
        return r;
    }
    if (!vm_base) {
        fprintf(stderr, "[prx] load failed: vm_base not initialized "
                        "(call vm_init before loading PRX modules)\n");
        return r;
    }

    r.base       = m->base;
    r.image_size = m->image_size;

    memcpy(vm_base + m->base, m->image, m->image_size);

    if (reg) {
        for (uint64_t i = 0; i < m->func_count; i++) {
            const prx_func_entry* e = &m->funcs[i];
            if (!e->func)
                continue;
            reg((uint32_t)e->addr, e->func);
            r.funcs_registered++;
        }
    }

    uint32_t exports_published = 0;
    if (m->exports) {
        for (uint32_t i = 0; i < m->export_count; i++) {
            const prx_export* e = &m->exports[i];
            if (e->nid == 0)
                continue;
            prx_export_register(e->nid, m->base + e->vaddr);
            exports_published++;
        }
    }

    r.ok = 1;
    fprintf(stderr,
            "[prx] loaded %-12s base=0x%08X size=%u (0x%X) funcs=%u/%llu exports=%u\n",
            m->name ? m->name : "?", m->base, m->image_size, m->image_size,
            r.funcs_registered, (unsigned long long)m->func_count,
            exports_published);
    return r;
}

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[prx] cannot open image '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[prx] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (uint32_t)n;
    return buf;
}
