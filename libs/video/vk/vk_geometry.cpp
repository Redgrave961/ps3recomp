#include "vk_geometry.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <limits>

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

bool disjoint(uint32_t prim)
{
    switch (prim) {
    case PRIM_LINE_LOOP: case PRIM_LINE_STRIP: case PRIM_POLYGON: case PRIM_QUAD_STRIP:
    case PRIM_TRIANGLE_FAN: case PRIM_TRIANGLE_STRIP:
        return false;
    default:
        return true;
    }
}

template <typename T>
index_result convert(uint32_t prim, const uint8_t* src, uint32_t count, bool restart, uint32_t restart_index, T* dst)
{
    constexpr T limit = std::numeric_limits<T>::max();
    if (sizeof(T) == 2 && restart_index > 0xFFFFu) restart = false;
    const T ri = (T)restart_index;
    T lo = limit, hi = 0;
    uint32_t n = 0;
    auto track = [&](T v) {
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        return v;
    };

    switch (prim) {
    case PRIM_QUADS: {
        T quad[4];
        uint32_t k = 0;
        for (uint32_t i = 0; i < count; i++) {
            const T v = load_be<T>(src + (size_t)i * sizeof(T));
            if (restart && v == ri) {
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
        break;
    }
    default: {
        if (!restart) {
            uint32_t i = 0;
#ifdef __AVX2__
            if constexpr (sizeof(T) == 2) {
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
#endif
            for (; i < count; i++) dst[i] = track(load_be<T>(src + (size_t)i * sizeof(T)));
            n = count;
        } else if (disjoint(prim)) {
            for (uint32_t i = 0; i < count; i++) {
                const T v = load_be<T>(src + (size_t)i * sizeof(T));
                if (v != ri) dst[n++] = track(v);
            }
        } else {
            uint32_t i = 0;
#ifdef __AVX2__
            if constexpr (sizeof(T) == 2) {
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
                        const __m256i eq = _mm256_cmpeq_epi16(v, vri);
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
                        const __m256i eq = _mm256_cmpeq_epi32(v, vri);
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
#endif
            for (; i < count; i++) {
                const T v = load_be<T>(src + (size_t)i * sizeof(T));
                dst[i] = v == ri ? limit : track(v);
            }
            n = count;
        }
        if (prim == PRIM_LINE_LOOP && count) dst[n++] = load_be<T>(src);
        break;
    }
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
    case PRIM_QUADS: return (6u * count) / 4u;
    default: return count;
    }
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

index_result convert_indices(uint32_t prim, bool wide, const uint8_t* src_be, uint32_t count, bool restart, uint32_t restart_index,
                             void* dst)
{
    if (wide) return convert<uint32_t>(prim, src_be, count, restart, restart_index, static_cast<uint32_t*>(dst));
    return convert<uint16_t>(prim, src_be, count, restart, restart_index, static_cast<uint16_t*>(dst));
}

}
