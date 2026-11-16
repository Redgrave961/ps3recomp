/*
 * ps3recomp - sysPrxForUser HLE
 *
 * Core PRX runtime: process management, printf and the CRT string shims.
 */

#ifndef PS3RECOMP_SYS_PRX_FOR_USER_H
#define PS3RECOMP_SYS_PRX_FOR_USER_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

void sys_process_exit(s32 exitcode);
s32 sys_process_getpid(void);
s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags);

/* ---------------------------------------------------------------------------
 * Printf / sprintf / snprintf
 * -----------------------------------------------------------------------*/

s32 _sys_printf(const char* fmt, ...);
s32 _sys_spu_printf_initialize(u32 agcb, u32 dgcb, u32 atcb, u32 dtcb);
s32 _sys_spu_printf_finalize(void);
s32 _sys_spu_printf_attach_group(u32 group);
s32 _sys_spu_printf_detach_group(u32 group);
/* NID 0x9FB6228E -- the abort half of the SDK internal assertion macro. */
s32 _sys_trap_process(const char* msg);
s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...);
s32 _sys_strlen(const char* str);
s32 _sys_strncpy(char* dst, const char* src, u32 size);
s32 _sys_strcat(char* dst, const char* src);
void* _sys_memset(void* dst, s32 val, u32 size);
void* _sys_memcpy(void* dst, const void* src, u32 size);
s32 _sys_memcmp(const void* s1, const void* s2, u32 size);

/* ---------------------------------------------------------------------------
 * PRX utilities
 * -----------------------------------------------------------------------*/

s32 sys_prx_exitspawn_with_level(void);
s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id);

/* ---------------------------------------------------------------------------
 * Process info
 * -----------------------------------------------------------------------*/

s32 sys_process_get_paramsfo(void* buf);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SYS_PRX_FOR_USER_H */
