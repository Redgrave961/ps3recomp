#include "ps3_self.h"
#include "ps3_aes.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t NP_KLIC_FREE[16] = { 0x72,0xF9,0x90,0x78,0x8F,0x9C,0xFF,0x74,0x57,0x25,0xF0,0x8E,0x4C,0x12,0x83,0x87 };
static const uint8_t NP_KLIC_KEY[16] = { 0xF2,0xFB,0xCA,0x7A,0x75,0xB0,0x4E,0xDC,0x13,0x90,0x63,0x8C,0xCD,0xFD,0xD1,0xEE };

static const struct {
    uint16_t revision;
    uint8_t erk[32];
    uint8_t riv[16];
} NPDRM_KEYS[] = {
    { 0x0010,
      { 0x4B,0x3C,0xD1,0x0F,0x6A,0x6A,0xA7,0xD9,0x9F,0x9B,0x3A,0x66,0x0C,0x35,0xAD,0xE0,
        0x8E,0xF0,0x1C,0x2C,0x33,0x6B,0x9E,0x46,0xD1,0xBB,0x56,0x78,0xB4,0x26,0x1A,0x61 },
      { 0xC0,0xF2,0xAB,0x86,0xE6,0xE0,0x45,0x75,0x52,0xDB,0x50,0xD7,0x21,0x93,0x71,0xC5 } },
};

enum { SELF_TYPE_NPDRM = 8, LICENSE_FREE = 3, CTRL_NPDRM = 3, PT_SEGMENT = 2, ENC_AES_CTR = 3, COMP_NONE = 1 };

static int in_range(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size - off; }

PS3_AES_TARGET int ps3_self_decrypt(const uint8_t* s, size_t size, uint8_t** elf, size_t* elf_size)
{
    *elf = NULL;
    *elf_size = 0;
    if (size < 0x70 || memcmp(s, "SCE", 4) || ps3_be16(s + 0x0A) != 1) return PS3_SELF_EFORMAT;
    const uint16_t revision = ps3_be16(s + 0x08);
    const uint64_t meta = 0x20 + (uint64_t)ps3_be32(s + 0x0C), header_len = ps3_be64(s + 0x10);
    const uint64_t app_off = ps3_be64(s + 0x28), elf_off = ps3_be64(s + 0x30), phdr_off = ps3_be64(s + 0x38);
    const uint64_t shdr_off = ps3_be64(s + 0x40), ctrl_off = ps3_be64(s + 0x58), ctrl_size = ps3_be64(s + 0x60);
    if (!in_range(app_off, 0x20, size) || !in_range(elf_off, 0x40, size) || !in_range(ctrl_off, ctrl_size, size) ||
        header_len > size || meta + 0x40 > header_len)
        return PS3_SELF_EFORMAT;
    if (ps3_be32(s + app_off + 0x0C) != SELF_TYPE_NPDRM) return PS3_SELF_EKEY;

    const uint8_t* npd = NULL;
    for (uint64_t pos = ctrl_off; pos + 8 <= ctrl_off + ctrl_size;) {
        const uint32_t type = ps3_be32(s + pos), len = ps3_be32(s + pos + 4);
        if (len < 8 || pos + len > ctrl_off + ctrl_size) return PS3_SELF_EFORMAT;
        if (type == CTRL_NPDRM && len >= 0x10 + 0x10) npd = s + pos + 0x10;
        pos += len;
    }
    if (!npd || memcmp(npd, "NPD", 4)) return PS3_SELF_EFORMAT;
    if ((ps3_be32(npd + 8) & 3) != LICENSE_FREE) return PS3_SELF_ELICENSE;

    int k = -1;
    for (int i = 0; i < (int)(sizeof NPDRM_KEYS / sizeof NPDRM_KEYS[0]); i++)
        if (NPDRM_KEYS[i].revision == revision) k = i;
    if (k < 0) return PS3_SELF_EKEY;

    static const uint8_t zero[16];
    ps3_aes_key key;
    uint8_t klic[16], info[0x40];
    ps3_aes128_init(&key, NP_KLIC_KEY);
    ps3_aes_ecb_dec(&key, NP_KLIC_FREE, klic);
    ps3_aes128_init(&key, klic);
    ps3_aes_cbc_dec(&key, zero, s + meta, info, sizeof info);
    ps3_aes256_init(&key, NPDRM_KEYS[k].erk);
    ps3_aes_cbc_dec(&key, NPDRM_KEYS[k].riv, info, info, sizeof info);
    if (memcmp(info + 0x10, zero, 16) || memcmp(info + 0x30, zero, 16)) return PS3_SELF_EKEY;

    const size_t hlen = (size_t)(header_len - (meta + 0x40));
    uint8_t* h = (uint8_t*)malloc(hlen);
    if (!h) return PS3_SELF_EFORMAT;
    ps3_aes128_init(&key, info);
    ps3_aes_ctr(&key, info + 0x20, 0, s + meta + 0x40, h, hlen);
    const uint32_t sections = hlen >= 0x20 ? ps3_be32(h + 0x0C) : 0, keys = hlen >= 0x20 ? ps3_be32(h + 0x10) : 0;
    const uint64_t key_base = 0x20 + (uint64_t)sections * 0x30;
    const uint8_t* ehdr = s + elf_off;
    const uint64_t e_phoff = ps3_be64(ehdr + 0x20), e_shoff = ps3_be64(ehdr + 0x28);
    const uint16_t e_phnum = ps3_be16(ehdr + 0x38), e_shentsize = ps3_be16(ehdr + 0x3A), e_shnum = ps3_be16(ehdr + 0x3C);
    int rc = !sections || !in_range(key_base, (uint64_t)keys * 16, hlen) || !in_range(phdr_off, (uint64_t)e_phnum * 0x38, size) ||
             (e_shnum && !in_range(shdr_off, (uint64_t)e_shnum * e_shentsize, size)) ? PS3_SELF_EFORMAT : PS3_SELF_OK;
    uint64_t out_size = e_shnum ? e_shoff + (uint64_t)e_shnum * e_shentsize : 0;
    if (e_phoff + (uint64_t)e_phnum * 0x38 > out_size) out_size = e_phoff + (uint64_t)e_phnum * 0x38;
    for (uint16_t i = 0; !rc && i < e_phnum; i++) {
        const uint8_t* ph = s + phdr_off + (uint64_t)i * 0x38;
        if (ps3_be64(ph + 8) + ps3_be64(ph + 32) > out_size) out_size = ps3_be64(ph + 8) + ps3_be64(ph + 32);
    }
    uint8_t* out = !rc && out_size <= ((uint64_t)1 << 32) ? (uint8_t*)calloc(1, (size_t)out_size) : NULL;
    if (!out) rc = PS3_SELF_EFORMAT;
    if (!rc) {
        memcpy(out, ehdr, 0x40);
        memcpy(out + e_phoff, s + phdr_off, (size_t)e_phnum * 0x38);
        if (e_shnum) memcpy(out + e_shoff, s + shdr_off, (size_t)e_shnum * e_shentsize);
    }
    for (uint32_t i = 0; !rc && i < sections; i++) {
        const uint8_t* sec = h + 0x20 + (uint64_t)i * 0x30;
        const uint64_t off = ps3_be64(sec), len = ps3_be64(sec + 8);
        const uint32_t type = ps3_be32(sec + 0x10), prog = ps3_be32(sec + 0x14), enc = ps3_be32(sec + 0x20);
        const uint32_t key_idx = ps3_be32(sec + 0x24), iv_idx = ps3_be32(sec + 0x28), comp = ps3_be32(sec + 0x2C);
        if (type != PT_SEGMENT) continue;
        if (prog >= e_phnum || comp != COMP_NONE || !in_range(off, len, size)) { rc = PS3_SELF_EFORMAT; break; }
        const uint8_t* ph = s + phdr_off + (uint64_t)prog * 0x38;
        const uint64_t dst = ps3_be64(ph + 8);
        if (len > ps3_be64(ph + 32) || !in_range(dst, len, out_size)) { rc = PS3_SELF_EFORMAT; break; }
        if (enc != ENC_AES_CTR) {
            memcpy(out + dst, s + off, (size_t)len);
            continue;
        }
        if (key_idx >= keys || iv_idx >= keys) { rc = PS3_SELF_EFORMAT; break; }
        ps3_aes128_init(&key, h + key_base + (uint64_t)key_idx * 16);
        ps3_aes_ctr(&key, h + key_base + (uint64_t)iv_idx * 16, 0, s + off, out + dst, (size_t)len);
    }
    free(h);
    if (rc) {
        free(out);
        return rc;
    }
    *elf = out;
    *elf_size = (size_t)out_size;
    return PS3_SELF_OK;
}
