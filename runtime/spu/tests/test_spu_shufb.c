#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#if !defined(__SSE4_1__) && !defined(__ARM_NEON)
    puts("test_spu_shufb: built without SSE4.1 or NEON, nothing to compare");
    return 0;
#else
    srand(12345);
    for (int it = 0; it < 200000; it++) {
        u128 a, b, c;
        for (int i = 0; i < 16; i++) {
            a._u8[i] = (uint8_t)rand(); b._u8[i] = (uint8_t)rand();
            c._u8[i] = (uint8_t)rand();
        }
        u128 x = spu_shufb(a, b, c), y = spu_shufb_ref(a, b, c);
        if (memcmp(&x, &y, 16)) {
            printf("test_spu_shufb: MISMATCH at iteration %d\n", it);
            return 1;
        }
    }
    puts("test_spu_shufb: ok (200000 random cases)");
    return 0;
#endif
}
