#ifndef VM_TRACK_H
#define VM_TRACK_H

#include <stddef.h>
#include <stdint.h>
#include "vm_page_map.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vm_page_fn)(void* user, uint32_t page);

#ifdef _WIN32
void vm_track_install(void);
#endif
void vm_track(uint32_t ea, uint32_t size, vm_page_fn changed, void* user);
uint32_t vm_track_drain(vm_page_fn changed, void* user);
int vm_commit_host(const void* host, size_t size);
void vm_io_begin(const void* host, size_t size);
void vm_io_end(const void* host, size_t size);

#ifdef __cplusplus
}
#endif

#endif
