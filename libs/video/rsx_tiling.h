#ifndef RSX_TILING_H
#define RSX_TILING_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_TILE_COUNT 15u
#define RSX_TILE_NO_ADDRESS 0xFFFFFFFFu

typedef struct rsx_tile {
    u32 offset;
    u32 size;
    u32 pitch;
    u32 bank;
} rsx_tile;

static inline u32 rsx_tile_address(const rsx_tile* t, u32 lin)
{
    const u32 pitch = t->pitch;
    const u32 cols = pitch >> 8;
    if ((pitch & 0xffu) || cols < 2u || cols > 0x100u || lin < t->offset) return RSX_TILE_NO_ADDRESS;
    u32 shift = 0;
    while (!(cols >> shift & 1u)) shift++;
    switch (cols >> shift) {
    case 1: case 3: case 5: case 7: case 13: break;
    default: return RSX_TILE_NO_ADDRESS;
    }

    const u32 a = lin - t->offset;
    const u32 x = a % pitch, y = a / pitch;
    const u32 ix = lin & 0xffu, iy = y & 0x3fu;
    const u32 bx = x >> 8, by = y >> 6;
    const u32 base = t->offset >> 14;
    const u32 block = base + by * cols + bx;

    const u32 col = base + bx;
    u32 bank = shift ? ((col >> 1 & 1u) | (col << 1 & 2u)) + by : block;
    bank += t->bank;

    const u32 in = (ix & 0x1fu)
        | (iy & 3u) << 5
        | ((iy >> 2 ^ ix >> 6) & 1u) << 7
        | (ix >> 5 & 1u) << 8
        | ((ix >> 6 ^ iy >> 4 ^ block) & 1u) << 9
        | ((ix >> 7 ^ iy >> 3) & 1u) << 10
        | (iy >> 3) << 11;
    const u32 r = in | ((block & ~3u) | (bank & 3u)) << 14;
    return (r - t->offset < t->size) ? r : RSX_TILE_NO_ADDRESS;
}

void rsx_tiling_set(u32 index, u32 location, u32 offset, u32 size, u32 pitch, u32 bank);

int rsx_tiling_active(void);

int rsx_tiling_find(u32 location, u32 offset, u32 len, rsx_tile* out);

typedef struct rsx_tiled_mem {
    u8* (*ptr)(void* user, u32 location, u32 offset, u32 len);
    void (*store)(void* user, u8* dst, const u8* src, u32 len);
    void* user;
} rsx_tiled_mem;

int rsx_tiled_read(const rsx_tiled_mem* mem, u32 location, u32 offset, void* dst, u32 len);
int rsx_tiled_write(const rsx_tiled_mem* mem, u32 location, u32 offset, const void* src, u32 len);

#ifdef __cplusplus
}
#endif

#endif
