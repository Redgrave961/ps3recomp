#include "vk_geometry.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <type_traits>

namespace vk {

namespace {

template <typename T>
T load_be(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
    else return __builtin_bswap32(v);
}

bool restart_topology(VkPrimitiveTopology t)
{
    return t == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
}

template <typename S, typename D>
index_result convert(uint32_t prim, const index_format& f, const uint8_t* src, uint32_t count, D* dst)
{
    constexpr D limit = std::numeric_limits<D>::max();
    const S ri = (S)f.restart_index;
    D lo = limit, hi = 0;
    uint32_t n = 0;
    auto track = [&](D v) {
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        return v;
    };

    if (prim == PRIM_QUADS) {
        D quad[4];
        uint32_t k = 0;
        for (uint32_t i = 0; i < count; i++) {
            const S v = load_be<S>(src + (size_t)i * sizeof(S));
            if (f.restart && v == ri) {
                k = 0;
                continue;
            }
            quad[k++] = track(v);
            if (k == 4) {
                dst[n++] = quad[0];
                dst[n++] = quad[1];
                dst[n++] = quad[2];
                dst[n++] = quad[2];
                dst[n++] = quad[3];
                dst[n++] = quad[0];
                k = 0;
            }
        }
    } else if (!f.restart) {
        uint32_t i = 0;
#ifdef __AVX2__
        if constexpr (std::is_same_v<S, D>) {
            if constexpr (sizeof(D) == 2) {
                if (count >= 16u) {
                    const __m256i shuf = _mm256_setr_epi8(
                        1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
                        1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
                    __m256i vlo = _mm256_set1_epi16(-1);
                    __m256i vhi = _mm256_setzero_si256();
                    for (; i + 16u <= count; i += 16u) {
                        const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + (size_t)i * 2u)), shuf);
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v);
                        vlo = _mm256_min_epu16(vlo, v);
                        vhi = _mm256_max_epu16(vhi, v);
                    }
                    __m128i lo128 = _mm_min_epu16(_mm256_castsi256_si128(vlo), _mm256_extracti128_si256(vlo, 1));
                    __m128i hi128 = _mm_max_epu16(_mm256_castsi256_si128(vhi), _mm256_extracti128_si256(vhi, 1));
                    lo128 = _mm_min_epu16(lo128, _mm_srli_si128(lo128, 8));
                    hi128 = _mm_max_epu16(hi128, _mm_srli_si128(hi128, 8));
                    lo128 = _mm_min_epu16(lo128, _mm_srli_si128(lo128, 4));
                    hi128 = _mm_max_epu16(hi128, _mm_srli_si128(hi128, 4));
                    lo128 = _mm_min_epu16(lo128, _mm_srli_si128(lo128, 2));
                    hi128 = _mm_max_epu16(hi128, _mm_srli_si128(hi128, 2));
                    lo = (uint16_t)_mm_cvtsi128_si32(lo128);
                    hi = (uint16_t)_mm_cvtsi128_si32(hi128);
                }
            } else {
                if (count >= 8u) {
                    const __m256i shuf = _mm256_setr_epi8(
                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
                    __m256i vlo = _mm256_set1_epi32(-1);
                    __m256i vhi = _mm256_setzero_si256();
                    for (; i + 8u <= count; i += 8u) {
                        const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + (size_t)i * 4u)), shuf);
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v);
                        vlo = _mm256_min_epu32(vlo, v);
                        vhi = _mm256_max_epu32(vhi, v);
                    }
                    __m128i lo128 = _mm_min_epu32(_mm256_castsi256_si128(vlo), _mm256_extracti128_si256(vlo, 1));
                    __m128i hi128 = _mm_max_epu32(_mm256_castsi256_si128(vhi), _mm256_extracti128_si256(vhi, 1));
                    lo128 = _mm_min_epu32(lo128, _mm_srli_si128(lo128, 8));
                    hi128 = _mm_max_epu32(hi128, _mm_srli_si128(hi128, 8));
                    lo128 = _mm_min_epu32(lo128, _mm_srli_si128(lo128, 4));
                    hi128 = _mm_max_epu32(hi128, _mm_srli_si128(hi128, 4));
                    lo = (uint32_t)_mm_cvtsi128_si32(lo128);
                    hi = (uint32_t)_mm_cvtsi128_si32(hi128);
                }
            }
        }
#endif
        for (; i < count; i++) dst[i] = track(load_be<S>(src + (size_t)i * sizeof(S)));
        n = count;
        if (prim == PRIM_LINE_LOOP && count) dst[n++] = load_be<S>(src);
    } else if (!f.sentinel) {
        for (uint32_t i = 0; i < count; i++) {
            const S v = load_be<S>(src + (size_t)i * sizeof(S));
            if (v != ri) dst[n++] = track(v);
        }
    } else if (prim == PRIM_LINE_LOOP) {
        D head = 0;
        bool open = false;
        for (uint32_t i = 0; i < count; i++) {
            const S v = load_be<S>(src + (size_t)i * sizeof(S));
            if (v == ri || (D)v == limit) {
                if (open) dst[n++] = head;
                open = false;
                continue;
            }
            if (!open) {
                if (n) dst[n++] = limit;
                head = v;
                open = true;
            }
            dst[n++] = track(v);
        }
        if (open) dst[n++] = head;
    } else {
        uint32_t i = 0;
#ifdef __AVX2__
        if constexpr (std::is_same_v<S, D>) {
            if constexpr (sizeof(D) == 2) {
                if (count >= 16u) {
                    const __m256i shuf = _mm256_setr_epi8(
                        1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14,
                        1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
                    const __m256i vri = _mm256_set1_epi16((short)ri);
                    const __m256i vlim = _mm256_set1_epi16(-1);
                    __m256i vlo = vlim;
                    __m256i vhi = _mm256_setzero_si256();
                    for (; i + 16u <= count; i += 16u) {
                        const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + (size_t)i * 2u)), shuf);
                        const __m256i eq = _mm256_or_si256(_mm256_cmpeq_epi16(v, vri), _mm256_cmpeq_epi16(v, vlim));
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_or_si256(v, eq));
                        vlo = _mm256_min_epu16(vlo, _mm256_or_si256(v, eq));
                        vhi = _mm256_max_epu16(vhi, _mm256_andnot_si256(eq, v));
                    }
                    __m128i lo128 = _mm_min_epu16(_mm256_castsi256_si128(vlo), _mm256_extracti128_si256(vlo, 1));
                    __m128i hi128 = _mm_max_epu16(_mm256_castsi256_si128(vhi), _mm256_extracti128_si256(vhi, 1));
                    lo128 = _mm_min_epu16(lo128, _mm_srli_si128(lo128, 8));
                    hi128 = _mm_max_epu16(hi128, _mm_srli_si128(hi128, 8));
                    lo128 = _mm_min_epu16(lo128, _mm_srli_si128(lo128, 4));
                    hi128 = _mm_max_epu16(hi128, _mm_srli_si128(hi128, 4));
                    lo128 = _mm_min_epu16(lo128, _mm_srli_si128(lo128, 2));
                    hi128 = _mm_max_epu16(hi128, _mm_srli_si128(hi128, 2));
                    lo = (uint16_t)_mm_cvtsi128_si32(lo128);
                    hi = (uint16_t)_mm_cvtsi128_si32(hi128);
                }
            } else {
                if (count >= 8u) {
                    const __m256i shuf = _mm256_setr_epi8(
                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
                    const __m256i vri = _mm256_set1_epi32((int)ri);
                    const __m256i vlim = _mm256_set1_epi32(-1);
                    __m256i vlo = vlim;
                    __m256i vhi = _mm256_setzero_si256();
                    for (; i + 8u <= count; i += 8u) {
                        const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + (size_t)i * 4u)), shuf);
                        const __m256i eq = _mm256_or_si256(_mm256_cmpeq_epi32(v, vri), _mm256_cmpeq_epi32(v, vlim));
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_or_si256(v, eq));
                        vlo = _mm256_min_epu32(vlo, _mm256_or_si256(v, eq));
                        vhi = _mm256_max_epu32(vhi, _mm256_andnot_si256(eq, v));
                    }
                    __m128i lo128 = _mm_min_epu32(_mm256_castsi256_si128(vlo), _mm256_extracti128_si256(vlo, 1));
                    __m128i hi128 = _mm_max_epu32(_mm256_castsi256_si128(vhi), _mm256_extracti128_si256(vhi, 1));
                    lo128 = _mm_min_epu32(lo128, _mm_srli_si128(lo128, 8));
                    hi128 = _mm_max_epu32(hi128, _mm_srli_si128(hi128, 8));
                    lo128 = _mm_min_epu32(lo128, _mm_srli_si128(lo128, 4));
                    hi128 = _mm_max_epu32(hi128, _mm_srli_si128(hi128, 4));
                    lo = (uint32_t)_mm_cvtsi128_si32(lo128);
                    hi = (uint32_t)_mm_cvtsi128_si32(hi128);
                }
            }
        }
#endif
        for (; i < count; i++) {
            const S v = load_be<S>(src + (size_t)i * sizeof(S));
            dst[i] = (v == ri || (D)v == limit) ? limit : track(v);
        }
        n = count;
    }
    return { n, (uint32_t)lo, (uint32_t)hi };
}

}

VkPrimitiveTopology primitive_topology(uint32_t prim, bool& emulated)
{
    emulated = false;
    switch (prim) {
    case PRIM_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_LINE_LOOP: emulated = true; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TRIANGLE_STRIP: case PRIM_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TRIANGLE_FAN: case PRIM_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_QUADS: emulated = true; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

uint32_t topology_class(uint32_t prim)
{
    switch (prim) {
    case PRIM_POINTS: return 0;
    case PRIM_LINES: case PRIM_LINE_LOOP: case PRIM_LINE_STRIP: return 1;
    default: return 2;
    }
}

uint32_t expanded_index_count(uint32_t prim, uint32_t count)
{
    switch (prim) {
    case PRIM_LINE_LOOP: return count + 1u;
    case PRIM_QUADS: return (count / 4u) * 6u;
    default: return count;
    }
}

uint32_t index_capacity(uint32_t prim, uint32_t count)
{
    return prim == PRIM_LINE_LOOP ? count * 2u : expanded_index_count(prim, count);
}

void generate_indices(uint32_t prim, uint32_t count, uint32_t* dst)
{
    switch (prim) {
    case PRIM_LINE_LOOP:
        for (uint32_t i = 0; i < count; i++) dst[i] = i;
        dst[count] = 0;
        return;

    case PRIM_QUADS:
        for (uint32_t i = 0; i < count / 4u; i++) {
            dst[6 * i] = 4 * i;
            dst[6 * i + 1] = 4 * i + 1;
            dst[6 * i + 2] = 4 * i + 2;
            dst[6 * i + 3] = 4 * i + 2;
            dst[6 * i + 4] = 4 * i + 3;
            dst[6 * i + 5] = 4 * i;
        }
        return;
    default:
        for (uint32_t i = 0; i < count; i++) dst[i] = i;
        return;
    }
}

index_format index_format_for(uint32_t prim, bool wide, bool restart, uint32_t restart_index)
{
    bool emulated;
    index_format f{};
    f.wide = wide;
    f.restart = restart && (wide || restart_index <= 0xFFFFu);
    f.sentinel = f.restart && restart_topology(primitive_topology(prim, emulated));
    f.promote = f.sentinel && !wide && restart_index != 0xFFFFu;
    f.restart_index = restart_index;
    return f;
}

index_result convert_indices(uint32_t prim, const index_format& format, const uint8_t* src_be, uint32_t count, void* dst)
{
    if (format.wide) return convert<uint32_t, uint32_t>(prim, format, src_be, count, static_cast<uint32_t*>(dst));
    if (format.promote) return convert<uint16_t, uint32_t>(prim, format, src_be, count, static_cast<uint32_t*>(dst));
    return convert<uint16_t, uint16_t>(prim, format, src_be, count, static_cast<uint16_t*>(dst));
}

}
