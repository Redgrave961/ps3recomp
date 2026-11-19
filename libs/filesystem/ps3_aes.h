#ifndef PS3RECOMP_PS3_AES_H
#define PS3RECOMP_PS3_AES_H

#include <stddef.h>
#include <stdint.h>
#include <wmmintrin.h>
#include <smmintrin.h>

#define PS3_AES_TARGET __attribute__((target("aes,sse4.1")))

typedef struct { __m128i e[15], d[15]; int rounds; } ps3_aes_key;

#define PS3_AES_EXPAND(a, t) do { __m128i p_ = (a); p_ = _mm_xor_si128(p_, _mm_slli_si128(p_, 4)); \
    p_ = _mm_xor_si128(p_, _mm_slli_si128(p_, 4)); (a) = _mm_xor_si128(_mm_xor_si128(p_, _mm_slli_si128(p_, 4)), (t)); } while (0)
#define PS3_AES128_STEP(k, i, rc) do { (k)->e[i] = (k)->e[(i) - 1]; \
    PS3_AES_EXPAND((k)->e[i], _mm_shuffle_epi32(_mm_aeskeygenassist_si128((k)->e[(i) - 1], rc), 0xFF)); } while (0)
#define PS3_AES256_STEP(k, i, rc, sh) do { (k)->e[i] = (k)->e[(i) - 2]; \
    PS3_AES_EXPAND((k)->e[i], _mm_shuffle_epi32(_mm_aeskeygenassist_si128((k)->e[(i) - 1], rc), sh)); } while (0)

PS3_AES_TARGET static inline void ps3_aes_finish(ps3_aes_key* k)
{
    const int r = k->rounds;
    k->d[0] = k->e[r];
    for (int i = 1; i < r; i++) k->d[i] = _mm_aesimc_si128(k->e[r - i]);
    k->d[r] = k->e[0];
}

PS3_AES_TARGET static inline void ps3_aes128_init(ps3_aes_key* k, const uint8_t key[16])
{
    k->rounds = 10;
    k->e[0] = _mm_loadu_si128((const __m128i*)key);
    PS3_AES128_STEP(k, 1, 0x01); PS3_AES128_STEP(k, 2, 0x02); PS3_AES128_STEP(k, 3, 0x04); PS3_AES128_STEP(k, 4, 0x08);
    PS3_AES128_STEP(k, 5, 0x10); PS3_AES128_STEP(k, 6, 0x20); PS3_AES128_STEP(k, 7, 0x40); PS3_AES128_STEP(k, 8, 0x80);
    PS3_AES128_STEP(k, 9, 0x1B); PS3_AES128_STEP(k, 10, 0x36);
    ps3_aes_finish(k);
}

PS3_AES_TARGET static inline void ps3_aes256_init(ps3_aes_key* k, const uint8_t key[32])
{
    k->rounds = 14;
    k->e[0] = _mm_loadu_si128((const __m128i*)key);
    k->e[1] = _mm_loadu_si128((const __m128i*)(key + 16));
    PS3_AES256_STEP(k, 2, 0x01, 0xFF); PS3_AES256_STEP(k, 3, 0x00, 0xAA);
    PS3_AES256_STEP(k, 4, 0x02, 0xFF); PS3_AES256_STEP(k, 5, 0x00, 0xAA);
    PS3_AES256_STEP(k, 6, 0x04, 0xFF); PS3_AES256_STEP(k, 7, 0x00, 0xAA);
    PS3_AES256_STEP(k, 8, 0x08, 0xFF); PS3_AES256_STEP(k, 9, 0x00, 0xAA);
    PS3_AES256_STEP(k, 10, 0x10, 0xFF); PS3_AES256_STEP(k, 11, 0x00, 0xAA);
    PS3_AES256_STEP(k, 12, 0x20, 0xFF); PS3_AES256_STEP(k, 13, 0x00, 0xAA);
    PS3_AES256_STEP(k, 14, 0x40, 0xFF);
    ps3_aes_finish(k);
}

PS3_AES_TARGET static inline __m128i ps3_aes_enc(const ps3_aes_key* k, __m128i x)
{
    x = _mm_xor_si128(x, k->e[0]);
    for (int i = 1; i < k->rounds; i++) x = _mm_aesenc_si128(x, k->e[i]);
    return _mm_aesenclast_si128(x, k->e[k->rounds]);
}

PS3_AES_TARGET static inline __m128i ps3_aes_dec(const ps3_aes_key* k, __m128i x)
{
    x = _mm_xor_si128(x, k->d[0]);
    for (int i = 1; i < k->rounds; i++) x = _mm_aesdec_si128(x, k->d[i]);
    return _mm_aesdeclast_si128(x, k->d[k->rounds]);
}

PS3_AES_TARGET static inline void ps3_aes_ecb_enc(const ps3_aes_key* k, const uint8_t in[16], uint8_t out[16])
{
    _mm_storeu_si128((__m128i*)out, ps3_aes_enc(k, _mm_loadu_si128((const __m128i*)in)));
}

PS3_AES_TARGET static inline void ps3_aes_ecb_dec(const ps3_aes_key* k, const uint8_t in[16], uint8_t out[16])
{
    _mm_storeu_si128((__m128i*)out, ps3_aes_dec(k, _mm_loadu_si128((const __m128i*)in)));
}

PS3_AES_TARGET static inline void ps3_aes_cbc_dec(const ps3_aes_key* k, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n)
{
    __m128i prev = _mm_loadu_si128((const __m128i*)iv);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        const __m128i c = _mm_loadu_si128((const __m128i*)(in + i));
        _mm_storeu_si128((__m128i*)(out + i), _mm_xor_si128(ps3_aes_dec(k, c), prev));
        prev = c;
    }
}

PS3_AES_TARGET static inline void ps3_aes_ctr(const ps3_aes_key* k, const uint8_t iv[16], uint64_t block, const uint8_t* in, uint8_t* out, size_t n)
{
    uint64_t hi = (uint64_t)iv[0] << 56 | (uint64_t)iv[1] << 48 | (uint64_t)iv[2] << 40 | (uint64_t)iv[3] << 32 |
                  (uint64_t)iv[4] << 24 | (uint64_t)iv[5] << 16 | (uint64_t)iv[6] << 8 | iv[7];
    uint64_t lo = (uint64_t)iv[8] << 56 | (uint64_t)iv[9] << 48 | (uint64_t)iv[10] << 40 | (uint64_t)iv[11] << 32 |
                  (uint64_t)iv[12] << 24 | (uint64_t)iv[13] << 16 | (uint64_t)iv[14] << 8 | iv[15];
    const uint64_t old = lo;
    lo += block;
    hi += lo < old;
    const __m128i swap = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    for (size_t i = 0; i < n; i += 16) {
        const __m128i ks = ps3_aes_enc(k, _mm_shuffle_epi8(_mm_set_epi64x((int64_t)hi, (int64_t)lo), swap));
        if (!++lo) hi++;
        if (n - i >= 16) {
            _mm_storeu_si128((__m128i*)(out + i), _mm_xor_si128(_mm_loadu_si128((const __m128i*)(in + i)), ks));
            continue;
        }
        uint8_t pad[16];
        _mm_storeu_si128((__m128i*)pad, ks);
        for (size_t j = 0; i + j < n; j++) out[i + j] = in[i + j] ^ pad[j];
    }
}

#endif
