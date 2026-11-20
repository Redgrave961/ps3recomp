#ifndef SPU_RAW_H
#define SPU_RAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_RAW_BASE      0xE0000000u
#define SPU_RAW_STRIDE    0x00100000u
#define SPU_RAW_COUNT     6u
#define SPU_RAW_SPAN      (SPU_RAW_COUNT * SPU_RAW_STRIDE)
#define SPU_RAW_PROB_OFF  0x40000u

#define SPU_RAW_MFC_LSA        0x43004u
#define SPU_RAW_MFC_EAH        0x43008u
#define SPU_RAW_MFC_EAL        0x4300Cu
#define SPU_RAW_MFC_SIZE_TAG   0x43010u
#define SPU_RAW_MFC_CLASS_CMD  0x43014u
#define SPU_RAW_MFC_QSTATUS    0x43104u
#define SPU_RAW_PRXY_QUERYMASK 0x4321Cu
#define SPU_RAW_PRXY_TAGSTATUS 0x4322Cu
#define SPU_RAW_OUT_MBOX       0x44004u
#define SPU_RAW_IN_MBOX        0x4400Cu
#define SPU_RAW_MBOX_STATUS    0x44014u
#define SPU_RAW_RUNCNTL        0x4401Cu
#define SPU_RAW_STATUS         0x44024u
#define SPU_RAW_NPC            0x44034u
#define SPU_RAW_SIG_NOTIFY1    0x5400Cu
#define SPU_RAW_SIG_NOTIFY2    0x5C00Cu

static inline int spu_raw_is_reg(uint32_t ea)
{
    uint32_t off = ea - SPU_RAW_BASE;
    return off < SPU_RAW_SPAN && (off & (SPU_RAW_STRIDE - 1)) >= SPU_RAW_PROB_OFF;
}

void spu_raw_reg_store(uint32_t ea, uint32_t val, int width);

int  spu_raw_reg_load(uint32_t ea, uint32_t* out);
uint8_t* spu_raw_shadow(uint32_t ea);

struct lv2_syscall_table;
void sys_raw_spu_init(struct lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif
