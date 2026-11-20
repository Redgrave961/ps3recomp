/* ppu_vmx.h -- exact VMX (AltiVec) semantics for lifted PPU code.
 *
 * Vector registers are held byte-reversed: host byte j is guest byte 15 - j.
 * One reversal turns every big-endian element of every width into a native
 * little-endian lane (in mirrored lane order), so element-wise operations run
 * on the register as stored and only guest memory accesses convert
 * (vmx_rev). Positional operations address elements by their guest index
 * through vmx_b/vmx_h/vmx_w and the matching setters. Every helper computes
 * into a temporary first, so the destination may alias any source.
 *
 * Semantics follow PowerISA 2.02 VMX as RPCS3's PPU interpreter implements it
 * with its accuracy options on:
 *   - saturating ops OR the SAT bit (VSCR 0x00000001) into *vscr;
 *   - VSCR[NJ] (0x00010000) flushes denormal float inputs and results to zero;
 *   - a NaN operand propagates (A, then B, then C) with its quiet bit set; a NaN
 *     produced from non-NaN operands is the default QNaN 0x7FC00000.
 * Each helper is the whole instruction, so the lifter emits one call per op.
 */
#ifndef PPU_VMX_H
#define PPU_VMX_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <immintrin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMX_SAT 0x00000001u
#define VMX_NJ  0x00010000u
#define VMX_KA  0x10000000u
#define VMX_KB  0x20000000u
#define VMX_KC  0x40000000u
#define VMX_INL static inline

VMX_INL __m128i vmx_ld(const void* p) { return _mm_loadu_si128((const __m128i*)p); }
VMX_INL void vmx_st(void* p, __m128i v) { _mm_storeu_si128((__m128i*)p, v); }
VMX_INL __m128 vmx_ldf(const void* p) { return _mm_castsi128_ps(vmx_ld(p)); }
VMX_INL void vmx_stf(void* p, __m128 v) { vmx_st(p, _mm_castps_si128(v)); }
VMX_INL __m128 vmx_k(uint32_t u) { return _mm_castsi128_ps(_mm_set1_epi32((int)u)); }
VMX_INL __m128i vmx_revv(__m128i v) { return _mm_shuffle_epi8(v, _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)); }
VMX_INL void vmx_rev(void* d, const void* s) { vmx_st(d, vmx_revv(vmx_ld(s))); }

VMX_INL __m128 vmx_flushv(__m128 x, uint32_t vscr) {
    if (!(vscr & VMX_NJ)) return x;
    const __m128 den = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(x), _mm_set1_epi32(0x7F800000)),
                                                        _mm_setzero_si128()));
    return _mm_andnot_ps(_mm_and_ps(den, vmx_k(0x7FFFFFFFu)), x);
}
VMX_INL __m128 vmx_fixv(__m128 r) { return _mm_blendv_ps(r, vmx_k(0x7FC00000u), _mm_cmpunord_ps(r, r)); }
VMX_INL __m128 vmx_nanv(__m128 r, __m128 x) {
    return _mm_blendv_ps(r, _mm_or_ps(x, vmx_k(0x7FC00000u)), _mm_cmpunord_ps(x, x));
}
VMX_INL __m128 vmx_tiny(__m128 x) {
    return _mm_and_ps(_mm_cmp_ps(_mm_and_ps(x, vmx_k(0x7FFFFFFFu)), _mm_set1_ps(FLT_MIN), _CMP_LT_OQ),
                      _mm_cmp_ps(x, _mm_setzero_ps(), _CMP_NEQ_OQ));
}
/* Lanes the exact path would change: a NaN or (under NJ) denormal result, or a
 * denormal input under NJ. A NaN input needs no test of its own for these ops:
 * the host result is then a NaN too. */
VMX_INL __m128 vmx_kq(__m128 x, uint32_t vscr, uint32_t k) { return (vscr & k) ? _mm_setzero_ps() : x; }
VMX_INL int vmx_plain(__m128 r, __m128 x, __m128 y, __m128 z, uint32_t vscr) {
    __m128 bad = _mm_cmpunord_ps(r, r);
    if (vscr & VMX_NJ) bad = _mm_or_ps(bad, _mm_or_ps(_mm_or_ps(vmx_tiny(r), vmx_tiny(x)), _mm_or_ps(vmx_tiny(y), vmx_tiny(z))));
    return _mm_testz_ps(bad, bad);
}
VMX_INL uint32_t vmx_cr6(int mask, int full) { return (uint32_t)((mask == full) << 3 | (mask == 0) << 1); }

/* ---- element access by guest (big-endian) index ---- */
VMX_INL uint8_t vmx_b(const uint8_t* v, int i) { return v[15 - i]; }
VMX_INL uint16_t vmx_h(const uint8_t* v, int i) { uint16_t x; memcpy(&x, v + 14 - 2 * i, 2); return x; }
VMX_INL uint32_t vmx_w(const uint8_t* v, int i) { uint32_t x; memcpy(&x, v + 12 - 4 * i, 4); return x; }
VMX_INL void vmx_sb(uint8_t* v, int i, uint8_t x) { v[15 - i] = x; }
VMX_INL void vmx_sh(uint8_t* v, int i, uint16_t x) { memcpy(v + 14 - 2 * i, &x, 2); }
VMX_INL void vmx_sw(uint8_t* v, int i, uint32_t x) { memcpy(v + 12 - 4 * i, &x, 4); }
VMX_INL float    vmx_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
VMX_INL uint32_t vmx_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

#define VMX_D  void* d_
#define VMX_A  const void* a_
#define VMX_B  const void* b_
#define VMX_C  const void* c_
#define VMX_ARGS_PREP const uint8_t* a = (const uint8_t*)a_; (void)a; \
    const uint8_t* b = (const uint8_t*)b_; (void)b; uint8_t r[16]
#define VMX_OUT memcpy(d_, r, 16)

/* ---- saturation ---- */
VMX_INL int32_t vmx_sat_s(int64_t x, int64_t lo, int64_t hi, uint32_t* vscr) {
    if (x < lo) { *vscr |= VMX_SAT; return (int32_t)lo; }
    if (x > hi) { *vscr |= VMX_SAT; return (int32_t)hi; }
    return (int32_t)x;
}
VMX_INL uint32_t vmx_sat_u(int64_t x, int64_t hi, uint32_t* vscr) {
    if (x < 0) { *vscr |= VMX_SAT; return 0; }
    if (x > hi) { *vscr |= VMX_SAT; return (uint32_t)hi; }
    return (uint32_t)x;
}

/* ---- float helpers ---- */
VMX_INL uint32_t vmx_fix(uint32_t r) { return (r & 0x7FFFFFFFu) > 0x7F800000u ? 0x7FC00000u : r; }

/* ======================================================================
 * integer element-wise
 * ==================================================================== */

#define VMX_BIN(name, cnt, get, set, expr) \
VMX_INL void name(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; \
    for (int i = 0; i < cnt; i++) { uint64_t x = get(a, i), y = get(b, i); (void)x; (void)y; set(r, i, expr); } VMX_OUT; }

/* rotates / shifts: count from the low bits of the matching element of B */
VMX_BIN(vmx_vrlb, 16, vmx_b, vmx_sb, (uint8_t)((x << (y & 7)) | (x >> ((8 - (y & 7)) & 7))))
VMX_BIN(vmx_vrlh, 8, vmx_h, vmx_sh, (uint16_t)((x << (y & 15)) | (x >> ((16 - (y & 15)) & 15))))
VMX_BIN(vmx_vslb, 16, vmx_b, vmx_sb, (uint8_t)(x << (y & 7)))
VMX_BIN(vmx_vslh, 8, vmx_h, vmx_sh, (uint16_t)(x << (y & 15)))
VMX_BIN(vmx_vsrb, 16, vmx_b, vmx_sb, (uint8_t)(x >> (y & 7)))
VMX_BIN(vmx_vsrh, 8, vmx_h, vmx_sh, (uint16_t)(x >> (y & 15)))
VMX_BIN(vmx_vsrab, 16, vmx_b, vmx_sb, (uint8_t)((int8_t)x >> (y & 7)))
VMX_BIN(vmx_vsrah, 8, vmx_h, vmx_sh, (uint16_t)((int16_t)x >> (y & 15)))
VMX_INL __m128i vmx_cnt(const void* b_) { return _mm_and_si128(vmx_ld(b_), _mm_set1_epi32(31)); }
VMX_INL void vmx_vslw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_sllv_epi32(vmx_ld(a_), vmx_cnt(b_))); }
VMX_INL void vmx_vsrw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_srlv_epi32(vmx_ld(a_), vmx_cnt(b_))); }
VMX_INL void vmx_vsraw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_srav_epi32(vmx_ld(a_), vmx_cnt(b_))); }
VMX_INL void vmx_vrlw(VMX_D, VMX_A, VMX_B) { const __m128i x = vmx_ld(a_), n = vmx_cnt(b_);
    vmx_st(d_, _mm_or_si128(_mm_sllv_epi32(x, n), _mm_srlv_epi32(x, _mm_sub_epi32(_mm_set1_epi32(32), n)))); }

VMX_INL void vmx_vaddubm(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_add_epi8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vsububm(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_sub_epi8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vadduhm(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_add_epi16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vsubuhm(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_sub_epi16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vadduwm(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_add_epi32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vsubuwm(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_sub_epi32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vmaxub(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_max_epu8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vminub(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_min_epu8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vmaxsb(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_max_epi8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vminsb(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_min_epi8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vmaxuh(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_max_epu16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vminuh(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_min_epu16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vmaxsh(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_max_epi16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vminsh(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_min_epi16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vmaxuw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_max_epu32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vminuw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_min_epu32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vmaxsw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_max_epi32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL void vmx_vminsw(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_min_epi32(vmx_ld(a_), vmx_ld(b_))); }

/* vperm: guest byte s of A:B sits at host byte 15 - (s & 15) of A or B, so the
 * index mirrors with one xor and bit 4 still picks the source. */
VMX_INL void vmx_vperm(VMX_D, VMX_A, VMX_B, VMX_C) {
    const __m128i c = vmx_ld(c_), i = _mm_xor_si128(_mm_and_si128(c, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x0F));
    vmx_st(d_, _mm_blendv_epi8(_mm_shuffle_epi8(vmx_ld(a_), i), _mm_shuffle_epi8(vmx_ld(b_), i), _mm_slli_epi16(c, 3)));
}
VMX_INL __m128i vmx_iota(void) { return _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
VMX_INL void vmx_lvlx(VMX_D, uint64_t lo, uint64_t hi, uint32_t sh) {
    vmx_st(d_, _mm_shuffle_epi8(_mm_set_epi64x((long long)hi, (long long)lo), _mm_sub_epi8(_mm_set1_epi8((char)(0x7F + sh)), vmx_iota())));
}
VMX_INL void vmx_lvrx(VMX_D, uint64_t lo, uint64_t hi, uint32_t sh) {
    vmx_st(d_, _mm_shuffle_epi8(_mm_set_epi64x((long long)hi, (long long)lo), _mm_sub_epi8(_mm_set1_epi8((char)(sh - 1)), vmx_iota())));
}

/* averages: (a + b + 1) >> 1 without overflow */
VMX_BIN(vmx_vavgub, 16, vmx_b, vmx_sb, (uint8_t)((x + y + 1) >> 1))
VMX_BIN(vmx_vavguh, 8, vmx_h, vmx_sh, (uint16_t)((x + y + 1) >> 1))
VMX_BIN(vmx_vavguw, 4, vmx_w, vmx_sw, (uint32_t)((x + y + 1) >> 1))
VMX_BIN(vmx_vavgsb, 16, vmx_b, vmx_sb, (uint8_t)(((int64_t)(int8_t)x + (int8_t)y + 1) >> 1))
VMX_BIN(vmx_vavgsh, 8, vmx_h, vmx_sh, (uint16_t)(((int64_t)(int16_t)x + (int16_t)y + 1) >> 1))
VMX_BIN(vmx_vavgsw, 4, vmx_w, vmx_sw, (uint32_t)(((int64_t)(int32_t)x + (int32_t)y + 1) >> 1))

/* carry / borrow out */
VMX_BIN(vmx_vaddcuw, 4, vmx_w, vmx_sw, (uint32_t)((x + y) >> 32))
VMX_BIN(vmx_vsubcuw, 4, vmx_w, vmx_sw, (uint32_t)(x >= y))

/* saturating add / sub */
#define VMX_SATOP(name, cnt, get, set, expr) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP; \
    for (int i = 0; i < cnt; i++) { uint64_t x = get(a, i), y = get(b, i); set(r, i, expr); } VMX_OUT; }
VMX_SATOP(vmx_vaddubs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_u((int64_t)x + (int64_t)y, 0xFF, vscr))
VMX_SATOP(vmx_vadduhs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_u((int64_t)x + (int64_t)y, 0xFFFF, vscr))
VMX_SATOP(vmx_vadduws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_u((int64_t)x + (int64_t)y, 0xFFFFFFFFll, vscr))
VMX_SATOP(vmx_vaddsbs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_s((int64_t)(int8_t)x + (int8_t)y, -128, 127, vscr))
VMX_SATOP(vmx_vaddshs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_s((int64_t)(int16_t)x + (int16_t)y, -32768, 32767, vscr))
VMX_SATOP(vmx_vaddsws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_s((int64_t)(int32_t)x + (int32_t)y, INT32_MIN, INT32_MAX, vscr))
VMX_SATOP(vmx_vsububs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_u((int64_t)x - (int64_t)y, 0xFF, vscr))
VMX_SATOP(vmx_vsubuhs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_u((int64_t)x - (int64_t)y, 0xFFFF, vscr))
VMX_SATOP(vmx_vsubuws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_u((int64_t)x - (int64_t)y, 0xFFFFFFFFll, vscr))
VMX_SATOP(vmx_vsubsbs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_s((int64_t)(int8_t)x - (int8_t)y, -128, 127, vscr))
VMX_SATOP(vmx_vsubshs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_s((int64_t)(int16_t)x - (int16_t)y, -32768, 32767, vscr))
VMX_SATOP(vmx_vsubsws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_s((int64_t)(int32_t)x - (int32_t)y, INT32_MIN, INT32_MAX, vscr))

/* even / odd multiplies: element i of the result is the product of source
 * element 2i (even) or 2i+1 (odd), big-endian numbering */
#define VMX_MUL(name, n, get, set, cast, par) \
VMX_INL void name(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; \
    for (int i = 0; i < n; i++) { int64_t x = (cast)get(a, 2 * i + par), y = (cast)get(b, 2 * i + par); set(r, i, x * y); } VMX_OUT; }
VMX_MUL(vmx_vmuleub, 8, vmx_b, vmx_sh, uint8_t, 0)
VMX_MUL(vmx_vmuloub, 8, vmx_b, vmx_sh, uint8_t, 1)
VMX_MUL(vmx_vmulesb, 8, vmx_b, vmx_sh, int8_t, 0)
VMX_MUL(vmx_vmulosb, 8, vmx_b, vmx_sh, int8_t, 1)
VMX_INL void vmx_vmuleuh(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_mullo_epi32(_mm_srli_epi32(vmx_ld(a_), 16), _mm_srli_epi32(vmx_ld(b_), 16))); }
VMX_INL void vmx_vmulesh(VMX_D, VMX_A, VMX_B) { vmx_st(d_, _mm_mullo_epi32(_mm_srai_epi32(vmx_ld(a_), 16), _mm_srai_epi32(vmx_ld(b_), 16))); }
VMX_INL void vmx_vmulouh(VMX_D, VMX_A, VMX_B) { const __m128i m = _mm_set1_epi32(0xFFFF);
    vmx_st(d_, _mm_mullo_epi32(_mm_and_si128(vmx_ld(a_), m), _mm_and_si128(vmx_ld(b_), m))); }
VMX_INL void vmx_vmulosh(VMX_D, VMX_A, VMX_B) {
    vmx_st(d_, _mm_mullo_epi32(_mm_srai_epi32(_mm_slli_epi32(vmx_ld(a_), 16), 16), _mm_srai_epi32(_mm_slli_epi32(vmx_ld(b_), 16), 16))); }

/* ---- multiply-add / multiply-sum (VA-form) ---- */
VMX_INL void vmx_vmhaddshs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 8; i++) {
        int64_t p = ((int64_t)(int16_t)vmx_h(a, i) * (int16_t)vmx_h(b, i)) >> 15;
        vmx_sh(r, i, (uint16_t)vmx_sat_s(p + (int16_t)vmx_h(c, i), -32768, 32767, vscr));
    } VMX_OUT; }
VMX_INL void vmx_vmhraddshs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 8; i++) {
        int64_t p = ((int64_t)(int16_t)vmx_h(a, i) * (int16_t)vmx_h(b, i) + 0x4000) >> 15;
        vmx_sh(r, i, (uint16_t)vmx_sat_s(p + (int16_t)vmx_h(c, i), -32768, 32767, vscr));
    } VMX_OUT; }
VMX_INL void vmx_vmladduhm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 8; i++) vmx_sh(r, i, (uint16_t)(vmx_h(a, i) * vmx_h(b, i) + vmx_h(c, i))); VMX_OUT; }
VMX_INL void vmx_vmsumubm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) { uint32_t s = vmx_w(c, i);
        for (int j = 0; j < 4; j++) s += (uint32_t)vmx_b(a, 4 * i + j) * vmx_b(b, 4 * i + j);
        vmx_sw(r, i, s); } VMX_OUT; }
VMX_INL void vmx_vmsummbm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) { uint32_t s = vmx_w(c, i);
        for (int j = 0; j < 4; j++) s += (uint32_t)((int32_t)(int8_t)vmx_b(a, 4 * i + j) * (int32_t)vmx_b(b, 4 * i + j));
        vmx_sw(r, i, s); } VMX_OUT; }
VMX_INL void vmx_vmsumuhm(VMX_D, VMX_A, VMX_B, VMX_C) { const __m128i a = vmx_ld(a_), b = vmx_ld(b_), m = _mm_set1_epi32(0xFFFF);
    vmx_st(d_, _mm_add_epi32(vmx_ld(c_), _mm_add_epi32(_mm_mullo_epi32(_mm_srli_epi32(a, 16), _mm_srli_epi32(b, 16)),
                                                       _mm_mullo_epi32(_mm_and_si128(a, m), _mm_and_si128(b, m))))); }
VMX_INL void vmx_vmsumuhs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) {
        int64_t s = (int64_t)vmx_w(c, i) + (int64_t)vmx_h(a, 2 * i) * vmx_h(b, 2 * i)
                  + (int64_t)vmx_h(a, 2 * i + 1) * vmx_h(b, 2 * i + 1);
        vmx_sw(r, i, vmx_sat_u(s, 0xFFFFFFFFll, vscr)); } VMX_OUT; }
VMX_INL void vmx_vmsumshm(VMX_D, VMX_A, VMX_B, VMX_C) { vmx_st(d_, _mm_add_epi32(vmx_ld(c_), _mm_madd_epi16(vmx_ld(a_), vmx_ld(b_)))); }
VMX_INL void vmx_vmsumshs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) {
        int64_t s = (int64_t)(int32_t)vmx_w(c, i) + (int64_t)(int16_t)vmx_h(a, 2 * i) * (int16_t)vmx_h(b, 2 * i)
                  + (int64_t)(int16_t)vmx_h(a, 2 * i + 1) * (int16_t)vmx_h(b, 2 * i + 1);
        vmx_sw(r, i, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }

/* ---- sums across ---- */
VMX_INL void vmx_vsum4ubs(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    for (int i = 0; i < 4; i++) { int64_t s = vmx_w(b, i);
        for (int j = 0; j < 4; j++) s += vmx_b(a, 4 * i + j);
        vmx_sw(r, i, vmx_sat_u(s, 0xFFFFFFFFll, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsum4sbs(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    for (int i = 0; i < 4; i++) { int64_t s = (int32_t)vmx_w(b, i);
        for (int j = 0; j < 4; j++) s += (int8_t)vmx_b(a, 4 * i + j);
        vmx_sw(r, i, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsum4shs(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    for (int i = 0; i < 4; i++) {
        int64_t s = (int64_t)(int32_t)vmx_w(b, i) + (int16_t)vmx_h(a, 2 * i) + (int16_t)vmx_h(a, 2 * i + 1);
        vmx_sw(r, i, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsum2sws(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    memset(r, 0, 16);
    for (int i = 0; i < 2; i++) {
        int64_t s = (int64_t)(int32_t)vmx_w(a, 2 * i) + (int32_t)vmx_w(a, 2 * i + 1) + (int32_t)vmx_w(b, 2 * i + 1);
        vmx_sw(r, 2 * i + 1, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsumsws(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    memset(r, 0, 16);
    int64_t s = (int32_t)vmx_w(b, 3);
    for (int i = 0; i < 4; i++) s += (int32_t)vmx_w(a, i);
    vmx_sw(r, 3, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); VMX_OUT; }

/* ---- packs: A's elements fill the high half, B's the low half ---- */
#define VMX_PACK(name, n, get, set, conv) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP; (void)vscr; \
    for (int i = 0; i < n; i++) { int64_t x = get(a, i); set(r, i, conv); } \
    for (int i = 0; i < n; i++) { int64_t x = get(b, i); set(r, n + i, conv); } VMX_OUT; }
VMX_PACK(vmx_vpkuhum, 8, vmx_h, vmx_sb, (uint8_t)x)
VMX_PACK(vmx_vpkuwum, 4, vmx_w, vmx_sh, (uint16_t)x)
VMX_PACK(vmx_vpkuhus, 8, vmx_h, vmx_sb, (uint8_t)vmx_sat_u(x, 0xFF, vscr))
VMX_PACK(vmx_vpkuwus, 4, vmx_w, vmx_sh, (uint16_t)vmx_sat_u(x, 0xFFFF, vscr))
VMX_PACK(vmx_vpkshus, 8, vmx_h, vmx_sb, (uint8_t)vmx_sat_u((int16_t)x, 0xFF, vscr))
VMX_PACK(vmx_vpkswus, 4, vmx_w, vmx_sh, (uint16_t)vmx_sat_u((int32_t)x, 0xFFFF, vscr))
VMX_PACK(vmx_vpkshss, 8, vmx_h, vmx_sb, (uint8_t)vmx_sat_s((int16_t)x, -128, 127, vscr))
VMX_PACK(vmx_vpkswss, 4, vmx_w, vmx_sh, (uint16_t)vmx_sat_s((int32_t)x, -32768, 32767, vscr))
VMX_PACK(vmx_vpkpx, 4, vmx_w, vmx_sh, (uint16_t)(((x >> 24) & 1) << 15 | ((x >> 19) & 0x1F) << 10 |
                                                 ((x >> 11) & 0x1F) << 5 | ((x >> 3) & 0x1F)))

/* ---- unpacks (sign-extend; pixel 1:5:5:5 -> 8:8:8:8) ---- */
VMX_INL void vmx_vupkpx(VMX_D, VMX_B, int lo) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) { uint16_t h = vmx_h(b, i + 4 * lo);
        vmx_sb(r, 4 * i, (h & 0x8000) ? 0xFF : 0); vmx_sb(r, 4 * i + 1, (h >> 10) & 0x1F);
        vmx_sb(r, 4 * i + 2, (h >> 5) & 0x1F); vmx_sb(r, 4 * i + 3, h & 0x1F); } VMX_OUT; }
VMX_INL void vmx_vupksb(VMX_D, VMX_B, int lo) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 8; i++) vmx_sh(r, i, (uint16_t)(int16_t)(int8_t)vmx_b(b, i + 8 * lo)); VMX_OUT; }
VMX_INL void vmx_vupksh(VMX_D, VMX_B, int lo) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) vmx_sw(r, i, (uint32_t)(int32_t)(int16_t)vmx_h(b, i + 4 * lo)); VMX_OUT; }

/* ---- whole-vector shifts ---- */
/* vsl / vsr: RPCS3 shifts each byte by that byte's own count (B byte & 7),
 * funnelling bits in from the neighbour; with all counts equal (the ISA's
 * defined case) this is the 128-bit shift. */
VMX_INL void vmx_vsl(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP;
    for (int i = 0; i < 16; i++) { int s = vmx_b(b, i) & 7; uint8_t nx = i < 15 ? vmx_b(a, i + 1) : 0;
        vmx_sb(r, i, (uint8_t)((vmx_b(a, i) << s) | (s ? nx >> (8 - s) : 0))); } VMX_OUT; }
VMX_INL void vmx_vsr(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP;
    for (int i = 0; i < 16; i++) { int s = vmx_b(b, i) & 7; uint8_t pv = i > 0 ? vmx_b(a, i - 1) : 0;
        vmx_sb(r, i, (uint8_t)((vmx_b(a, i) >> s) | (s ? pv << (8 - s) : 0))); } VMX_OUT; }
/* vslo / vsro: by octets, count from bits 1:4 of B's last byte */
VMX_INL void vmx_vslo(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; int n = (vmx_b(b, 15) >> 3) & 15;
    for (int i = 0; i < 16; i++) vmx_sb(r, i, i + n < 16 ? vmx_b(a, i + n) : 0); VMX_OUT; }
VMX_INL void vmx_vsro(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; int n = (vmx_b(b, 15) >> 3) & 15;
    for (int i = 0; i < 16; i++) vmx_sb(r, i, i - n >= 0 ? vmx_b(a, i - n) : 0); VMX_OUT; }

/* ======================================================================
 * floating point
 * ==================================================================== */

/* Ops whose host result is a NaN whenever an operand is: the plain host result
 * stands unless vmx_plain finds a lane the exact sequence would change. */
#define VMX_FBIN(name, op) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t vscr) { \
    __m128 x = vmx_ldf(a_), y = vmx_ldf(b_); \
    const __m128 q = op; \
    if (vmx_plain(q, vmx_kq(x, vscr, VMX_KA), vmx_kq(y, vscr, VMX_KB), _mm_setzero_ps(), vscr)) { vmx_stf(d_, q); return; } \
    x = vmx_flushv(x, vscr); y = vmx_flushv(y, vscr); \
    vmx_stf(d_, vmx_flushv(vmx_nanv(vmx_nanv(vmx_fixv(op), y), x), vscr)); }
VMX_FBIN(vmx_vaddfp, _mm_add_ps(x, y))
VMX_FBIN(vmx_vsubfp, _mm_sub_ps(x, y))

#define VMX_FMINMAX(name, op) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t vscr) { \
    const __m128 x = vmx_ldf(a_), y = vmx_ldf(b_); \
    const __m128 n = _mm_or_ps(_mm_blendv_ps(y, x, _mm_cmpunord_ps(x, x)), vmx_k(0x7FC00000u)); \
    vmx_stf(d_, vmx_flushv(_mm_blendv_ps(op, n, _mm_cmpunord_ps(x, y)), vscr)); }
VMX_FMINMAX(vmx_vmaxfp, _mm_and_ps(_mm_max_ps(x, y), _mm_max_ps(y, x)))
VMX_FMINMAX(vmx_vminfp, _mm_or_ps(_mm_min_ps(x, y), _mm_min_ps(y, x)))

#define VMX_FMA(name, expr) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) { \
    __m128 x = vmx_ldf(a_), y = vmx_ldf(b_), z = vmx_ldf(c_); \
    const __m128 q = expr; \
    if (vmx_plain(q, vmx_kq(x, vscr, VMX_KA), vmx_kq(y, vscr, VMX_KB), vmx_kq(z, vscr, VMX_KC), vscr)) { vmx_stf(d_, q); return; } \
    x = vmx_flushv(x, vscr); y = vmx_flushv(y, vscr); z = vmx_flushv(z, vscr); \
    vmx_stf(d_, vmx_flushv(vmx_fixv(expr), vscr)); }
VMX_FMA(vmx_vmaddfp, _mm_fmadd_ps(x, z, y))
VMX_FMA(vmx_vnmsubfp, _mm_xor_ps(_mm_fmsub_ps(x, z, y), vmx_k(0x80000000u)))

#define VMX_FUN(name, expr, flush_in) \
VMX_INL void name(VMX_D, VMX_B, uint32_t vscr) { __m128 f = vmx_ldf(b_); \
    const __m128 q = expr; \
    if (vmx_plain(q, flush_in ? vmx_kq(f, vscr, VMX_KB) : _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps(), vscr)) { vmx_stf(d_, q); return; } \
    if (flush_in) f = vmx_flushv(f, vscr); \
    vmx_stf(d_, vmx_flushv(vmx_nanv(vmx_fixv(expr), f), vscr)); }
VMX_FUN(vmx_vrefp, _mm_div_ps(_mm_set1_ps(1.0f), f), 1)
VMX_FUN(vmx_vrsqrtefp, _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(f)), 1)
VMX_FUN(vmx_vrfin, _mm_round_ps(f, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC), 0)
VMX_FUN(vmx_vrfiz, _mm_round_ps(f, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC), 0)
VMX_FUN(vmx_vrfip, _mm_round_ps(f, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC), 1)
VMX_FUN(vmx_vrfim, _mm_round_ps(f, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC), 1)

#define VMX_LIBM(name, fn) \
VMX_INL void name(VMX_D, VMX_B, uint32_t vscr) { (void)vscr; const uint8_t* b = (const uint8_t*)b_; uint8_t r[16]; \
    for (int i = 0; i < 4; i++) { uint32_t z = vmx_u(fn(vmx_f(vmx_w(b, i)))); vmx_sw(r, i, vmx_fix(z)); } VMX_OUT; }
VMX_LIBM(vmx_vexptefp, exp2f)
VMX_LIBM(vmx_vlogefp, log2f)

/* int <-> float with a 2^uimm scale. uimm is 5 bits, so 2^-uimm and 2^uimm are
 * normal floats and the scaling is exact. */
VMX_INL __m128 vmx_pow2v(int e) { return vmx_k((uint32_t)(127 + e) << 23); }
VMX_INL void vmx_vcfsx(VMX_D, VMX_B, unsigned uimm) {
    vmx_stf(d_, _mm_mul_ps(_mm_cvtepi32_ps(vmx_ld(b_)), vmx_pow2v(-(int)(uimm & 31u)))); }
VMX_INL void vmx_vcfux(VMX_D, VMX_B, unsigned uimm) { const __m128i u = vmx_ld(b_);
    const __m128 f = _mm_add_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(u, 16)), _mm_set1_ps(65536.0f)),
                                _mm_cvtepi32_ps(_mm_and_si128(u, _mm_set1_epi32(0xFFFF))));
    vmx_stf(d_, _mm_mul_ps(f, vmx_pow2v(-(int)(uimm & 31u)))); }
VMX_INL void vmx_vctsxs(VMX_D, VMX_B, unsigned uimm, uint32_t* vscr) {
    const __m128 f = _mm_mul_ps(vmx_ldf(b_), vmx_pow2v((int)(uimm & 31u)));
    const __m128 hi = _mm_cmp_ps(f, _mm_set1_ps(2147483648.0f), _CMP_GE_OQ);
    const __m128 lo = _mm_cmp_ps(f, _mm_set1_ps(-2147483648.0f), _CMP_LT_OQ);
    __m128i r = _mm_blendv_epi8(_mm_cvttps_epi32(f), _mm_set1_epi32(INT32_MAX), _mm_castps_si128(hi));
    r = _mm_andnot_si128(_mm_castps_si128(_mm_cmpunord_ps(f, f)), r);
    if (_mm_movemask_ps(_mm_or_ps(hi, lo))) *vscr |= VMX_SAT;
    vmx_st(d_, r); }
VMX_INL void vmx_vctuxs(VMX_D, VMX_B, unsigned uimm, uint32_t* vscr) {
    const __m128 f = _mm_mul_ps(vmx_ldf(b_), vmx_pow2v((int)(uimm & 31u))), two31 = _mm_set1_ps(2147483648.0f);
    const __m128 hi = _mm_cmp_ps(f, _mm_set1_ps(4294967296.0f), _CMP_GE_OQ);
    const __m128 neg = _mm_cmp_ps(f, _mm_setzero_ps(), _CMP_LT_OQ);
    const __m128 big = _mm_cmp_ps(f, two31, _CMP_GE_OQ);
    const __m128i t = _mm_cvttps_epi32(_mm_sub_ps(f, _mm_and_ps(big, two31)));
    __m128i r = _mm_xor_si128(t, _mm_and_si128(_mm_castps_si128(big), _mm_set1_epi32(INT32_MIN)));
    r = _mm_or_si128(r, _mm_castps_si128(hi));
    r = _mm_andnot_si128(_mm_castps_si128(_mm_or_ps(neg, _mm_cmpunord_ps(f, f))), r);
    if (_mm_movemask_ps(_mm_or_ps(hi, neg))) *vscr |= VMX_SAT;
    vmx_st(d_, r); }

/* ---- compares; record forms return the CR6 nibble [all, 0, none, 0] ---- */
VMX_INL uint32_t vmx_cmpout(void* d_, __m128i m) { vmx_st(d_, m); return vmx_cr6(_mm_movemask_epi8(m), 0xFFFF); }
VMX_INL __m128i vmx_ub(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi8((char)0x80)); }
VMX_INL __m128i vmx_uh(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi16((short)0x8000)); }
VMX_INL __m128i vmx_uw(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi32(INT32_MIN)); }
VMX_INL uint32_t vmx_vcmpequb(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpeq_epi8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL uint32_t vmx_vcmpequh(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpeq_epi16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL uint32_t vmx_vcmpequw(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpeq_epi32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL uint32_t vmx_vcmpgtsb(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpgt_epi8(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL uint32_t vmx_vcmpgtub(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpgt_epi8(vmx_ub(vmx_ld(a_)), vmx_ub(vmx_ld(b_)))); }
VMX_INL uint32_t vmx_vcmpgtsh(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpgt_epi16(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL uint32_t vmx_vcmpgtuh(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpgt_epi16(vmx_uh(vmx_ld(a_)), vmx_uh(vmx_ld(b_)))); }
VMX_INL uint32_t vmx_vcmpgtsw(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpgt_epi32(vmx_ld(a_), vmx_ld(b_))); }
VMX_INL uint32_t vmx_vcmpgtuw(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_cmpgt_epi32(vmx_uw(vmx_ld(a_)), vmx_uw(vmx_ld(b_)))); }
VMX_INL uint32_t vmx_vcmpeqfp(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_castps_si128(_mm_cmp_ps(vmx_ldf(a_), vmx_ldf(b_), _CMP_EQ_OQ))); }
VMX_INL uint32_t vmx_vcmpgefp(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_castps_si128(_mm_cmp_ps(vmx_ldf(a_), vmx_ldf(b_), _CMP_GE_OQ))); }
VMX_INL uint32_t vmx_vcmpgtfp(VMX_D, VMX_A, VMX_B) { return vmx_cmpout(d_, _mm_castps_si128(_mm_cmp_ps(vmx_ldf(a_), vmx_ldf(b_), _CMP_GT_OQ))); }
/* vcmpbfp: bit 31 = !(a <= b), bit 30 = !(a >= -b); CR6 EQ = every lane in bounds */
VMX_INL uint32_t vmx_vcmpbfp(VMX_D, VMX_A, VMX_B) { const __m128 x = vmx_ldf(a_), y = vmx_ldf(b_);
    const __m128i le = _mm_castps_si128(_mm_cmp_ps(x, y, _CMP_LE_OQ));
    const __m128i ge = _mm_castps_si128(_mm_cmp_ps(x, _mm_xor_ps(y, vmx_k(0x80000000u)), _CMP_GE_OQ));
    const __m128i v = _mm_or_si128(_mm_andnot_si128(le, _mm_set1_epi32(INT32_MIN)), _mm_andnot_si128(ge, _mm_set1_epi32(0x40000000)));
    vmx_st(d_, v);
    return (uint32_t)(_mm_testz_si128(v, v) << 1); }

#ifdef __cplusplus
}
#endif
#endif /* PPU_VMX_H */
