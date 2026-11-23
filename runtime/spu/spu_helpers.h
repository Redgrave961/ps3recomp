#ifndef SPU_HELPERS_H
#define SPU_HELPERS_H

#include "spu_context.h"
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int spu_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }

static inline void spu_arch_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#define SPU_W(P) (((P) & ~3) | (3 - ((P) & 3)))

#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
#include <arm_neon.h>
static inline uint32x4_t spu__ld(u128 a) { return vld1q_u32(a._u32); }
static inline u128 spu__st(uint32x4_t v) { u128 r; vst1q_u32(r._u32, v); return r; }
static inline u128 spu__perm(u128 a, uint8x16_t I) {
    uint8x16_t ih = veorq_u8(vrev32q_u8(I), vdupq_n_u8(3));
    u128 r; vst1q_u8(r._u8, vqtbl1q_u8(vld1q_u8(a._u8), ih)); return r;
}
static inline uint8x16_t spu__iota(void) {
    static const uint8_t k[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    return vld1q_u8(k);
}
#endif
#if !defined(__ARM_NEON) && defined(__SSE4_1__) && \
    !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
#define SPU_X86 1
#include <immintrin.h>
#define SPU__XI static inline __attribute__((always_inline))
SPU__XI __m128i spu__xl(u128 a) { __m128i v; memcpy(&v, &a, 16); return v; }
SPU__XI u128 spu__xs(__m128i v) { u128 r; memcpy(&r, &v, 16); return r; }
SPU__XI u128 spu__xperm(u128 a, __m128i ir) {
    return spu__xs(_mm_shuffle_epi8(spu__xl(a), _mm_xor_si128(ir, _mm_set1_epi8(3))));
}
SPU__XI __m128i spu__xiota(void) { return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }
SPU__XI __m128i spu__xshl(int sh) { return _mm_add_epi8(spu__xiota(), _mm_set1_epi8((char)(sh + 0x70))); }
#define SPU__XR1(h, s) (uint8_t)((((((h) ^ 3) + (s)) & 15) ^ 3))
#define SPU__XR(s) { SPU__XR1(0, s), SPU__XR1(1, s), SPU__XR1(2, s), SPU__XR1(3, s), SPU__XR1(4, s), SPU__XR1(5, s), \
                     SPU__XR1(6, s), SPU__XR1(7, s), SPU__XR1(8, s), SPU__XR1(9, s), SPU__XR1(10, s), SPU__XR1(11, s), \
                     SPU__XR1(12, s), SPU__XR1(13, s), SPU__XR1(14, s), SPU__XR1(15, s) }
static const _Alignas(16) uint8_t spu__xrot_tbl[16][16] = {
    SPU__XR(0), SPU__XR(1), SPU__XR(2), SPU__XR(3), SPU__XR(4), SPU__XR(5), SPU__XR(6), SPU__XR(7),
    SPU__XR(8), SPU__XR(9), SPU__XR(10), SPU__XR(11), SPU__XR(12), SPU__XR(13), SPU__XR(14), SPU__XR(15) };
#undef SPU__XR
#undef SPU__XR1
SPU__XI u128 spu__xrotl(u128 a, uint32_t sh) {
    return spu__xs(_mm_shuffle_epi8(spu__xl(a), _mm_load_si128((const __m128i*)spu__xrot_tbl[sh & 15])));
}
#define SPU__XLR1(h, s) (uint8_t)((((h) ^ 3) + (s)) & 15)
#define SPU__XLR(s) { SPU__XLR1(0, s), SPU__XLR1(1, s), SPU__XLR1(2, s), SPU__XLR1(3, s), SPU__XLR1(4, s), SPU__XLR1(5, s), \
                      SPU__XLR1(6, s), SPU__XLR1(7, s), SPU__XLR1(8, s), SPU__XLR1(9, s), SPU__XLR1(10, s), SPU__XLR1(11, s), \
                      SPU__XLR1(12, s), SPU__XLR1(13, s), SPU__XLR1(14, s), SPU__XLR1(15, s) }
static const _Alignas(16) uint8_t spu__xls_rot_tbl[16][16] = {
    SPU__XLR(0), SPU__XLR(1), SPU__XLR(2), SPU__XLR(3), SPU__XLR(4), SPU__XLR(5), SPU__XLR(6), SPU__XLR(7),
    SPU__XLR(8), SPU__XLR(9), SPU__XLR(10), SPU__XLR(11), SPU__XLR(12), SPU__XLR(13), SPU__XLR(14), SPU__XLR(15) };
#undef SPU__XLR
#undef SPU__XLR1
SPU__XI u128 spu_ls_ld128_rotqby(const uint8_t* __restrict ls, uint32_t lsa, uint32_t sh) {
    lsa &= SPU_LS_MASK & ~0xFu;
    return spu__xs(_mm_shuffle_epi8(_mm_load_si128((const __m128i*)&ls[lsa]),
                                    _mm_load_si128((const __m128i*)spu__xls_rot_tbl[sh & 15])));
}
SPU__XI void spu_ls_st32(uint8_t* __restrict ls, uint32_t lsa, uint32_t val) {
    uint32_t w = __builtin_bswap32(val);
    memcpy(&ls[lsa & (SPU_LS_MASK & ~3u)], &w, 4);
}
SPU__XI __m128i spu__xshr(int sh) { return _mm_sub_epi8(spu__xiota(), _mm_set1_epi8((char)sh)); }
SPU__XI __m128i spu__xflip(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi32((int)0x80000000u)); }
#else
#define SPU_X86 0
static inline u128 spu_ls_ld128_rotqby(const uint8_t* __restrict ls, uint32_t lsa, uint32_t sh) {
    u128 v = spu_ls_ld128(ls, lsa);
    u128 r;
    const int s = (int)(sh & 15u);
    for (int i = 0; i < 16; i++) r._u8[SPU_W(i)] = v._u8[SPU_W((i + s) & 15)];
    return r;
}
static inline void spu_ls_st32(uint8_t* __restrict ls, uint32_t lsa, uint32_t val) {
    uint8_t* p = &ls[lsa & (SPU_LS_MASK & ~3u)];
    p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >>  8); p[3] = (uint8_t)(val);
}
#endif

static inline uint32_t spu_pref_word(u128 a) {
#if SPU_X86
    return (uint32_t)_mm_cvtsi128_si32(spu__xl(a));
#else
    return a._u32[0];
#endif
}

static inline void spu_ls_copy_fwd(uint8_t* ls, uint32_t d, uint32_t s, uint32_t n) {
    const uint32_t p = d - s;
    if (p == 1u) {
        memset(ls + d, ls[s], n);
        return;
    }
    if (p - 1u >= n - 1u) {
        memmove(ls + d, ls + s, n);
        return;
    }
    for (uint32_t w = 0, c; w < n; w += c) {
        c = w + p < n - w ? w + p : n - w;
        memcpy(ls + d + w, ls + s, c);
    }
}

static inline void spu_copy_exact_nonoverlap(uint8_t* dp, const uint8_t* sp, uint32_t len) {
    if (len >= 16u) {
        if (len <= 32u) {
            uint8_t a[16], b[16];
            memcpy(a, sp, 16);
            memcpy(b, sp + len - 16, 16);
            memcpy(dp, a, 16);
            memcpy(dp + len - 16, b, 16);
            return;
        }
        memcpy(dp, sp, len);
        return;
    }
    if (len >= 8u) {
        uint64_t a, b;
        memcpy(&a, sp, 8);
        memcpy(&b, sp + len - 8, 8);
        memcpy(dp, &a, 8);
        memcpy(dp + len - 8, &b, 8);
        return;
    }
    if (len >= 4u) {
        uint32_t a, b;
        memcpy(&a, sp, 4);
        memcpy(&b, sp + len - 4, 4);
        memcpy(dp, &a, 4);
        memcpy(dp + len - 4, &b, 4);
        return;
    }
    dp[0] = sp[0];
    if (len >= 2u) {
        dp[1] = sp[1];
        if (len == 3u) dp[2] = sp[2];
    }
}

static inline void spu_ls_lzf_decompress(uint8_t* ls, uint32_t* p_src, uint32_t src_end, uint32_t* p_dst) {
    uint32_t src = *p_src, dst = *p_dst;
    // Fast non-wrapping path: 99.99% of SPU LZF streams stay well inside [0, 0x40000).
    // Uses 16B/32B wild copies when there is >= 32B headroom to LS end and no overlap.
    while (src < src_end) {
        const uint32_t s0 = src & 0x3FFFFu, d0 = dst & 0x3FFFFu;
        if (__builtin_expect(src_end <= 0x40000u && s0 < 0x3FFE0u && d0 < 0x3FFE0u && src == s0 && dst == d0, 1)) {
            uint8_t* sp = ls + s0;
            uint8_t* dp = ls + d0;
            uint8_t* const send = ls + src_end;
            uint8_t* const dsafe = ls + (0x40000u - 32u);
            while (sp < send && dp < dsafe) {
                const uint32_t ctrl = *sp++;
                if (ctrl < 32u) {
                    const uint32_t len = ctrl + 1u;
                    if (__builtin_expect(sp + 32 <= ls + 0x40000u && (dp >= sp + len || sp >= dp + 32), 1)) {
                        memcpy(dp, sp, 16);
                        if (len > 16u) memcpy(dp + 16, sp + 16, 16);
                    } else if (sp + len <= ls + 0x40000u && dp + len <= ls + 0x40000u) {
                        if (dp >= sp + len || sp >= dp + len)
                            spu_copy_exact_nonoverlap(dp, sp, len);
                        else
                            spu_ls_copy_fwd(ls, (uint32_t)(dp - ls), (uint32_t)(sp - ls), len);
                    } else {
                        sp--;
                        break;
                    }
                    sp += len;
                    dp += len;
                } else {
                    uint32_t len = ctrl >> 5;
                    uint32_t back = ((ctrl & 31u) << 8) + 1u;
                    if (len == 7u) {
                        if (__builtin_expect(sp >= send, 0)) { sp--; break; }
                        len += *sp++;
                    }
                    if (__builtin_expect(sp >= send, 0)) { sp--; break; }
                    back += *sp++;
                    len += 2u;
                    if (__builtin_expect((uint32_t)(dp - ls) < back || dp + len > ls + 0x40000u, 0)) {
                        // Wrap-around ref or near LS end: hand off to exact wrapping loop.
                        src = (uint32_t)(sp - ls);
                        dst = (uint32_t)(dp - ls);
                        const uint32_t ref = dst - back;
                        for (uint32_t i = 0; i < len; ++i)
                            ls[(dst + i) & 0x3FFFFu] = ls[(ref + i) & 0x3FFFFu];
                        dst += len;
                        goto next_outer;
                    }
                    const uint8_t* mp = dp - back;
                    if (__builtin_expect(back >= 16u, 1)) {
                        if (__builtin_expect(len <= 16u, 1)) {
                            memcpy(dp, mp, 16);
                        } else if (dp + len + 16u <= ls + 0x40000u) {
                            uint32_t i = 0;
                            do {
                                memcpy(dp + i, mp + i, 16);
                                i += 16u;
                            } while (i < len);
                        } else {
                            spu_copy_exact_nonoverlap(dp, mp, len);
                        }
                    } else if (back == 1u) {
                        const uint64_t pat = (uint64_t)mp[0] * 0x0101010101010101ull;
                        if (len <= 16u) {
                            memcpy(dp, &pat, 8);
                            if (len > 8u) memcpy(dp + 8, &pat, 8);
                        } else {
                            memset(dp, mp[0], len);
                        }
                    } else if (back >= 8u) {
                        uint32_t i = 0;
                        if (dp + len + 8u <= ls + 0x40000u) {
                            do {
                                memcpy(dp + i, mp + i, 8);
                                i += 8u;
                            } while (i < len);
                        } else {
                            for (; i + 8u <= len; i += 8u) memcpy(dp + i, mp + i, 8);
                            for (; i < len; ++i) dp[i] = mp[i];
                        }
                    } else if (back == 4u && dp + len + 8u <= ls + 0x40000u) {
                        uint32_t p4;
                        memcpy(&p4, mp, 4);
                        const uint64_t p8 = (uint64_t)p4 | ((uint64_t)p4 << 32);
                        uint32_t i = 0;
                        do {
                            memcpy(dp + i, &p8, 8);
                            i += 8u;
                        } while (i < len);
                    } else if (back == 2u && dp + len + 8u <= ls + 0x40000u) {
                        uint16_t p2;
                        memcpy(&p2, mp, 2);
                        const uint64_t p8 = (uint64_t)p2 * 0x0001000100010001ull;
                        uint32_t i = 0;
                        do {
                            memcpy(dp + i, &p8, 8);
                            i += 8u;
                        } while (i < len);
                    } else {
                        for (uint32_t i = 0; i < len; ++i) dp[i] = mp[i];
                    }
                    dp += len;
                }
            }
            src = (uint32_t)(sp - ls);
            dst = (uint32_t)(dp - ls);
            if (src >= src_end) break;
        }
        const uint32_t ctrl = ls[src++ & 0x3FFFFu];
        if (ctrl < 32u) {
            const uint32_t len = ctrl + 1u;
            const uint32_t s = src & 0x3FFFFu, d = dst & 0x3FFFFu;
            if (__builtin_expect(s + len <= 0x40000u && d + len <= 0x40000u, 1)) {
                if (__builtin_expect(d >= s + len || s >= d + len, 1))
                    spu_copy_exact_nonoverlap(ls + d, ls + s, len);
                else
                    spu_ls_copy_fwd(ls, d, s, len);
            } else {
                for (uint32_t i = 0; i < len; ++i)
                    ls[(dst + i) & 0x3FFFFu] = ls[(src + i) & 0x3FFFFu];
            }
            src += len;
            dst += len;
        } else {
            uint32_t len = ctrl >> 5;
            uint32_t ref = dst - ((ctrl & 31u) << 8) - 1u;
            if (len == 7u) len += ls[src++ & 0x3FFFFu];
            ref -= ls[src++ & 0x3FFFFu];
            len += 2u;
            const uint32_t s = ref & 0x3FFFFu, d = dst & 0x3FFFFu;
            if (__builtin_expect(s + len <= 0x40000u && d + len <= 0x40000u, 1)) {
                uint8_t* dp = ls + d;
                const uint8_t* sp = ls + s;
                const uint32_t dist = d - s;
                if (dist >= len) {
                    spu_copy_exact_nonoverlap(dp, sp, len);
                } else if (dist == 1u) {
                    memset(dp, sp[0], len);
                } else if (dist >= 8u) {
                    uint32_t i = 0;
                    for (; i + 8u <= len; i += 8u) memcpy(dp + i, sp + i, 8);
                    for (; i < len; ++i) dp[i] = sp[i];
                } else {
                    for (uint32_t i = 0; i < len; ++i) dp[i] = sp[i];
                }
            } else {
                for (uint32_t i = 0; i < len; ++i)
                    ls[(dst + i) & 0x3FFFFu] = ls[(ref + i) & 0x3FFFFu];
            }
            dst += len;
        }
next_outer:;
    }
    *p_src = src;
    *p_dst = dst;
}


static inline u128 spu_absdb(u128 a, u128 b);
static inline u128 spu_avgb(u128 a, u128 b);
static inline u128 spu_ceqb(u128 a, u128 b);
static inline u128 spu_cgtb(u128 a, u128 b);
static inline u128 spu_clgtb(u128 a, u128 b);
static inline u128 spu_ceqh(u128 a, u128 b);
static inline u128 spu_cgth(u128 a, u128 b);
static inline u128 spu_clgth(u128 a, u128 b);
static inline u128 spu_ah(u128 a, u128 b);
static inline u128 spu_sfh(u128 a, u128 b);
static inline u128 spu_shlh(u128 a, u128 b);
static inline u128 spu_roth(u128 a, u128 b);
static inline u128 spu_rothm(u128 a, u128 b);
static inline u128 spu_rothma(u128 a, u128 b);
static inline u128 spu_shl(u128 a, u128 b);
static inline u128 spu_rot(u128 a, u128 b);
static inline u128 spu_rotm(u128 a, u128 b);
static inline u128 spu_rotma(u128 a, u128 b);
static inline u128 spu_cg(u128 a, u128 b);
static inline u128 spu_bg(u128 a, u128 b);
static inline u128 spu_mpy(u128 a, u128 b);
static inline u128 spu_mpyu(u128 a, u128 b);
static inline u128 spu_mpyh(u128 a, u128 b);
static inline u128 spu_mpyhh(u128 a, u128 b);
static inline u128 spu_mpyhhu(u128 a, u128 b);
static inline u128 spu_mpys(u128 a, u128 b);
static inline u128 spu_sumb(u128 a, u128 b);
static inline u128 spu_ceqbi(u128 a, int32_t imm);
static inline u128 spu_cgtbi(u128 a, int32_t imm);
static inline u128 spu_clgtbi(u128 a, int32_t imm);
static inline u128 spu_ceqhi(u128 a, int32_t imm);
static inline u128 spu_cgthi(u128 a, int32_t imm);
static inline u128 spu_clgthi(u128 a, int32_t imm);
static inline u128 spu_ahi(u128 a, int32_t imm);
static inline u128 spu_sfhi(u128 a, int32_t imm);
static inline u128 spu_mpyi(u128 a, int32_t imm);
static inline u128 spu_mpyui(u128 a, int32_t imm);
static inline u128 spu_rothi(u128 a, int sh);
static inline u128 spu_roti(u128 a, int sh);
static inline u128 spu_rotmahi(u128 a, int i7);
static inline u128 spu_rotmai(u128 a, int i7);
static inline u128 spu_mpya(u128 a, u128 b, u128 c);
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t);
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t);
static inline u128 spu_addx(u128 a, u128 b, u128 t);
static inline u128 spu_sfx(u128 a, u128 b, u128 t);
static inline u128 spu_cgx(u128 a, u128 b, u128 t);
static inline u128 spu_bgx(u128 a, u128 b, u128 t);
static inline u128 spu_cntb(u128 a);
static inline u128 spu_xsbh(u128 a);
static inline u128 spu_xshw(u128 a);
static inline u128 spu_clz(u128 a);
static inline u128 spu_xswd(u128 a);
static inline u128 spu_orx(u128 a);
static inline u128 spu_gb(u128 a);
static inline u128 spu_gbh(u128 a);
static inline u128 spu_gbb(u128 a);
static inline u128 spu_fsm(u128 a);
static inline u128 spu_fsmh(u128 a);
static inline u128 spu_fsmb(u128 a);

static inline u128 spu_splat_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=v; r._u32[2]=v; r._u32[3]=v; return r;
}
static inline void spu_unsupported(uint32_t pc, const char* mnemonic)
{
    static int warned;
    if (warned < 64) {
        warned++;
        fprintf(stderr, "[spu] UNSUPPORTED %s at LS 0x%05X -- executed, "
                        "result is wrong from here\n", mnemonic, pc);
        fflush(stderr);
    }
}

static inline u128 spu_link(uint32_t addr) {
    u128 r; r._u32[0]=addr; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
static inline u128 spu_pref_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
static inline u128 spu_splat_u16(uint16_t v) {
#if SPU_X86
    return spu__xs(_mm_set1_epi16((short)v));
#else
    u128 r; for (int i=0;i<8;i++) r._u16[i]=v; return r;
#endif
}
static inline u128 spu_splat_u8(uint8_t v) {
#if SPU_X86
    return spu__xs(_mm_set1_epi8((char)v));
#else
    u128 r; for (int i=0;i<16;i++) r._u8[i]=v; return r;
#endif
}
static inline u128 spu_zero(void) {
#if SPU_X86
    return spu__xs(_mm_setzero_si128());
#else
    u128 r; memset(&r,0,sizeof r); return r;
#endif
}

static inline u128 spu_a_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_a(u128 a, u128 b) {
    return spu__st(vaddq_u32(spu__ld(a), spu__ld(b)));
}
#elif SPU_X86
static inline u128 spu_a(u128 a, u128 b) {
    return spu__xs(_mm_add_epi32(spu__xl(a), spu__xl(b)));
}
#else
static inline u128 spu_a(u128 a, u128 b) { return spu_a_ref(a, b); }
#endif
static inline u128 spu_sf_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]-a._u32[i]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_sf(u128 a, u128 b) {
    return spu__st(vsubq_u32(spu__ld(b), spu__ld(a)));
}
#elif SPU_X86
static inline u128 spu_sf(u128 a, u128 b) {
    return spu__xs(_mm_sub_epi32(spu__xl(b), spu__xl(a)));
}
#else
static inline u128 spu_sf(u128 a, u128 b) { return spu_sf_ref(a, b); }
#endif
static inline u128 spu_ah_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+b._u16[i]; return r; }
static inline u128 spu_sfh_ref(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=b._u16[i]-a._u16[i]; return r; }
static inline u128 spu_ai_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ai(u128 a, int32_t imm) {
    return spu__st(vaddq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#elif SPU_X86
static inline u128 spu_ai(u128 a, int32_t imm) {
    return spu__xs(_mm_add_epi32(spu__xl(a), _mm_set1_epi32(imm)));
}
#else
static inline u128 spu_ai(u128 a, int32_t imm) { return spu_ai_ref(a, imm); }
#endif
static inline u128 spu_ahi_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+(uint16_t)imm; return r; }
static inline u128 spu_sfi_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)imm-a._u32[i]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_sfi(u128 a, int32_t imm) {
    return spu__st(vsubq_u32(vdupq_n_u32((uint32_t)imm), spu__ld(a)));
}
#elif SPU_X86
static inline u128 spu_sfi(u128 a, int32_t imm) {
    return spu__xs(_mm_sub_epi32(_mm_set1_epi32(imm), spu__xl(a)));
}
#else
static inline u128 spu_sfi(u128 a, int32_t imm) { return spu_sfi_ref(a, imm); }
#endif
static inline u128 spu_sfhi_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)imm-a._u16[i]; return r; }

static inline u128 spu_mpy_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]; return r; }
static inline u128 spu_mpya_ref(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]+c._s32[i]; return r; }
static inline u128 spu_sfx_ref(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
static inline u128 spu_mpyu_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[i*2]*(uint32_t)b._u16[i*2]; return r; }
static inline u128 spu_mpyi_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int16_t)imm; return r; }

static inline u128 spu_and_ref(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]&b._u64[0]; r._u64[1]=a._u64[1]&b._u64[1]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_and(u128 a, u128 b) {
    return spu__st(vandq_u32(spu__ld(a), spu__ld(b)));
}
#elif SPU_X86
static inline u128 spu_and(u128 a, u128 b) {
    return spu__xs(_mm_and_si128(spu__xl(a), spu__xl(b)));
}
#else
static inline u128 spu_and(u128 a, u128 b) { return spu_and_ref(a, b); }
#endif
static inline u128 spu_or_ref(u128 a, u128 b)  { u128 r; r._u64[0]=a._u64[0]|b._u64[0]; r._u64[1]=a._u64[1]|b._u64[1]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_or(u128 a, u128 b) {
    return spu__st(vorrq_u32(spu__ld(a), spu__ld(b)));
}
#elif SPU_X86
static inline u128 spu_or(u128 a, u128 b) {
    return spu__xs(_mm_or_si128(spu__xl(a), spu__xl(b)));
}
#else
static inline u128 spu_or(u128 a, u128 b) { return spu_or_ref(a, b); }
#endif
static inline u128 spu_xor_ref(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]^b._u64[0]; r._u64[1]=a._u64[1]^b._u64[1]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_xor(u128 a, u128 b) {
    return spu__st(veorq_u32(spu__ld(a), spu__ld(b)));
}
#elif SPU_X86
static inline u128 spu_xor(u128 a, u128 b) {
    return spu__xs(_mm_xor_si128(spu__xl(a), spu__xl(b)));
}
#else
static inline u128 spu_xor(u128 a, u128 b) { return spu_xor_ref(a, b); }
#endif
#if SPU_X86
static inline u128 spu_nand(u128 a, u128 b){ return spu__xs(_mm_xor_si128(_mm_and_si128(spu__xl(a), spu__xl(b)), _mm_set1_epi32(-1))); }
static inline u128 spu_nor(u128 a, u128 b) { return spu__xs(_mm_xor_si128(_mm_or_si128(spu__xl(a), spu__xl(b)), _mm_set1_epi32(-1))); }
static inline u128 spu_andc(u128 a, u128 b){ return spu__xs(_mm_andnot_si128(spu__xl(b), spu__xl(a))); }
static inline u128 spu_orc(u128 a, u128 b) { return spu__xs(_mm_or_si128(spu__xl(a), _mm_xor_si128(spu__xl(b), _mm_set1_epi32(-1)))); }
#else
static inline u128 spu_nand(u128 a, u128 b){ u128 r; r._u64[0]=~(a._u64[0]&b._u64[0]); r._u64[1]=~(a._u64[1]&b._u64[1]); return r; }
static inline u128 spu_nor(u128 a, u128 b) { u128 r; r._u64[0]=~(a._u64[0]|b._u64[0]); r._u64[1]=~(a._u64[1]|b._u64[1]); return r; }
static inline u128 spu_andc(u128 a, u128 b){ u128 r; r._u64[0]=a._u64[0]&~b._u64[0]; r._u64[1]=a._u64[1]&~b._u64[1]; return r; }
static inline u128 spu_orc(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]|~b._u64[0]; r._u64[1]=a._u64[1]|~b._u64[1]; return r; }
#endif
static inline u128 spu_andi_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]&(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_andi(u128 a, int32_t imm) {
    return spu__st(vandq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#elif SPU_X86
static inline u128 spu_andi(u128 a, int32_t imm) {
    return spu__xs(_mm_and_si128(spu__xl(a), _mm_set1_epi32(imm)));
}
#else
static inline u128 spu_andi(u128 a, int32_t imm) { return spu_andi_ref(a, imm); }
#endif
static inline u128 spu_ori_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ori(u128 a, int32_t imm) {
    return spu__st(vorrq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#elif SPU_X86
static inline u128 spu_ori(u128 a, int32_t imm) {
    return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi32(imm)));
}
#else
static inline u128 spu_ori(u128 a, int32_t imm) { return spu_ori_ref(a, imm); }
#endif
static inline u128 spu_xori_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]^(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_xori(u128 a, int32_t imm) {
    return spu__st(veorq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#elif SPU_X86
static inline u128 spu_xori(u128 a, int32_t imm) {
    return spu__xs(_mm_xor_si128(spu__xl(a), _mm_set1_epi32(imm)));
}
#else
static inline u128 spu_xori(u128 a, int32_t imm) { return spu_xori_ref(a, imm); }
#endif

static inline u128 spu_clz_ref(u128 a)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)spu_clz32(a._u32[i]); return r; }
static inline u128 spu_cntb_ref(u128 a) { u128 r; for(int i=0;i<16;i++){ uint8_t v=a._u8[i],c=0; while(v){c+=v&1;v>>=1;} r._u8[i]=c; } return r; }

static inline u128 spu_ceq_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]==b._u32[i])?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ceq(u128 a, u128 b) {
    return spu__st(vceqq_u32(spu__ld(a), spu__ld(b)));
}
#elif SPU_X86
static inline u128 spu_ceq(u128 a, u128 b) {
    return spu__xs(_mm_cmpeq_epi32(spu__xl(a), spu__xl(b)));
}
#else
static inline u128 spu_ceq(u128 a, u128 b) { return spu_ceq_ref(a, b); }
#endif
static inline u128 spu_ceqh_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]==b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_ceqb_ref(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_cgt_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>b._s32[i])?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_cgt(u128 a, u128 b) {
    return spu__st(vcgtq_s32(vreinterpretq_s32_u32(spu__ld(a)), vreinterpretq_s32_u32(spu__ld(b))));
}
#elif SPU_X86
static inline u128 spu_cgt(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi32(spu__xl(a), spu__xl(b)));
}
#else
static inline u128 spu_cgt(u128 a, u128 b) { return spu_cgt_ref(a, b); }
#endif
static inline u128 spu_cgth_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>b._s16[i])?0xFFFFu:0; return r; }
static inline u128 spu_cgtb_ref(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>b._s8[i])?0xFFu:0; return r; }
static inline u128 spu_clgt_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>b._u32[i])?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_clgt(u128 a, u128 b) {
    return spu__st(vcgtq_u32(spu__ld(a), spu__ld(b)));
}
#elif SPU_X86
static inline u128 spu_clgt(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi32(spu__xflip(spu__xl(a)), spu__xflip(spu__xl(b))));
}
#else
static inline u128 spu_clgt(u128 a, u128 b) { return spu_clgt_ref(a, b); }
#endif
static inline u128 spu_clgth_ref(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_clgtb_ref(u128 a, u128 b){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_ceqi_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]==imm)?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ceqi(u128 a, int32_t imm) {
    return spu__st(vceqq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#elif SPU_X86
static inline u128 spu_ceqi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpeq_epi32(spu__xl(a), _mm_set1_epi32(imm)));
}
#else
static inline u128 spu_ceqi(u128 a, int32_t imm) { return spu_ceqi_ref(a, imm); }
#endif
static inline u128 spu_cgti_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>imm)?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_cgti(u128 a, int32_t imm) {
    return spu__st(vcgtq_s32(vreinterpretq_s32_u32(spu__ld(a)), vdupq_n_s32(imm)));
}
#elif SPU_X86
static inline u128 spu_cgti(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi32(spu__xl(a), _mm_set1_epi32(imm)));
}
#else
static inline u128 spu_cgti(u128 a, int32_t imm) { return spu_cgti_ref(a, imm); }
#endif
static inline u128 spu_clgti_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>(uint32_t)imm)?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_clgti(u128 a, int32_t imm) {
    return spu__st(vcgtq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#elif SPU_X86
static inline u128 spu_clgti(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi32(spu__xflip(spu__xl(a)), spu__xflip(_mm_set1_epi32(imm))));
}
#else
static inline u128 spu_clgti(u128 a, int32_t imm) { return spu_clgti_ref(a, imm); }
#endif

static inline u128 spu_selb_ref(u128 a, u128 b, u128 c) {
    u128 r;
    r._u64[0]=(a._u64[0]&~c._u64[0])|(b._u64[0]&c._u64[0]);
    r._u64[1]=(a._u64[1]&~c._u64[1])|(b._u64[1]&c._u64[1]);
    return r;
}
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_selb(u128 a, u128 b, u128 c) {
    return spu__st(vbslq_u32(spu__ld(c), spu__ld(b), spu__ld(a)));
}
#elif SPU_X86
static inline u128 spu_selb(u128 a, u128 b, u128 c) {
    const __m128i m = spu__xl(c); return spu__xs(_mm_or_si128(_mm_andnot_si128(m, spu__xl(a)), _mm_and_si128(m, spu__xl(b))));
}
#else
static inline u128 spu_selb(u128 a, u128 b, u128 c) { return spu_selb_ref(a, b, c); }
#endif
#if defined(__SSE4_1__)
#include <smmintrin.h>
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    __m128i A, B, C; memcpy(&A, &a, 16); memcpy(&B, &b, 16); memcpy(&C, &c, 16);
    const __m128i idx = _mm_xor_si128(C, _mm_set1_epi8(3));
    const __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), _mm_slli_epi16(C, 3));
    const __m128i special = _mm_shuffle_epi8(_mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, (char)0x80, (char)0x80),
                                             _mm_and_si128(_mm_srli_epi16(C, 4), _mm_set1_epi8(0x0F)));
    const __m128i out = _mm_blendv_epi8(r, special, C);
    u128 o; memcpy(&o, &out, 16); return o;
}
#elif defined(__ARM_NEON)
#include <arm_neon.h>
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    uint8x16x2_t t;
    t.val[0] = vrev32q_u8(vld1q_u8(a._u8));
    t.val[1] = vrev32q_u8(vld1q_u8(b._u8));
    const uint8x16_t C = vrev32q_u8(vld1q_u8(c._u8));
    uint8x16_t r = vqtbl2q_u8(t, vandq_u8(C, vdupq_n_u8(0x1F)));
    const uint8x16_t m80 = vtstq_u8(C, vdupq_n_u8(0x80));
    const uint8x16_t mC0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xC0)), vdupq_n_u8(0xC0));
    const uint8x16_t mE0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xE0)), vdupq_n_u8(0xE0));
    const uint8x16_t special = vbslq_u8(mE0, vdupq_n_u8(0x80), mC0);
    r = vrev32q_u8(vbslq_u8(m80, special, r));
    u128 out; vst1q_u8(out._u8, r); return out;
}
#endif
#if defined(__SSE4_1__) || defined(__ARM_NEON)
static inline u128 spu_shufb_ref(u128 a, u128 b, u128 c) {
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];
    u128 r;
    for (int t=0;t<16;t++) {
        uint8_t s = c._u8[SPU_W(t)];
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#else
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];
    u128 r;
    for (int t=0;t<16;t++) {
        uint8_t s = c._u8[SPU_W(t)];
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#define spu_shufb_ref spu_shufb
#endif
#if defined(__SSE4_1__)
static inline u128 spu_shufb_ins(u128 a, u128 b, u128 c) {
    __m128i A, B, C; memcpy(&A, &a, 16); memcpy(&B, &b, 16); memcpy(&C, &c, 16);
    const __m128i out = _mm_blendv_epi8(_mm_shuffle_epi8(A, _mm_xor_si128(C, _mm_set1_epi8(3))),
                                        B, _mm_slli_epi16(C, 3));
    u128 o; memcpy(&o, &out, 16); return o;
}
static inline __attribute__((always_inline)) u128 spu_splat_w(u128 a, int lane) {
    const __m128i A = spu__xl(a);
    if (lane == 0) return spu__xs(_mm_shuffle_epi32(A, 0x00));
    if (lane == 1) return spu__xs(_mm_shuffle_epi32(A, 0x55));
    if (lane == 2) return spu__xs(_mm_shuffle_epi32(A, 0xAA));
    return spu__xs(_mm_shuffle_epi32(A, 0xFF));
}
#else
static inline u128 spu_shufb_ins(u128 a, u128 b, u128 c) { return spu_shufb(a, b, c); }
static inline u128 spu_splat_w(u128 a, int lane) { return spu_splat_u32(a._u32[lane & 3]); }
#endif

#if SPU_X86
static inline u128 spu_shli(u128 a, int sh)  { sh&=0x3F; return spu__xs(_mm_sll_epi32(spu__xl(a), _mm_cvtsi32_si128(sh))); }
static inline u128 spu_shlhi(u128 a, int sh) { sh&=0x1F; return spu__xs(_mm_sll_epi16(spu__xl(a), _mm_cvtsi32_si128(sh))); }
static inline u128 spu_roti_ref(u128 a, int sh)  { u128 r; sh&=31; for(int i=0;i<4;i++) r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; return r; }
static inline u128 spu_rothi_ref(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmi(u128 a, int i7)  { int sh=(0-i7)&0x3F; return spu__xs(_mm_srl_epi32(spu__xl(a), _mm_cvtsi32_si128(sh))); }
static inline u128 spu_rotmai_ref(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { int sh=(0-i7)&0x1F; return spu__xs(_mm_srl_epi16(spu__xl(a), _mm_cvtsi32_si128(sh))); }
#else
static inline u128 spu_shli(u128 a, int sh)  { u128 r; sh&=0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); return r; }
static inline u128 spu_shlhi(u128 a, int sh) { u128 r; sh&=0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); return r; }
static inline u128 spu_roti_ref(u128 a, int sh)  { u128 r; sh&=31; for(int i=0;i<4;i++) r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; return r; }
static inline u128 spu_rothi_ref(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmi(u128 a, int i7)  { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); return r; }
static inline u128 spu_rotmai_ref(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
#endif
static inline u128 spu_shlqbyi_ref(u128 a, int sh) { u128 r=spu_zero(); sh&=0x1F; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_shlqbyi(u128 a, int sh) {
    return spu__perm(a, vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)(sh & 0x1F))));
}
#elif SPU_X86
static inline u128 spu_shlqbyi(u128 a, int sh) {
    const int s = sh & 0x1F;
    if (s == 0)  return a;
    if (s >= 16) return spu_zero();
    if (s == 4)  return spu__xs(_mm_srli_si128(spu__xl(a), 4));
    if (s == 8)  return spu__xs(_mm_srli_si128(spu__xl(a), 8));
    if (s == 12) return spu__xs(_mm_srli_si128(spu__xl(a), 12));
    return spu__xperm(a, spu__xshl(s));
}
#else
static inline u128 spu_shlqbyi(u128 a, int sh) { return spu_shlqbyi_ref(a, sh); }
#endif
static inline u128 spu_rotqbyi_ref(u128 a, int sh) { u128 r; sh&=0x0F; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqbyi(u128 a, int sh) {
    return spu__perm(a, vandq_u8(vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)sh)), vdupq_n_u8(15)));
}
#elif SPU_X86
static inline u128 spu_rotqbyi(u128 a, int sh) {
    const int s = sh & 15;
    if (s == 0)  return a;
    if (s == 4)  return spu__xs(_mm_shuffle_epi32(spu__xl(a), _MM_SHUFFLE(0, 3, 2, 1)));
    if (s == 8)  return spu__xs(_mm_shuffle_epi32(spu__xl(a), _MM_SHUFFLE(1, 0, 3, 2)));
    if (s == 12) return spu__xs(_mm_shuffle_epi32(spu__xl(a), _MM_SHUFFLE(2, 1, 0, 3)));
    return spu__xrotl(a, (uint32_t)s);
}
#else
static inline u128 spu_rotqbyi(u128 a, int sh) { return spu_rotqbyi_ref(a, sh); }
#endif
static inline u128 spu_shlqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }

static inline uint32_t spu__sf_pack(uint32_t sign, unsigned __int128 sig, int e2) {
    const uint64_t hi = (uint64_t)(sig >> 64), lo = (uint64_t)sig;
    const int top = hi ? 127 - __builtin_clzll(hi) : 63 - __builtin_clzll(lo);
    const int biased = top + e2 + 127;
    if (biased > 255) return sign | 0x7FFFFFFFu;
    if (biased < 1)   return 0;
    const uint32_t m24 = top >= 23 ? (uint32_t)(sig >> (top - 23)) : (uint32_t)(sig << (23 - top));
    return sign | ((uint32_t)biased << 23) | (m24 & 0x7FFFFFu);
}
static inline int spu__sf_split(uint32_t x, uint32_t* sig, int* e2) {
    const uint32_t be = (x >> 23) & 0xFFu;
    if (!be) return 0;
    *sig = (x & 0x7FFFFFu) | 0x800000u;
    *e2 = (int)be - 150;
    return 1;
}
#define SPU__SF_V2(name, expr) static inline u128 name(u128 a, u128 b) { u128 r; \
    for (int i = 0; i < 4; i++) { const uint32_t x = a._u32[i], y = b._u32[i]; r._u32[i] = (expr); } return r; }
#define SPU__SF_V3(name, expr) static inline u128 name(u128 a, u128 b, u128 c) { u128 r; \
    for (int i = 0; i < 4; i++) { const uint32_t x = a._u32[i], y = b._u32[i], z = c._u32[i]; r._u32[i] = (expr); } return r; }
#define SPU_FPCR_RZFZ 0x01C00000ull
#if SPU_X86
#define SPU_MXCSR_RZ 0xFFC0u
SPU__XI u128 spu__xf_out(__m128 s) {
    /* SPU_MXCSR_RZ enables FTZ+DAZ+RZ so inputs clamped by spu__xf always yield
     * finite results; adding +0.0f clears -0.0f -> +0.0f in a single vaddps. */
    return spu__xs(_mm_castps_si128(_mm_add_ps(s, _mm_setzero_ps())));
}
SPU__XI u128 spu__xf_out_k(__m128 s, int k) {
    return (k & 8) ? spu__xs(_mm_castps_si128(s)) : spu__xf_out(s);
}
SPU__XI __m128 spu__xf(u128 a) {
    return _mm_castsi128_ps(_mm_min_epi32(_mm_min_epu32(spu__xl(a), _mm_set1_epi32((int)0xFF7FFFFFu)), _mm_set1_epi32(0x7F7FFFFF)));
}
#if defined(__FMA__)
#define SPU__XFMA(a, b, c)  _mm_fmadd_ps(a, b, c)
#define SPU__XFMS(a, b, c)  _mm_fmsub_ps(a, b, c)
#define SPU__XFNMS(a, b, c) _mm_fnmadd_ps(a, b, c)
#else
#define SPU__XFMA(a, b, c)  _mm_add_ps(_mm_mul_ps(a, b), c)
#define SPU__XFMS(a, b, c)  _mm_sub_ps(_mm_mul_ps(a, b), c)
#define SPU__XFNMS(a, b, c) _mm_sub_ps(c, _mm_mul_ps(a, b))
#endif
SPU__XI __m128 spu__xfk(u128 a, int clean) { return clean ? _mm_castsi128_ps(spu__xl(a)) : spu__xf(a); }
SPU__XI __m128 spu__xfko(u128 a, int clean) { __m128 x = spu__xfk(a, clean); __asm__("" : "+x"(x)); return x; }
static inline u128 spu_fa_k(u128 a, u128 b, int k) { return spu__xf_out_k(_mm_add_ps(spu__xfko(a, k & 1), spu__xfk(b, k & 2)), k); }
static inline u128 spu_fs_k(u128 a, u128 b, int k) { return spu__xf_out_k(_mm_sub_ps(spu__xfko(a, k & 1), spu__xfk(b, k & 2)), k); }
static inline u128 spu_fm_k(u128 a, u128 b, int k) { return spu__xf_out_k(_mm_mul_ps(spu__xfko(a, k & 1), spu__xfk(b, k & 2)), k); }
static inline u128 spu_fma_k(u128 a, u128 b, u128 c, int k)  { return spu__xf_out_k(SPU__XFMA(spu__xfko(a, k & 1), spu__xfk(b, k & 2), spu__xfk(c, k & 4)), k); }
static inline u128 spu_fms_k(u128 a, u128 b, u128 c, int k)  { return spu__xf_out_k(SPU__XFMS(spu__xfko(a, k & 1), spu__xfk(b, k & 2), spu__xfk(c, k & 4)), k); }
static inline u128 spu_fnms_k(u128 a, u128 b, u128 c, int k) { return spu__xf_out_k(SPU__XFNMS(spu__xfko(a, k & 1), spu__xfk(b, k & 2), spu__xfk(c, k & 4)), k); }
static inline u128 spu_fa(u128 a, u128 b) { return spu_fa_k(a, b, 0); }
static inline u128 spu_fs(u128 a, u128 b) { return spu_fs_k(a, b, 0); }
static inline u128 spu_fm(u128 a, u128 b) { return spu_fm_k(a, b, 0); }
static inline u128 spu_fma(u128 a, u128 b, u128 c)  { return spu_fma_k(a, b, c, 0); }
static inline u128 spu_fms(u128 a, u128 b, u128 c)  { return spu_fms_k(a, b, c, 0); }
static inline u128 spu_fnms(u128 a, u128 b, u128 c) { return spu_fnms_k(a, b, c, 0); }
#else
static inline float spu__sf_f(uint32_t x) {
    if ((x & 0x7F800000u) == 0x7F800000u) x = (x & 0x80000000u) | 0x7F7FFFFFu;
    float f; memcpy(&f, &x, 4); return f;
}
static inline uint32_t spu__sf_out(float s) {
    if (!(s <= 0x1.fffffep127f)) s = 0x1.fffffep127f;
    if (s < -0x1.fffffep127f) s = -0x1.fffffep127f;
    if (s > -0x1p-126f && s < 0x1p-126f) return 0;
    uint32_t u; memcpy(&u, &s, 4); return u;
}
SPU__SF_V2(spu_fa, spu__sf_out(spu__sf_f(x) + spu__sf_f(y)))
SPU__SF_V2(spu_fs, spu__sf_out(spu__sf_f(x) - spu__sf_f(y)))
SPU__SF_V2(spu_fm, spu__sf_out(spu__sf_f(x) * spu__sf_f(y)))
SPU__SF_V3(spu_fma,  spu__sf_out(fmaf(spu__sf_f(x), spu__sf_f(y), spu__sf_f(z))))
SPU__SF_V3(spu_fms,  spu__sf_out(fmaf(spu__sf_f(x), spu__sf_f(y), -spu__sf_f(z))))
SPU__SF_V3(spu_fnms, spu__sf_out(fmaf(-spu__sf_f(x), spu__sf_f(y), spu__sf_f(z))))
static inline u128 spu_fa_k(u128 a, u128 b, int k) { (void)k; return spu_fa(a, b); }
static inline u128 spu_fs_k(u128 a, u128 b, int k) { (void)k; return spu_fs(a, b); }
static inline u128 spu_fm_k(u128 a, u128 b, int k) { (void)k; return spu_fm(a, b); }
static inline u128 spu_fma_k(u128 a, u128 b, u128 c, int k)  { (void)k; return spu_fma(a, b, c); }
static inline u128 spu_fms_k(u128 a, u128 b, u128 c, int k)  { (void)k; return spu_fms(a, b, c); }
static inline u128 spu_fnms_k(u128 a, u128 b, u128 c, int k) { (void)k; return spu_fnms(a, b, c); }
#endif
static inline int64_t spu__sf_key(uint32_t x) {
    if (!(x & 0x7F800000u)) return 0;
    const int64_t m = x & 0x7FFFFFFFu;
    return (x & 0x80000000u) ? -m : m;
}
static inline int64_t spu__sf_mag(uint32_t x) { return (x & 0x7F800000u) ? (int64_t)(x & 0x7FFFFFFFu) : 0; }
SPU__SF_V2(spu_fceq_ref,  spu__sf_key(x) == spu__sf_key(y) ? 0xFFFFFFFFu : 0u)
SPU__SF_V2(spu_fcgt_ref,  spu__sf_key(x) >  spu__sf_key(y) ? 0xFFFFFFFFu : 0u)
SPU__SF_V2(spu_fcmeq_ref, spu__sf_mag(x) == spu__sf_mag(y) ? 0xFFFFFFFFu : 0u)
SPU__SF_V2(spu_fcmgt_ref, spu__sf_mag(x) >  spu__sf_mag(y) ? 0xFFFFFFFFu : 0u)
#if defined(__SSSE3__) && !defined(SPU_SCALAR_HELPERS)
#include <tmmintrin.h>
static inline __m128i spu__sse_mag(__m128i x) {
    const __m128i m = _mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF));
    return _mm_and_si128(m, _mm_cmpgt_epi32(m, _mm_set1_epi32(0x7FFFFF)));
}
static inline __m128i spu__sse_key(__m128i x) { return _mm_sign_epi32(spu__sse_mag(x), x); }
#define SPU__SSE_CMP(name, cmp, key) static inline u128 name(u128 a, u128 b) { \
    __m128i A, B; memcpy(&A, &a, 16); memcpy(&B, &b, 16); \
    const __m128i r = cmp(key(A), key(B)); u128 out; memcpy(&out, &r, 16); return out; }
SPU__SSE_CMP(spu_fceq,  _mm_cmpeq_epi32, spu__sse_key)
SPU__SSE_CMP(spu_fcgt,  _mm_cmpgt_epi32, spu__sse_key)
SPU__SSE_CMP(spu_fcmeq, _mm_cmpeq_epi32, spu__sse_mag)
SPU__SSE_CMP(spu_fcmgt, _mm_cmpgt_epi32, spu__sse_mag)
#undef SPU__SSE_CMP
#else
static inline u128 spu_fceq(u128 a, u128 b)  { return spu_fceq_ref(a, b); }
static inline u128 spu_fcgt(u128 a, u128 b)  { return spu_fcgt_ref(a, b); }
static inline u128 spu_fcmeq(u128 a, u128 b) { return spu_fcmeq_ref(a, b); }
static inline u128 spu_fcmgt(u128 a, u128 b) { return spu_fcmgt_ref(a, b); }
#endif
#undef SPU__SF_V2
#undef SPU__SF_V3

static inline uint32_t spu__cfl_lane(uint32_t x, int scale, int is_unsigned) {
    uint32_t m; int e;
    if (!spu__sf_split(x, &m, &e)) return 0;
    const int sh = e + scale, neg = (x >> 31) != 0;
    if (is_unsigned) {
        if (neg) return 0;
        if (sh >= 9) return 0xFFFFFFFFu;
        return sh >= 0 ? m << sh : (sh <= -24 ? 0 : m >> -sh);
    }
    if (sh >= 8) return neg ? 0x80000000u : 0x7FFFFFFFu;
    const uint32_t v = sh >= 0 ? m << sh : (sh <= -24 ? 0 : m >> -sh);
    return neg ? 0u - v : v;
}
static inline u128 spu_cflts_ref(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = spu__cfl_lane(a._u32[i], 173 - (i8 & 0xFF), 0); return r; }
static inline u128 spu_cfltu_ref(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = spu__cfl_lane(a._u32[i], 173 - (i8 & 0xFF), 1); return r; }
static inline u128 spu_csflt_ref(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) { const int32_t v = a._s32[i];
    r._u32[i] = v ? spu__sf_pack(v < 0 ? 0x80000000u : 0, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, -(155 - (i8 & 0xFF))) : 0; } return r; }
static inline u128 spu_cuflt_ref(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) { const uint32_t v = a._u32[i];
    r._u32[i] = v ? spu__sf_pack(0, v, -(155 - (i8 & 0xFF))) : 0; } return r; }

static inline u128 spu_fi_ref(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        const uint32_t est = b._u32[i];
        const uint32_t sign = est & 0x80000000u;
        int exp = (int)((est >> 23) & 0xFFu);
        const uint64_t base = (est >> 10) & 0x1FFFu;
        const uint64_t step = est & 0x3FFu;
        const uint64_t y    = a._u32[i] & 0x7FFFFu;
        if (exp == 0) { r._u32[i] = 0; continue; }
        uint64_t m = ((((uint64_t)1 << 13) | base) << 19) - step * y;
        if (m < ((uint64_t)1 << 32)) {
            m <<= 1;
            if (--exp == 0) { r._u32[i] = 0; continue; }
        }
        const uint32_t frac = (uint32_t)((m >> 9) & 0x7FFFFFu);
        r._u32[i] = sign | ((uint32_t)exp << 23) | frac;
    }
    return r;
}

static inline uint64_t spu__dw_get(u128 r, int i) { return ((uint64_t)r._u32[2*i] << 32) | r._u32[2*i+1]; }
static inline void spu__dw_put(u128* r, int i, uint64_t u) { r->_u32[2*i] = (uint32_t)(u >> 32); r->_u32[2*i+1] = (uint32_t)u; }
static inline int spu__d_isnan(uint64_t u) { return (u & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline double spu__d_in(uint64_t u) {
    if (!(u & 0x7FF0000000000000ull)) u &= 0x8000000000000000ull;
    double d; memcpy(&d, &u, 8); return d;
}
static inline uint64_t spu__d_out(double d) {
    uint64_t u; memcpy(&u, &d, 8);
    return spu__d_isnan(u) ? 0x7FF8000000000000ull : u;
}
#define SPU__D_V2(name, op) static inline u128 name(u128 a, u128 b) { u128 r; for (int i = 0; i < 2; i++) { \
    const uint64_t x = spu__dw_get(a, i), y = spu__dw_get(b, i); \
    spu__dw_put(&r, i, (spu__d_isnan(x) || spu__d_isnan(y)) ? 0x7FF8000000000000ull \
                       : spu__d_out(spu__d_in(x) op spu__d_in(y))); } return r; }
SPU__D_V2(spu_dfa_h_, +)
SPU__D_V2(spu_dfs_h_, -)
SPU__D_V2(spu_dfm_h_, *)
#undef SPU__D_V2
static inline u128 spu__dfmx(u128 a, u128 b, u128 c, int negate, int subtract) { u128 r;
    for (int i = 0; i < 2; i++) {
        const uint64_t x = spu__dw_get(a, i), y = spu__dw_get(b, i), z = spu__dw_get(c, i);
        if (spu__d_isnan(x) || spu__d_isnan(y) || spu__d_isnan(z)) { spu__dw_put(&r, i, 0x7FF8000000000000ull); continue; }
        const double zz = subtract ? -spu__d_in(z) : spu__d_in(z);
        const double v = fma(spu__d_in(x), spu__d_in(y), zz);
        const uint64_t u = spu__d_out(v);
        spu__dw_put(&r, i, (negate && !spu__d_isnan(u)) ? u ^ 0x8000000000000000ull : u);
    }
    return r; }
static inline u128 spu_dfma_h_(u128 a, u128 b, u128 t)  { return spu__dfmx(a, b, t, 0, 0); }
static inline u128 spu_dfms_h_(u128 a, u128 b, u128 t)  { return spu__dfmx(a, b, t, 0, 1); }
static inline u128 spu_dfnms_h_(u128 a, u128 b, u128 t) { return spu__dfmx(a, b, t, 1, 1); }
static inline u128 spu_dfnma_h_(u128 a, u128 b, u128 t) { return spu__dfmx(a, b, t, 1, 0); }
static inline u128 spu_fesd(u128 a) { u128 r;
    for (int i = 0; i < 2; i++) {
        const uint32_t w = a._u32[2*i], be = (w >> 23) & 0xFFu, f = w & 0x7FFFFFu;
        const uint64_t sign = (uint64_t)(w >> 31) << 63;
        uint64_t u;
        if (be == 0)        u = f ? 0 : sign;
        else if (be == 255) u = f ? 0x7FF8000000000000ull : (sign | 0x7FF0000000000000ull);
        else                u = sign | ((uint64_t)(be - 127 + 1023) << 52) | ((uint64_t)f << 29);
        spu__dw_put(&r, i, u);
    }
    return r; }
static inline u128 spu_frds_h_(u128 a) { u128 r;
    for (int i = 0; i < 2; i++) {
        const uint64_t u = spu__dw_get(a, i);
        if (spu__d_isnan(u)) { r._u32[2*i] = 0x7FC00000u; r._u32[2*i+1] = 0; continue; }
        const float f = (float)spu__d_in(u);
        uint32_t w; memcpy(&w, &f, 4);
        r._u32[2*i] = w; r._u32[2*i+1] = 0;
    }
    return r; }


static inline double spu__dget(u128 r, int i) {
    uint64_t u = ((uint64_t)r._u32[i*2] << 32) | r._u32[i*2+1];
    double d; memcpy(&d, &u, sizeof d); return d;
}
static inline u128 spu_dfceq_h_(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)==spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmeq_h_(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x==y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_mpyhhu_ref(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
static inline u128 spu_cgx_ref(u128 a, u128 b, u128 t){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)(t._u32[i]&1u)+a._u32[i]+b._u32[i])>>32); return r; }

#if SPU_X86
static inline u128 spu_eqv(u128 a, u128 b)   { return spu__xs(_mm_xor_si128(_mm_xor_si128(spu__xl(a), spu__xl(b)), _mm_set1_epi32(-1))); }
static inline u128 spu_xorbi(u128 a, int32_t imm){ return spu__xs(_mm_xor_si128(spu__xl(a), _mm_set1_epi8((char)imm))); }
#else
static inline u128 spu_eqv(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=~(a._u32[i]^b._u32[i]); return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
#endif
static inline u128 spu_absdb_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<16;i++){ uint8_t x=a._u8[i],y=b._u8[i]; r._u8[i]=(uint8_t)(x>y?x-y:y-x); } return r; }
static inline u128 spu_avgb_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
static inline u128 spu_mpyhha_ref(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=t._s32[i]+(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpyhhau_ref(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=t._u32[i]+(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
static inline u128 spu_dfcgt_h_(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)>spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmgt_h_(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x>y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }

#if defined(__aarch64__) || (defined(__x86_64__) && defined(__AVX2__) && !defined(SPU_SCALAR_HELPERS))
#if defined(__aarch64__)
static inline uint64_t spu__fpcr_now(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void spu__fpcr_put(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
#define SPU__FP_DEFAULT 0u
#define SPU__FP_IS_DEFAULT(m) ((m) == 0)
#else
static inline uint64_t spu__fpcr_now(void) { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static inline void spu__fpcr_put(uint64_t v) { const uint32_t x = (uint32_t)v; __asm__ volatile("ldmxcsr %0" :: "m"(x)); }
#define SPU__FP_DEFAULT 0x1F80u
#define SPU__FP_IS_DEFAULT(m) (((m) & SPU_MXCSR_RZ) == SPU__FP_DEFAULT)
#endif
#define SPU__DP_RN1(f) static inline u128 f(u128 a) { const uint64_t m = spu__fpcr_now(); \
    if (__builtin_expect(SPU__FP_IS_DEFAULT(m), 1)) return f##_h_(a); \
    spu__fpcr_put(SPU__FP_DEFAULT); __asm__ volatile("" : "+m"(a)); u128 r = f##_h_(a); \
    __asm__ volatile("" : "+m"(r)); spu__fpcr_put(m); return r; }
#define SPU__DP_RN2(f) static inline u128 f(u128 a, u128 b) { const uint64_t m = spu__fpcr_now(); \
    if (__builtin_expect(SPU__FP_IS_DEFAULT(m), 1)) return f##_h_(a, b); \
    spu__fpcr_put(SPU__FP_DEFAULT); __asm__ volatile("" : "+m"(a), "+m"(b)); u128 r = f##_h_(a, b); \
    __asm__ volatile("" : "+m"(r)); spu__fpcr_put(m); return r; }
#define SPU__DP_RN3(f) static inline u128 f(u128 a, u128 b, u128 c) { const uint64_t m = spu__fpcr_now(); \
    if (__builtin_expect(SPU__FP_IS_DEFAULT(m), 1)) return f##_h_(a, b, c); \
    spu__fpcr_put(SPU__FP_DEFAULT); __asm__ volatile("" : "+m"(a), "+m"(b), "+m"(c)); u128 r = f##_h_(a, b, c); \
    __asm__ volatile("" : "+m"(r)); spu__fpcr_put(m); return r; }
#else
#define SPU__DP_RN1(f) static inline u128 f(u128 a) { return f##_h_(a); }
#define SPU__DP_RN2(f) static inline u128 f(u128 a, u128 b) { return f##_h_(a, b); }
#define SPU__DP_RN3(f) static inline u128 f(u128 a, u128 b, u128 c) { return f##_h_(a, b, c); }
#endif
SPU__DP_RN2(spu_dfa) SPU__DP_RN2(spu_dfs) SPU__DP_RN2(spu_dfm)
SPU__DP_RN3(spu_dfma) SPU__DP_RN3(spu_dfms) SPU__DP_RN3(spu_dfnms) SPU__DP_RN3(spu_dfnma)
SPU__DP_RN1(spu_frds)
SPU__DP_RN2(spu_dfceq) SPU__DP_RN2(spu_dfcmeq) SPU__DP_RN2(spu_dfcgt) SPU__DP_RN2(spu_dfcmgt)
static inline uint64_t spu__dftsv1(uint64_t v, uint32_t i7) {
    uint64_t e = (v >> 52) & 0x7FF, f = v & 0xFFFFFFFFFFFFFull; int neg = (int)(v >> 63);
    int t = 0;
    if (e == 0x7FF && f) t |= i7 & 0x40;
    if (e == 0x7FF && !f) t |= i7 & (neg ? 0x10 : 0x20);
    if (e == 0 && !f) t |= i7 & (neg ? 0x04 : 0x08);
    if (e == 0 && f) t |= i7 & (neg ? 0x01 : 0x02);
    return t ? ~0ull : 0ull;
}
static inline u128 spu_dftsv(u128 a, int32_t imm){ u128 r;
    for (int i = 0; i < 2; i++) { uint64_t v = (uint64_t)a._u32[2*i] << 32 | a._u32[2*i+1];
        uint64_t m = spu__dftsv1(v, (uint32_t)imm & 0x7F); r._u32[2*i] = (uint32_t)(m >> 32); r._u32[2*i+1] = (uint32_t)m; }
    return r; }

static inline u128 spu_shl_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); } return r; }
static inline u128 spu_shlh_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); } return r; }
static inline u128 spu_rot_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&31; r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; } return r; }
static inline u128 spu_roth_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&15; r._u16[i]= sh ? (uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))) : a._u16[i]; } return r; }
static inline u128 spu_shlqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_shlqby_ref(u128 a, u128 b){ int sh=b._u32[0]&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_shlqby(u128 a, u128 b) {
    return spu__perm(a, vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)(b._u32[0] & 0x1F))));
}
#elif SPU_X86
static inline u128 spu_shlqby(u128 a, u128 b) {
    return spu__xperm(a, spu__xshl((int)(b._u32[0] & 0x1F)));
}
#else
static inline u128 spu_shlqby(u128 a, u128 b) { return spu_shlqby_ref(a, b); }
#endif
static inline u128 spu_rotqby_ref(u128 a, u128 b){ int sh=b._u32[0]&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqby(u128 a, u128 b) {
    return spu__perm(a, vandq_u8(vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)b._u32[0])), vdupq_n_u8(15)));
}
#elif SPU_X86
static inline u128 spu_rotqby(u128 a, u128 b) {
    return spu__xrotl(a, b._u32[0]);
}
#else
static inline u128 spu_rotqby(u128 a, u128 b) { return spu_rotqby_ref(a, b); }
#endif
static inline u128 spu_shlqbybi_ref(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_shlqbybi(u128 a, u128 b) {
    return spu__perm(a, vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)((b._u32[0] >> 3) & 0x1F))));
}
#elif SPU_X86
static inline u128 spu_shlqbybi(u128 a, u128 b) {
    return spu__xperm(a, spu__xshl((int)((b._u32[0] >> 3) & 0x1F)));
}
#else
static inline u128 spu_shlqbybi(u128 a, u128 b) { return spu_shlqbybi_ref(a, b); }
#endif
static inline u128 spu_rotqbybi_ref(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqbybi(u128 a, u128 b) {
    return spu__perm(a, vandq_u8(vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)(b._u32[0] >> 3))), vdupq_n_u8(15)));
}
#elif SPU_X86
static inline u128 spu_rotqbybi(u128 a, u128 b) {
    return spu__xrotl(a, b._u32[0] >> 3);
}
#else
static inline u128 spu_rotqbybi(u128 a, u128 b) { return spu_rotqbybi_ref(a, b); }
#endif

static inline u128 spu_rotmahi_ref(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }

static inline u128 spu_ceqbi_ref(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_ceqhi_ref(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_clgtbi_ref(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgthi_ref(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi_ref(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgtbi_ref(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }

static inline u128 spu_fscrrd(u128 a) { (void)a; return spu_zero(); }
static inline u128 spu_gb_ref(u128 a) {
    uint32_t v = ((a._u32[0]&1)<<3)|((a._u32[1]&1)<<2)|((a._u32[2]&1)<<1)|(a._u32[3]&1);
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_gbh_ref(u128 a) {
    uint32_t v=0; for(int i=0;i<8;i++) v |= ((uint32_t)(a._u16[i^1]&1) << (7-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_gbb_ref(u128 a) {
    uint32_t v=0; for(int i=0;i<16;i++) v |= ((uint32_t)(a._u8[SPU_W(i)]&1) << (15-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_cg_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)a._u32[i]+(uint64_t)b._u32[i])>>32); return r; }
static inline u128 spu_sumb_ref(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint32_t wa=a._u32[i], wb=b._u32[i];
        uint32_t sa=((wa>>24)&0xFF)+((wa>>16)&0xFF)+((wa>>8)&0xFF)+(wa&0xFF);
        uint32_t sb=((wb>>24)&0xFF)+((wb>>16)&0xFF)+((wb>>8)&0xFF)+(wb&0xFF);
        r._u32[i]=(sb<<16)|sa;
    }
    return r;
}
static inline u128 spu_bg_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)b._u32[i]+(uint64_t)(~a._u32[i])+1u)>>32); return r; }
static inline u128 spu_addx_ref(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]+(t._u32[i]&1); return r; }
static inline u128 spu_mpyh_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._s32[i]=((int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i]) << 16; return r; }
static inline u128 spu_mpyhh_ref(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpys_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++){ int32_t p=(int32_t)a._s16[2*i]*(int32_t)b._s16[2*i]; r._s32[i]=(int16_t)(p>>16); } return r; }
static inline u128 spu_mpyui_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i]*(uint32_t)(uint16_t)imm; return r; }
static const uint32_t spu_frest_fraction_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07
};

static const uint32_t spu_frsqest_fraction_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA
};

static inline u128 spu_frsqest_ref(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x3Fu;
        uint32_t rexp = (exp == 0u) ? 0xFFu : (190u - (exp + 1u) / 2u);
        uint32_t fraction = spu_frsqest_fraction_lut[frac_idx];
        uint32_t out = (rexp << 23) | (fraction & 0x7FFFFFu);
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}
static inline u128 spu_frest_ref(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t sign = bits & 0x80000000u;
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x1Fu;
        uint32_t rexp = (exp == 0u) ? 255u : (exp >= 253u) ? 0u : (253u - exp);
        uint32_t out = sign | (rexp << 23) | (spu_frest_fraction_lut[frac_idx] & 0x7FFFFFu);
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}

#if defined(__AVX2__) && !defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
#include <immintrin.h>
#define SPU__XE_INLINE static inline __attribute__((always_inline))
SPU__XE_INLINE __m128i spu__xe_ld(u128 a) { return _mm_loadu_si128((const __m128i*)a._u32); }
SPU__XE_INLINE u128 spu__xe_st(__m128i v) { u128 r; _mm_storeu_si128((__m128i*)r._u32, v); return r; }
SPU__XE_INLINE __m128i spu__xe_narrow(__m256i v) {
    return _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(v, _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6)));
}
SPU__XE_INLINE u128 spu_fi(u128 a, u128 b) {
    const __m128i est = spu__xe_ld(b), y = _mm_and_si128(spu__xe_ld(a), _mm_set1_epi32(0x7FFFF));
    const __m128i exp = _mm_and_si128(_mm_srli_epi32(est, 23), _mm_set1_epi32(0xFF));
    const __m128i b19 = _mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(est, 10), _mm_set1_epi32(0x1FFF)), 19);
    const __m128i sy = _mm_mullo_epi32(_mm_and_si128(est, _mm_set1_epi32(0x3FF)), y);
    const __m128i big = _mm_cmpeq_epi32(_mm_max_epu32(b19, sy), b19);
    const __m128i d = _mm_sub_epi32(b19, sy);
    const __m128i frac = _mm_and_si128(_mm_blendv_epi8(_mm_srli_epi32(d, 8), _mm_srli_epi32(d, 9), big),
                                       _mm_set1_epi32(0x7FFFFF));
    const __m128i e2 = _mm_sub_epi32(exp, _mm_andnot_si128(big, _mm_set1_epi32(1)));
    const __m128i r = _mm_or_si128(_mm_or_si128(_mm_and_si128(est, _mm_set1_epi32((int)0x80000000u)),
                                                _mm_slli_epi32(e2, 23)), frac);
    const __m128i zero = _mm_or_si128(_mm_cmpeq_epi32(exp, _mm_setzero_si128()), _mm_cmpeq_epi32(e2, _mm_setzero_si128()));
    return spu__xe_st(_mm_andnot_si128(zero, r));
}
SPU__XE_INLINE __m128i spu__xe_lut(const uint32_t* tbl, int n, __m128i idx) {
    const __m256i i8 = _mm256_castsi128_si256(idx);
    const __m128i row = _mm_srli_epi32(idx, 3);
    __m128i r = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)tbl), i8));
    for (int k = 1; k < n; k++) {
        const __m128i v = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)(tbl + 8 * k)), i8));
        r = _mm_blendv_epi8(r, v, _mm_cmpeq_epi32(row, _mm_set1_epi32(k)));
    }
    return r;
}
SPU__XE_INLINE u128 spu_frest(u128 a) {
    const __m128i x = spu__xe_ld(a);
    const __m128i exp = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    const __m128i idx = _mm_and_si128(_mm_srli_epi32(x, 18), _mm_set1_epi32(0x1F));
    const __m128i frac = _mm_and_si128(spu__xe_lut(spu_frest_fraction_lut, 4, idx), _mm_set1_epi32(0x7FFFFF));
    __m128i rexp = _mm_sub_epi32(_mm_set1_epi32(253), exp);
    rexp = _mm_andnot_si128(_mm_cmpgt_epi32(exp, _mm_set1_epi32(252)), rexp);
    rexp = _mm_blendv_epi8(rexp, _mm_set1_epi32(255), _mm_cmpeq_epi32(exp, _mm_setzero_si128()));
    return spu__xe_st(_mm_or_si128(_mm_or_si128(_mm_and_si128(x, _mm_set1_epi32((int)0x80000000u)),
                                                _mm_slli_epi32(rexp, 23)), frac));
}
SPU__XE_INLINE u128 spu_frsqest(u128 a) {
    const __m128i x = spu__xe_ld(a);
    const __m128i exp = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    const __m128i idx = _mm_and_si128(_mm_srli_epi32(x, 18), _mm_set1_epi32(0x3F));
    const __m128i frac = _mm_and_si128(spu__xe_lut(spu_frsqest_fraction_lut, 8, idx), _mm_set1_epi32(0x7FFFFF));
    __m128i rexp = _mm_sub_epi32(_mm_set1_epi32(190), _mm_srli_epi32(_mm_add_epi32(exp, _mm_set1_epi32(1)), 1));
    rexp = _mm_blendv_epi8(rexp, _mm_set1_epi32(255), _mm_cmpeq_epi32(exp, _mm_setzero_si128()));
    return spu__xe_st(_mm_or_si128(_mm_slli_epi32(rexp, 23), frac));
}
SPU__XE_INLINE __m128i spu__xe_cfl(u128 a, int i8, int is_unsigned) {
    const __m128i x = spu__xe_ld(a);
    const __m128i be = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    const __m128i m = _mm_or_si128(_mm_and_si128(x, _mm_set1_epi32(0x7FFFFF)), _mm_set1_epi32(0x800000));
    const __m128i sh = _mm_add_epi32(be, _mm_set1_epi32(23 - (i8 & 0xFF)));
    const __m128i lsh = _mm_max_epi32(sh, _mm_setzero_si128());
    const __m128i rsh = _mm_max_epi32(_mm_sub_epi32(_mm_setzero_si128(), sh), _mm_setzero_si128());
    __m128i v = _mm_blendv_epi8(_mm_srlv_epi32(m, rsh), _mm_sllv_epi32(m, lsh),
                                _mm_cmpgt_epi32(sh, _mm_set1_epi32(-1)));
    const __m128i neg = _mm_srai_epi32(x, 31), isz = _mm_cmpeq_epi32(be, _mm_setzero_si128());
    if (is_unsigned) {
        v = _mm_or_si128(v, _mm_cmpgt_epi32(sh, _mm_set1_epi32(8)));
        return _mm_andnot_si128(_mm_or_si128(neg, isz), v);
    }
    v = _mm_sub_epi32(_mm_xor_si128(v, neg), neg);
    const __m128i sat = _mm_xor_si128(_mm_set1_epi32(0x7FFFFFFF), neg);
    v = _mm_blendv_epi8(v, sat, _mm_cmpgt_epi32(sh, _mm_set1_epi32(7)));
    return _mm_andnot_si128(isz, v);
}
SPU__XE_INLINE u128 spu_cflts(u128 a, int i8) { return spu__xe_st(spu__xe_cfl(a, i8, 0)); }
SPU__XE_INLINE u128 spu_cfltu(u128 a, int i8) { return spu__xe_st(spu__xe_cfl(a, i8, 1)); }
SPU__XE_INLINE __m128i spu__xe_cxflt(__m256d d, __m128i sign, __m128i nz, int i8) {
    const __m256i u = _mm256_castpd_si256(d);
    const __m128i top = _mm_sub_epi32(spu__xe_narrow(_mm256_and_si256(_mm256_srli_epi64(u, 52), _mm256_set1_epi64x(0x7FF))),
                                      _mm_set1_epi32(1023));
    const __m128i frac = _mm_and_si128(spu__xe_narrow(_mm256_srli_epi64(u, 29)), _mm_set1_epi32(0x7FFFFF));
    const __m128i biased = _mm_add_epi32(top, _mm_set1_epi32((i8 & 0xFF) - 28));
    __m128i r = _mm_or_si128(sign, _mm_or_si128(_mm_slli_epi32(biased, 23), frac));
    r = _mm_blendv_epi8(r, _mm_or_si128(sign, _mm_set1_epi32(0x7FFFFFFF)), _mm_cmpgt_epi32(biased, _mm_set1_epi32(255)));
    const __m128i flush = _mm_cmpgt_epi32(_mm_set1_epi32(1), biased);
    return _mm_and_si128(_mm_andnot_si128(flush, r), nz);
}
SPU__XE_INLINE u128 spu_csflt(u128 a, int i8) {
    const __m128i v = spu__xe_ld(a);
    const int k = i8 & 0xFF;
    if (k >= 29 && k <= 251) {
        __m128 f = _mm_cvtepi32_ps(v);
        if (k != 155) {
            const uint32_t scale_bits = (uint32_t)(127 + (k - 155)) << 23;
            f = _mm_mul_ps(f, _mm_castsi128_ps(_mm_set1_epi32((int)scale_bits)));
        }
        return spu__xe_st(_mm_castps_si128(f));
    }
    const __m256d d = _mm256_andnot_pd(_mm256_set1_pd(-0.0), _mm256_cvtepi32_pd(v));
    const __m128i sign = _mm_and_si128(v, _mm_set1_epi32((int)0x80000000u));
    const __m128i nz = _mm_xor_si128(_mm_cmpeq_epi32(v, _mm_setzero_si128()), _mm_set1_epi32(-1));
    return spu__xe_st(spu__xe_cxflt(d, sign, nz, i8));
}
SPU__XE_INLINE u128 spu_cuflt(u128 a, int i8) {
    const __m128i v = spu__xe_ld(a);
    const __m256d d = _mm256_add_pd(_mm256_cvtepi32_pd(_mm_xor_si128(v, _mm_set1_epi32((int)0x80000000u))),
                                    _mm256_set1_pd(2147483648.0));
    const __m128i nz = _mm_xor_si128(_mm_cmpeq_epi32(v, _mm_setzero_si128()), _mm_set1_epi32(-1));
    return spu__xe_st(spu__xe_cxflt(d, _mm_setzero_si128(), nz, i8));
}
#elif defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
#include <arm_neon.h>
static inline u128 spu_fi(u128 a, u128 b) {
    uint32x4_t est, av; memcpy(&est, &b, 16); memcpy(&av, &a, 16);
    const uint32x4_t exp  = vandq_u32(vshrq_n_u32(est, 23), vdupq_n_u32(0xFFu));
    const uint32x4_t u0   = vshlq_n_u32(vandq_u32(vshrq_n_u32(est, 10), vdupq_n_u32(0x1FFFu)), 19);
    const uint32x4_t t    = vmulq_u32(vandq_u32(est, vdupq_n_u32(0x3FFu)), vandq_u32(av, vdupq_n_u32(0x7FFFFu)));
    const uint32x4_t u    = vsubq_u32(u0, t);
    const uint32x4_t brw  = vcltq_u32(u0, t);
    const uint32x4_t frac = vbslq_u32(brw, vandq_u32(vshrq_n_u32(u, 8), vdupq_n_u32(0x7FFFFFu)), vshrq_n_u32(u, 9));
    const uint32x4_t e2   = vaddq_u32(exp, brw);
    uint32x4_t r = vorrq_u32(vorrq_u32(vandq_u32(est, vdupq_n_u32(0x80000000u)), vshlq_n_u32(e2, 23)), frac);
    r = vbicq_u32(r, vorrq_u32(vceqzq_u32(exp), vceqzq_u32(e2)));
    u128 out; memcpy(&out, &r, 16); return out;
}
static inline u128 spu_frest(u128 a)           { return spu_frest_ref(a); }
static inline u128 spu_frsqest(u128 a)         { return spu_frsqest_ref(a); }
static inline u128 spu_cflts(u128 a, int i8) {
    const int scale = 173 - (i8 & 0xFF);
    if (scale < 0 || scale > 127) return spu_cflts_ref(a, i8);
    uint32x4_t x; memcpy(&x, &a, 16);
    const float32x4_t f = vmulq_f32(vreinterpretq_f32_u32(x),
                                    vreinterpretq_f32_u32(vdupq_n_u32((uint32_t)(scale + 127) << 23)));
    uint32x4_t r = vreinterpretq_u32_s32(vcvtq_s32_f32(f));
    const uint32x4_t e = vandq_u32(vshrq_n_u32(x, 23), vdupq_n_u32(0xFFu));
    const uint32x4_t sat = vaddq_u32(vshrq_n_u32(x, 31), vdupq_n_u32(0x7FFFFFFFu));
    r = vbslq_u32(vceqq_u32(e, vdupq_n_u32(0xFFu)), sat, r);
    r = vbicq_u32(r, vceqzq_u32(e));
    u128 out; memcpy(&out, &r, 16); return out;
}
static inline u128 spu_cfltu(u128 a, int i8)   { return spu_cfltu_ref(a, i8); }
static inline u128 spu_csflt(u128 a, int i8)   { return spu_csflt_ref(a, i8); }
static inline u128 spu_cuflt(u128 a, int i8)   { return spu_cuflt_ref(a, i8); }
#elif SPU_X86
static inline u128 spu_fi(u128 a, u128 b) {
    const __m128i est = spu__xl(b), av = spu__xl(a);
    const __m128i exp  = _mm_and_si128(_mm_srli_epi32(est, 23), _mm_set1_epi32(0xFF));
    const __m128i u0   = _mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(est, 10), _mm_set1_epi32(0x1FFF)), 19);
    const __m128i t    = _mm_mullo_epi32(_mm_and_si128(est, _mm_set1_epi32(0x3FF)), _mm_and_si128(av, _mm_set1_epi32(0x7FFFF)));
    const __m128i u    = _mm_sub_epi32(u0, t);
    const __m128i brw  = _mm_cmpgt_epi32(spu__xflip(t), spu__xflip(u0));
    const __m128i frac = _mm_blendv_epi8(_mm_srli_epi32(u, 9), _mm_and_si128(_mm_srli_epi32(u, 8), _mm_set1_epi32(0x7FFFFF)), brw);
    const __m128i e2   = _mm_add_epi32(exp, brw);
    __m128i r = _mm_or_si128(_mm_or_si128(_mm_and_si128(est, _mm_set1_epi32((int)0x80000000u)), _mm_slli_epi32(e2, 23)), frac);
    const __m128i z = _mm_setzero_si128();
    r = _mm_andnot_si128(_mm_or_si128(_mm_cmpeq_epi32(exp, z), _mm_cmpeq_epi32(e2, z)), r);
    return spu__xs(r);
}
static inline u128 spu_frest(u128 a)           { return spu_frest_ref(a); }
static inline u128 spu_frsqest(u128 a)         { return spu_frsqest_ref(a); }
static inline u128 spu_cflts(u128 a, int i8) {
    const int scale = 173 - (i8 & 0xFF);
    if (scale < 0 || scale > 127) return spu_cflts_ref(a, i8);
    const __m128i x = spu__xl(a);
    const __m128 f = scale ? _mm_mul_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(_mm_set1_epi32((int)((uint32_t)(scale + 127) << 23)))) : _mm_castsi128_ps(x);
    const __m128i e = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    const __m128 hi = _mm_cmpge_ps(f, _mm_set1_ps(2147483648.0f));
    __m128i r = _mm_blendv_epi8(_mm_cvttps_epi32(f), _mm_set1_epi32(0x7FFFFFFF), _mm_castps_si128(hi));
    const __m128i sat = _mm_add_epi32(_mm_srli_epi32(x, 31), _mm_set1_epi32(0x7FFFFFFF));
    r = _mm_blendv_epi8(r, sat, _mm_cmpeq_epi32(e, _mm_set1_epi32(0xFF)));
    r = _mm_andnot_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), r);
    return spu__xs(r);
}
static inline u128 spu_cfltu(u128 a, int i8) {
    const int scale = 173 - (i8 & 0xFF);
    if (scale < 0 || scale > 127) return spu_cfltu_ref(a, i8);
    const __m128i x = spu__xl(a);
    const __m128 f = scale ? _mm_mul_ps(_mm_castsi128_ps(x), _mm_castsi128_ps(_mm_set1_epi32((int)((uint32_t)(scale + 127) << 23)))) : _mm_castsi128_ps(x);
    const __m128 two31 = _mm_set1_ps(2147483648.0f);
    const __m128 hi = _mm_cmpge_ps(f, _mm_set1_ps(4294967296.0f));
    const __m128 big = _mm_cmpge_ps(f, two31);
    const __m128i t = _mm_cvttps_epi32(_mm_sub_ps(f, _mm_and_ps(big, two31)));
    __m128i r = _mm_xor_si128(t, _mm_and_si128(_mm_castps_si128(big), _mm_set1_epi32((int)0x80000000u)));
    const __m128i e = _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF));
    r = _mm_or_si128(r, _mm_or_si128(_mm_castps_si128(hi), _mm_cmpeq_epi32(e, _mm_set1_epi32(0xFF))));
    r = _mm_andnot_si128(_mm_or_si128(_mm_srai_epi32(x, 31), _mm_cmpeq_epi32(e, _mm_setzero_si128())), r);
    return spu__xs(r);
}
static inline u128 spu_csflt(u128 a, int i8) {
    const int scale = (i8 & 0xFF) - 155;
    if (scale < -126 || scale > 127) return spu_csflt_ref(a, i8);
    const __m128 f = _mm_cvtepi32_ps(spu__xl(a));
    if (scale == 0) return spu__xs(_mm_castps_si128(f));
    return spu__xs(_mm_castps_si128(_mm_mul_ps(f, _mm_castsi128_ps(_mm_set1_epi32((int)((uint32_t)(scale + 127) << 23))))));
}
static inline u128 spu_cuflt(u128 a, int i8) {
    const int scale = (i8 & 0xFF) - 155;
    if (scale < -126 || scale > 127) return spu_cuflt_ref(a, i8);
    const __m128i u = spu__xl(a);
    const __m128 hi = _mm_cvtepi32_ps(_mm_srli_epi32(u, 16));
    const __m128 lo = _mm_cvtepi32_ps(_mm_and_si128(u, _mm_set1_epi32(0xFFFF)));
    const __m128 f = SPU__XFMA(hi, _mm_set1_ps(65536.0f), lo);
    if (scale == 0) return spu__xs(_mm_castps_si128(f));
    return spu__xs(_mm_castps_si128(_mm_mul_ps(f, _mm_castsi128_ps(_mm_set1_epi32((int)((uint32_t)(scale + 127) << 23))))));
}
#else
static inline u128 spu_fi(u128 a, u128 b)      { return spu_fi_ref(a, b); }
static inline u128 spu_frest(u128 a)           { return spu_frest_ref(a); }
static inline u128 spu_frsqest(u128 a)         { return spu_frsqest_ref(a); }
static inline u128 spu_cflts(u128 a, int i8)   { return spu_cflts_ref(a, i8); }
static inline u128 spu_cfltu(u128 a, int i8)   { return spu_cfltu_ref(a, i8); }
static inline u128 spu_csflt(u128 a, int i8)   { return spu_csflt_ref(a, i8); }
static inline u128 spu_cuflt(u128 a, int i8)   { return spu_cuflt_ref(a, i8); }
#endif

static inline u128 spu_xsbh_ref(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_xshw_ref(u128 a) { u128 r; for(int i=0;i<4;i++) r._s32[i] = (int16_t)a._s16[2*i]; return r; }
static inline u128 spu_xswd_ref(u128 a) {
    u128 r;
    for (int d = 0; d < 2; d++) {
        int32_t v = (int32_t)a._u32[2*d + 1];
        r._u32[2*d]     = (uint32_t)(v >> 31);
        r._u32[2*d + 1] = (uint32_t)v;
    }
    return r;
}

static inline u128 spu_orx_ref(u128 a) {
    u128 r = spu_zero();
    r._u32[0] = a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3];
    return r;
}

static inline u128 spu_fsm_ref(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xF;
    for(int i=0;i<4;i++) r._u32[i] = ((v>>(3-i))&1) ? 0xFFFFFFFFu : 0;
    return r;
}
static inline u128 spu_fsmh_ref(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFF;
    for(int i=0;i<8;i++) r._u16[i^1] = ((v>>(7-i))&1) ? 0xFFFFu : 0;
    return r;
}
static inline u128 spu_fsmb_ref(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}
static inline u128 spu_fsmbi(int32_t imm) {
    u128 r; uint32_t v = imm & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}

static inline u128 spu__ctl_base(void) {
    u128 r; r._u32[0] = 0x10111213u; r._u32[1] = 0x14151617u; r._u32[2] = 0x18191A1Bu; r._u32[3] = 0x1C1D1E1Fu;
    return r;
}
#if SPU_X86
SPU__XI u128 spu__ctl(__m128i m, __m128i v) {
    return spu__xs(_mm_blendv_epi8(_mm_setr_epi32(0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F), v, m));
}
static inline u128 spu_cbd_pos(int pos) {
    return spu__ctl(_mm_cmpeq_epi8(_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
                                   _mm_set1_epi8((char)((pos & 0xF) ^ 3))), _mm_set1_epi8(0x03));
}
static inline u128 spu_chd_pos(int pos) {
    return spu__ctl(_mm_cmpeq_epi16(_mm_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7), _mm_set1_epi16((short)(((pos & 0xE) >> 1) ^ 1))),
                    _mm_set1_epi16(0x0203));
}
static inline u128 spu_cwd_pos(int pos) {
    return spu__ctl(_mm_cmpeq_epi32(_mm_setr_epi32(0, 1, 2, 3), _mm_set1_epi32((pos & 0xC) >> 2)), _mm_set1_epi32(0x00010203));
}
static inline u128 spu_cdd_pos(int pos) {
    return spu__ctl(_mm_cmpeq_epi64(_mm_setr_epi32(0, 0, 1, 0), _mm_set1_epi64x((pos & 0x8) >> 3)),
                    _mm_set1_epi64x(0x0405060700010203ll));
}
#else
static inline u128 spu_cbd_pos(int pos) {
    u128 r = spu__ctl_base();
    r._u8[SPU_W(pos & 0xF)] = 0x03;
    return r;
}
static inline u128 spu_chd_pos(int pos) {
    u128 r = spu__ctl_base();
    r._u16[(((pos & 0xF) & ~1) >> 1) ^ 1] = 0x0203u;
    return r;
}
static inline u128 spu_cwd_pos(int pos) {
    u128 r = spu__ctl_base();
    r._u32[((pos & 0xF) & ~3) >> 2] = 0x00010203u;
    return r;
}
static inline u128 spu_cdd_pos(int pos) {
    u128 r = spu__ctl_base();
    const int w = ((pos & 0xF) & ~7) >> 2;
    r._u32[w] = 0x00010203u; r._u32[w + 1] = 0x04050607u;
    return r;
}
#endif
static inline u128 spu_cbd(u128 a, int i7){ return spu_cbd_pos((int)a._u32[0]+i7); }
static inline u128 spu_chd(u128 a, int i7){ return spu_chd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cwd_ref(u128 a, int i7){ return spu_cwd_pos((int)a._u32[0]+i7); }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_cwd(u128 a, int i7) {
    static const uint32_t base[4] = {0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu}, lane[4] = {0, 1, 2, 3};
    uint32x4_t m = vceqq_u32(vld1q_u32(lane), vdupq_n_u32((((uint32_t)a._u32[0] + (uint32_t)i7) >> 2) & 3));
    return spu__st(vbslq_u32(m, vdupq_n_u32(0x00010203u), vld1q_u32(base)));
}
#elif SPU_X86
static inline u128 spu_cwd(u128 a, int i7) { return spu_cwd_pos((int)a._u32[0] + i7); }
#else
static inline u128 spu_cwd(u128 a, int i7) { return spu_cwd_ref(a, i7); }
#endif
static inline u128 spu_cdd(u128 a, int i7){ return spu_cdd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cbx(u128 a, u128 b){ return spu_cbd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_chx(u128 a, u128 b){ return spu_chd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cwx(u128 a, u128 b){ return spu_cwd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cdx(u128 a, u128 b){ return spu_cdd_pos((int)(a._u32[0]+b._u32[0])); }

static inline u128 spu_rotm_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); } return r; }
static inline u128 spu_rotma_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); } return r; }
static inline u128 spu_rothm_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); } return r; }
static inline u128 spu_rothma_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); } return r; }
#if SPU_X86
static inline u128 spu_rothmi(u128 a, int i7) { int sh=(0-i7)&0x1F; return spu__xs(_mm_srl_epi16(spu__xl(a), _mm_cvtsi32_si128(sh))); }
#else
static inline u128 spu_rothmi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
#endif
static inline u128 spu_rotqmbi(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmby_ref(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqmby(u128 a, u128 b) {
    return spu__perm(a, vsubq_u8(spu__iota(), vdupq_n_u8((uint8_t)((0 - (int)b._u32[0]) & 0x1F))));
}
#elif SPU_X86
static inline u128 spu_rotqmby(u128 a, u128 b) {
    return spu__xperm(a, spu__xshr((0 - (int)b._u32[0]) & 0x1F));
}
#else
static inline u128 spu_rotqmby(u128 a, u128 b) { return spu_rotqmby_ref(a, b); }
#endif
static inline u128 spu_rotqmbybi_ref(u128 a, u128 b) { int sh=(0-((int)b._u32[0]>>3))&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqmbybi(u128 a, u128 b) {
    return spu__perm(a, vsubq_u8(spu__iota(), vdupq_n_u8((uint8_t)((0 - ((int)b._u32[0] >> 3)) & 0x1F))));
}
#elif SPU_X86
static inline u128 spu_rotqmbybi(u128 a, u128 b) {
    return spu__xperm(a, spu__xshr((0 - ((int)b._u32[0] >> 3)) & 0x1F));
}
#else
static inline u128 spu_rotqmbybi(u128 a, u128 b) { return spu_rotqmbybi_ref(a, b); }
#endif
static inline u128 spu_rotqmbii(u128 a, int i7)  { int sh=(0-i7)&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmbyi_ref(u128 a, int i7)  { int sh=(0-i7)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqmbyi(u128 a, int i7) {
    return spu__perm(a, vsubq_u8(spu__iota(), vdupq_n_u8((uint8_t)((0 - i7) & 0x1F))));
}
#elif SPU_X86
static inline u128 spu_rotqmbyi(u128 a, int i7) {
    const int s = (0 - i7) & 0x1F;
    if (s == 0)  return a;
    if (s >= 16) return spu_zero();
    if (s == 4)  return spu__xs(_mm_slli_si128(spu__xl(a), 4));
    if (s == 8)  return spu__xs(_mm_slli_si128(spu__xl(a), 8));
    if (s == 12) return spu__xs(_mm_slli_si128(spu__xl(a), 12));
    return spu__xperm(a, spu__xshr(s));
}
#else
static inline u128 spu_rotqmbyi(u128 a, int i7) { return spu_rotqmbyi_ref(a, i7); }
#endif

#if SPU_X86
static inline u128 spu_andhi(u128 a, int32_t imm) { return spu__xs(_mm_and_si128(spu__xl(a), _mm_set1_epi16((short)imm))); }
static inline u128 spu_andbi(u128 a, int32_t imm) { return spu__xs(_mm_and_si128(spu__xl(a), _mm_set1_epi8((char)imm))); }
static inline u128 spu_orhi(u128 a, int32_t imm)  { return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi16((short)imm))); }
static inline u128 spu_orbi(u128 a, int32_t imm)  { return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi8((char)imm))); }
static inline u128 spu_xorhi(u128 a, int32_t imm) { return spu__xs(_mm_xor_si128(spu__xl(a), _mm_set1_epi16((short)imm))); }
#else
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }
#endif

static inline u128 spu_bgx_ref(u128 a, u128 b, u128 t) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint64_t s = (uint64_t)b._u32[i] + (uint64_t)(~a._u32[i]) + (uint64_t)(t._u32[i]&1);
        r._u32[i] = (uint32_t)(s >> 32);
    }
    return r;
}

static inline u128 spu_mfspr(u128 a) { (void)a; return spu_zero(); }

static inline u128 spu_il(int32_t imm)   { return spu_splat_u32((uint32_t)imm); }
static inline u128 spu_ila(uint32_t i18) { return spu_splat_u32(i18 & 0x3FFFF); }
static inline u128 spu_ilh(uint16_t imm) { return spu_splat_u16(imm); }
static inline u128 spu_ilhu(uint16_t imm){ return spu_splat_u32((uint32_t)imm << 16); }
static inline u128 spu_iohl(u128 a, uint16_t imm){
#if SPU_X86
    return spu__xs(_mm_or_si128(spu__xl(a), _mm_set1_epi32((int)(uint32_t)imm)));
#else
    u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r;
#endif
}

#ifdef __cplusplus
}
#endif

#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline uint16x8_t vnegq_u16_compat(uint16x8_t x) { return vsubq_u16(vdupq_n_u16(0), x); }
static inline u128 spu_absdb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vabdq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_avgb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vrhaddq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_ceqb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vceqq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_cgtb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_s8(vreinterpretq_s8_u32(spu__ld(a)), vreinterpretq_s8_u32(spu__ld(b)))));
}
static inline u128 spu_clgtb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_ceqh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vceqq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_u16_u32(spu__ld(b)))));
}
static inline u128 spu_cgth(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_s16(vreinterpretq_s16_u32(spu__ld(a)), vreinterpretq_s16_u32(spu__ld(b)))));
}
static inline u128 spu_clgth(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_u16_u32(spu__ld(b)))));
}
static inline u128 spu_ah(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vaddq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_u16_u32(spu__ld(b)))));
}
static inline u128 spu_sfh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(spu__ld(b)), vreinterpretq_u16_u32(spu__ld(a)))));
}
static inline u128 spu_shlh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vshlq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_s16_u32(vandq_u32(spu__ld(b), vdupq_n_u32(0x001F001Fu))))));
}
static inline u128 spu_roth(u128 a, u128 b) {
    const uint16x8_t x = vreinterpretq_u16_u32(spu__ld(a)); const int16x8_t n = vreinterpretq_s16_u32(vandq_u32(spu__ld(b), vdupq_n_u32(0x000F000Fu)));
    return spu__st(vreinterpretq_u32_u16(vorrq_u16(vshlq_u16(x, n), vshlq_u16(x, vsubq_s16(n, vdupq_n_s16(16))))));
}
static inline u128 spu_rothm(u128 a, u128 b) {
    const int16x8_t m = vnegq_s16(vreinterpretq_s16_u16(vandq_u16(vnegq_u16_compat(vreinterpretq_u16_u32(spu__ld(b))), vdupq_n_u16(0x1F))));
    return spu__st(vreinterpretq_u32_u16(vshlq_u16(vreinterpretq_u16_u32(spu__ld(a)), m)));
}
static inline u128 spu_rothma(u128 a, u128 b) {
    const int16x8_t m = vnegq_s16(vreinterpretq_s16_u16(vandq_u16(vnegq_u16_compat(vreinterpretq_u16_u32(spu__ld(b))), vdupq_n_u16(0x1F))));
    return spu__st(vreinterpretq_u32_s16(vshlq_s16(vreinterpretq_s16_u32(spu__ld(a)), m)));
}
static inline u128 spu_shl(u128 a, u128 b) {
    return spu__st(vshlq_u32(spu__ld(a), vreinterpretq_s32_u32(vandq_u32(spu__ld(b), vdupq_n_u32(0x3F)))));
}
static inline u128 spu_rot(u128 a, u128 b) {
    const uint32x4_t x = spu__ld(a); const int32x4_t n = vreinterpretq_s32_u32(vandq_u32(spu__ld(b), vdupq_n_u32(31)));
    return spu__st(vorrq_u32(vshlq_u32(x, n), vshlq_u32(x, vsubq_s32(n, vdupq_n_s32(32)))));
}
static inline u128 spu_rotm(u128 a, u128 b) {
    const int32x4_t m = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(vsubq_u32(vdupq_n_u32(0), spu__ld(b)), vdupq_n_u32(0x3F))));
    return spu__st(vshlq_u32(spu__ld(a), m));
}
static inline u128 spu_rotma(u128 a, u128 b) {
    const int32x4_t m = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(vsubq_u32(vdupq_n_u32(0), spu__ld(b)), vdupq_n_u32(0x3F))));
    return spu__st(vreinterpretq_u32_s32(vshlq_s32(vreinterpretq_s32_u32(spu__ld(a)), m)));
}
static inline u128 spu_cg(u128 a, u128 b) {
    const uint32x4_t x = spu__ld(a);
    return spu__st(vshrq_n_u32(vcltq_u32(vaddq_u32(x, spu__ld(b)), x), 31));
}
static inline u128 spu_bg(u128 a, u128 b) {
    return spu__st(vshrq_n_u32(vcgeq_u32(spu__ld(b), spu__ld(a)), 31));
}
static inline u128 spu_mpy(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16), 16))));
}
static inline u128 spu_mpyu(u128 a, u128 b) {
    const uint32x4_t m = vdupq_n_u32(0xFFFFu);
    return spu__st(vmulq_u32(vandq_u32(spu__ld(a), m), vandq_u32(spu__ld(b), m)));
}
static inline u128 spu_mpyh(u128 a, u128 b) {
    return spu__st(vshlq_n_u32(vmulq_u32(vshrq_n_u32(spu__ld(a), 16), spu__ld(b)), 16));
}
static inline u128 spu_mpyhh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16))));
}
static inline u128 spu_mpyhhu(u128 a, u128 b) {
    return spu__st(vmulq_u32(vshrq_n_u32(spu__ld(a), 16), vshrq_n_u32(spu__ld(b), 16)));
}
static inline u128 spu_mpys(u128 a, u128 b) {
    const int32x4_t p = vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16), 16));
    return spu__st(vreinterpretq_u32_s32(vshrq_n_s32(p, 16)));
}
static inline u128 spu_sumb(u128 a, u128 b) {
    const uint32x4_t sa = vpaddlq_u16(vpaddlq_u8(vreinterpretq_u8_u32(spu__ld(a)))), sb = vpaddlq_u16(vpaddlq_u8(vreinterpretq_u8_u32(spu__ld(b))));
    return spu__st(vorrq_u32(vshlq_n_u32(sb, 16), sa));
}
static inline u128 spu_ceqbi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u8(vceqq_u8(vreinterpretq_u8_u32(spu__ld(a)), vdupq_n_u8((uint8_t)imm))));
}
static inline u128 spu_cgtbi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_s8(vreinterpretq_s8_u32(spu__ld(a)), vdupq_n_s8((int8_t)imm))));
}
static inline u128 spu_clgtbi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_u8(vreinterpretq_u8_u32(spu__ld(a)), vdupq_n_u8((uint8_t)imm))));
}
static inline u128 spu_ceqhi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vceqq_s16(vreinterpretq_s16_u32(spu__ld(a)), vdupq_n_s16((int16_t)imm))));
}
static inline u128 spu_cgthi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_s16(vreinterpretq_s16_u32(spu__ld(a)), vdupq_n_s16((int16_t)imm))));
}
static inline u128 spu_clgthi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_u16(vreinterpretq_u16_u32(spu__ld(a)), vdupq_n_u16((uint16_t)imm))));
}
static inline u128 spu_ahi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vaddq_u16(vreinterpretq_u16_u32(spu__ld(a)), vdupq_n_u16((uint16_t)imm))));
}
static inline u128 spu_sfhi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vsubq_u16(vdupq_n_u16((uint16_t)imm), vreinterpretq_u16_u32(spu__ld(a)))));
}
static inline u128 spu_mpyi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vdupq_n_s32((int16_t)imm))));
}
static inline u128 spu_mpyui(u128 a, int32_t imm) {
    return spu__st(vmulq_u32(vandq_u32(spu__ld(a), vdupq_n_u32(0xFFFFu)), vdupq_n_u32((uint16_t)imm)));
}
static inline u128 spu_rothi(u128 a, int sh) {
    const uint16x8_t x = vreinterpretq_u16_u32(spu__ld(a)); sh &= 15;
    return spu__st(vreinterpretq_u32_u16(vorrq_u16(vshlq_u16(x, vdupq_n_s16((int16_t)sh)), vshlq_u16(x, vdupq_n_s16((int16_t)(sh - 16))))));
}
static inline u128 spu_roti(u128 a, int sh) {
    const uint32x4_t x = spu__ld(a); sh &= 31;
    return spu__st(vorrq_u32(vshlq_u32(x, vdupq_n_s32(sh)), vshlq_u32(x, vdupq_n_s32(sh - 32))));
}
static inline u128 spu_rotmahi(u128 a, int i7) {
    return spu__st(vreinterpretq_u32_s16(vshlq_s16(vreinterpretq_s16_u32(spu__ld(a)), vdupq_n_s16((int16_t)-((0 - i7) & 0x1F)))));
}
static inline u128 spu_rotmai(u128 a, int i7) {
    return spu__st(vreinterpretq_u32_s32(vshlq_s32(vreinterpretq_s32_u32(spu__ld(a)), vdupq_n_s32(-((0 - i7) & 0x3F)))));
}
static inline u128 spu_mpya(u128 a, u128 b, u128 c) {
    return spu__st(vaddq_u32(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16), 16))), spu__ld(c)));
}
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t) {
    return spu__st(vaddq_u32(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16))), spu__ld(t)));
}
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) {
    return spu__st(vmlaq_u32(spu__ld(t), vshrq_n_u32(spu__ld(a), 16), vshrq_n_u32(spu__ld(b), 16)));
}
static inline u128 spu_addx(u128 a, u128 b, u128 t) {
    return spu__st(vaddq_u32(vaddq_u32(spu__ld(a), spu__ld(b)), vandq_u32(spu__ld(t), vdupq_n_u32(1))));
}
static inline u128 spu_sfx(u128 a, u128 b, u128 t) {
    return spu__st(vaddq_u32(vaddq_u32(spu__ld(b), vmvnq_u32(spu__ld(a))), vandq_u32(spu__ld(t), vdupq_n_u32(1))));
}
static inline u128 spu_cgx(u128 a, u128 b, u128 t) {
    const uint32x4_t x = spu__ld(a), s = vaddq_u32(x, spu__ld(b)), s2 = vaddq_u32(s, vandq_u32(spu__ld(t), vdupq_n_u32(1)));
    return spu__st(vshrq_n_u32(vorrq_u32(vcltq_u32(s, x), vcltq_u32(s2, s)), 31));
}
static inline u128 spu_bgx(u128 a, u128 b, u128 t) {
    const uint32x4_t y = spu__ld(b), s = vaddq_u32(y, vmvnq_u32(spu__ld(a))), s2 = vaddq_u32(s, vandq_u32(spu__ld(t), vdupq_n_u32(1)));
    return spu__st(vshrq_n_u32(vorrq_u32(vcltq_u32(s, y), vcltq_u32(s2, s)), 31));
}
static inline u128 spu_cntb(u128 a) {
    return spu__st(vreinterpretq_u32_u8(vcntq_u8(vreinterpretq_u8_u32(spu__ld(a)))));
}
static inline u128 spu_xsbh(u128 a) {
    return spu__st(vreinterpretq_u32_s16(vshrq_n_s16(vshlq_n_s16(vreinterpretq_s16_u32(spu__ld(a)), 8), 8)));
}
static inline u128 spu_xshw(u128 a) {
    return spu__st(vreinterpretq_u32_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16)));
}
static inline u128 spu_clz(u128 a) {
    return spu__st(vclzq_u32(spu__ld(a)));
}
static inline u128 spu_xswd(u128 a) {
    const uint32x4_t x = spu__ld(a);
    return spu__st(vtrn2q_u32(vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(x), 31)), x));
}
static inline u128 spu_orx(u128 a) {
    uint32x4_t x = spu__ld(a);
    x = vorrq_u32(x, vextq_u32(x, x, 2));
    x = vorrq_u32(x, vextq_u32(x, x, 1));
    return spu__st(vandq_u32(x, vreinterpretq_u32_u64(vsetq_lane_u64(0xFFFFFFFFull, vdupq_n_u64(0), 0))));
}
static inline u128 spu_gb(u128 a) {
    static const uint32_t w[4] = {8, 4, 2, 1};
    return spu_pref_u32(vaddvq_u32(vmulq_u32(vandq_u32(spu__ld(a), vdupq_n_u32(1)), vld1q_u32(w))));
}
static inline u128 spu_gbh(u128 a) {
    static const uint16_t w[8] = {64, 128, 16, 32, 4, 8, 1, 2};
    return spu_pref_u32(vaddvq_u16(vmulq_u16(vandq_u16(vreinterpretq_u16_u32(spu__ld(a)), vdupq_n_u16(1)), vld1q_u16(w))));
}
static inline u128 spu_gbb(u128 a) {
    static const uint8_t w[16] = {16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8};
    uint8x16_t m = vandq_u8(vtstq_u8(vreinterpretq_u8_u32(spu__ld(a)), vdupq_n_u8(1)), vld1q_u8(w));
    m = vpaddq_u8(m, m); m = vpaddq_u8(m, m); m = vpaddq_u8(m, m);
    return spu__st(vandq_u32(vreinterpretq_u32_u8(vrev16q_u8(m)), vdupq_n_u32(0xFFFFu) & vreinterpretq_u32_u64(vsetq_lane_u64(0xFFFFFFFFull, vdupq_n_u64(0), 0))));
}
static inline u128 spu_fsm(u128 a) {
    static const uint32_t w[4] = {8, 4, 2, 1};
    return spu__st(vtstq_u32(vdupq_n_u32(a._u32[0]), vld1q_u32(w)));
}
static inline u128 spu_fsmh(u128 a) {
    static const uint16_t w[8] = {64, 128, 16, 32, 4, 8, 1, 2};
    return spu__st(vreinterpretq_u32_u16(vtstq_u16(vdupq_n_u16((uint16_t)a._u32[0]), vld1q_u16(w))));
}
static inline u128 spu_fsmb(u128 a) {
    static const uint8_t w[16] = {16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8};
    const uint8x16_t v = vcombine_u8(vdup_n_u8((uint8_t)(a._u32[0] >> 8)), vdup_n_u8((uint8_t)a._u32[0]));
    return spu__st(vreinterpretq_u32_u8(vtstq_u8(v, vld1q_u8(w))));
}
#elif SPU_X86
static inline u128 spu_absdb(u128 a, u128 b) {
    const __m128i x = spu__xl(a), y = spu__xl(b); return spu__xs(_mm_sub_epi8(_mm_max_epu8(x, y), _mm_min_epu8(x, y)));
}
static inline u128 spu_avgb(u128 a, u128 b) {
    return spu__xs(_mm_avg_epu8(spu__xl(a), spu__xl(b)));
}
static inline u128 spu_ceqb(u128 a, u128 b) {
    return spu__xs(_mm_cmpeq_epi8(spu__xl(a), spu__xl(b)));
}
static inline u128 spu_cgtb(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi8(spu__xl(a), spu__xl(b)));
}
static inline u128 spu_clgtb(u128 a, u128 b) {
    const __m128i f = _mm_set1_epi8((char)0x80); return spu__xs(_mm_cmpgt_epi8(_mm_xor_si128(spu__xl(a), f), _mm_xor_si128(spu__xl(b), f)));
}
static inline u128 spu_ceqh(u128 a, u128 b) {
    return spu__xs(_mm_cmpeq_epi16(spu__xl(a), spu__xl(b)));
}
static inline u128 spu_cgth(u128 a, u128 b) {
    return spu__xs(_mm_cmpgt_epi16(spu__xl(a), spu__xl(b)));
}
static inline u128 spu_clgth(u128 a, u128 b) {
    const __m128i f = _mm_set1_epi16((short)0x8000); return spu__xs(_mm_cmpgt_epi16(_mm_xor_si128(spu__xl(a), f), _mm_xor_si128(spu__xl(b), f)));
}
static inline u128 spu_ah(u128 a, u128 b) {
    return spu__xs(_mm_add_epi16(spu__xl(a), spu__xl(b)));
}
static inline u128 spu_sfh(u128 a, u128 b) {
    return spu__xs(_mm_sub_epi16(spu__xl(b), spu__xl(a)));
}
static inline u128 spu_shlh(u128 a, u128 b) { return spu_shlh_ref(a, b); }
static inline u128 spu_roth(u128 a, u128 b) { return spu_roth_ref(a, b); }
static inline u128 spu_rothm(u128 a, u128 b) { return spu_rothm_ref(a, b); }
static inline u128 spu_rothma(u128 a, u128 b) { return spu_rothma_ref(a, b); }
#if defined(__AVX2__)
static inline u128 spu_shl(u128 a, u128 b) {
    return spu__xs(_mm_sllv_epi32(spu__xl(a), _mm_and_si128(spu__xl(b), _mm_set1_epi32(0x3F))));
}
static inline u128 spu_rot(u128 a, u128 b) {
    const __m128i x = spu__xl(a), n = _mm_and_si128(spu__xl(b), _mm_set1_epi32(31));
    return spu__xs(_mm_or_si128(_mm_sllv_epi32(x, n), _mm_srlv_epi32(x, _mm_sub_epi32(_mm_set1_epi32(32), n))));
}
static inline u128 spu_rotm(u128 a, u128 b) {
    return spu__xs(_mm_srlv_epi32(spu__xl(a), _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), spu__xl(b)), _mm_set1_epi32(0x3F))));
}
static inline u128 spu_rotma(u128 a, u128 b) {
    return spu__xs(_mm_srav_epi32(spu__xl(a), _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), spu__xl(b)), _mm_set1_epi32(0x3F))));
}
#else
static inline u128 spu_shl(u128 a, u128 b) { return spu_shl_ref(a, b); }
static inline u128 spu_rot(u128 a, u128 b) { return spu_rot_ref(a, b); }
static inline u128 spu_rotm(u128 a, u128 b) { return spu_rotm_ref(a, b); }
static inline u128 spu_rotma(u128 a, u128 b) { return spu_rotma_ref(a, b); }
#endif
static inline u128 spu_cg(u128 a, u128 b) {
    const __m128i x = spu__xl(a); return spu__xs(_mm_srli_epi32(_mm_cmpgt_epi32(spu__xflip(x), spu__xflip(_mm_add_epi32(x, spu__xl(b)))), 31));
}
static inline u128 spu_bg(u128 a, u128 b) {
    return spu__xs(_mm_srli_epi32(_mm_xor_si128(_mm_cmpgt_epi32(spu__xflip(spu__xl(a)), spu__xflip(spu__xl(b))), _mm_set1_epi32(-1)), 31));
}
static inline u128 spu_mpy(u128 a, u128 b) {
    return spu__xs(_mm_madd_epi16(spu__xl(a), _mm_and_si128(spu__xl(b), _mm_set1_epi32(0xFFFF))));
}
static inline u128 spu_mpyu(u128 a, u128 b) {
    const __m128i m = _mm_set1_epi32(0xFFFF); return spu__xs(_mm_mullo_epi32(_mm_and_si128(spu__xl(a), m), _mm_and_si128(spu__xl(b), m)));
}
static inline u128 spu_mpyh(u128 a, u128 b) {
    return spu__xs(_mm_slli_epi32(_mm_mullo_epi32(_mm_srli_epi32(spu__xl(a), 16), spu__xl(b)), 16));
}
static inline u128 spu_mpyhh(u128 a, u128 b) {
    return spu__xs(_mm_mullo_epi32(_mm_srai_epi32(spu__xl(a), 16), _mm_srai_epi32(spu__xl(b), 16)));
}
static inline u128 spu_mpyhhu(u128 a, u128 b) {
    return spu__xs(_mm_mullo_epi32(_mm_srli_epi32(spu__xl(a), 16), _mm_srli_epi32(spu__xl(b), 16)));
}
static inline u128 spu_mpys(u128 a, u128 b) {
    return spu__xs(_mm_srai_epi32(_mm_madd_epi16(spu__xl(a), _mm_and_si128(spu__xl(b), _mm_set1_epi32(0xFFFF))), 16));
}
static inline u128 spu_sumb(u128 a, u128 b) { return spu_sumb_ref(a, b); }
static inline u128 spu_ceqbi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpeq_epi8(spu__xl(a), _mm_set1_epi8((char)imm)));
}
static inline u128 spu_cgtbi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi8(spu__xl(a), _mm_set1_epi8((char)imm)));
}
static inline u128 spu_clgtbi(u128 a, int32_t imm) {
    const __m128i f = _mm_set1_epi8((char)0x80); return spu__xs(_mm_cmpgt_epi8(_mm_xor_si128(spu__xl(a), f), _mm_xor_si128(_mm_set1_epi8((char)imm), f)));
}
static inline u128 spu_ceqhi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpeq_epi16(spu__xl(a), _mm_set1_epi16((short)imm)));
}
static inline u128 spu_cgthi(u128 a, int32_t imm) {
    return spu__xs(_mm_cmpgt_epi16(spu__xl(a), _mm_set1_epi16((short)imm)));
}
static inline u128 spu_clgthi(u128 a, int32_t imm) {
    const __m128i f = _mm_set1_epi16((short)0x8000); return spu__xs(_mm_cmpgt_epi16(_mm_xor_si128(spu__xl(a), f), _mm_xor_si128(_mm_set1_epi16((short)imm), f)));
}
static inline u128 spu_ahi(u128 a, int32_t imm) {
    return spu__xs(_mm_add_epi16(spu__xl(a), _mm_set1_epi16((short)imm)));
}
static inline u128 spu_sfhi(u128 a, int32_t imm) {
    return spu__xs(_mm_sub_epi16(_mm_set1_epi16((short)imm), spu__xl(a)));
}
static inline u128 spu_mpyi(u128 a, int32_t imm) {
    return spu__xs(_mm_madd_epi16(spu__xl(a), _mm_set1_epi32((uint16_t)imm)));
}
static inline u128 spu_mpyui(u128 a, int32_t imm) {
    return spu__xs(_mm_mullo_epi32(_mm_and_si128(spu__xl(a), _mm_set1_epi32(0xFFFF)), _mm_set1_epi32((uint16_t)imm)));
}
static inline u128 spu_rothi(u128 a, int sh) {
    const __m128i x = spu__xl(a); sh &= 15;
    return spu__xs(_mm_or_si128(_mm_sll_epi16(x, _mm_cvtsi32_si128(sh)), _mm_srl_epi16(x, _mm_cvtsi32_si128(16 - sh))));
}
static inline u128 spu_roti(u128 a, int sh) {
    const __m128i x = spu__xl(a); sh &= 31;
    return spu__xs(_mm_or_si128(_mm_sll_epi32(x, _mm_cvtsi32_si128(sh)), _mm_srl_epi32(x, _mm_cvtsi32_si128(32 - sh))));
}
static inline u128 spu_rotmahi(u128 a, int i7) {
    return spu__xs(_mm_sra_epi16(spu__xl(a), _mm_cvtsi32_si128((0 - i7) & 0x1F)));
}
static inline u128 spu_rotmai(u128 a, int i7) {
    return spu__xs(_mm_sra_epi32(spu__xl(a), _mm_cvtsi32_si128((0 - i7) & 0x3F)));
}
static inline u128 spu_mpya(u128 a, u128 b, u128 c) {
    return spu__xs(_mm_add_epi32(_mm_madd_epi16(spu__xl(a), _mm_and_si128(spu__xl(b), _mm_set1_epi32(0xFFFF))), spu__xl(c)));
}
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t) {
    return spu__xs(_mm_add_epi32(_mm_mullo_epi32(_mm_srai_epi32(spu__xl(a), 16), _mm_srai_epi32(spu__xl(b), 16)), spu__xl(t)));
}
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) {
    return spu__xs(_mm_add_epi32(_mm_mullo_epi32(_mm_srli_epi32(spu__xl(a), 16), _mm_srli_epi32(spu__xl(b), 16)), spu__xl(t)));
}
static inline u128 spu_addx(u128 a, u128 b, u128 t) {
    return spu__xs(_mm_add_epi32(_mm_add_epi32(spu__xl(a), spu__xl(b)), _mm_and_si128(spu__xl(t), _mm_set1_epi32(1))));
}
static inline u128 spu_sfx(u128 a, u128 b, u128 t) {
    return spu__xs(_mm_add_epi32(_mm_add_epi32(spu__xl(b), _mm_xor_si128(spu__xl(a), _mm_set1_epi32(-1))), _mm_and_si128(spu__xl(t), _mm_set1_epi32(1))));
}
static inline u128 spu_cgx(u128 a, u128 b, u128 t) { return spu_cgx_ref(a, b, t); }
static inline u128 spu_bgx(u128 a, u128 b, u128 t) { return spu_bgx_ref(a, b, t); }
static inline u128 spu_cntb(u128 a) {
    const __m128i lut = _mm_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4), m = _mm_set1_epi8(15), x = spu__xl(a);
    return spu__xs(_mm_add_epi8(_mm_shuffle_epi8(lut, _mm_and_si128(x, m)), _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(x, 4), m))));
}
static inline u128 spu_xsbh(u128 a) {
    return spu__xs(_mm_srai_epi16(_mm_slli_epi16(spu__xl(a), 8), 8));
}
static inline u128 spu_xshw(u128 a) {
    return spu__xs(_mm_srai_epi32(_mm_slli_epi32(spu__xl(a), 16), 16));
}
static inline u128 spu_clz(u128 a) { return spu_clz_ref(a); }
static inline u128 spu_xswd(u128 a) { return spu_xswd_ref(a); }
static inline u128 spu_orx(u128 a) {
    __m128i x = spu__xl(a);
    x = _mm_or_si128(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(1, 0, 3, 2)));
    x = _mm_or_si128(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)));
    return spu_pref_u32((uint32_t)_mm_cvtsi128_si32(x));
}
static inline u128 spu_gb(u128 a) {
    static const uint8_t rev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    return spu_pref_u32(rev4[_mm_movemask_ps(_mm_castsi128_ps(_mm_slli_epi32(spu__xl(a), 31)))]);
}
static inline u128 spu_gbh(u128 a) {
    const __m128i p = _mm_shuffle_epi8(spu__xl(a), _mm_setr_epi8(12, 14, 8, 10, 4, 6, 0, 2, -1, -1, -1, -1, -1, -1, -1, -1));
    return spu_pref_u32((uint32_t)_mm_movemask_epi8(_mm_slli_epi16(p, 7)));
}
static inline u128 spu_gbb(u128 a) {
    const __m128i r = _mm_shuffle_epi32(spu__xl(a), _MM_SHUFFLE(0, 1, 2, 3));
    return spu_pref_u32((uint32_t)_mm_movemask_epi8(_mm_slli_epi16(r, 7)));
}
static inline u128 spu_fsm(u128 a) {
    const __m128i m = _mm_setr_epi32(8, 4, 2, 1);
    return spu__xs(_mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32((int)a._u32[0]), m), m));
}
static inline u128 spu_fsmh(u128 a) {
    const __m128i m = _mm_setr_epi16(0x40, 0x80, 0x10, 0x20, 0x04, 0x08, 0x01, 0x02);
    return spu__xs(_mm_cmpeq_epi16(_mm_and_si128(_mm_set1_epi16((short)a._u32[0]), m), m));
}
static inline u128 spu_fsmb(u128 a) {
    const __m128i b = _mm_shuffle_epi8(_mm_cvtsi32_si128((int)a._u32[0]),
                                       _mm_setr_epi8(1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0));
    const __m128i m = _mm_setr_epi8(0x10, 0x20, 0x40, (char)0x80, 0x01, 0x02, 0x04, 0x08,
                                    0x10, 0x20, 0x40, (char)0x80, 0x01, 0x02, 0x04, 0x08);
    return spu__xs(_mm_cmpeq_epi8(_mm_and_si128(b, m), m));
}
#else
static inline u128 spu_absdb(u128 a, u128 b) { return spu_absdb_ref(a, b); }
static inline u128 spu_avgb(u128 a, u128 b) { return spu_avgb_ref(a, b); }
static inline u128 spu_ceqb(u128 a, u128 b) { return spu_ceqb_ref(a, b); }
static inline u128 spu_cgtb(u128 a, u128 b) { return spu_cgtb_ref(a, b); }
static inline u128 spu_clgtb(u128 a, u128 b) { return spu_clgtb_ref(a, b); }
static inline u128 spu_ceqh(u128 a, u128 b) { return spu_ceqh_ref(a, b); }
static inline u128 spu_cgth(u128 a, u128 b) { return spu_cgth_ref(a, b); }
static inline u128 spu_clgth(u128 a, u128 b) { return spu_clgth_ref(a, b); }
static inline u128 spu_ah(u128 a, u128 b) { return spu_ah_ref(a, b); }
static inline u128 spu_sfh(u128 a, u128 b) { return spu_sfh_ref(a, b); }
static inline u128 spu_shlh(u128 a, u128 b) { return spu_shlh_ref(a, b); }
static inline u128 spu_roth(u128 a, u128 b) { return spu_roth_ref(a, b); }
static inline u128 spu_rothm(u128 a, u128 b) { return spu_rothm_ref(a, b); }
static inline u128 spu_rothma(u128 a, u128 b) { return spu_rothma_ref(a, b); }
static inline u128 spu_shl(u128 a, u128 b) { return spu_shl_ref(a, b); }
static inline u128 spu_rot(u128 a, u128 b) { return spu_rot_ref(a, b); }
static inline u128 spu_rotm(u128 a, u128 b) { return spu_rotm_ref(a, b); }
static inline u128 spu_rotma(u128 a, u128 b) { return spu_rotma_ref(a, b); }
static inline u128 spu_cg(u128 a, u128 b) { return spu_cg_ref(a, b); }
static inline u128 spu_bg(u128 a, u128 b) { return spu_bg_ref(a, b); }
static inline u128 spu_mpy(u128 a, u128 b) { return spu_mpy_ref(a, b); }
static inline u128 spu_mpyu(u128 a, u128 b) { return spu_mpyu_ref(a, b); }
static inline u128 spu_mpyh(u128 a, u128 b) { return spu_mpyh_ref(a, b); }
static inline u128 spu_mpyhh(u128 a, u128 b) { return spu_mpyhh_ref(a, b); }
static inline u128 spu_mpyhhu(u128 a, u128 b) { return spu_mpyhhu_ref(a, b); }
static inline u128 spu_mpys(u128 a, u128 b) { return spu_mpys_ref(a, b); }
static inline u128 spu_sumb(u128 a, u128 b) { return spu_sumb_ref(a, b); }
static inline u128 spu_ceqbi(u128 a, int32_t imm) { return spu_ceqbi_ref(a, imm); }
static inline u128 spu_cgtbi(u128 a, int32_t imm) { return spu_cgtbi_ref(a, imm); }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { return spu_clgtbi_ref(a, imm); }
static inline u128 spu_ceqhi(u128 a, int32_t imm) { return spu_ceqhi_ref(a, imm); }
static inline u128 spu_cgthi(u128 a, int32_t imm) { return spu_cgthi_ref(a, imm); }
static inline u128 spu_clgthi(u128 a, int32_t imm) { return spu_clgthi_ref(a, imm); }
static inline u128 spu_ahi(u128 a, int32_t imm) { return spu_ahi_ref(a, imm); }
static inline u128 spu_sfhi(u128 a, int32_t imm) { return spu_sfhi_ref(a, imm); }
static inline u128 spu_mpyi(u128 a, int32_t imm) { return spu_mpyi_ref(a, imm); }
static inline u128 spu_mpyui(u128 a, int32_t imm) { return spu_mpyui_ref(a, imm); }
static inline u128 spu_rothi(u128 a, int sh) { return spu_rothi_ref(a, sh); }
static inline u128 spu_roti(u128 a, int sh) { return spu_roti_ref(a, sh); }
static inline u128 spu_rotmahi(u128 a, int i7) { return spu_rotmahi_ref(a, i7); }
static inline u128 spu_rotmai(u128 a, int i7) { return spu_rotmai_ref(a, i7); }
static inline u128 spu_mpya(u128 a, u128 b, u128 c) { return spu_mpya_ref(a, b, c); }
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t) { return spu_mpyhha_ref(a, b, t); }
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) { return spu_mpyhhau_ref(a, b, t); }
static inline u128 spu_addx(u128 a, u128 b, u128 t) { return spu_addx_ref(a, b, t); }
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { return spu_sfx_ref(a, b, t); }
static inline u128 spu_cgx(u128 a, u128 b, u128 t) { return spu_cgx_ref(a, b, t); }
static inline u128 spu_bgx(u128 a, u128 b, u128 t) { return spu_bgx_ref(a, b, t); }
static inline u128 spu_cntb(u128 a) { return spu_cntb_ref(a); }
static inline u128 spu_xsbh(u128 a) { return spu_xsbh_ref(a); }
static inline u128 spu_xshw(u128 a) { return spu_xshw_ref(a); }
static inline u128 spu_clz(u128 a) { return spu_clz_ref(a); }
static inline u128 spu_xswd(u128 a) { return spu_xswd_ref(a); }
static inline u128 spu_orx(u128 a) { return spu_orx_ref(a); }
static inline u128 spu_gb(u128 a) { return spu_gb_ref(a); }
static inline u128 spu_gbh(u128 a) { return spu_gbh_ref(a); }
static inline u128 spu_gbb(u128 a) { return spu_gbb_ref(a); }
static inline u128 spu_fsm(u128 a) { return spu_fsm_ref(a); }
static inline u128 spu_fsmh(u128 a) { return spu_fsmh_ref(a); }
static inline u128 spu_fsmb(u128 a) { return spu_fsmb_ref(a); }
#endif

#endif
