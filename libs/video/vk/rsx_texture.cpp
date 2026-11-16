#include "rsx_texture.h"

#include <emmintrin.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace rsx {

namespace {

enum : uint32_t {
    FMT_B8 = 0x81,
    FMT_A1R5G5B5 = 0x82,
    FMT_A4R4G4B4 = 0x83,
    FMT_R5G6B5 = 0x84,
    FMT_A8R8G8B8 = 0x85,
    FMT_DXT1 = 0x86,
    FMT_DXT23 = 0x87,
    FMT_DXT45 = 0x88,
    FMT_G8B8 = 0x8B,
    FMT_B8R8_G8R8 = 0x8D,
    FMT_R8B8_R8G8 = 0x8E,
    FMT_R6G5B5 = 0x8F,
    FMT_DEPTH24_D8 = 0x90,
    FMT_DEPTH24_D8_FLOAT = 0x91,
    FMT_DEPTH16 = 0x92,
    FMT_DEPTH16_FLOAT = 0x93,
    FMT_X16 = 0x94,
    FMT_Y16_X16 = 0x95,
    FMT_R5G5B5A1 = 0x97,
    FMT_HILO8 = 0x98,
    FMT_HILO_S8 = 0x99,
    FMT_W16_Z16_Y16_X16_FLOAT = 0x9A,
    FMT_W32_Z32_Y32_X32_FLOAT = 0x9B,
    FMT_X32_FLOAT = 0x9C,
    FMT_D1R5G5B5 = 0x9D,
    FMT_D8R8G8B8 = 0x9E,
    FMT_Y16_X16_FLOAT = 0x9F,
    FMT_LN = 0x20,
    FMT_UN = 0x40,
    REMAP_IDENTITY = 0xAAE4,
};

enum conv_kind : uint8_t {
    CONV_COPY8,
    CONV_SWAP16,
    CONV_SWAP16X2,
    CONV_SWAP32,
    CONV_RGB655,
    CONV_D24,
    CONV_D24F,
    CONV_D16F,
    CONV_DXT,
    CONV_BR_GR,
    CONV_RB_RG,
};

enum : uint8_t {
    FEAT_SIGNED = 1,
    FEAT_BIASED = 2,
    FEAT_GAMMA = 4,
};

struct format_info {
    VkFormat vk;
    uint8_t block_bytes;
    uint8_t block_w;
    uint8_t block_h;
    uint8_t out_bytes;
    uint8_t conv;
    uint8_t features;
    uint8_t snorm_mask;
    uint8_t map[4];
};

constexpr std::array<format_info, 32> make_formats()
{
    std::array<format_info, 32> t{};
    constexpr uint8_t A = VK_COMPONENT_SWIZZLE_A;
    constexpr uint8_t R = VK_COMPONENT_SWIZZLE_R;
    constexpr uint8_t G = VK_COMPONENT_SWIZZLE_G;
    constexpr uint8_t B = VK_COMPONENT_SWIZZLE_B;
    constexpr uint8_t O = VK_COMPONENT_SWIZZLE_ONE;
    constexpr uint8_t all = FEAT_SIGNED | FEAT_BIASED | FEAT_GAMMA;
    auto set = [&](uint32_t f, VkFormat vk, uint8_t bb, uint8_t bw, uint8_t bh, uint8_t ob, uint8_t conv, uint8_t feat,
                   uint8_t snorm, uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
        t[f - 0x80] = { vk, bb, bw, bh, ob, conv, feat, snorm, { a, r, g, b } };
    };
    set(FMT_B8, VK_FORMAT_R8_UNORM, 1, 1, 1, 1, CONV_COPY8, all, 0xE, O, R, R, R);
    set(FMT_A1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, 1, 2, CONV_SWAP16, all, 0xF, A, R, G, B);
    set(FMT_A4R4G4B4, VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, 1, 1, 2, CONV_SWAP16, all, 0xF, R, G, B, A);
    set(FMT_R5G6B5, VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, 1, 2, CONV_SWAP16, all, 0xE, A, R, G, B);
    set(FMT_A8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 1, 4, CONV_SWAP32, all, 0xF, A, R, G, B);
    set(FMT_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 4, 8, CONV_DXT, all, 0xF, A, R, G, B);
    set(FMT_DXT23, VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 4, 16, CONV_DXT, all, 0xF, A, R, G, B);
    set(FMT_DXT45, VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 4, 16, CONV_DXT, all, 0xF, A, R, G, B);
    set(FMT_G8B8, VK_FORMAT_R8G8_UNORM, 2, 1, 1, 2, CONV_SWAP16, all, 0xF, G, R, G, R);
    set(FMT_B8R8_G8R8, VK_FORMAT_B8G8R8A8_UNORM, 4, 2, 1, 4, CONV_BR_GR, all, 0xE, A, R, G, B);
    set(FMT_R8B8_R8G8, VK_FORMAT_B8G8R8A8_UNORM, 4, 2, 1, 4, CONV_RB_RG, all, 0xE, A, R, G, B);
    set(FMT_R6G5B5, VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, 1, 2, CONV_RGB655, all, 0xE, A, R, G, B);
    set(FMT_DEPTH24_D8, VK_FORMAT_D32_SFLOAT, 4, 1, 1, 4, CONV_D24, FEAT_BIASED, 0xF, R, R, R, R);
    set(FMT_DEPTH24_D8_FLOAT, VK_FORMAT_D32_SFLOAT, 4, 1, 1, 4, CONV_D24F, FEAT_BIASED, 0xF, R, R, R, R);
    set(FMT_DEPTH16, VK_FORMAT_D16_UNORM, 2, 1, 1, 2, CONV_SWAP16, FEAT_BIASED, 0xF, R, R, R, R);
    set(FMT_DEPTH16_FLOAT, VK_FORMAT_D32_SFLOAT, 2, 1, 1, 4, CONV_D16F, FEAT_BIASED, 0xF, R, R, R, R);
    set(FMT_X16, VK_FORMAT_R16_UNORM, 2, 1, 1, 2, CONV_SWAP16, FEAT_BIASED, 0xF, R, O, R, O);
    set(FMT_Y16_X16, VK_FORMAT_R16G16_UNORM, 4, 1, 1, 4, CONV_SWAP16X2, FEAT_SIGNED | FEAT_BIASED, 0xF, G, R, G, R);
    set(FMT_R5G5B5A1, VK_FORMAT_R5G5B5A1_UNORM_PACK16, 2, 1, 1, 2, CONV_SWAP16, all, 0xF, A, R, G, B);
    set(FMT_HILO8, VK_FORMAT_R8G8_UNORM, 2, 1, 1, 2, CONV_SWAP16, FEAT_BIASED, 0xF, R, G, R, G);
    set(FMT_HILO_S8, VK_FORMAT_R8G8_SNORM, 2, 1, 1, 2, CONV_SWAP16, 0, 0xF, R, G, R, G);
    set(FMT_W16_Z16_Y16_X16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, 1, 8, CONV_SWAP16X2, 0, 0xF, A, R, G, B);
    set(FMT_W32_Z32_Y32_X32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, 1, 16, CONV_SWAP32, 0, 0xF, A, R, G, B);
    set(FMT_X32_FLOAT, VK_FORMAT_R32_SFLOAT, 4, 1, 1, 4, CONV_SWAP32, 0, 0xF, R, R, R, R);
    set(FMT_D1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, 1, 2, CONV_SWAP16, all, 0xE, O, R, G, B);
    set(FMT_D8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 1, 4, CONV_SWAP32, all, 0xE, O, R, G, B);
    set(FMT_Y16_X16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 4, 1, 1, 4, CONV_SWAP16X2, 0, 0xF, R, G, R, G);
    return t;
}

constexpr std::array<format_info, 32> g_formats = make_formats();

inline const format_info* find_format(uint32_t fmt)
{
    if (fmt < 0x80 || fmt > 0x9F) return nullptr;
    const format_info& f = g_formats[fmt - 0x80];
    return f.block_bytes ? &f : nullptr;
}

inline uint32_t floor_log2(uint32_t v)
{
    return v <= 1 ? 0 : 31u - static_cast<uint32_t>(std::countl_zero(v));
}

inline uint32_t ceil_log2(uint32_t v)
{
    return floor_log2(v) + ((v & (v - 1)) ? 1u : 0u);
}

inline uint32_t next_pow2(uint32_t x)
{
    if (x <= 2) return x;
    return static_cast<uint32_t>((1ull << 32) >> std::countl_zero(x - 1));
}

inline bool is_pow2(uint32_t v)
{
    return (v & (v - 1)) == 0;
}

inline uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

inline uint16_t load16(const uint8_t* p)
{
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

inline uint32_t load32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline void store16(uint8_t* p, uint16_t v)
{
    std::memcpy(p, &v, 2);
}

inline void store32(uint8_t* p, uint32_t v)
{
    std::memcpy(p, &v, 4);
}

inline uint16_t bswap16(uint16_t v)
{
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

inline uint32_t bswap32(uint32_t v)
{
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_bswap32(v);
#else
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
#endif
}

inline __m128i vswap16(__m128i v)
{
    return _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
}

inline __m128i vswap32(__m128i v)
{
    v = vswap16(v);
    return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xB1), 0xB1);
}

struct op_copy8 {
    static constexpr uint32_t in = 1, out = 1, simd = 0;
    static void run(const uint8_t* s, uint8_t* d, size_t n) { std::memcpy(d, s, n); }
};

struct op_swap16 {
    static constexpr uint32_t in = 2, out = 2, simd = 2;
    static __m128i vec(__m128i v) { return vswap16(v); }
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) store16(d + i * 2, bswap16(load16(s + i * 2)));
    }
};

struct op_swap16x2 {
    static constexpr uint32_t in = 4, out = 4, simd = 4;
    static __m128i vec(__m128i v) { return vswap16(v); }
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            const uint32_t v = load32(s + i * 4);
            store32(d + i * 4, ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u));
        }
    }
};

struct op_swap32 {
    static constexpr uint32_t in = 4, out = 4, simd = 4;
    static __m128i vec(__m128i v) { return vswap32(v); }
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) store32(d + i * 4, bswap32(load32(s + i * 4)));
    }
};

struct op_rgb655 {
    static constexpr uint32_t in = 2, out = 2, simd = 0;
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            const uint16_t v = bswap16(load16(s + i * 2));
            store16(d + i * 2, static_cast<uint16_t>((v & 0xF81Fu) | ((v & 0x3E0u) << 1)));
        }
    }
};

struct op_d24 {
    static constexpr uint32_t in = 4, out = 4, simd = 0;
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            const float f = static_cast<float>(bswap32(load32(s + i * 4)) >> 8) / 16777215.f;
            std::memcpy(d + i * 4, &f, 4);
        }
    }
};

struct op_d24f {
    static constexpr uint32_t in = 4, out = 4, simd = 0;
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) store32(d + i * 4, (bswap32(load32(s + i * 4)) >> 8) << 7);
    }
};

inline uint32_t e4m12_to_f32(uint32_t v)
{
    const uint32_t e = (v >> 12) & 0xF;
    const uint32_t m = v & 0xFFF;
    if (e) return ((e + 111u) << 23) | (m << 11);
    return std::bit_cast<uint32_t>(static_cast<float>(m) * 0x1p-27f);
}

struct op_d16f {
    static constexpr uint32_t in = 2, out = 4, simd = 0;
    static void run(const uint8_t* s, uint8_t* d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) store32(d + i * 4, e4m12_to_f32(bswap16(load16(s + i * 2))));
    }
};

struct level_layout {
    uint32_t w, h, d;
    uint32_t wb, hb;
    uint32_t pitch_blocks;
    uint32_t full_h;
    uint32_t border;
    uint64_t offset;
    uint64_t size;
};

constexpr uint32_t MAX_LEVELS = 17;

inline uint32_t shift_of(uint32_t v)
{
    return static_cast<uint32_t>(std::countr_zero(v));
}

uint32_t build_layout(const texture_desc& desc, const format_info& fi, level_layout* out, uint64_t& layer_size)
{
    const uint32_t bs = desc.border ? (desc.linear ? 1u : 4u) : 0u;
    const uint32_t sw = shift_of(fi.block_w), sh = shift_of(fi.block_h), sb = shift_of(fi.block_bytes);
    const uint32_t lin_pitch = desc.pitch >> sb;
    uint32_t w = desc.width, h = desc.height, d = desc.depth_slices;
    uint64_t off = 0;
    const uint32_t n = std::min(desc.levels, MAX_LEVELS);
    for (uint32_t i = 0; i < n; ++i) {
        level_layout L;
        L.w = w;
        L.h = h;
        L.d = d;
        L.wb = (w + fi.block_w - 1) >> sw;
        L.hb = (h + fi.block_h - 1) >> sh;
        L.border = bs;
        if (desc.linear) {
            L.pitch_blocks = lin_pitch;
            L.full_h = L.hb + bs * 2;
        } else if (!bs) {
            L.pitch_blocks = L.wb;
            L.full_h = L.hb;
        } else {
            L.pitch_blocks = next_pow2(L.wb + bs * 2);
            L.full_h = next_pow2(L.hb + bs * 2);
        }
        L.offset = off;
        L.size = (uint64_t(L.pitch_blocks) * L.full_h * d) << sb;
        off += L.size;
        if (out) out[i] = L;
        w = std::max(w >> 1, 1u);
        h = std::max(h >> 1, 1u);
        d = std::max(d >> 1, 1u);
    }
    layer_size = off;
    return n;
}

inline uint64_t layer_stride(const texture_desc& desc, uint64_t layer_size)
{
    return desc.linear ? layer_size : align_up(layer_size, 128);
}

inline bool is_dxt(const format_info& fi)
{
    return fi.conv == CONV_DXT;
}

inline bool is_422(const format_info& fi)
{
    return fi.conv == CONV_BR_GR || fi.conv == CONV_RB_RG;
}

inline uint64_t staging_row_bytes(const format_info& fi, const level_layout& L)
{
    return is_422(fi) ? uint64_t(L.w) * 4 : uint64_t(L.wb) * fi.out_bytes;
}

inline uint64_t staging_level_bytes(const format_info& fi, const level_layout& L)
{
    const uint64_t rows = is_dxt(fi) ? L.hb : L.h;
    return staging_row_bytes(fi, L) * rows * L.d;
}

constexpr uint64_t STAGING_ALIGN = 16;

struct zmasks {
    uint64_t x, y, z;
};

inline zmasks make_masks(uint32_t lw, uint32_t lh, uint32_t ld)
{
    zmasks m{ 0, 0, 0 };
    uint64_t bit = 1;
    while (lw | lh | ld) {
        if (lw) {
            m.x |= bit;
            bit <<= 1;
            --lw;
        }
        if (lh) {
            m.y |= bit;
            bit <<= 1;
            --lh;
        }
        if (ld) {
            m.z |= bit;
            bit <<= 1;
            --ld;
        }
    }
    return m;
}

inline uint64_t deposit(uint64_t v, uint64_t mask)
{
    uint64_t r = 0;
    for (uint64_t b = 1; mask && v; b <<= 1) {
        const uint64_t low = mask & (0 - mask);
        if (v & b) {
            r |= low;
            v &= ~b;
        }
        mask &= mask - 1;
    }
    return r;
}

template <class Op>
void deswizzle_4x4_u32(const uint8_t* src, uint8_t* dst, uint32_t vw, uint32_t vh, const zmasks& m)
{
    const uint64_t xs = m.x & ~uint64_t(5);
    const uint64_t ys = m.y & ~uint64_t(10);
    const size_t row = size_t(vw) * 4;
    uint64_t yo = 0;
    for (uint32_t y = 0; y < vh; y += 4) {
        uint8_t* r0 = dst + size_t(y) * row;
        uint8_t* r1 = r0 + row;
        uint8_t* r2 = r1 + row;
        uint8_t* r3 = r2 + row;
        uint64_t xo = 0;
        for (uint32_t x = 0; x < vw; x += 4) {
            const uint8_t* s = src + (yo + xo) * 4;
            const __m128i q0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
            const __m128i q1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
            const __m128i q2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
            const __m128i q3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
            const size_t o = size_t(x) * 4;
            _mm_storeu_si128(reinterpret_cast<__m128i*>(r0 + o), Op::vec(_mm_unpacklo_epi64(q0, q1)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(r1 + o), Op::vec(_mm_unpackhi_epi64(q0, q1)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(r2 + o), Op::vec(_mm_unpacklo_epi64(q2, q3)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(r3 + o), Op::vec(_mm_unpackhi_epi64(q2, q3)));
            xo = (xo - xs) & xs;
        }
        yo = (yo - ys) & ys;
    }
}

template <class Op>
void deswizzle_4x4_u16(const uint8_t* src, uint8_t* dst, uint32_t vw, uint32_t vh, const zmasks& m)
{
    const uint64_t xs = m.x & ~uint64_t(5);
    const uint64_t ys = m.y & ~uint64_t(10);
    const size_t row = size_t(vw) * 2;
    uint64_t yo = 0;
    for (uint32_t y = 0; y < vh; y += 4) {
        uint8_t* r0 = dst + size_t(y) * row;
        uint8_t* r1 = r0 + row;
        uint8_t* r2 = r1 + row;
        uint8_t* r3 = r2 + row;
        uint64_t xo = 0;
        for (uint32_t x = 0; x < vw; x += 4) {
            const uint8_t* s = src + (yo + xo) * 2;
            const __m128i q0 = Op::vec(_mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s)), 0xD8));
            const __m128i q1 = Op::vec(_mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16)), 0xD8));
            const size_t o = size_t(x) * 2;
            _mm_storel_epi64(reinterpret_cast<__m128i*>(r0 + o), q0);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(r1 + o), _mm_unpackhi_epi64(q0, q0));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(r2 + o), q1);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(r3 + o), _mm_unpackhi_epi64(q1, q1));
            xo = (xo - xs) & xs;
        }
        yo = (yo - ys) & ys;
    }
}

template <class Op>
void deswizzle_2x2(const uint8_t* src, uint8_t* dst, uint32_t vw, uint32_t vh, const zmasks& m)
{
    const uint64_t xs = m.x & ~uint64_t(1);
    const uint64_t ys = m.y & ~uint64_t(2);
    const size_t row = size_t(vw) * Op::out;
    uint64_t yo = 0;
    for (uint32_t y = 0; y < vh; y += 2) {
        uint8_t* r0 = dst + size_t(y) * row;
        uint8_t* r1 = r0 + row;
        uint64_t xo = 0;
        for (uint32_t x = 0; x < vw; x += 2) {
            const uint8_t* s = src + (yo + xo) * Op::in;
            Op::run(s, r0 + size_t(x) * Op::out, 2);
            Op::run(s + 2 * Op::in, r1 + size_t(x) * Op::out, 2);
            xo = (xo - xs) & xs;
        }
        yo = (yo - ys) & ys;
    }
}

template <class Op>
void deswizzle_generic(const uint8_t* src, uint64_t avail_units, uint8_t* dst, const zmasks& m, uint32_t vd, uint32_t x0,
                       uint32_t xn, uint32_t y0, uint32_t yn)
{
    const uint64_t xstart = deposit(x0, m.x);
    const uint64_t ystart = deposit(y0, m.y);
    uint64_t zo = 0;
    for (uint32_t z = 0; z < vd; ++z) {
        uint64_t yo = ystart;
        for (uint32_t y = 0; y < yn; ++y) {
            uint64_t xo = xstart;
            for (uint32_t x = 0; x < xn; ++x) {
                const uint64_t idx = zo + yo + xo;
                if (idx < avail_units) {
                    Op::run(src + idx * Op::in, dst, 1);
                } else {
                    std::memset(dst, 0, Op::out);
                }
                dst += Op::out;
                xo = (xo - m.x) & m.x;
            }
            yo = (yo - m.y) & m.y;
        }
        zo = (zo - m.z) & m.z;
    }
}

template <class Op>
void deswizzle_level(const uint8_t* src, uint64_t avail, uint8_t* dst, const level_layout& L, uint32_t bpb)
{
    const uint32_t k = bpb > 4 ? bpb / 4 : 1;
    const uint32_t vw = L.pitch_blocks * k;
    const uint32_t vh = L.full_h;
    const uint32_t vd = L.d;
    const uint32_t lw = ceil_log2(vw);
    const uint32_t lh = ceil_log2(vh);
    const uint32_t ld = ceil_log2(vd);
    const zmasks m = make_masks(lw, lh, ld);
    const bool pow2 = is_pow2(vw) && is_pow2(vh) && is_pow2(vd);
    const uint64_t full = uint64_t(vw) * vh * vd * Op::in;
    if (!L.border && vd == 1 && pow2 && full <= avail) {
        if constexpr (Op::simd == 4) {
            if (lw >= 2 && lh >= 2) {
                deswizzle_4x4_u32<Op>(src, dst, vw, vh, m);
                return;
            }
        }
        if constexpr (Op::simd == 2) {
            if (lw >= 2 && lh >= 2) {
                deswizzle_4x4_u16<Op>(src, dst, vw, vh, m);
                return;
            }
        }
        if (lw >= 1 && lh >= 1) {
            deswizzle_2x2<Op>(src, dst, vw, vh, m);
            return;
        }
    }
    deswizzle_generic<Op>(src, avail / Op::in, dst, m, vd, L.border * k, L.wb * k, L.border, L.hb);
}

template <class Op>
void linear_level(const uint8_t* src, uint8_t* dst, const level_layout& L, uint32_t bpb)
{
    const size_t units = size_t(L.wb) * bpb / Op::in;
    const size_t row_out = units * Op::out;
    const size_t pitch = size_t(L.pitch_blocks) * bpb;
    if (!L.border && L.pitch_blocks == L.wb) {
        Op::run(src, dst, units * L.hb * L.d);
        return;
    }
    const size_t h_porch = size_t(L.border) * bpb;
    for (uint32_t z = 0; z < L.d; ++z) {
        const uint8_t* slice = src + size_t(z) * L.full_h * pitch + size_t(L.border) * pitch + h_porch;
        for (uint32_t y = 0; y < L.hb; ++y) {
            Op::run(slice + size_t(y) * pitch, dst, units);
            dst += row_out;
        }
    }
}

template <class Op>
void convert_plain(const uint8_t* src, uint64_t avail, uint8_t* dst, const level_layout& L, uint32_t bpb, bool linear)
{
    if (linear) {
        linear_level<Op>(src, dst, L, bpb);
    } else {
        deswizzle_level<Op>(src, avail, dst, L, bpb);
    }
}

void convert_dxt(const uint8_t* src, uint8_t* dst, const level_layout& L, uint32_t bb)
{
    const size_t plane = size_t(L.wb) * L.hb;
    if (L.d > 1 && is_pow2(L.w) && is_pow2(L.h)) {
        const uint32_t n4 = L.d & ~3u;
        for (uint32_t z = 0; z < L.d; ++z) {
            size_t base, tile, sub;
            if (z < n4) {
                base = size_t(z & ~3u) * plane;
                tile = 4;
                sub = z & 3u;
            } else {
                base = size_t(n4) * plane;
                tile = L.d - n4;
                sub = z - n4;
            }
            const uint8_t* s = src + (base + sub) * bb;
            for (size_t i = 0; i < plane; ++i) {
                std::memcpy(dst, s + i * tile * bb, bb);
                dst += bb;
            }
        }
        return;
    }
    const size_t row = size_t(L.wb) * bb;
    const size_t pitch = size_t(L.pitch_blocks) * bb;
    if (L.pitch_blocks == L.wb && L.full_h == L.hb) {
        std::memcpy(dst, src, row * L.hb * L.d);
        return;
    }
    for (uint32_t z = 0; z < L.d; ++z) {
        const uint8_t* slice = src + size_t(z) * L.full_h * pitch;
        for (uint32_t y = 0; y < L.hb; ++y) {
            std::memcpy(dst, slice + size_t(y) * pitch, row);
            dst += row;
        }
    }
}

void convert_422(const uint8_t* src, uint8_t* dst, const level_layout& L, bool br_gr)
{
    const size_t pitch = size_t(L.pitch_blocks) * 4;
    const size_t h_porch = size_t(L.border) * 4;
    for (uint32_t z = 0; z < L.d; ++z) {
        const uint8_t* slice = src + size_t(z) * L.full_h * pitch + size_t(L.border) * pitch + h_porch;
        for (uint32_t y = 0; y < L.h; ++y) {
            const uint8_t* s = slice + size_t(y) * pitch;
            for (uint32_t x = 0; x < L.w; ++x) {
                const uint8_t* b = s + (x >> 1) * 4;
                uint8_t blue, green, red;
                if (br_gr) {
                    blue = b[0];
                    green = b[2];
                    red = (x & 1) ? b[3] : b[1];
                } else {
                    blue = b[1];
                    green = b[3];
                    red = (x & 1) ? b[2] : b[0];
                }
                dst[0] = blue;
                dst[1] = green;
                dst[2] = red;
                dst[3] = 0xFF;
                dst += 4;
            }
        }
    }
}

void convert_level(const format_info& fi, const uint8_t* src, uint64_t avail, uint8_t* dst, const level_layout& L, bool linear)
{
    const uint32_t bpb = fi.block_bytes;
    if (linear && L.wb > L.pitch_blocks) {
        std::memset(dst, 0, staging_level_bytes(fi, L));
        return;
    }
    switch (fi.conv) {
    case CONV_COPY8: convert_plain<op_copy8>(src, avail, dst, L, bpb, linear); break;
    case CONV_SWAP16: convert_plain<op_swap16>(src, avail, dst, L, bpb, linear); break;
    case CONV_SWAP16X2: convert_plain<op_swap16x2>(src, avail, dst, L, bpb, linear); break;
    case CONV_SWAP32: convert_plain<op_swap32>(src, avail, dst, L, bpb, linear); break;
    case CONV_RGB655: convert_plain<op_rgb655>(src, avail, dst, L, bpb, linear); break;
    case CONV_D24: convert_plain<op_d24>(src, avail, dst, L, bpb, linear); break;
    case CONV_D24F: convert_plain<op_d24f>(src, avail, dst, L, bpb, linear); break;
    case CONV_D16F: convert_plain<op_d16f>(src, avail, dst, L, bpb, linear); break;
    case CONV_DXT: convert_dxt(src, dst, L, bpb); break;
    case CONV_BR_GR: convert_422(src, dst, L, true); break;
    case CONV_RB_RG: convert_422(src, dst, L, false); break;
    default: break;
    }
}

struct remap_t {
    uint8_t channel[4];
    uint8_t control[4];
};

inline uint32_t fixup_remap(uint32_t ctl, uint32_t fmt)
{
    const uint32_t over = (ctl >> 16) & 0xFFFF;
    switch (fmt) {
    case FMT_X32_FLOAT:
    case FMT_W32_Z32_Y32_X32_FLOAT:
    case FMT_W16_Z16_Y16_X16_FLOAT:
        return (ctl & ~0xFFu) | 0xE4u;
    case FMT_Y16_X16_FLOAT:
        return (ctl & ~0xFFu) | (over ? 0x56u : 0x66u);
    case FMT_X16:
    case FMT_Y16_X16:
    case FMT_HILO8:
    case FMT_HILO_S8: {
        uint32_t lo = ctl & 0xFF;
        switch (lo) {
        case 0xE4: lo = over ? 0x56u : 0x66u; break;
        case 0x4E: lo = over ? 0xA9u : 0x99u; break;
        case 0xEE: lo = 0xAA; break;
        case 0x44: lo = 0x55; break;
        default: break;
        }
        return (ctl & 0xFF00u) | lo;
    }
    default: return ctl;
    }
}

inline remap_t decode_remap(uint32_t ctl)
{
    remap_t r;
    for (uint32_t i = 0; i < 4; ++i) {
        r.channel[i] = static_cast<uint8_t>((ctl >> (i * 2)) & 3);
        r.control[i] = static_cast<uint8_t>((ctl >> (8 + i * 2)) & 3);
    }
    return r;
}

inline uint32_t shuffle_mask(const remap_t& r, uint32_t encoded, uint32_t bits)
{
    if (!bits || encoded == REMAP_IDENTITY) return bits;
    uint32_t out = 0;
    for (uint32_t c = 0; c < 4; ++c) {
        if (r.control[c] != 2 || !(bits & (1u << r.channel[c]))) continue;
        out |= 1u << c;
    }
    return out;
}

inline uint8_t argb_to_rgba(uint32_t m)
{
    return static_cast<uint8_t>(((m >> 1) & 7u) | ((m & 1u) << 3));
}

inline VkFormat snorm_variant(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SNORM;
    case VK_FORMAT_R16_UNORM: return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_SNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

inline VkFormat srgb_variant(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

inline uint32_t wrap_bits(const texture_regs& regs, bool vertex_unit, uint32_t axis)
{
    if (axis == 2 && vertex_unit) return 1;
    return (regs.address >> (axis * 8)) & 0xF;
}

inline bool hw_clamp_wrap(uint32_t w)
{
    return w == 3 || w == 4 || w == 5;
}

inline bool vertex_hw_unnormalized(const texture_regs& regs, uint32_t dim, bool cube, uint32_t levels)
{
    if (!((regs.format >> 8) & FMT_UN)) return false;
    if (dim > 2 || cube || levels != 1) return false;
    if (!hw_clamp_wrap(regs.address & 0xF)) return false;
    return dim == 1 || hw_clamp_wrap((regs.address >> 8) & 0xF);
}

inline VkSamplerAddressMode vk_wrap(uint32_t w)
{
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 6:
    case 7:
    case 8: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

inline float decode_lod(uint32_t bits)
{
    return static_cast<float>(bits & 0xFFF) / 256.f;
}

inline float decode_bias(uint32_t filter)
{
    const uint32_t bits = filter & 0x1FFF;
    float bias;
    if (bits & 0x1000) {
        bias = -static_cast<float>((0u - bits) & 0x1FFF) / 256.f;
    } else {
        bias = static_cast<float>(bits) / 256.f;
    }
    return std::clamp(bias, -16.f, 16.f - 1.f / 256.f);
}

inline VkCompareOp vk_compare_reversed(uint32_t f)
{
    switch (f) {
    case 1: return VK_COMPARE_OP_GREATER;
    case 2: return VK_COMPARE_OP_EQUAL;
    case 3: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 4: return VK_COMPARE_OP_LESS;
    case 5: return VK_COMPARE_OP_NOT_EQUAL;
    case 6: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 7: return VK_COMPARE_OP_ALWAYS;
    default: return VK_COMPARE_OP_NEVER;
    }
}

}

bool texture_describe(const texture_regs& regs, bool vertex_unit, texture_desc& out)
{
    out = {};
    if (!(regs.control0 >> 31)) return false;
    const uint32_t loc = regs.format & 3;
    if (loc != 1 && loc != 2) return false;
    const uint32_t dim = (regs.format >> 4) & 0xF;
    if (dim < 1 || dim > 3) return false;
    const uint32_t raw_fmt = (regs.format >> 8) & 0xFF;
    const uint32_t fmt = raw_fmt & ~(FMT_LN | FMT_UN);
    const format_info* pfi = find_format(fmt);
    if (!pfi) return false;
    const format_info& fi = *pfi;
    const uint32_t mip = regs.format >> 16;
    if (!mip) return false;

    const bool cube = dim == 2 && ((regs.format >> 2) & 1);
    const bool linear = (raw_fmt & FMT_LN) != 0;
    uint32_t width = regs.image_rect >> 16;
    uint32_t height = dim == 1 ? 1 : (regs.image_rect & 0xFFFF);
    uint32_t depth = dim == 3 ? (regs.control3 >> 20) : 1;
    if (!width || !height || !depth) return false;
    if (cube && width != height) return false;

    const bool border = !vertex_unit && !((regs.format >> 3) & 1) && !is_dxt(fi);
    uint32_t pitch;
    const uint32_t wb0 = (width + fi.block_w - 1) >> shift_of(fi.block_w);
    if (linear) {
        const uint32_t packed = (wb0 + (border ? 2u : 0u)) * fi.block_bytes;
        pitch = regs.control3 & 0xFFFFF;
        if (pitch < packed) {
            if (pitch) {
                width = std::max(pitch / fi.block_bytes * fi.block_w, 1u);
            } else {
                height = 1;
                depth = 1;
                pitch = packed;
            }
        }
    } else {
        pitch = (border ? next_pow2(wb0 + 8) : wb0) * fi.block_bytes;
    }

    uint32_t max_levels;
    if (is_dxt(fi) && !vertex_unit) {
        max_levels = floor_log2(std::min(width / 4, height / 4)) + 1;
    } else {
        max_levels = floor_log2(std::max(width, height)) + 1;
    }

    out.location = loc - 1;
    out.offset = regs.offset & 0x7FFFFFFF;
    out.gcm_format = fmt;
    out.linear = linear;
    out.cube = cube;
    out.compressed = is_dxt(fi);
    out.depth = fmt >= FMT_DEPTH24_D8 && fmt <= FMT_DEPTH16_FLOAT;
    out.border = border;
    out.dimension = (dim == 1 && out.compressed) ? 2 : dim;
    out.width = width;
    out.height = height;
    out.depth_slices = depth;
    out.levels = std::min(mip, max_levels);
    out.layers = cube ? 6 : 1;
    out.pitch = pitch;
    out.aspect = out.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    uint64_t layer_size = 0;
    build_layout(out, fi, nullptr, layer_size);
    const uint64_t total = uint64_t(out.layers - 1) * layer_stride(out, layer_size) + layer_size;
    if (total > 0xFFFFFFFFull) return false;
    out.guest_size = static_cast<uint32_t>(total);

    VkFormat format = fi.vk;
    const bool un = (raw_fmt & FMT_UN) != 0;
    uint32_t remap_ctl = REMAP_IDENTITY;
    uint32_t sign = 0, gamma = 0, bx2 = 0;
    if (!vertex_unit) {
        remap_ctl = fixup_remap(regs.control1, fmt);
        const remap_t rm = decode_remap(remap_ctl);
        if (fi.features & FEAT_SIGNED) sign = shuffle_mask(rm, remap_ctl, (regs.filter >> 28) & 0xF);
        if (fi.features & FEAT_GAMMA) {
            const uint32_t g = (regs.address >> 20) & 0xF;
            gamma = (((g << 1) & 0xF) | (g >> 3)) & ~sign;
        }
        if ((fi.features & FEAT_BIASED) && ((regs.address >> 12) & 0xF) == 1) {
            bx2 = shuffle_mask(rm, remap_ctl, 0xF) & ~(sign | gamma);
        }
        if (sign | gamma | bx2) {
            if (sign == fi.snorm_mask) {
                const VkFormat f = snorm_variant(format);
                if (f != VK_FORMAT_UNDEFINED) {
                    format = f;
                    sign = 0;
                }
            } else if (regs.control1 == REMAP_IDENTITY && gamma == 0xE) {
                const VkFormat f = srgb_variant(format);
                if (f != VK_FORMAT_UNDEFINED) {
                    format = f;
                    gamma = 0;
                }
            }
        }
        const uint32_t zfunc = (regs.address >> 28) & 0xF;
        if (out.depth && !((regs.control0 >> 2) & 1) && zfunc > 0 && zfunc < 7) {
            out.ops.depth_compare = static_cast<uint8_t>(zfunc);
        }
        out.ops.unnormalized = un ? 1 : 0;
    } else {
        out.ops.unnormalized = (un && !vertex_hw_unnormalized(regs, out.dimension, cube, out.levels)) ? 1 : 0;
    }
    out.ops.sign_mask = argb_to_rgba(sign);
    out.ops.gamma_mask = argb_to_rgba(gamma);
    out.ops.bx2_mask = argb_to_rgba(bx2);
    out.format = format;

    if (out.depth) {
        out.swizzle = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R };
    } else {
        const remap_t rm = decode_remap(remap_ctl);
        VkComponentSwizzle m[4];
        for (uint32_t c = 0; c < 4; ++c) {
            switch (rm.control[c]) {
            case 0: m[c] = VK_COMPONENT_SWIZZLE_ZERO; break;
            case 1: m[c] = VK_COMPONENT_SWIZZLE_ONE; break;
            default: m[c] = static_cast<VkComponentSwizzle>(fi.map[rm.channel[c]]); break;
            }
        }
        out.swizzle = { m[1], m[2], m[3], m[0] };
    }
    return true;
}

uint64_t texture_staging_size(const texture_desc& desc)
{
    const format_info* pfi = find_format(desc.gcm_format);
    if (!pfi) return 0;
    level_layout layout[MAX_LEVELS];
    uint64_t layer_size = 0;
    const uint32_t n = build_layout(desc, *pfi, layout, layer_size);
    uint64_t off = 0;
    for (uint32_t layer = 0; layer < desc.layers; ++layer) {
        for (uint32_t i = 0; i < n; ++i) {
            off = align_up(off, STAGING_ALIGN) + staging_level_bytes(*pfi, layout[i]);
        }
    }
    return off;
}

void texture_convert(const texture_desc& desc, const uint8_t* guest, uint8_t* staging, std::vector<texture_copy>& copies)
{
    const format_info* pfi = find_format(desc.gcm_format);
    if (!pfi) return;
    const format_info& fi = *pfi;
    level_layout layout[MAX_LEVELS];
    uint64_t layer_size = 0;
    const uint32_t n = build_layout(desc, fi, layout, layer_size);
    const uint64_t stride = layer_stride(desc, layer_size);
    copies.reserve(copies.size() + size_t(desc.layers) * n);
    uint64_t off = 0;
    const bool dxt = is_dxt(fi);
    const bool plain_layout = dxt || is_422(fi) || desc.linear;
    for (uint32_t layer = 0; layer < desc.layers; ++layer) {
        for (uint32_t i = 0; i < n; ++i) {
            const level_layout& L = layout[i];
            off = align_up(off, STAGING_ALIGN);
            const uint64_t src_off = layer * stride + L.offset;
            const uint64_t avail = desc.guest_size > src_off ? desc.guest_size - src_off : 0;
            convert_level(fi, guest + src_off, avail, staging + off, L, plain_layout);
            texture_copy c;
            c.level = i;
            c.layer = layer;
            c.width = L.w;
            c.height = L.h;
            c.depth = L.d;
            c.offset = off;
            c.row_texels = dxt ? L.wb * 4 : L.w;
            c.rows = dxt ? L.hb * 4 : L.h;
            copies.push_back(c);
            off += staging_level_bytes(fi, L);
        }
    }
}

void texture_sampler(const texture_regs& regs, bool vertex_unit, const texture_desc& desc, float max_anisotropy,
                     VkSamplerCreateInfo& info, VkSamplerCustomBorderColorCreateInfoEXT& border)
{
    info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    border = {};
    border.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;

    const uint32_t ws = wrap_bits(regs, vertex_unit, 0);
    const uint32_t wt = wrap_bits(regs, vertex_unit, 1);
    const uint32_t wr = wrap_bits(regs, vertex_unit, 2);
    info.addressModeU = vk_wrap(ws);
    info.addressModeV = vk_wrap(wt);
    info.addressModeW = vk_wrap(wr);
    info.maxAnisotropy = 1.f;
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    info.compareOp = VK_COMPARE_OP_NEVER;

    const uint32_t dim = (regs.format >> 4) & 0xF;
    const bool cube = dim == 2 && ((regs.format >> 2) & 1);

    if (vertex_unit) {
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.minLod = decode_lod(regs.control0 >> 19);
        info.maxLod = decode_lod(regs.control0 >> 7);
        if (vertex_hw_unnormalized(regs, desc.dimension, cube, desc.levels)) {
            info.unnormalizedCoordinates = VK_TRUE;
            info.minLod = 0.f;
            info.maxLod = 0.f;
            if (dim == 1) info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    } else {
        static constexpr uint8_t min_tab[8][3] = {
            { 0, 0, 0 }, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 0, 0 },
        };
        const uint32_t minf = (regs.filter >> 16) & 7;
        const uint32_t magf = (regs.filter >> 24) & 7;
        VkFilter min_filter = min_tab[minf][0] ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VkSamplerMipmapMode mip_mode = min_tab[minf][1] ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        const bool sample_mips = min_tab[minf][2] != 0;
        VkFilter mag_filter = (magf == 2 || magf == 4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

        const bool can_linear = desc.depth || desc.ops.sign_mask == 0;
        if (!can_linear) {
            mag_filter = VK_FILTER_NEAREST;
            min_filter = VK_FILTER_NEAREST;
            mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }

        float min_lod = 0.f, max_lod = 0.f, bias = 0.f;
        if (sample_mips && desc.levels > 1) {
            const float top = static_cast<float>(desc.levels - 1);
            min_lod = std::min(decode_lod(regs.control0 >> 19), top);
            max_lod = std::min(decode_lod(regs.control0 >> 7), top);
            bias = decode_bias(regs.filter);
            if (mip_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST) bias = std::floor(bias * 2.f + 0.5f) * 0.5f;
        } else {
            mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }

        info.magFilter = mag_filter;
        info.minFilter = min_filter;
        info.mipmapMode = mip_mode;
        info.minLod = min_lod;
        info.maxLod = max_lod;
        info.mipLodBias = bias;

        static constexpr float aniso_tab[8] = { 1.f, 2.f, 4.f, 6.f, 8.f, 10.f, 12.f, 16.f };
        float af = aniso_tab[(regs.control0 >> 4) & 7];
        af = std::min(af, std::max(max_anisotropy, 1.f));
        if (af >= 2.f) {
            info.anisotropyEnable = VK_TRUE;
            info.maxAnisotropy = af;
        }

        if (desc.ops.depth_compare) {
            info.compareEnable = VK_TRUE;
            info.compareOp = vk_compare_reversed(desc.ops.depth_compare);
        }
    }
    if (info.maxLod < info.minLod) info.maxLod = info.minLod;

    bool clamped;
    switch (dim) {
    case 1: clamped = ws == 4; break;
    case 2: clamped = ws == 4 || wt == 4; break;
    case 3: clamped = ws == 4 || wt == 4 || wr == 4; break;
    default: clamped = false; break;
    }
    if (!clamped) return;

    uint32_t raw = regs.border;
    float c[4];
    if (!vertex_unit) {
        const uint32_t sext = (regs.filter >> 28) & 0xF;
        if (desc.ops.sign_mask) {
            uint32_t mask = 0;
            if (sext & 1) mask |= 0xFF000000u;
            if (sext & 2) mask |= 0x00FF0000u;
            if (sext & 4) mask |= 0x0000FF00u;
            if (sext & 8) mask |= 0x000000FFu;
            raw ^= 0x80808080u & mask;
        }
        const float in[4] = { static_cast<float>((raw >> 24) & 0xFF) / 255.f, static_cast<float>((raw >> 16) & 0xFF) / 255.f,
                              static_cast<float>((raw >> 8) & 0xFF) / 255.f, static_cast<float>(raw & 0xFF) / 255.f };
        if (regs.control1 == REMAP_IDENTITY) {
            std::memcpy(c, in, sizeof(c));
        } else {
            const remap_t rm = decode_remap(fixup_remap(regs.control1, desc.gcm_format));
            for (uint32_t i = 0; i < 4; ++i) {
                switch (rm.control[i]) {
                case 0: c[i] = 0.f; break;
                case 1: c[i] = 1.f; break;
                default: c[i] = in[rm.channel[i]]; break;
                }
            }
        }
        const format_info* pfi = find_format(desc.gcm_format);
        if (pfi && desc.format != pfi->vk && snorm_variant(pfi->vk) == desc.format) {
            for (uint32_t i = 0; i < 4; ++i) {
                if (sext & (1u << i)) c[i] = (c[i] - 128.f / 255.f) * (255.f / 127.f);
            }
        }
    } else {
        c[0] = static_cast<float>((raw >> 24) & 0xFF) / 255.f;
        c[1] = static_cast<float>((raw >> 16) & 0xFF) / 255.f;
        c[2] = static_cast<float>((raw >> 8) & 0xFF) / 255.f;
        c[3] = static_cast<float>(raw & 0xFF) / 255.f;
    }

    const float a = c[0], r = c[1], g = c[2], b = c[3];
    if (a == 0.f && r == 0.f && g == 0.f && b == 0.f) {
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    } else if (a == 1.f && r == 1.f && g == 1.f && b == 1.f) {
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    } else if (a == 1.f && r == 0.f && g == 0.f && b == 0.f) {
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else {
        info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        border.format = desc.format;
        border.customBorderColor.float32[0] = r;
        border.customBorderColor.float32[1] = g;
        border.customBorderColor.float32[2] = b;
        border.customBorderColor.float32[3] = a;
    }
}

}
