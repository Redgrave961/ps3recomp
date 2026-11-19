#include "npdrm.h"

#include <stdlib.h>
#include <string.h>
#include "ps3_aes.h"


#define FLAG_COMPRESSED    0x00000001u
#define FLAG_PLAIN         0x00000002u
#define FLAG_ENCRYPTED_KEY 0x00000008u
#define FLAG_0x10          0x00000010u
#define FLAG_0x20          0x00000020u
#define FLAG_SDAT          0x01000000u
#define FLAG_DEBUG         0x80000000u

static const uint8_t RAP_KEY[16] = { 0x86,0x9F,0x77,0x45,0xC1,0x3F,0xD8,0x90,0xCC,0xF2,0x91,0x88,0xE3,0xCC,0x3E,0xDF };
static const uint8_t RAP_PBOX[16] = { 0x0C,0x03,0x06,0x04,0x01,0x0B,0x0F,0x08,0x02,0x07,0x00,0x05,0x0A,0x0E,0x0D,0x09 };
static const uint8_t RAP_E1[16] = { 0xA9,0x3E,0x1F,0xD6,0x7C,0x55,0xA3,0x29,0xB7,0x5F,0xDD,0xA6,0x2A,0x95,0xC7,0xA5 };
static const uint8_t RAP_E2[16] = { 0x67,0xD4,0x5D,0xA3,0x29,0x6D,0x00,0x6A,0x4E,0x7C,0x53,0x7B,0xF5,0x53,0x8C,0x74 };
static const uint8_t EDAT_KEY[2][16] = {
    { 0xBE,0x95,0x9C,0xA8,0x30,0x8D,0xEF,0xA2,0xE5,0xE1,0x80,0xC6,0x37,0x12,0xA9,0xAE },
    { 0x4C,0xA9,0xC1,0x4B,0x01,0xC9,0x53,0x09,0x96,0x9B,0xEC,0x68,0xAA,0x0B,0xC0,0x81 } };
static const uint8_t NP_OMAC_KEY_2[16] = { 0x6B,0xA5,0x29,0x76,0xEF,0xDA,0x16,0xEF,0x3C,0x33,0x9F,0xB2,0x97,0x1E,0x25,0x6B };
static const uint8_t SDAT_KEY[16] = { 0x0D,0x65,0x5E,0xF8,0xE6,0x74,0xA9,0x8A,0xB8,0x50,0x5C,0xFA,0x7D,0x01,0x29,0x33 };
static const uint8_t KLIC_BUILTIN[4][16] = {
    { 0x72,0xF9,0x90,0x78,0x8F,0x9C,0xFF,0x74,0x57,0x25,0xF0,0x8E,0x4C,0x12,0x83,0x87 },
    { 0x52,0xC0,0xB5,0xCA,0x76,0xD6,0x13,0x4B,0xB4,0x5F,0xC6,0x6C,0xA6,0x37,0xF2,0xC1 },
    { 0x2A,0x6A,0xFB,0xCF,0x43,0xD1,0x57,0x9F,0x7D,0x73,0x87,0x41,0xA1,0x3B,0xD4,0x2E },
    { 0x0D,0xB8,0x57,0x32,0x36,0x6C,0xD7,0x34,0xFC,0x87,0x9E,0x74,0x33,0x43,0xBB,0x4F } };

PS3_AES_TARGET static void aes_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    ps3_aes_key k;
    ps3_aes128_init(&k, key);
    ps3_aes_ecb_enc(&k, in, out);
}

PS3_AES_TARGET static void aes_cbc_dec(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n)
{
    ps3_aes_key k;
    ps3_aes128_init(&k, key);
    ps3_aes_cbc_dec(&k, iv, in, out, n);
}

static void dbl(uint8_t b[16])
{
    const uint8_t carry = b[0] >> 7;
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
    b[15] = (uint8_t)(b[15] << 1) ^ (carry ? 0x87 : 0);
}

PS3_AES_TARGET static void aes_cmac(const uint8_t key[16], const uint8_t* m, size_t n, uint8_t mac[16])
{
    ps3_aes_key k;
    ps3_aes128_init(&k, key);
    uint8_t sub[16] = { 0 }, last[16] = { 0 };
    _mm_storeu_si128((__m128i*)sub, ps3_aes_enc(&k, _mm_setzero_si128()));
    dbl(sub);
    const size_t blocks = n ? (n + 15) / 16 : 1;
    const int full = n && n % 16 == 0;
    if (!full) dbl(sub);
    __m128i x = _mm_setzero_si128();
    for (size_t i = 0; i + 1 < blocks; i++) x = ps3_aes_enc(&k, _mm_xor_si128(x, _mm_loadu_si128((const __m128i*)(m + i * 16))));
    const size_t rem = n - (blocks - 1) * 16;
    memcpy(last, m + (blocks - 1) * 16, rem);
    if (!full) last[rem] = 0x80;
    for (int i = 0; i < 16; i++) last[i] ^= sub[i];
    _mm_storeu_si128((__m128i*)mac, ps3_aes_enc(&k, _mm_xor_si128(x, _mm_loadu_si128((const __m128i*)last))));
}

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol(uint32_t v, int c) { return v << c | v >> (32 - c); }

static void sha1_block(sha1_ctx* c, const uint8_t* p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        const uint32_t t = i < 20 ? ((b & d) | (~b & e)) + 0x5A827999u
                         : i < 40 ? (b ^ d ^ e) + 0x6ED9EBA1u
                         : i < 60 ? ((b & d) | (b & e) | (d & e)) + 0x8F1BBCDCu
                                  : (b ^ d ^ e) + 0xCA62C1D6u;
        const uint32_t tmp = rol(a, 5) + t + f + w[i];
        f = e; e = d; d = rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1_init(sha1_ctx* c)
{
    static const uint32_t iv[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1_ctx* c, const uint8_t* p, size_t n)
{
    c->len += n;
    if (c->n) {
        const size_t take = n < 64 - c->n ? n : 64 - c->n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n < 64) return;
        sha1_block(c, c->buf);
        c->n = 0;
    }
    for (; n >= 64; p += 64, n -= 64) sha1_block(c, p);
    memcpy(c->buf, p, n);
    c->n = n;
}

static void sha1_final(sha1_ctx* c, uint8_t out[20])
{
    const uint64_t bits = c->len * 8;
    static const uint8_t pad[64] = { 0x80 };
    sha1_update(c, pad, c->n < 56 ? 56 - c->n : 120 - c->n);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) { out[i * 4] = (uint8_t)(c->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
                                  out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); out[i * 4 + 3] = (uint8_t)c->h[i]; }
}

static void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* m, size_t n, uint8_t out[20])
{
    uint8_t ipad[64], opad[64], inner[20];
    for (int i = 0; i < 64; i++) {
        const uint8_t k = i < (int)key_len ? key[i] : 0;
        ipad[i] = k ^ 0x36;
        opad[i] = k ^ 0x5C;
    }
    sha1_ctx c;
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, m, n); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint8_t s_klic[16][16];
static volatile int s_klic_n;

void npdrm_add_klicensee(const uint8_t k[16])
{
    for (int i = 0; i < s_klic_n; i++) if (!memcmp(s_klic[i], k, 16)) return;
    if (s_klic_n < 16) { memcpy(s_klic[s_klic_n], k, 16); __atomic_add_fetch(&s_klic_n, 1, __ATOMIC_RELEASE); }
}

static void rap_to_rif(const uint8_t rap[16], uint8_t key[16])
{
    static const uint8_t zero[16];
    aes_cbc_dec(RAP_KEY, zero, rap, key, 16);
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 16; i++) key[RAP_PBOX[i]] ^= RAP_E1[RAP_PBOX[i]];
        for (int i = 15; i >= 1; i--) key[RAP_PBOX[i]] ^= key[RAP_PBOX[i - 1]];
        int o = 0;
        for (int i = 0; i < 16; i++) {
            const int p = RAP_PBOX[i];
            const uint8_t kc = (uint8_t)(key[p] - o), ec = RAP_E2[p];
            if (o != 1 || kc != 0xFF) { o = kc < ec; key[p] = (uint8_t)(kc - ec); }
            else key[p] = kc;
        }
    }
}

void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

static int rap_key(const uint8_t* content_id, uint8_t key[16])
{
    char cid[49], guest[96], path[1100];
    memcpy(cid, content_id, 48);
    cid[48] = 0;
    snprintf(guest, sizeof guest, "/dev_hdd0/home/00000001/exdata/%s.rap", cid);
    sys_fs_translate_path(guest, path, (int)sizeof path);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t rap[16];
    const int ok = fread(rap, 1, 16, f) == 16;
    fclose(f);
    if (ok) rap_to_rif(rap, key);
    return ok;
}

static int klic_matches(const uint8_t* hdr, const uint8_t k[16])
{
    uint8_t ck[16], mac[16];
    for (int i = 0; i < 16; i++) ck[i] = k[i] ^ NP_OMAC_KEY_2[i];
    aes_cmac(ck, hdr, 0x60, mac);
    return !memcmp(mac, hdr + 0x60, 16);
}

struct npdrm_file {
    ps3_vfile* f;
    uint8_t hdr[0x100];
    uint8_t key[16];
    uint32_t flags, block_size, blocks, version;
    uint64_t size;
    int64_t cached;
    uint32_t cached_len;
    uint8_t* plain;
    uint8_t* enc;
};

static int64_t load_block(npdrm_file* d, uint32_t b)
{
    if (d->cached == b) return d->cached_len;
    const int lead = (d->flags & FLAG_0x20) != 0;
    const uint32_t meta = lead ? 0x20u : 0x10u;
    const uint64_t meta_off = lead ? 0x100 + (uint64_t)b * (meta + d->block_size) : 0x100 + (uint64_t)b * meta;
    const uint64_t data_off = lead ? meta_off + meta : 0x100 + (uint64_t)d->blocks * meta + (uint64_t)b * d->block_size;
    uint32_t len = d->block_size;
    if (b + 1 == d->blocks && d->size % d->block_size) len = (uint32_t)(d->size % d->block_size);
    const uint32_t padded = (len + 15u) & ~15u;
    uint8_t m[0x20], expect[20];
    if (ps3_vfile_seek(d->f, (int64_t)meta_off, SEEK_SET) || ps3_vfile_read(m, meta, d->f) != meta) return -1;
    if (lead) { for (int i = 0; i < 16; i++) expect[i] = m[i] ^ m[i + 16]; memcpy(expect + 16, m + 16, 4); }
    else memcpy(expect, m, 16);
    if (ps3_vfile_seek(d->f, (int64_t)data_off, SEEK_SET) || ps3_vfile_read(d->enc, padded, d->f) != padded) return -1;
    if (d->flags & FLAG_DEBUG) {
        memcpy(d->plain, d->enc, len);
    } else {
        uint8_t bk[16] = { 0 }, ck[16], hk[20] = { 0 }, mac[20];
        if (d->version > 1) memcpy(bk, d->hdr + 0x60, 12);
        bk[12] = (uint8_t)(b >> 24); bk[13] = (uint8_t)(b >> 16); bk[14] = (uint8_t)(b >> 8); bk[15] = (uint8_t)b;
        aes_ecb(d->key, bk, ck);
        if (d->flags & FLAG_0x10) aes_ecb(d->key, ck, hk); else memcpy(hk, ck, 16);
        if (d->flags & FLAG_ENCRYPTED_KEY) {
            static const uint8_t zero[16];
            const uint8_t* erk = EDAT_KEY[d->version == 4];
            uint8_t t[16];
            memcpy(t, ck, 16); aes_cbc_dec(erk, zero, t, ck, 16);
            memcpy(t, hk, 16); aes_cbc_dec(erk, zero, t, hk, 16);
        }
        if (!(d->flags & FLAG_0x10)) aes_cmac(hk, d->enc, padded, mac);
        else hmac_sha1(hk, (d->flags & FLAG_0x20) ? 20 : 16, d->enc, padded, mac);
        if (memcmp(mac, expect, (d->flags & (FLAG_0x10 | FLAG_0x20)) == (FLAG_0x10 | FLAG_0x20) ? 20 : 16)) return -1;
        static const uint8_t zero_iv[16];
        if (d->flags & FLAG_PLAIN) memcpy(d->plain, d->enc, len);
        else aes_cbc_dec(ck, d->version > 1 ? d->hdr + 0x40 : zero_iv, d->enc, d->plain, padded);
    }
    d->cached = b;
    d->cached_len = len;
    return len;
}

int npdrm_is_npd(ps3_vfile* f)
{
    uint8_t m[4];
    const int64_t pos = ps3_vfile_tell(f);
    const int npd = ps3_vfile_seek(f, 0, SEEK_SET) == 0 && ps3_vfile_read(m, 4, f) == 4 && !memcmp(m, "NPD", 4);
    ps3_vfile_seek(f, pos, SEEK_SET);
    return npd;
}

npdrm_file* npdrm_open(ps3_vfile* f)
{
    npdrm_file* d = (npdrm_file*)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->f = f;
    d->cached = -1;
    if (ps3_vfile_seek(f, 0, SEEK_SET) || ps3_vfile_read(d->hdr, 0x100, f) != 0x100 || memcmp(d->hdr, "NPD", 4)) goto fail;
    d->version = be32(d->hdr + 4);
    const uint32_t license = be32(d->hdr + 8);
    d->flags = be32(d->hdr + 0x80);
    d->block_size = be32(d->hdr + 0x84);
    d->size = (uint64_t)be32(d->hdr + 0x88) << 32 | be32(d->hdr + 0x8C);
    if ((d->flags & FLAG_COMPRESSED) || !d->block_size || d->block_size > (1u << 24)) goto fail;
    d->blocks = (uint32_t)((d->size + d->block_size - 1) / d->block_size);
    if (d->flags & FLAG_SDAT) {
        for (int i = 0; i < 16; i++) d->key[i] = d->hdr[0x60 + i] ^ SDAT_KEY[i];
    } else if ((license & 3) == 3) {
        int found = 0;
        for (int i = 0; i < s_klic_n && !found; i++) if (klic_matches(d->hdr, s_klic[i])) { memcpy(d->key, s_klic[i], 16); found = 1; }
        for (int i = 0; i < 4 && !found; i++) if (klic_matches(d->hdr, KLIC_BUILTIN[i])) { memcpy(d->key, KLIC_BUILTIN[i], 16); found = 1; }
        if (!found) goto fail;
    } else if (!rap_key(d->hdr + 0x10, d->key)) {
        goto fail;
    }
    d->plain = (uint8_t*)malloc(d->block_size + 16u);
    d->enc = (uint8_t*)malloc(d->block_size + 16u);
    if (!d->plain || !d->enc || (d->blocks && load_block(d, 0) < 0)) goto fail;
    return d;
fail:
    free(d->plain);
    free(d->enc);
    free(d);
    return NULL;
}

int64_t npdrm_read(npdrm_file* d, uint64_t offset, void* dst, uint64_t size)
{
    if (offset >= d->size) return 0;
    if (size > d->size - offset) size = d->size - offset;
    uint8_t* out = (uint8_t*)dst;
    uint64_t done = 0;
    while (done < size) {
        const uint64_t pos = offset + done;
        const uint32_t b = (uint32_t)(pos / d->block_size), in = (uint32_t)(pos % d->block_size);
        const int64_t len = load_block(d, b);
        if (len < 0) return -1;
        uint64_t take = (uint64_t)len - in;
        if (take > size - done) take = size - done;
        memcpy(out + done, d->plain + in, take);
        done += take;
    }
    return (int64_t)done;
}

uint64_t npdrm_size(const npdrm_file* d) { return d->size; }

void npdrm_close(npdrm_file* d)
{
    if (!d) return;
    ps3_vfile_close(d->f);
    free(d->plain);
    free(d->enc);
    free(d);
}

int npdrm_license_available(const char* host_path, const uint8_t* klicensee)
{
    if (!host_path) return (int)NPDRM_ERROR_INVALID_PARAM;
    if (klicensee) npdrm_add_klicensee(klicensee);
    ps3_vfile* f = ps3_vfile_open(host_path, "rb");
    if (!f) return (int)NPDRM_ERROR_IO;
    if (!npdrm_is_npd(f)) { ps3_vfile_close(f); return 0; }
    npdrm_file* d = npdrm_open(f);
    if (!d) {
        uint8_t hdr[0x10];
        const int license2 = ps3_vfile_seek(f, 0, SEEK_SET) == 0 && ps3_vfile_read(hdr, 0x10, f) == 0x10 && (be32(hdr + 8) & 3) != 3;
        ps3_vfile_close(f);
        return (int)(license2 ? NPDRM_ERROR_LICENSE_NOT_FOUND : NPDRM_ERROR_NO_ENTITLEMENT);
    }
    npdrm_close(d);
    return 0;
}
