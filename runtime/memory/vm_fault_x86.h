#ifndef VM_FAULT_X86_H
#define VM_FAULT_X86_H

#include <stdint.h>
#include <string.h>

typedef void (*vm_fault_read)(uint32_t ea, int width, uint8_t* out);

static inline int vm_emulate_load(uint64_t* r, uint64_t* rip, const uint8_t* vb, vm_fault_read read)
{
    const uint8_t* p = (const uint8_t*)(uintptr_t)*rip;
    int o16 = 0, rex = 0, width, swap = 0, zx = 0;
    if (*p == 0x66) { o16 = 1; p++; }
    if ((*p & 0xF0) == 0x40) rex = *p++;
    if (p[0] == 0x0F && (p[1] == 0xB6 || p[1] == 0xB7)) { width = p[1] == 0xB6 ? 1 : 2; zx = 1; p += 2; }
    else if (p[0] == 0x8A) { width = 1; p += 1; }
    else if (p[0] == 0x8B) { width = rex & 8 ? 8 : o16 ? 2 : 4; p += 1; }
    else if (p[0] == 0x0F && p[1] == 0x38 && p[2] == 0xF0) { width = rex & 8 ? 8 : o16 ? 2 : 4; swap = 1; p += 3; }
    else return 0;
    const uint8_t modrm = *p++;
    const int mod = modrm >> 6, reg = ((modrm >> 3) & 7) | (rex & 4) << 1;
    uint64_t ea;
    int32_t d;
    if (mod == 3) return 0;
    if ((modrm & 7) == 4) {
        const uint8_t sib = *p++;
        const int idx = ((sib >> 3) & 7) | (rex & 2) << 2, base = (sib & 7) | (rex & 1) << 3;
        ea = idx == 4 ? 0 : r[idx] << (sib >> 6);
        if ((base & 7) == 5 && mod == 0) { memcpy(&d, p, 4); p += 4; ea += (uint64_t)(int64_t)d; }
        else ea += r[base];
    } else if ((modrm & 7) == 5 && mod == 0) {
        return 0;
    } else {
        ea = r[(modrm & 7) | (rex & 1) << 3];
    }
    if (mod == 1) ea += (uint64_t)(int64_t)(int8_t)*p++;
    else if (mod == 2) { memcpy(&d, p, 4); p += 4; ea += (uint64_t)(int64_t)d; }
    const uint64_t g = ea - (uintptr_t)vb;
    if (g + (uint64_t)width > (1ull << 32)) return 0;
    uint8_t b[8];
    read((uint32_t)g, width, b);
    uint64_t x = 0;
    for (int i = 0; i < width; i++) x |= (uint64_t)b[swap ? width - 1 - i : i] << (8 * i);
    if (width == 1 && !zx && !rex && reg >= 4) r[reg - 4] = (r[reg - 4] & ~0xFF00ull) | x << 8;
    else r[reg] = (width < 4 && !zx) ? (r[reg] & ~((1ull << (8 * width)) - 1)) | x : x;
    *rip = (uint64_t)(uintptr_t)p;
    return 1;
}

#endif
