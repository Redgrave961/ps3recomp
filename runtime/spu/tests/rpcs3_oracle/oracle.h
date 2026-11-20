#ifndef SPU_ORACLE_H
#define SPU_ORACLE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int ok;
    uint8_t gpr[128][16];
    uint32_t next_pc;
    int halted, stopped; uint32_t stop_code;
    int ch_ops; uint32_t ch_no, ch_out;
    int mem; uint32_t mem_addr; uint8_t mem_quad[16];
} oracle_result;
void rpcs3_oracle_set_ls(const uint8_t* ls_256k);
void rpcs3_oracle_poke(uint32_t addr, const uint8_t* p, uint32_t n);
int  rpcs3_oracle_run(uint32_t insn, uint32_t pc, const uint8_t gpr[128][16], uint32_t srr0,
                      uint32_t ch_in, uint32_t ch_cnt, uint32_t events, oracle_result* out);
#ifdef __cplusplus
}
#endif
#endif
