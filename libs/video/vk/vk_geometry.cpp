#include "vk_geometry.h"

#include <cstring>
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
    case PRIM_POLYGON:
    case PRIM_TRIANGLE_FAN: {
        bool need_anchor = true;
        T anchor = limit, last = limit;
        for (uint32_t i = 0; i < count; i++) {
            const T v = load_be<T>(src + (size_t)i * sizeof(T));
            if (need_anchor) {
                if (restart && v == ri) continue;
                anchor = track(v);
                need_anchor = false;
                continue;
            }
            if (restart && v == ri) {
                need_anchor = true;
                last = limit;
                continue;
            }
            if (last == limit) {
                last = track(v);
                continue;
            }
            dst[n++] = anchor;
            dst[n++] = last;
            dst[n++] = track(v);
            last = v;
        }
        break;
    }
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
            for (uint32_t i = 0; i < count; i++) dst[i] = track(load_be<T>(src + (size_t)i * sizeof(T)));
            n = count;
        } else if (disjoint(prim)) {
            for (uint32_t i = 0; i < count; i++) {
                const T v = load_be<T>(src + (size_t)i * sizeof(T));
                if (v != ri) dst[n++] = track(v);
            }
        } else {
            for (uint32_t i = 0; i < count; i++) {
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
    case PRIM_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_QUADS: case PRIM_POLYGON: emulated = true; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
    case PRIM_POLYGON: case PRIM_TRIANGLE_FAN: return count >= 2u ? (count - 2u) * 3u : 0u;
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
    case PRIM_POLYGON:
    case PRIM_TRIANGLE_FAN:
        if (count < 3u) return;
        for (uint32_t i = 0; i < count - 2u; i++) {
            dst[3 * i] = 0;
            dst[3 * i + 1] = i + 1u;
            dst[3 * i + 2] = i + 2u;
        }
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
