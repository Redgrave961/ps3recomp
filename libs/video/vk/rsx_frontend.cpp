#include "rsx_frontend.h"
#include "rsx_defaults.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <iterator>

namespace rsx {

namespace {

struct vertex_method {
    u32 base;
    u32 regs_per_attribute;
    u32 size;
    u32 type;
    u32 bytes;
};

const vertex_method k_vertex_methods[] = {
    { M_VERTEX_DATA_SCALED4S, 2u, 4u, VT_S1, 2u },
    { M_VERTEX_DATA3F, 4u, 3u, VT_F, 4u },
    { M_VERTEX_DATA2F, 2u, 2u, VT_F, 4u },
    { M_VERTEX_DATA2S, 1u, 2u, VT_S32K, 2u },
    { M_VERTEX_DATA4UB, 1u, 4u, VT_UB, 1u },
    { M_VERTEX_DATA4S, 2u, 4u, VT_S32K, 2u },
    { M_VERTEX_DATA4F, 4u, 4u, VT_F, 4u },
    { M_VERTEX_DATA1F, 1u, 1u, VT_F, 4u },
};

enum method_kind : u8 {
    K_STORE,
    K_PROGRAM,
    K_CONSTANT,
    K_BEGIN_END,
    K_DRAW_ARRAYS,
    K_DRAW_INDEX,
    K_INLINE,
    K_ELEMENT16,
    K_ELEMENT32,
    K_CLEAR,
    K_REPORT,
    K_VERTEX,
};

struct reg_meta {
    u16 bits;
    u8 kind;
    u8 tex_unit;
};

struct groups {
    reg_meta meta[REG_COUNT] = {};

    void mark(u32 first, u32 last, u32 bit)
    {
        for (u32 m = first; m <= last; m += 4u) meta[m >> 2].bits |= (u16)bit;
    }

    groups()
    {
        for (u32 i = 0; i < REG_COUNT; i++) meta[i].tex_unit = 0xFFu;
        mark(M_DMA_COLOR_B, M_DMA_ZETA, D_SURFACE);
        mark(M_DMA_COLOR_C, M_DMA_COLOR_D, D_SURFACE);
        mark(M_SURFACE_CLIP_H, M_SURFACE_PITCH_Z, D_SURFACE);
        mark(M_SURFACE_PITCH_C, M_SURFACE_COLOR_D, D_SURFACE);
        mark(M_WINDOW_OFFSET, M_WINDOW_OFFSET, D_SURFACE | D_VIEWPORT);
        mark(M_ANTI_ALIASING_CONTROL, M_ANTI_ALIASING_CONTROL, D_SURFACE | D_BLEND | D_FRAGMENT_STATE);
        mark(M_SURFACE_COMPRESSION, M_SURFACE_COMPRESSION, D_SURFACE);
        mark(M_CONTROL0, M_CONTROL0, D_SURFACE);

        mark(M_SCISSOR_H, M_SCISSOR_V, D_VIEWPORT);
        mark(M_VIEWPORT_H, M_VIEWPORT_V, D_VIEWPORT);
        mark(M_VIEWPORT_OFFSET, M_VIEWPORT_SCALE + 12u, D_VIEWPORT | D_VERTEX_STATE);
        mark(M_CLIP_MIN, M_CLIP_MAX, D_VIEWPORT | D_VERTEX_STATE);
        mark(M_SURFACE_CLIP_H, M_SURFACE_CLIP_V, D_VIEWPORT | D_VERTEX_STATE);

        mark(M_SHADE_MODE, M_SHADE_MODE, D_RASTER | D_FRAGMENT_PROGRAM | D_VERTEX_PROGRAM);
        mark(M_LINE_WIDTH, M_LINE_SMOOTH, D_RASTER);
        mark(M_POLY_OFFSET_POINT, M_POLY_OFFSET_FILL, D_RASTER);
        mark(M_POLY_OFFSET_SCALE, M_POLY_OFFSET_BIAS, D_RASTER);
        mark(M_FRONT_POLYGON_MODE, M_CULL_FACE_ENABLE, D_RASTER);
        mark(M_ZMIN_MAX_CONTROL, M_ZMIN_MAX_CONTROL, D_RASTER | D_FRAGMENT_STATE);
        mark(M_RESTART_INDEX_ENABLE, M_LINE_STIPPLE_PATTERN, D_RASTER);
        mark(M_POLYGON_STIPPLE, M_POLYGON_STIPPLE_PATTERN + 0x7Cu, D_RASTER);
        mark(M_POINT_SIZE, M_POINT_SPRITE_CONTROL, D_RASTER | D_VERTEX_STATE | D_FRAGMENT_PROGRAM);
        mark(M_FLAT_SHADE_OP, M_FLAT_SHADE_OP, D_RASTER);

        mark(M_STENCIL_TEST_ENABLE, M_BACK_STENCIL_ZPASS, D_DEPTH_STENCIL);
        mark(M_DEPTH_BOUNDS_ENABLE, M_DEPTH_BOUNDS_MAX, D_DEPTH_STENCIL);
        mark(M_DEPTH_FUNC, M_DEPTH_TEST_ENABLE, D_DEPTH_STENCIL);

        mark(M_DITHER_ENABLE, M_DITHER_ENABLE, D_BLEND);
        mark(M_BLEND_ENABLE, M_COLOR_MASK, D_BLEND);
        mark(M_BLEND_ENABLE_MRT, M_BLEND_COLOR2, D_BLEND);

        mark(M_SHADER_PROGRAM, M_SHADER_PROGRAM, D_FRAGMENT_PROGRAM);
        mark(M_SHADER_CONTROL, M_SHADER_CONTROL, D_FRAGMENT_PROGRAM | D_FRAGMENT_STATE);
        mark(M_SHADER_PACKER, M_SHADER_PACKER, D_FRAGMENT_PROGRAM | D_FRAGMENT_STATE);
        mark(M_TEX_COORD_CONTROL, M_TEX_COORD_CONTROL + 0x24u, D_FRAGMENT_PROGRAM);
        mark(M_SPECULAR_ENABLE, M_TWO_SIDE_LIGHT, D_FRAGMENT_PROGRAM | D_VERTEX_STATE);

        mark(M_ALPHA_TEST_ENABLE, M_ALPHA_REF, D_FRAGMENT_STATE);
        mark(M_FOG_MODE, M_FOG_PARAMS + 8u, D_FRAGMENT_STATE);
        mark(M_SHADER_WINDOW, M_SHADER_WINDOW, D_FRAGMENT_STATE);
        mark(M_POINT_CENTER_MODE, M_POINT_CENTER_MODE, D_FRAGMENT_STATE);

        mark(M_DMA_VERTEX_A, M_DMA_VERTEX_B, D_VERTEX_LAYOUT);
        mark(M_VERTEX_ARRAY_OFFSET, M_VERTEX_ARRAY_OFFSET + 0x3Cu, D_VERTEX_LAYOUT);
        mark(M_VERTEX_ARRAY_FORMAT, M_VERTEX_ARRAY_FORMAT + 0x3Cu, D_VERTEX_LAYOUT);
        mark(M_VERTEX_BASE_OFFSET, M_VERTEX_BASE_INDEX, D_VERTEX_LAYOUT);
        mark(M_FREQUENCY_DIVIDER_OPERATION, M_FREQUENCY_DIVIDER_OPERATION, D_VERTEX_LAYOUT);
        mark(M_INDEX_ARRAY_ADDRESS, M_INDEX_ARRAY_DMA, D_VERTEX_LAYOUT);
        mark(M_VERTEX_ATTRIB_INPUT_MASK, M_VERTEX_ATTRIB_INPUT_MASK, D_VERTEX_LAYOUT | D_VERTEX_PROGRAM);

        mark(M_TRANSFORM_PROGRAM_START, M_TRANSFORM_PROGRAM_START, D_VERTEX_PROGRAM);
        mark(M_TRANSFORM_EXECUTION_MODE, M_TRANSFORM_EXECUTION_MODE, D_VERTEX_PROGRAM);
        mark(M_VERTEX_ATTRIB_OUTPUT_MASK, M_VERTEX_ATTRIB_OUTPUT_MASK, D_VERTEX_PROGRAM | D_FRAGMENT_PROGRAM);
        mark(M_TRANSFORM_BRANCH_BITS, M_TRANSFORM_BRANCH_BITS, D_VERTEX_STATE);
        mark(M_INDEXED_CONSTANT_READ_LIMITS, M_INDEXED_CONSTANT_READ_LIMITS, D_VERTEX_STATE);
        mark(M_USER_CLIP_PLANE_CONTROL, M_USER_CLIP_PLANE_CONTROL, D_VERTEX_STATE | D_VERTEX_PROGRAM);

        mark(M_TEXTURE_OFFSET, M_TEXTURE_OFFSET + 0x1FCu, D_TEXTURES);
        mark(M_TEXTURE_CONTROL3, M_TEXTURE_CONTROL3 + 0x3Cu, D_TEXTURES);
        mark(M_TEXTURE_CONTROL2, M_TEXTURE_CONTROL2 + 0x3Cu, D_TEXTURES);
        mark(M_VTEX_OFFSET, M_VTEX_OFFSET + 0x7Cu, D_VERTEX_TEXTURES);
        for (u32 u = 0; u < TEXTURES; u++) {
            for (u32 o = 0; o < 0x20u; o += 4u) meta[(M_TEXTURE_OFFSET + u * 0x20u + o) >> 2].tex_unit = (u8)u;
            meta[(M_TEXTURE_CONTROL2 + u * 4u) >> 2].tex_unit = (u8)u;
            meta[(M_TEXTURE_CONTROL3 + u * 4u) >> 2].tex_unit = (u8)u;
        }
        for (u32 u = 0; u < VERTEX_TEXTURES; u++)
            for (u32 o = 0; o < 0x20u; o += 4u) meta[(M_VTEX_OFFSET + u * 0x20u + o) >> 2].tex_unit = (u8)(TEXTURES + u);
        for (u32 v = 0; v < std::size(k_vertex_methods); v++)
            for (u32 m = k_vertex_methods[v].base; m < k_vertex_methods[v].base + ATTRIBS * k_vertex_methods[v].regs_per_attribute * 4u; m += 4u)
                meta[m >> 2].kind = (u8)(K_VERTEX + v);
        for (u32 m = 0; m < 0x80u; m += 4u) {
            meta[(M_TRANSFORM_PROGRAM + m) >> 2].kind = K_PROGRAM;
            meta[(M_TRANSFORM_CONSTANT + m) >> 2].kind = K_CONSTANT;
        }
        meta[M_BEGIN_END >> 2].kind = K_BEGIN_END;
        meta[M_DRAW_ARRAYS >> 2].kind = K_DRAW_ARRAYS;
        meta[M_DRAW_INDEX_ARRAY >> 2].kind = K_DRAW_INDEX;
        meta[M_INLINE_ARRAY >> 2].kind = K_INLINE;
        meta[M_ARRAY_ELEMENT16 >> 2].kind = K_ELEMENT16;
        meta[M_ARRAY_ELEMENT32 >> 2].kind = K_ELEMENT32;
        meta[M_CLEAR_SURFACE >> 2].kind = K_CLEAR;
        meta[M_CLEAR_REPORT_VALUE >> 2].kind = K_REPORT;
        meta[M_ZPASS_PIXEL_COUNT_ENABLE >> 2].kind = K_REPORT;
        meta[M_ZCULL_STATS_ENABLE >> 2].kind = K_REPORT;
        meta[M_RENDER_ENABLE >> 2].kind = K_REPORT;
    }
};

const groups k_groups;

u32 swap(u32 v) { return __builtin_bswap32(v); }

u32 words_per_vertex(u32 type, u32 size)
{
    switch (type) {
    case VT_F: return size;
    case VT_UB: case VT_UB256: return 1u;
    default: return size / 2u;
    }
}


}

frontend::frontend()
{
    for (const reg_default& d : k_init_defaults) reg[d.method >> 2] = d.value;
}

float frontend::f(u32 method) const
{
    float v;
    std::memcpy(&v, &reg[method >> 2], sizeof v);
    return v;
}

void frontend::reset()
{
    flush_ranges();
    for (const reg_default& d : k_flip_defaults) reg[d.method >> 2] = d.value;
    dirty = D_ALL;
    dirty_tex = 0xFFFFFu;
    vp_code_dirty = true;
}

void frontend::flush_ranges()
{
    if (draw.ranges.empty()) return;
    on_draw(draw);
    draw.ranges.clear();
}

void frontend::store(u32 method, u32 value)
{
    const u32 i = method >> 2;
    if (reg[i] == value) return;
    const reg_meta m = k_groups.meta[i];
    if (in_begin_end && m.bits) flush_ranges();
    reg[i] = value;
    dirty |= m.bits;
    if (m.tex_unit != 0xFFu) dirty_tex |= 1u << m.tex_unit;
}

void frontend::push_vertex(u32 index, u32 sub, u32 size, u32 type, u32 value)
{
    if (!(reg[M_VERTEX_ATTRIB_INPUT_MASK >> 2] & (1u << index))) return;
    const push_attribute& head = push[0];
    const u32 vertex = head.size ? head.dwords / words_per_vertex(head.type, head.size) : 0u;
    push_attribute& p = push[index];
    pushed |= 1u << index;
    p.type = type;
    p.size = size;
    const u32 per = words_per_vertex(type, size);
    const u32 need = vertex + 1u;
    if (p.vertices < need) {
        p.data.resize((size_t)per * need);
        for (u32 v = p.vertices; v + 1u < need; v++)
            std::memcpy(&p.data[(size_t)v * per], attribute[index].data, per * sizeof(u32));
        p.vertices = need;
    }
    p.data[(size_t)(p.vertices - 1u) * per + sub] = value;
    p.dwords++;
}

void frontend::vertex_data(u32 method, u32 be)
{
    const vertex_method& v = k_vertex_methods[k_groups.meta[method >> 2].kind - K_VERTEX];
    const u32 word = (method - v.base) >> 2;
    const u32 index = word / v.regs_per_attribute;
    const u32 sub = word % v.regs_per_attribute;
    const u32 value = v.bytes == 2u ? (be << 16) | (be >> 16) : v.bytes == 1u ? swap(be) : be;
    if (in_begin_end) push_vertex(index, sub, v.size, v.type, value);
    register_attribute& a = attribute[index];
    a.type = v.type;
    a.size = v.size;
    a.data[sub] = value;
    dirty |= D_VERTEX_LAYOUT;
}

void frontend::begin_end(u32 value)
{
    if (const u32 prim = value & 0xFFu) {
        in_begin_end = true;
        if (prim > 10u) return;
        draw.primitive = prim;
        draw.kind = draw_kind::none;
        draw.immediate = false;
        draw.ranges.clear();
        draw.inline_words.clear();
        draw.elements.clear();
        return;
    }
    if (draw.kind == draw_kind::none) {
        draw.immediate = true;
        if (!draw.elements.empty()) {
            draw.kind = draw_kind::indexed;
            draw.ranges.push_back({ 0u, (u32)draw.elements.size() });
        } else if (push[0].vertices) {
            draw.kind = draw_kind::array;
            draw.ranges.push_back({ 0u, push[0].vertices });
        }
    }
    if (draw.kind == draw_kind::inlined) {
        if (!draw.inline_words.empty()) on_draw(draw);
    } else {
        flush_ranges();
    }
    in_begin_end = false;
    draw.kind = draw_kind::none;
    draw.immediate = false;
    draw.ranges.clear();
    draw.inline_words.clear();
    draw.elements.clear();
    for (; pushed; pushed &= pushed - 1u) {
        push_attribute& p = push[__builtin_ctz(pushed)];
        p.vertices = 0;
        p.dwords = 0;
        p.size = 0;
        p.data.clear();
    }
}

u32 frontend::program_methods(u32 index, const u32* be, u32 count)
{
    const u32 load = reg[M_TRANSFORM_PROGRAM_LOAD >> 2];
    const u32 limit = VP_SLOTS * 4u;
    const u32 first = load < VP_SLOTS ? load * 4u + (index & 3u) : limit;
    const u32 n = std::min(count, limit - first);
    bool changed = false;
    u32 i = 0;
#ifdef __AVX2__
    if (n >= 8u) {
        const __m256i shuf = _mm256_setr_epi8(
            3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
            3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        __m256i diff = _mm256_setzero_si256();
        for (; i + 8u <= n; i += 8u) {
            const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(be + i)), shuf);
            const __m256i cur = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(program + first + i));
            diff = _mm256_or_si256(diff, _mm256_xor_si256(v, cur));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(program + first + i), v);
        }
        changed = !_mm256_testz_si256(diff, diff);
    }
#endif
    for (; i < n; i++) {
        const u32 w = swap(be[i]);
        changed |= program[first + i] != w;
        program[first + i] = w;
    }
    if (changed) {
        dirty |= D_VERTEX_PROGRAM;
        vp_code_dirty = true;
    }
    reg[M_TRANSFORM_PROGRAM_LOAD >> 2] = load + ((n + (index & 3u)) >> 2);
    return count;
}

u32 frontend::constant_methods(u32 index, u32 inc, const u32* be, u32 count)
{
    const u32 skip = inc ? 0u : count - 1u;
    const u32 n = count - skip;
    const u32 load = reg[M_TRANSFORM_CONSTANT_LOAD >> 2];
    const u32 limit = VP_CONSTANTS * 4u;
    if (load >= VP_CONSTANTS) return count;
    const u32 first = load * 4u + index;
    if (first >= limit) return count;
    const u32 w = std::min(n, limit - first);
    const u32* src = be + skip;
    u32* dst = constants + first;
    bool changed = false;
    u32 i = 0;
#ifdef __AVX2__
    const __m256i shuf = _mm256_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (; i + 8u <= w; i += 8u) {
        const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i)), shuf);
        const __m256i d = _mm256_xor_si256(v, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i)));
        if (!_mm256_testz_si256(d, d)) { changed = true; break; }
    }
#endif
    if (!changed) {
        for (; i < w; i++) {
            if (dst[i] != swap(src[i])) { changed = true; break; }
        }
    }
    if (!changed) return count;
    if (in_begin_end) flush_ranges();
    u32 j = i;
#ifdef __AVX2__
    for (; j + 8u <= w; j += 8u) {
        const __m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + j)), shuf);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + j), v);
    }
#endif
    for (; j < w; j++) dst[j] = swap(src[j]);
    dirty |= D_VERTEX_CONSTANTS;
    return count;
}

void frontend::methods(u32 method, u32 inc, const u32* be, u32 count)
{
    while (count) {
        u32 used = 1;
        switch (k_groups.meta[method >> 2].kind) {
        case K_STORE: {
            if (inc == 4u && count > 1u) {
                u32 idx = method >> 2;
                u32 n = 0;
                if (__builtin_expect(!in_begin_end, 1)) {
                    u32 local_dirty = 0;
                    u32 local_tex = 0;
                    do {
                        const u32 val = swap(be[n]);
                        if (reg[idx] != val) {
                            const reg_meta m = k_groups.meta[idx];
                            reg[idx] = val;
                            local_dirty |= m.bits;
                            if (m.tex_unit != 0xFFu) local_tex |= 1u << m.tex_unit;
                        }
                        n++;
                        idx++;
                    } while (n < count && idx < REG_COUNT && k_groups.meta[idx].kind == K_STORE);
                    dirty |= local_dirty;
                    dirty_tex |= local_tex;
                } else {
                    do {
                        const u32 val = swap(be[n]);
                        if (reg[idx] != val) {
                            const reg_meta m = k_groups.meta[idx];
                            if (m.bits) flush_ranges();
                            reg[idx] = val;
                            dirty |= m.bits;
                            if (m.tex_unit != 0xFFu) dirty_tex |= 1u << m.tex_unit;
                        }
                        n++;
                        idx++;
                    } while (n < count && idx < REG_COUNT && k_groups.meta[idx].kind == K_STORE);
                }
                used = n;
            } else {
                store(method, swap(*be));
            }
            break;
        }
        case K_PROGRAM: {
            const u32 index = (method - M_TRANSFORM_PROGRAM) >> 2;
            used = program_methods(index, be, inc ? std::min(count, 32u - index) : count);
            break;
        }
        case K_CONSTANT: {
            const u32 index = (method - M_TRANSFORM_CONSTANT) >> 2;
            used = constant_methods(index, inc, be, inc ? std::min(count, 32u - index) : count);
            break;
        }
        case K_BEGIN_END:
            begin_end(swap(*be));
            break;
        case K_DRAW_ARRAYS:
        case K_DRAW_INDEX: {
            const u32 value = swap(*be);
            draw.kind = method == M_DRAW_ARRAYS ? draw_kind::array : draw_kind::indexed;
            const draw_range range{ value & 0xFFFFFFu, (value >> 24) + 1u };
            if (!draw.ranges.empty() && draw.ranges.back().first + draw.ranges.back().count == range.first)
                draw.ranges.back().count += range.count;
            else
                draw.ranges.push_back(range);
            break;
        }
        case K_INLINE:
            draw.kind = draw_kind::inlined;
            if (!inc) {
                draw.inline_words.insert(draw.inline_words.end(), be, be + count);
                return;
            }
            draw.inline_words.push_back(*be);
            break;
        case K_ELEMENT16:
            if (in_begin_end) {
                if (!inc) {
                    const size_t base = draw.elements.size();
                    draw.elements.resize(base + (size_t)count * 2u);
                    u32* dst = draw.elements.data() + base;
                    for (u32 i = 0; i < count; i++) {
                        const u32 value = swap(be[i]);
                        dst[i * 2u] = value & 0xFFFFu;
                        dst[i * 2u + 1u] = value >> 16;
                    }
                    return;
                }
                const u32 value = swap(*be);
                draw.elements.push_back(value & 0xFFFFu);
                draw.elements.push_back(value >> 16);
            } else if (!inc) {
                return;
            }
            break;
        case K_ELEMENT32:
            if (in_begin_end) {
                if (!inc) {
                    const size_t base = draw.elements.size();
                    draw.elements.resize(base + count);
                    u32* dst = draw.elements.data() + base;
                    for (u32 i = 0; i < count; i++) dst[i] = swap(be[i]);
                    return;
                }
                draw.elements.push_back(swap(*be));
            } else if (!inc) {
                return;
            }
            break;
        case K_CLEAR:
            reg[method >> 2] = swap(*be);
            on_clear(reg[method >> 2]);
            break;
        case K_REPORT:
            reg[method >> 2] = swap(*be);
            on_report(method, reg[method >> 2]);
            break;
        default:
            vertex_data(method, *be);
            break;
        }
        if (used >= count) return;
        count -= used;
        be += used;
        method += inc * used;
    }
}
}
