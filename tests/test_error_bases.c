#include <stdio.h>
#include <stdlib.h>
#include "ps3emu/error_codes.h"

static int fails;
#define EXPECT(name, value) do { if ((uint32_t)(name) != (uint32_t)(value)) { \
    fprintf(stderr, "FAIL: %s = 0x%08X, want 0x%08X\n", #name, (unsigned)(uint32_t)(name), (unsigned)(value)); fails++; } } while (0)

int main(void)
{
    EXPECT(CELL_GCM_ERROR_NO_IO_PAGE_TABLE,  0x80210001u);
    EXPECT(CELL_GCM_ERROR_INVALID_ALIGNMENT, 0x80210004u);
    EXPECT(CELL_GCM_ERROR_FAILURE,           0x802100FFu);
    EXPECT(CELL_AUDIO_ERROR_ALREADY_INIT,    0x80310701u);
    EXPECT(CELL_PAD_ERROR_FATAL,             0x80121101u);
    if (fails) return 1;
    puts("PASS: GCM, audio and pad error bases");
    return 0;
}
