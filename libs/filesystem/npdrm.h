#ifndef PS3RECOMP_NPDRM_H
#define PS3RECOMP_NPDRM_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPDRM_ERROR_INVALID_PARAM      0x80029502u
#define NPDRM_ERROR_NO_ENTITLEMENT     0x80029513u
#define NPDRM_ERROR_BAD_FORMAT         0x80029515u
#define NPDRM_ERROR_LICENSE_NOT_FOUND  0x80029521u
#define NPDRM_ERROR_IO                 0x80029525u

typedef struct npdrm_file npdrm_file;

int         npdrm_is_npd(FILE* f);
npdrm_file* npdrm_open(FILE* f, const char* file_name);
int64_t     npdrm_read(npdrm_file* d, uint64_t offset, void* dst, uint64_t size);
uint64_t    npdrm_size(const npdrm_file* d);
void        npdrm_close(npdrm_file* d);

void        npdrm_add_klicensee(const uint8_t klicensee[16]);
int         npdrm_license_available(const char* host_path, const uint8_t* klicensee);

#ifdef __cplusplus
}
#endif

#endif
