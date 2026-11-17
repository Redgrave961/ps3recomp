#include <stdint.h>

uint32_t ppu_hle_inject_base = 0x20000000u;
int32_t ppu_thread_priority(uint64_t tid) { (void)tid; return 0; }
