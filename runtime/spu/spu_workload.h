#ifndef SPU_WORKLOAD_H
#define SPU_WORKLOAD_H

#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* FNV-1a 64-bit over a byte range. The canonical workload fingerprint; the
 * offline extractor computes the same value so registrations and the images a
 * title loads line up. */
uint64_t spu_workload_fingerprint(const void* data, size_t n);

/* Register one lifted SPU binary under its image fingerprint. `name` is borrowed
 * (kept by pointer for logging — pass a string literal / static). Idempotent on
 * fingerprint: a second register of the same fp updates the entry. */
void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name);

/* As spu_workload_register, but binds the image's indirect-branch table id
 * (the value passed to spu_begin_image() before this image's recomp_register).
 * A lookup sets the run context's image_id to this so spu_indirect_branch
 * resolves within the correct image when several share LS addresses. */
void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name);

/* Look up a lifted entry by fingerprint; NULL if none registered. */
spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint);

/* As spu_workload_find, but also returns the image's indirect-branch table id. */
spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out);

/* Register an image whose loaded bytes include an area the loading code fills
 * in on every load: a parameter block of pointers and per-level values that
 * the PPU writes into the program before it DMAs it into a raw SPU. Its bytes
 * differ from load to load, so a plain fingerprint of the loaded run never
 * repeats. `fingerprint` is FNV-1a-64 over the `size` bytes with
 * [var_off, var_off + var_len) taken as zero -- for an image cut out of an ELF
 * whose parameter block is zero there, simply the fingerprint of those bytes.
 * spu_workload_find_img_masked() matches it against a run of exactly `size`
 * bytes, whatever the area holds. */
void spu_workload_register_img_masked(uint64_t fingerprint, spu_lifted_entry_fn fn,
                                      int image_id, const char* name,
                                      uint32_t size, uint32_t var_off,
                                      uint32_t var_len);

/* Look `data` (n bytes) up among the masked registrations of that size: the
 * entry whose fingerprint matches `data` with its variable area zeroed. Writes
 * that fingerprint to *fp_out and the image id to *image_id_out on a match;
 * NULL when none matches. */
spu_lifted_entry_fn spu_workload_find_img_masked(const void* data, size_t n,
                                                 uint64_t* fp_out,
                                                 int* image_id_out);

/* Load a 32-bit big-endian SPU ELF image into a 256 KB local store: each PT_LOAD
 * segment is copied to its p_vaddr (BSS zero-filled). Returns 1 on success and
 * writes the entry vaddr (e_entry, which may legitimately be 0) to *entry_out;
 * returns 0 if `image` is not a valid SPU ELF or a segment is out of range.
 * `ls` must point to SPU_LS_SIZE bytes (caller-zeroed if a clean BSS is wanted). */
int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out);

/* Compute the true on-disk size of a 32-bit BE SPU ELF: the extent that spans
 * the section-header table and all program/section content (max p_offset+p_filesz
 * and non-NOBITS sh_offset+sh_size). This matches the offline extractor's sizing,
 * so fingerprints agree. Use it when a caller is handed an SPU ELF pointer without an explicit length. `max_avail` bounds the read in
 * case the header is corrupt; returns 0 if `image` is not a valid SPU ELF. */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail);

/* Number of currently registered lifted SPU binaries (diagnostics/tests). */
unsigned spu_workload_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_WORKLOAD_H */
