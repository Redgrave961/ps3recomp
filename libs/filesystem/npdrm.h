#ifndef PS3RECOMP_NPDRM_H
#define PS3RECOMP_NPDRM_H

#include <stdint.h>

#include "ps3_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NPDRM_ERROR_INVALID_PARAM      0x80029502u
#define NPDRM_ERROR_NO_ENTITLEMENT     0x80029513u
#define NPDRM_ERROR_LICENSE_NOT_FOUND  0x80029521u
#define NPDRM_ERROR_IO                 0x80029525u

typedef struct npdrm_file npdrm_file;

int         npdrm_is_npd(ps3_vfile* f);
npdrm_file* npdrm_open(ps3_vfile* f);
int64_t     npdrm_read(npdrm_file* d, uint64_t offset, void* dst, uint64_t size);
uint64_t    npdrm_size(const npdrm_file* d);
void        npdrm_close(npdrm_file* d);

void        npdrm_add_klicensee(const uint8_t klicensee[16]);
int         npdrm_license_available(const char* host_path, const uint8_t* klicensee);
/* Returns 0 if not NPD, no .rap needed, or unlocked by any .rap in exdata_host_dir; 1 if missing .rap (cid_out filled). */
int         npdrm_check_vfile(ps3_vfile* f, const char* exdata_host_dir, char cid_out[49]);
/* Searches dir for any .rap file (binary or hex, any filename) that cryptographically unlocks f. Returns 1 on match and fills rap_out/cid_out. */
int         npdrm_find_rap_in_dir(ps3_vfile* f, const char* dir, uint8_t rap_out[16], char cid_out[49]);

#ifdef __cplusplus
}
#endif

#endif
