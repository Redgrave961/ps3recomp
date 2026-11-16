#include "vk_renderer.h"
#include "vk_geometry.h"
#include "vk_state.h"

#include "../rsx_core.h"

#include <algorithm>
#include <cstring>

namespace vk {

namespace {

constexpr u32 k_index_type_u32 = 0;

u32 host_size(u32 type, u32 size)
{
    switch (type) {
    case rsx::VT_F: return size * 4u;
    case rsx::VT_UB: return size == 3u ? 4u : size;
    case rsx::VT_CMP: case rsx::VT_UB256: return 4u;
    default: return size == 3u ? 8u : size * 2u;
    }
}

struct copied {
    u32 src;
    u32 end;
    u32 dst;
};

u32 texture_dims(const rsx::texture_desc& d, bool valid)
{
    if (!valid) return RSX_SPV_TEX_2D;
    if (d.cube) return RSX_SPV_TEX_CUBE;
    if (d.dimension == 3u) return RSX_SPV_TEX_3D;
    if (d.dimension == 1u) return RSX_SPV_TEX_NONE;
    return RSX_SPV_TEX_2D;
}

float half_bits(uint16_t h)
{
    const uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    uint32_t bits = e == 0 ? s : e == 31 ? (s | 0x7F800000u | (m << 13)) : (s | ((e + 112u) << 23) | (m << 13));
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

}

bool renderer::create_dummies()
{
    for (u32 i = 0; i < 3; i++) {
        image& img = m_dummy[i];
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = i == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ii.flags = i == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = { 1, 1, 1 };
        ii.mipLevels = 1;
        ii.arrayLayers = i == 1 ? 6u : 1u;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_TRY(vmaCreateImage(m_d->vma, &ii, &ai, &img.handle, &img.mem, nullptr));
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img.handle;
        vi.viewType = i == 0 ? VK_IMAGE_VIEW_TYPE_2D : i == 1 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_3D;
        vi.format = ii.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers };
        VK_TRY(vkCreateImageView(m_d->dev, &vi, nullptr, &img.view));
        img.format = ii.format;
        img.width = img.height = 1;
        VkCommandBuffer c = cmd();
        barrier(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue white{ { 0.f, 0.f, 0.f, 1.f } };
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers };
        vkCmdClearColorImage(c, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        barrier(c, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VK_TRY(vkCreateSampler(m_d->dev, &si, nullptr, &m_dummy_sampler));
    return true;
}

bool renderer::upload_vertices(const rsx::draw_clause& clause, u32 first, u32 count, u32 vertex_base, VkDescriptorBufferInfo& data,
                               VkDescriptorBufferInfo& layout_info)
{
    rsx_spv_vertex_layout layout{};
    const u32 input_mask = r(rsx::M_VERTEX_ATTRIB_INPUT_MASK) & (m_vp ? m_vp->info.input_mask : 0xFFFFu);
    const u32 base_offset = r(rsx::M_VERTEX_BASE_OFFSET);
    const u32 modulo_mask = r(rsx::M_FREQUENCY_DIVIDER_OPERATION);
    const bool immediate_arrays = clause.immediate && clause.kind != rsx::draw_kind::indexed;

    struct source {
        const uint8_t* ptr;
        u32 bytes;
    };
    source src[rsx::ATTRIBS + 1]{};
    u32 dst_offset[rsx::ATTRIBS + 1]{};
    u32 total = 0;
    copied regions[rsx::ATTRIBS + 1];
    u32 nregions = 0;

    auto place = [&](u32 slot, const uint8_t* ptr, u32 bytes) {
        src[slot] = { ptr, bytes };
        dst_offset[slot] = total;
        total += (bytes + 15u) & ~15u;
    };

    u32 inline_stride = 0;
    u32 inline_offset[rsx::ATTRIBS]{};
    if (clause.kind == rsx::draw_kind::inlined) {
        for (u32 k = 0; k < rsx::ATTRIBS; k++) {
            const u32 fmt = r(rsx::M_VERTEX_ARRAY_FORMAT + k * 4u);
            const u32 size = (fmt >> 4) & 0xFu;
            if (!size) continue;
            inline_offset[k] = inline_stride;
            inline_stride += host_size(fmt & 0xFu, size);
        }
        place(rsx::ATTRIBS, reinterpret_cast<const uint8_t*>(clause.inline_words.data()), (u32)clause.inline_words.size() * 4u);
    }

    u32 slot_of[rsx::ATTRIBS];
    struct pending_attr {
        u32 k;
        u32 type;
        u32 size;
        u32 stride;
        u32 format_bits;
        u32 freq;
        int region;
        u32 inner;
    };
    pending_attr pend[rsx::ATTRIBS];
    u32 npend = 0;
    std::vector<uint8_t> regbytes;

    for (u32 k = 0; k < rsx::ATTRIBS; k++) {
        layout.value[k][3] = 1.0f;
        slot_of[k] = ~0u;
        if (!(input_mask & (1u << k))) continue;
        const u32 fmt = r(rsx::M_VERTEX_ARRAY_FORMAT + k * 4u);
        u32 type = fmt & 0xFu, size = (fmt >> 4) & 0xFu, stride = (fmt >> 8) & 0xFFu, freq = fmt >> 16;
        rsx_spv_vertex_attrib& a = layout.attrib[k];

        if (immediate_arrays) {
            const rsx::push_attribute& p = push[k];
            if (p.vertices > 1 && p.size) {
                const u32 words = p.type == rsx::VT_F ? p.size : (p.type == rsx::VT_UB || p.type == rsx::VT_UB256) ? 1u : p.size / 2u;
                std::vector<u32> padded = p.data;
                const u32 need = push[0].vertices;
                for (u32 v = p.vertices; v < need; v++)
                    for (u32 w = 0; w < words; w++) padded.push_back(attribute[k].data[w]);
                const size_t at = regbytes.size();
                regbytes.resize(at + padded.size() * 4u);
                std::memcpy(regbytes.data() + at, padded.data(), padded.size() * 4u);
                pend[npend++] = { k, p.type, p.size, words * 4u, 0u, 0u, -2, (u32)at };
                continue;
            }
            size = 0;
        }

        if (clause.kind == rsx::draw_kind::inlined && size) {
            a.offset = inline_offset[k];
            a.stride = inline_stride;
            a.format = type | (size << RSX_SPV_VFMT_SIZE_SHIFT);
            a.frequency = 0;
            slot_of[k] = rsx::ATTRIBS;
            continue;
        }

        if (!size) {
            const rsx::register_attribute& reg_attr = attribute[k];
            if (!reg_attr.size) continue;
            const size_t at = regbytes.size();
            regbytes.resize(at + 16u);
            std::memcpy(regbytes.data() + at, reg_attr.data, 16u);
            pend[npend++] = { k, reg_attr.type, reg_attr.size, 0u, 0u, 0u, -2, (u32)at };
            continue;
        }

        const u32 off = r(rsx::M_VERTEX_ARRAY_OFFSET + k * 4u);
        const u32 location = off >> 31;
        const u32 ea = rsx_address(location ? RSX_DMA_MAIN : RSX_DMA_LOCAL, (base_offset + (off & 0x7FFFFFFFu)) & 0x7FFFFFFFu, 0);
        if (!ea) continue;
        const u32 elem = host_size(type, size);
        u32 e0 = first, n = count, format_bits = 0, f = 0;
        if (!stride) {
            e0 = 0;
            n = 1;
        } else if (freq > 1u && (modulo_mask & (1u << k))) {
            e0 = 0;
            n = freq;
            format_bits = RSX_SPV_VFMT_MODULO;
            f = freq;
        } else if (freq > 1u) {
            e0 = first / freq;
            n = (first + count - 1u) / freq - e0 + 1u;
            f = freq;
        }
        const u32 start = ea + e0 * stride;
        const u32 need = stride ? (n - 1u) * stride + elem : elem;
        int region = -1;
        for (u32 i = 0; i < nregions; i++)
            if (start >= regions[i].src && start + need <= regions[i].end) {
                region = (int)i;
                break;
            }
        if (region < 0) {
            regions[nregions] = { start, start + need + 256u, 0 };
            region = (int)nregions++;
        }
        pend[npend++] = { k, type, size, stride, format_bits, f, region, start - regions[region].src };
    }

    for (u32 i = 0; i < nregions; i++) {
        regions[i].dst = total;
        total += ((regions[i].end - regions[i].src) + 15u) & ~15u;
    }
    const u32 reg_base = total;
    total += (u32)regbytes.size();
    total = std::max(total, 16u);

    vk_slice slice = m_ring.alloc(total, m_ssbo_align);
    if (!slice.ptr) return false;
    if (src[rsx::ATTRIBS].ptr) std::memcpy(slice.ptr + dst_offset[rsx::ATTRIBS], src[rsx::ATTRIBS].ptr, src[rsx::ATTRIBS].bytes);
    for (u32 i = 0; i < nregions; i++) std::memcpy(slice.ptr + regions[i].dst, vm_base + regions[i].src, regions[i].end - regions[i].src);
    if (!regbytes.empty()) std::memcpy(slice.ptr + reg_base, regbytes.data(), regbytes.size());

    for (u32 k = 0; k < rsx::ATTRIBS; k++)
        if (slot_of[k] == rsx::ATTRIBS) layout.attrib[k].offset += dst_offset[rsx::ATTRIBS];
    for (u32 i = 0; i < npend; i++) {
        const pending_attr& p = pend[i];
        rsx_spv_vertex_attrib& a = layout.attrib[p.k];
        a.offset = p.region == -2 ? reg_base + p.inner : regions[p.region].dst + p.inner;
        a.stride = p.stride;
        a.format = p.type | (p.size << RSX_SPV_VFMT_SIZE_SHIFT) | p.format_bits;
        a.frequency = p.freq;
    }

    const float clip_w = (float)(r(rsx::M_SURFACE_CLIP_H) >> 16), clip_h = (float)(r(rsx::M_SURFACE_CLIP_V) >> 16);
    const float sx = f(rsx::M_VIEWPORT_SCALE) / (clip_w / 2.f);
    const float ox = (f(rsx::M_VIEWPORT_OFFSET) - clip_w / 2.f) / (clip_w / 2.f);
    const float sy = f(rsx::M_VIEWPORT_SCALE + 4u) / (clip_h / 2.f);
    const float oy = (f(rsx::M_VIEWPORT_OFFSET + 4u) - clip_h / 2.f) / (clip_h / 2.f);
    const float m[4][4] = {
        { sx, 0.f, 0.f, ox },
        { 0.f, sy, 0.f, oy },
        { 0.f, 0.f, f(rsx::M_VIEWPORT_SCALE + 8u), f(rsx::M_VIEWPORT_OFFSET + 8u) },
        { 0.f, 0.f, 0.f, 1.f },
    };
    std::memcpy(layout.scale_offset, m, sizeof m);
    layout.vertex_base = vertex_base;
    layout.instance_base = 0;
    const u32 ucp = r(rsx::M_USER_CLIP_PLANE_CONTROL);
    u32 planes = 0;
    for (u32 i = 0; i < 6; i++) {
        const u32 op = (ucp >> (i * 4u)) & 0xFu;
        const u32 enc = op == 1u ? (u32)RSX_SPV_CLIP_LT : op == 2u ? (u32)RSX_SPV_CLIP_GE : (u32)RSX_SPV_CLIP_DISABLED;
        planes |= enc << (i * 2u);
    }
    layout.clip_planes = planes;
    layout.transform_branch_bits = r(rsx::M_TRANSFORM_BRANCH_BITS);

    data = { slice.buffer, slice.offset, total };
    vk_slice ls = m_ring.alloc(sizeof layout, m_ubo_align);
    if (!ls.ptr) return false;
    std::memcpy(ls.ptr, &layout, sizeof layout);
    layout_info = { ls.buffer, ls.offset, sizeof layout };
    return true;
}

bool renderer::draw_setup()
{
    describe_units();
    u32 vdims = 0;
    for (u32 u = 0; u < rsx::VERTEX_TEXTURES; u++)
        vdims |= texture_dims(m_units[rsx::TEXTURES + u].desc, m_units[rsx::TEXTURES + u].valid) << (u * 2u);
    if (vdims != m_vertex_dims) {
        m_vertex_dims = vdims;
        dirty |= rsx::D_VERTEX_PROGRAM;
    }
    if ((dirty & rsx::D_VERTEX_PROGRAM) || !m_vp) {
        m_program_hash = shader_cache::hash(program, sizeof program, 0x5650u);
        rsx_spv_vp_key key{};
        key.start = r(rsx::M_TRANSFORM_PROGRAM_START);
        key.output_mask = r(rsx::M_VERTEX_ATTRIB_OUTPUT_MASK);
        key.texture_dims = vdims;

        m_vp = m_shaders.vertex(program, key, m_program_hash, true);
        dirty &= ~rsx::D_VERTEX_PROGRAM;
    }
    if (!m_vp) return false;

    const u32 sp = r(rsx::M_SHADER_PROGRAM);
    const u32 location = (sp & 3u) - 1u;
    m_fp_ea = rsx_address(location ? RSX_DMA_MAIN : RSX_DMA_LOCAL, sp & ~3u, 16);
    if (!m_fp_ea) {
        static int n = 0;
        if (n++ < 5) std::fprintf(stderr, "[vk] fragment program address unresolved: 0x%08X\n", sp);
        return false;
    }
    const u32* ucode = reinterpret_cast<const u32*>(vm_base + m_fp_ea);
    rsx_spv_fp_key fk{};
    fk.control = r(rsx::M_SHADER_CONTROL);
    for (u32 u = 0; u < rsx::TEXTURES; u++) {
        fk.texture_dims |= texture_dims(m_units[u].desc, m_units[u].valid) << (u * 2u);
        if (m_units[u].valid && m_units[u].desc.ops.depth_compare) fk.texture_shadow |= 1u << u;
    }
    for (u32 k = 0; k < 4; k++)
        if (m_fb.color_format[k] != VK_FORMAT_UNDEFINED) fk.output_mask |= 1u << k;
    fp_lookup& seen = m_fp_seen[m_fp_ea];
    if (seen.fp && !std::memcmp(&seen.key, &fk, sizeof fk)) {
        bool same = true;
        for (size_t i = 0; i < seen.words.size() && same; i++) same = seen.constant[i] || seen.words[i] == ucode[i];
        if (same) {
            m_fp = seen.fp;
            return true;
        }
    }
    m_fp = m_shaders.fragment(ucode, RSX_SPV_FP_MAX_INSTRUCTIONS * 4u, fk, true);
    if (!m_fp) return false;
    seen.fp = m_fp;
    seen.key = fk;
    seen.words.assign(ucode, ucode + m_fp->info.ucode_words);
    seen.constant.assign(seen.words.size(), 0);
    for (u32 i = 0; i < m_fp->info.constant_count; i++)
        for (u32 w = 0; w < 4u && m_fp->info.constant_word[i] + w < seen.constant.size(); w++) seen.constant[m_fp->info.constant_word[i] + w] = 1;
    return true;
}

void renderer::draw_execute(const rsx::draw_clause& clause, u32 id)
{
    const u32 prim = clause.primitive;
    bool emulated = false;
    primitive_topology(prim, emulated);
    const bool restart = r(rsx::M_RESTART_INDEX_ENABLE) != 0;
    const u32 restart_index = r(rsx::M_RESTART_INDEX);

    struct sub {
        u32 first;
        u32 count;
        int32_t vertex_offset;
        bool indexed;
    };
    std::vector<sub> subs;
    vk_slice index_slice{};
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    u32 e0 = 0, ecount = 0, vb = 0;

    if (clause.kind == rsx::draw_kind::indexed) {
        const u32 dma = r(rsx::M_INDEX_ARRAY_DMA);
        const bool wide = clause.immediate || ((dma >> 4) & 0xFu) == k_index_type_u32;
        const u32 isize = wide ? 4u : 2u;
        const uint8_t* base = nullptr;
        if (clause.immediate) {
            m_index_scratch.resize(clause.elements.size() * 4u);
            for (size_t i = 0; i < clause.elements.size(); i++) {
                const u32 v = __builtin_bswap32(clause.elements[i]);
                std::memcpy(m_index_scratch.data() + i * 4u, &v, 4u);
            }
            base = m_index_scratch.data();
        } else {
            const u32 ea = rsx_address((dma & 0xFu) ? RSX_DMA_MAIN : RSX_DMA_LOCAL, r(rsx::M_INDEX_ARRAY_ADDRESS), 0);
            if (!ea) return;
            base = vm_base + (ea & ~(isize - 1u));
        }
        u32 total = 0;
        for (const rsx::draw_range& rg : clause.ranges) total += expanded_index_count(prim, rg.count) + 1u;
        index_slice = m_ring.alloc((VkDeviceSize)total * isize, 16);
        if (!index_slice.ptr) return;
        index_type = wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        u32 lo = 0xFFFFFFFFu, hi = 0, at = 0;
        for (const rsx::draw_range& rg : clause.ranges) {
            const index_result res = convert_indices(prim, wide, base + (size_t)rg.first * isize, rg.count, restart, restart_index,
                                                     index_slice.ptr + (size_t)at * isize);
            if (!res.count || res.min > res.max) continue;
            lo = std::min(lo, res.min);
            hi = std::max(hi, res.max);
            subs.push_back({ at, res.count, 0, true });
            at += res.count;
        }
        if (subs.empty() || lo > hi) return;
        const u32 base_index = r(rsx::M_VERTEX_BASE_INDEX);
        e0 = lo + base_index;
        ecount = hi - lo + 1u;
        vb = e0;
        for (sub& s : subs) s.vertex_offset = (int32_t)base_index;
    } else {
        u32 first = 0, last = 0;
        if (clause.kind == rsx::draw_kind::inlined) {
            u32 stride = 0;
            for (u32 k = 0; k < rsx::ATTRIBS; k++) {
                const u32 fmt = r(rsx::M_VERTEX_ARRAY_FORMAT + k * 4u);
                if ((fmt >> 4) & 0xFu) stride += host_size(fmt & 0xFu, (fmt >> 4) & 0xFu);
            }
            if (!stride) return;
            last = (u32)clause.inline_words.size() * 4u / stride;
            subs.push_back({ 0u, last, 0, false });
        } else {
            first = 0xFFFFFFFFu;
            for (const rsx::draw_range& rg : clause.ranges) {
                first = std::min(first, rg.first);
                last = std::max(last, rg.first + rg.count);
                subs.push_back({ rg.first, rg.count, 0, false });
            }
        }
        if (subs.empty() || last <= first) return;
        e0 = first;
        ecount = last - first;
        vb = first;
        if (emulated) {
            u32 total = 0;
            for (const sub& s : subs) total += expanded_index_count(prim, s.count) + 1u;
            index_slice = m_ring.alloc((VkDeviceSize)total * 4u, 16);
            if (!index_slice.ptr) return;
            u32 at = 0;
            for (sub& s : subs) {
                const u32 n = expanded_index_count(prim, s.count);
                generate_indices(prim, s.count, reinterpret_cast<u32*>(index_slice.ptr) + at);
                s.vertex_offset = (int32_t)s.first;
                s.first = at;
                s.count = n;
                s.indexed = true;
                at += n;
            }
        }
    }

    VkDescriptorBufferInfo vdata{}, vlayout{};
    if (!upload_vertices(clause, e0, ecount, vb, vdata, vlayout)) return;

    if ((dirty & rsx::D_VERTEX_CONSTANTS) || m_constants_vp != m_vp) {
        vk_slice cs = m_ring.alloc(sizeof constants, m_ubo_align);
        if (!cs.ptr) return;
        const rsx_spv_vp_info& vi = m_vp->info;
        if (vi.constants_relative) {
            std::memcpy(cs.ptr, constants, sizeof constants);
        } else {
            for (u32 w = 0; w < std::size(vi.constants_used); w++)
                for (u32 bits = vi.constants_used[w]; bits;) {
                    const u32 first = (u32)__builtin_ctz(bits);
                    const u32 ones = ~(bits >> first);
                    const u32 n = ones ? (u32)__builtin_ctz(ones) : 32u;
                    std::memcpy(cs.ptr + (size_t)(w * 32u + first) * 16u, constants + (size_t)(w * 32u + first) * 4u, (size_t)n * 16u);
                    bits &= n == 32u ? 0u : ~(((1u << n) - 1u) << first);
                }
        }
        m_constants = { cs.buffer, cs.offset, sizeof constants };
        m_constants_vp = m_vp;
        dirty &= ~rsx::D_VERTEX_CONSTANTS;
    }

    const rsx_spv_fp_info& fi = m_fp->info;
    const u32 nconst = std::max(fi.constant_count, 1u);
    vk_slice fc = m_ring.alloc((VkDeviceSize)nconst * 16u, m_ubo_align);
    if (!fc.ptr) return;
    std::memset(fc.ptr, 0, (size_t)nconst * 16u);
    const u32* ucode = reinterpret_cast<const u32*>(vm_base + m_fp_ea);
    for (u32 i = 0; i < fi.constant_count; i++)
        for (u32 w = 0; w < 4; w++) {
            u32 v = __builtin_bswap32(ucode[fi.constant_word[i] + w]);
            v = (v << 16) | (v >> 16);
            std::memcpy(fc.ptr + i * 16u + w * 4u, &v, 4);
        }

    rsx_spv_fragment_state fs{};
    if (r(rsx::M_ALPHA_TEST_ENABLE)) fs.flags |= RSX_SPV_FPF_ALPHA_TEST;
    if (r(rsx::M_TWO_SIDE_LIGHT)) fs.flags |= RSX_SPV_FPF_TWO_SIDED;
    const u32 window = r(rsx::M_SHADER_WINDOW);
    if (((window >> 16) & 0xFu) == 1u) fs.flags |= RSX_SPV_FPF_PIXEL_CENTER_INTEGER;
    fs.alpha_func = r(rsx::M_ALPHA_FUNC);
    const u32 aref = r(rsx::M_ALPHA_REF);
    const VkFormat target = m_fb.color_format[0];
    if (target == VK_FORMAT_R16G16B16A16_SFLOAT) fs.alpha_ref = half_bits((uint16_t)(aref & 0xFFFFu));
    else if (target == VK_FORMAT_R32G32B32A32_SFLOAT || target == VK_FORMAT_R32_SFLOAT) std::memcpy(&fs.alpha_ref, &aref, 4);
    else fs.alpha_ref = (float)(aref & 0xFFu) / 255.f;
    fs.fog_mode = r(rsx::M_FOG_MODE);
    fs.fog_param[0] = f(rsx::M_FOG_PARAMS);
    fs.fog_param[1] = f(rsx::M_FOG_PARAMS + 4u);
    const bool bottom = ((window >> 12) & 0xFu) != 0;
    fs.wpos_scale = bottom ? -1.f : 1.f;
    fs.wpos_bias = bottom ? (float)(window & 0xFFFu) : 0.f;
    VkDescriptorImageInfo images[rsx::TEXTURES + rsx::VERTEX_TEXTURES];
    for (u32 u = 0; u < rsx::TEXTURES + rsx::VERTEX_TEXTURES; u++) {
        const bool vertex_unit = u >= rsx::TEXTURES;
        const u32 unit = vertex_unit ? u - rsx::TEXTURES : u;
        const u32 used = vertex_unit ? m_vp->info.texture_mask : m_fp->info.texture_mask;
        rsx_spv_texture_state scratch{};
        rsx_spv_texture_state& st = vertex_unit ? scratch : fs.texture[u];
        st.scale[0] = st.scale[1] = 1.f;
        if ((used >> unit) & 1u && resolve_texture(unit, vertex_unit, images[u], st)) continue;
        const u32 dims = texture_dims(m_units[u].desc, m_units[u].valid);
        const image& dummy = m_dummy[dims == RSX_SPV_TEX_CUBE ? 1 : dims == RSX_SPV_TEX_3D ? 2 : 0];
        images[u] = { m_dummy_sampler, dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    u32 tcc = 0;
    for (u32 i = 0; i < 10; i++)
        if (r(rsx::M_TEX_COORD_CONTROL + i * 4u) & 1u) tcc |= 1u << i;
    const u32 sprite = r(rsx::M_POINT_SPRITE_CONTROL);
    if (sprite & 1u) tcc |= ((sprite >> 8) & 0x3FFu) << 16;
    fs.texcoord_control = tcc;
    vk_slice fss = m_ring.alloc(sizeof fs, m_ubo_align);
    if (!fss.ptr) return;
    std::memcpy(fss.ptr, &fs, sizeof fs);

    draw_target t{};
    t.width = m_fb.width;
    t.height = m_fb.height;
    t.aa_u = m_fb.aa_u;
    t.aa_v = m_fb.aa_v;
    t.depth = m_fb.depth != nullptr;
    t.stencil = m_fb.depth && (m_fb.depth->img.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    for (u32 k = 0; k < 4; k++) t.color_format[k] = m_fb.color_format[k];

    pipeline_key key{};
    key.vs = m_vp->hash;
    key.fs = m_fp->hash;
    key.depth_format = m_fb.depth_format;
    fill_pipeline_state(*this, *m_d, t, prim, key);
    const VkPipeline pipe = m_pipelines.get(key, m_vp->code, m_fp->code);
    if (!pipe) {
        stats.skipped_pipeline++;
        if (m_trace) std::fprintf(stderr, "[trace] %u skipped: pipeline compiling or failed\n", id);
        return;
    }

    begin_rendering();
    VkCommandBuffer c = cmd();
    if (m_occlusion.enabled() && !m_occlusion.active()) m_occlusion.begin(c);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    set_dynamic_state(c, *this, *m_d, m_pipelines, t, prim, restart && clause.kind == rsx::draw_kind::indexed, key);

    VkDescriptorBufferInfo fp_consts{ fc.buffer, fc.offset, (VkDeviceSize)nconst * 16u };
    VkDescriptorBufferInfo fp_state{ fss.buffer, fss.offset, sizeof fs };
    VkWriteDescriptorSet w[5 + rsx::TEXTURES + rsx::VERTEX_TEXTURES]{};
    u32 nw = 0;
    auto buffer_write = [&](u32 binding, VkDescriptorType type, const VkDescriptorBufferInfo* info) {
        VkWriteDescriptorSet& x = w[nw++];
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstBinding = binding;
        x.descriptorCount = 1;
        x.descriptorType = type;
        x.pBufferInfo = info;
    };
    buffer_write(RSX_SPV_BIND_VP_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &m_constants);
    buffer_write(RSX_SPV_BIND_VERTEX_LAYOUT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &vlayout);
    buffer_write(RSX_SPV_BIND_VERTEX_DATA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vdata);
    buffer_write(RSX_SPV_BIND_FP_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &fp_consts);
    buffer_write(RSX_SPV_BIND_FP_STATE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &fp_state);
    for (u32 u = 0; u < rsx::TEXTURES + rsx::VERTEX_TEXTURES; u++) {
        const bool vertex_unit = u >= rsx::TEXTURES;
        VkWriteDescriptorSet& x = w[nw++];
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstBinding = vertex_unit ? RSX_SPV_BIND_VERTEX_TEXTURE0 + (u - rsx::TEXTURES) : RSX_SPV_BIND_TEXTURE0 + u;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        x.pImageInfo = &images[u];
    }
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines.layout(), 0, nw, w);

    if (index_slice.ptr) vkCmdBindIndexBuffer(c, index_slice.buffer, index_slice.offset, index_type);
    for (const sub& s : subs) {
        if (s.indexed) vkCmdDrawIndexed(c, s.count, 1, s.first, s.vertex_offset, 0);
        else vkCmdDraw(c, s.count, 1, s.first, 0);
    }

    for (u32 k = 0; k < m_fb.count; k++)
        if (m_fb.color[k] && m_fb.color_write[k]) m_fb.color[k]->gpu_dirty = true;
    if (m_fb.depth && m_fb.depth_write) m_fb.depth->gpu_dirty = true;
    m_draw_counter++;
    stats.draws++;
    if (m_trace) {
        u32 n = 0;
        for (const sub& s : subs) n += s.count;
        trace_draw(id, prim, n, index_slice.ptr != nullptr);
    }
}

}
