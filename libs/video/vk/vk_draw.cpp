#include "vk_renderer.h"
#include "vk_geometry.h"
#include "vk_state.h"


#include <algorithm>
#include <cstring>
#include <immintrin.h>

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

bool program_matches(const u32* ucode, const u32* code, u32 count)
{
    u32 i = 0;
    for (; i + 8u <= count; i += 8u, code += 16) {
        const __m256i u = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ucode + i));
        const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(code + 8));
        const __m256i x = _mm256_xor_si256(_mm256_and_si256(u, m), _mm256_loadu_si256(reinterpret_cast<const __m256i*>(code)));
        if (!_mm256_testz_si256(x, x)) return false;
    }
    if (i == count) return true;
    const __m256i lanes = _mm256_cmpgt_epi32(_mm256_set1_epi32((int)(count - i)), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    const __m256i u = _mm256_maskload_epi32(reinterpret_cast<const int*>(ucode + i), lanes);
    const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(code + 8));
    const __m256i x = _mm256_xor_si256(_mm256_and_si256(u, m), _mm256_loadu_si256(reinterpret_cast<const __m256i*>(code)));
    return _mm256_testz_si256(x, x);
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

bool renderer::create_buffer_set()
{
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VK_TRY(vkCreateDescriptorPool(m_d->dev, &pi, nullptr, &m_buffer_pool));
    const VkDescriptorSetLayout layout = m_pipelines.buffer_set_layout();
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_buffer_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VK_TRY(vkAllocateDescriptorSets(m_d->dev, &ai, &m_buffer_set));
    const VkBuffer ring = m_ring.buffer();
    const VkDescriptorBufferInfo infos[5] = {
        { ring, 0, sizeof constants },
        { ring, 0, sizeof(rsx_spv_vertex_layout) },
        { ring, 0, m_ring.capacity() },
        { ring, 0, RSX_SPV_FP_MAX_CONSTANTS * 16u },
        { ring, 0, sizeof(rsx_spv_fragment_state) },
    };
    const u32 bindings[5] = { RSX_SPV_BIND_VP_CONSTANTS, RSX_SPV_BIND_VERTEX_LAYOUT, RSX_SPV_BIND_VERTEX_DATA, RSX_SPV_BIND_FP_CONSTANTS,
                              RSX_SPV_BIND_FP_STATE };
    VkWriteDescriptorSet w[5]{};
    for (u32 i = 0; i < 5; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = m_buffer_set;
        w[i].dstBinding = bindings[i];
        w[i].descriptorCount = 1;
        w[i].descriptorType = bindings[i] == RSX_SPV_BIND_VERTEX_DATA ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_d->dev, 5, w, 0, nullptr);
    return true;
}

bool renderer::upload_vertices(const rsx::draw_clause& clause, u32 first, u32 count, u32 vertex_base, VkDescriptorBufferInfo& layout_info)
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
    std::vector<uint8_t>& regbytes = m_regbytes;
    regbytes.clear();

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
                const u32 need = push[0].vertices;
                const size_t at = regbytes.size();
                regbytes.resize(at + (p.data.size() + (size_t)(need > p.vertices ? need - p.vertices : 0) * words) * 4u);
                std::memcpy(regbytes.data() + at, p.data.data(), p.data.size() * 4u);
                for (u32 v = p.vertices, o = (u32)(at + p.data.size() * 4u); v < need; v++, o += words * 4u)
                    std::memcpy(regbytes.data() + o, attribute[k].data, words * 4u);
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
        const u32 bytes = regions[i].end - regions[i].src;
        cached& e = m_vertex_cache[(uint64_t)regions[i].src << 32 | bytes];
        u32 offset = 0, fresh = 0;
        uint8_t* out = nullptr;
        if (!cache_place(e, regions[i].src, bytes, bytes, offset, out)) return false;
        if (out) {
            fresh = refresh(regions[i].src, bytes, e.stamp, out);
            e.stamp = m_stamp;
        }
        stats.copy_bytes += fresh;
        stats.reuse_bytes += bytes - fresh;
        regions[i].dst = offset;
    }
    const u32 reg_base = total;
    total += (u32)regbytes.size();

    u32 transient = 0;
    if (total) {
        const vk_slice slice = m_ring.alloc(total, 16);
        if (!slice.ptr) return false;
        transient = (u32)slice.offset;
        if (src[rsx::ATTRIBS].ptr) std::memcpy(slice.ptr + dst_offset[rsx::ATTRIBS], src[rsx::ATTRIBS].ptr, src[rsx::ATTRIBS].bytes);
        if (!regbytes.empty()) std::memcpy(slice.ptr + reg_base, regbytes.data(), regbytes.size());
    }

    for (u32 k = 0; k < rsx::ATTRIBS; k++)
        if (slot_of[k] == rsx::ATTRIBS) layout.attrib[k].offset += transient + dst_offset[rsx::ATTRIBS];
    for (u32 i = 0; i < npend; i++) {
        const pending_attr& p = pend[i];
        rsx_spv_vertex_attrib& a = layout.attrib[p.k];
        a.offset = p.region == -2 ? transient + reg_base + p.inner : regions[p.region].dst + p.inner;
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
    layout.point_size = f(rsx::M_POINT_SIZE);
    layout.point_scale = m_fb.scale.value();

    return upload(m_layout_upload, &layout, sizeof layout, layout_info);
}

static inline bool bytes_equal(const uint8_t* a, const uint8_t* b, u32 n)
{
#ifdef __AVX2__
    u32 i = 0;
    for (; i + 32u <= n; i += 32u) {
        const __m256i d = _mm256_xor_si256(_mm256_loadu_si256((const __m256i*)(a + i)),
                                           _mm256_loadu_si256((const __m256i*)(b + i)));
        if (!_mm256_testz_si256(d, d)) return false;
    }
    return i == n || std::memcmp(a + i, b + i, n - i) == 0;
#else
    return std::memcmp(a, b, n) == 0;
#endif
}

bool renderer::upload(reused_upload& u, const void* data, u32 size, VkDescriptorBufferInfo& out)
{
    const uint64_t submission = *m_submitted + 1u;
    if (u.submission != submission || u.bytes.size() != size || !bytes_equal(u.bytes.data(), (const uint8_t*)data, size)) {
        const vk_slice s = m_ring.alloc(size, m_ubo_align);
        if (!s.ptr) return false;
        std::memcpy(s.ptr, data, size);
        u.bytes.resize(size);
        std::memcpy(u.bytes.data(), data, size);
        u.submission = submission;
        u.info = { s.buffer, s.offset, size };
    }
    out = u.info;
    return true;
}

bool renderer::cache_place(cached& e, u32 ea, u32 size, u32 bytes, u32& offset, uint8_t*& out)
{
    const bool changed = !e.seen || (size && memory_changed(ea, size, e.stamp));
    e.seen = true;
    e.used = m_flips;
    if (e.alloc && !changed) {
        e.submission = *m_submitted + 1u;
        offset = e.offset;
        out = nullptr;
        return true;
    }
    if (e.alloc) m_held_garbage.emplace_back(e.submission, std::exchange(e.alloc, VK_NULL_HANDLE));
    e.stamp = 0;
    VkDeviceSize at = 0;
    if (!changed && m_ring.hold(bytes, e.alloc, at)) {
        e.offset = (u32)at;
        e.submission = *m_submitted + 1u;
        offset = e.offset;
        out = m_ring.data() + at;
        return true;
    }
    const vk_slice s = m_ring.alloc(bytes, 16);
    offset = (u32)s.offset;
    out = s.ptr;
    return s.ptr != nullptr;
}

void renderer::evict(u64 age)
{
    const auto drop = [&](auto& cache) {
        cache.erase_if([&](const auto&, const cached& e) {
            if (m_flips - e.used < age) return false;
            if (e.alloc) m_held_garbage.emplace_back(e.submission, e.alloc);
            return true;
        });
    };
    drop(m_vertex_cache);
    drop(m_index_cache);
    vkGetSemaphoreCounterValue(m_d->dev, m_timeline, &m_reached);
    release_held();
}

void renderer::release_held()
{
    auto keep = std::remove_if(m_held_garbage.begin(), m_held_garbage.end(), [&](const std::pair<uint64_t, VmaVirtualAllocation>& g) {
        if (g.first > m_reached) return false;
        m_ring.release(g.second);
        return true;
    });
    m_held_garbage.erase(keep, m_held_garbage.end());
}

bool renderer::draw_setup()
{
    if (const u64 progress = rsx_core_progress(); progress != m_progress) {
        m_progress = progress;
        m_epoch++;
        stats.epochs++;
    }
    const u32 tex_dirty = dirty & (rsx::D_TEXTURES | rsx::D_VERTEX_TEXTURES);
    if (tex_dirty & rsx::D_TEXTURES) {
        describe_units(0, rsx::TEXTURES);
        u32 fdims = 0, fshadow = 0, fak = 0;
        for (u32 u = 0; u < rsx::TEXTURES; u++) {
            fdims |= texture_dims(m_units[u].desc, m_units[u].valid) << (u * 2u);
            if (m_units[u].valid) {
                if (m_units[u].desc.ops.depth_compare) fshadow |= 1u << u;
                if ((m_units[u].regs.control0 >> 2) & 1u) fak = RSX_SPV_FPOUT_ALPHAKILL;
            }
        }
        m_fp_tex_dims = fdims;
        m_fp_tex_shadow = fshadow;
        m_fp_tex_alphakill = fak;
        m_fp_tex_valid = true;
    }
    if (tex_dirty & rsx::D_VERTEX_TEXTURES) {
        describe_units(rsx::TEXTURES, rsx::TEXTURES + rsx::VERTEX_TEXTURES);
        u32 vdims = 0;
        for (u32 u = 0; u < rsx::VERTEX_TEXTURES; u++)
            vdims |= texture_dims(m_units[rsx::TEXTURES + u].desc, m_units[rsx::TEXTURES + u].valid) << (u * 2u);
        if (vdims != m_vertex_dims) {
            m_vertex_dims = vdims;
            dirty |= rsx::D_VERTEX_PROGRAM;
        }
    }
    dirty &= ~(rsx::D_TEXTURES | rsx::D_VERTEX_TEXTURES);
    if ((dirty & rsx::D_VERTEX_PROGRAM) || !m_vp) {
        rsx_spv_vp_key key{};
        key.start = r(rsx::M_TRANSFORM_PROGRAM_START);
        m_program_hash = shader_cache::program_hash(program, key.start);
        key.output_mask = r(rsx::M_VERTEX_ATTRIB_OUTPUT_MASK);
        key.texture_dims = m_vertex_dims;

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
    fp_seen_key seen_key{};
    seen_key.ea = m_fp_ea;
    rsx_spv_fp_key& fk = seen_key.key;
    fk.control = r(rsx::M_SHADER_CONTROL);
    if (__builtin_expect(m_fp_tex_valid, 1)) {
        fk.texture_dims = m_fp_tex_dims;
        fk.texture_shadow = m_fp_tex_shadow;
        fk.output_mask = m_fp_tex_alphakill;
    } else {
        for (u32 u = 0; u < rsx::TEXTURES; u++) {
            fk.texture_dims |= texture_dims(m_units[u].desc, m_units[u].valid) << (u * 2u);
            if (m_units[u].valid) {
                if (m_units[u].desc.ops.depth_compare) fk.texture_shadow |= 1u << u;
                if ((m_units[u].regs.control0 >> 2) & 1u) fk.output_mask |= RSX_SPV_FPOUT_ALPHAKILL;
            }
        }
    }
    for (u32 k = 0; k < 4; k++)
        if (m_fb.color_format[k] != VK_FORMAT_UNDEFINED) fk.output_mask |= 1u << k;
    if (r(rsx::M_ALPHA_TEST_ENABLE)) fk.output_mask |= RSX_SPV_FPOUT_ALPHA_TEST;
    if (!m_fp_last || !(seen_key == m_fp_last_key)) {
        fp_lookup*& found = m_fp_seen[seen_key];
        if (!found) found = &m_fp_pool.emplace_back();
        m_fp_last = found;
        m_fp_last_key = seen_key;
    }
    fp_lookup& seen = *m_fp_last;
    const u32 count = (u32)seen.slots.size();
    for (u32 k = 1; k <= count; k++) {
        fp_candidate& c = seen.slots[(seen.next + count - k) % count];
        if (!c.fp) continue;
        await_writes(m_fp_ea, c.count * 4u);
        if (program_matches(ucode, c.code.data(), c.count)) {
            m_fp = c.fp;
            return true;
        }
    }
    await_writes(m_fp_ea, RSX_SPV_FP_MAX_INSTRUCTIONS * 16u);
    m_fp = m_shaders.fragment(ucode, RSX_SPV_FP_MAX_INSTRUCTIONS * 4u, fk, true);
    if (!m_fp) return false;
    fp_candidate& c = seen.slots[seen.next];
    seen.next = (seen.next + 1u) % count;
    c.fp = m_fp;
    c.count = m_fp->info.ucode_words;
    c.dirty_hits = 0;
    c.tracked = false;
    c.stamp = 0;
    c.code.assign(((c.count + 7u) & ~size_t(7)) * 2u, 0u);
    for (u32 i = 0; i < c.count; i++) {
        c.code[(i & ~7u) * 2u + (i & 7u)] = ucode[i];
        c.code[(i & ~7u) * 2u + 8u + (i & 7u)] = ~0u;
    }
    for (u32 i = 0; i < m_fp->info.constant_count; i++)
        for (u32 w = m_fp->info.constant_word[i]; w < m_fp->info.constant_word[i] + 4u && w < c.count; w++) {
            c.code[(w & ~7u) * 2u + (w & 7u)] = 0u;
            c.code[(w & ~7u) * 2u + 8u + (w & 7u)] = 0u;
        }
    return true;
}

void renderer::draw_execute(const rsx::draw_clause& clause, u32 id)
{
    const u32 prim = clause.primitive;
    bool emulated = false;
    primitive_topology(prim, emulated);
    const bool restart = r(rsx::M_RESTART_INDEX_ENABLE) != 0;
    const u32 restart_index = r(rsx::M_RESTART_INDEX);


    std::vector<draw_sub>& subs = m_subs;
    subs.clear();
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    u32 e0 = 0, ecount = 0, vb = 0;

    if (clause.kind == rsx::draw_kind::indexed) {
        const u32 dma = r(rsx::M_INDEX_ARRAY_DMA);
        const bool wide = clause.immediate || ((dma >> 4) & 0xFu) == k_index_type_u32;
        const u32 isize = wide ? 4u : 2u;
        const uint8_t* base = nullptr;
        u32 source = 0;
        if (clause.immediate) {
            const size_t n = clause.elements.size();
            m_index_scratch.resize(n * 4u);
            size_t i = 0;
#ifdef __AVX2__
            const __m256i shuf = _mm256_setr_epi8(
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
            for (; i + 8u <= n; i += 8u) {
                const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(clause.elements.data() + i));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(m_index_scratch.data() + i * 4u), _mm256_shuffle_epi8(v, shuf));
            }
#endif
            for (; i < n; i++) {
                const u32 v = __builtin_bswap32(clause.elements[i]);
                std::memcpy(m_index_scratch.data() + i * 4u, &v, 4u);
            }
            base = m_index_scratch.data();
        } else {
            const u32 ea = rsx_address((dma & 0xFu) ? RSX_DMA_MAIN : RSX_DMA_LOCAL, r(rsx::M_INDEX_ARRAY_ADDRESS), 0);
            if (!ea) return;
            source = ea & ~(isize - 1u);
            base = vm_base + source;
        }
        index_type = wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        u32 total = 0;
        for (const rsx::draw_range& rg : clause.ranges) total += expanded_index_count(prim, rg.count) + 1u;
        cached* e = nullptr;
        u32 offset = 0;
        uint8_t* out = nullptr;
        if (source && clause.ranges.size() == 1) {
            const rsx::draw_range& rg = clause.ranges[0];
            e = &m_index_cache[index_key{ source, rg.first, rg.count, prim, restart_index, (u32)wide | (u32)restart << 1 }];
            if (!cache_place(*e, source + rg.first * isize, rg.count * isize, total * isize, offset, out)) return;
        } else {
            const vk_slice s = m_ring.alloc((VkDeviceSize)total * isize, 16);
            if (!s.ptr) return;
            offset = (u32)s.offset;
            out = s.ptr;
        }
        u32 lo = out ? 0xFFFFFFFFu : e->min, hi = out ? 0 : e->max;
        if (!out) {
            subs.push_back({ offset / isize, e->count, 0, true });
        } else {
            const u32 first = offset / isize;
            u32 at = 0;
            for (const rsx::draw_range& rg : clause.ranges) {
                const index_result res = convert_indices(prim, wide, base + (size_t)rg.first * isize, rg.count, restart, restart_index,
                                                         out + (size_t)at * isize);
                if (!res.count || res.min > res.max) continue;
                lo = std::min(lo, res.min);
                hi = std::max(hi, res.max);
                subs.push_back({ first + at, res.count, 0, true });
                at += res.count;
            }
            if (e) {
                e->stamp = m_stamp;
                e->count = at;
                e->min = lo;
                e->max = hi;
            }
        }
        if (subs.empty() || lo > hi) return;
        const u32 base_index = r(rsx::M_VERTEX_BASE_INDEX);
        e0 = lo + base_index;
        ecount = hi - lo + 1u;
        vb = e0;
        for (draw_sub& s : subs) s.vertex_offset = (int32_t)base_index;
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
            for (draw_sub& s : subs) {
                const u32 n = expanded_index_count(prim, s.count);
                cached& e = m_index_cache[index_key{ 0u, 0u, s.count, prim, 0u, 0u }];
                u32 offset = 0;
                uint8_t* out = nullptr;
                if (!cache_place(e, 0, 0, (n + 1u) * 4u, offset, out)) return;
                if (out) generate_indices(prim, s.count, reinterpret_cast<u32*>(out));
                s.vertex_offset = (int32_t)s.first;
                s.first = offset / 4u;
                s.count = n;
                s.indexed = true;
            }
        }
    }

    VkDescriptorBufferInfo vlayout{};
    if (!upload_vertices(clause, e0, ecount, vb, vlayout)) return;

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
    VkDescriptorBufferInfo fp_consts{};
    if (fi.constant_count) {
        m_fp_constants.resize((size_t)fi.constant_count * 4u);
        const u32* ucode = reinterpret_cast<const u32*>(vm_base + m_fp_ea);
        const __m128i swap16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        for (u32 i = 0; i < fi.constant_count; i++) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ucode + fi.constant_word[i]));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(m_fp_constants.data() + i * 4u), _mm_shuffle_epi8(v, swap16));
        }
        if (!upload(m_fp_constant_upload, m_fp_constants.data(), fi.constant_count * 16u, fp_consts)) return;
    } else {
        static constexpr u32 k_zero_const[4] = {};
        if (!upload(m_fp_constant_upload, k_zero_const, sizeof k_zero_const, fp_consts)) return;
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
    fs.wpos_scale = (bottom ? -(float)m_fb.scale.den : (float)m_fb.scale.den) / (float)m_fb.scale.num;
    fs.wpos_bias = bottom ? (float)(window & 0xFFFu) : 0.f;
    VkDescriptorImageInfo images[rsx::TEXTURES + rsx::VERTEX_TEXTURES];
    u8 unit0_tag = 0;
    for (u32 u = 0; u < rsx::TEXTURES + rsx::VERTEX_TEXTURES; u++) {
        const bool vertex_unit = u >= rsx::TEXTURES;
        const u32 unit = vertex_unit ? u - rsx::TEXTURES : u;
        const u32 used = vertex_unit ? m_vp->info.texture_mask : m_fp->info.texture_mask;
        rsx_spv_texture_state scratch{};
        rsx_spv_texture_state& st = vertex_unit ? scratch : fs.texture[u];
        st.scale[0] = st.scale[1] = 1.f;
        if ((used >> unit) & 1u && resolve_texture(unit, vertex_unit, images[u], st, u == 0 ? &unit0_tag : nullptr)) continue;
        const u32 dims = texture_dims(m_units[u].desc, m_units[u].valid);
        const image& dummy = m_dummy[dims == RSX_SPV_TEX_CUBE ? 1 : dims == RSX_SPV_TEX_3D ? 2 : 0];
        images[u] = { m_dummy_sampler, dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    if (__builtin_expect(unit0_tag >= 2u, 0)) {
        if (m_qte_ring_epoch != m_epoch) {
            m_qte_rings_prev = std::move(m_qte_rings_cur);
            m_qte_rings_cur.clear();
            m_qte_ring_epoch = m_epoch;
        }
        constexpr u32 k_gfx_pos_reg = 203u;
        if ((m_vp->info.constants_used[k_gfx_pos_reg / 32u] >> (k_gfx_pos_reg % 32u)) & 1u) {
            float c203[4];
            std::memcpy(c203, constants + k_gfx_pos_reg * 4u, sizeof c203);
            const float cx = c203[0], cy = c203[1];
            if (unit0_tag == 2u) {
                m_qte_rings_cur.push_back({ cx, cy });
            } else if (unit0_tag == 3u) {
                auto find_ring = [&](const std::vector<qte_anchor>& list, float& rx, float& ry) -> bool {
                    float best = 1e9f;
                    bool found = false;
                    for (const qte_anchor& a : list) {
                        const float dx = std::fabs(cx - a.cx), dy = std::fabs(cy - a.cy);
                        if (dx < 0.18f && dy < 0.28f && dx * dx + dy * dy < best) {
                            best = dx * dx + dy * dy;
                            rx = a.cx;
                            ry = a.cy;
                            found = true;
                        }
                    }
                    return found;
                };
                float rx = 0.f, ry = 0.f;
                if (find_ring(m_qte_rings_cur, rx, ry) || find_ring(m_qte_rings_prev, rx, ry)) {
                    vk_slice cs = m_ring.alloc(sizeof constants, m_ubo_align);
                    if (cs.ptr) {
                        std::memcpy(cs.ptr, constants, sizeof constants);
                        std::memcpy(cs.ptr + k_gfx_pos_reg * 16u, &rx, 4);
                        std::memcpy(cs.ptr + k_gfx_pos_reg * 16u + 4u, &ry, 4);
                        m_constants = { cs.buffer, cs.offset, sizeof constants };
                        m_constants_vp = nullptr;
                    }
                }
            }
        }
    }
    u32 tcc = 0;
    for (u32 i = 0; i < 10; i++)
        if (r(rsx::M_TEX_COORD_CONTROL + i * 4u) & 1u) tcc |= 1u << i;
    const u32 sprite = r(rsx::M_POINT_SPRITE_CONTROL);
    if (sprite & 1u) tcc |= ((sprite >> 8) & 0x3FFu) << 16;
    fs.texcoord_control = tcc;
    VkDescriptorBufferInfo fp_state{};
    if (!upload(m_fp_state_upload, &fs, sizeof fs, fp_state)) return;

    constexpr u32 k_dyn_mask = rsx::D_SURFACE | rsx::D_VIEWPORT | rsx::D_RASTER | rsx::D_DEPTH_STENCIL | rsx::D_BLEND;
    const bool restart_en = restart && clause.kind == rsx::draw_kind::indexed;
    const bool dyn_changed = !m_dynamic.valid || m_dyn_dirty || (dirty & k_dyn_mask) || m_last_dyn_prim != prim || m_last_dyn_restart != restart_en;
    draw_target t{};
    if (dyn_changed) {
        t.width = m_fb.width;
        t.height = m_fb.height;
        t.aa_u = m_fb.aa_u;
        t.aa_v = m_fb.aa_v;
        t.scale = m_fb.scale;
        t.depth = m_fb.depth != nullptr;
        t.stencil = m_fb.depth && (m_fb.depth->img.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
        for (u32 k = 0; k < 4; k++) t.color_format[k] = m_fb.color_format[k];
        m_last_pipe_key = {};
        fill_pipeline_state(*this, *m_d, t, prim, m_last_pipe_key);
    }

    pipeline_key key = m_last_pipe_key;
    key.vs = m_vp->hash;
    key.fs = m_fp->hash;
    key.depth_format = m_fb.depth_format;
    const VkPipeline pipe = m_pipelines.get(key, m_vp->code, m_fp->code);
    if (!pipe) {
        if (m_trace) std::fprintf(m_trace, "[trace] %u skipped: pipeline compiling or failed\n", id);
        return;
    }

    begin_rendering();
    VkCommandBuffer c = cmd();
    if (m_occlusion.enabled() && !m_occlusion.active()) m_occlusion.begin(c, m_fb.scale);
    if (pipe != m_bound.pipeline) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        m_bound.pipeline = pipe;
    }
    if (dyn_changed) {
        set_dynamic_state(c, *this, *m_d, m_pipelines, t, prim, restart_en, key, m_dynamic);
        m_dyn_dirty = 0;
        dirty &= ~k_dyn_mask;
        m_last_dyn_prim = prim;
        m_last_dyn_restart = restart_en;
    }

    const bool all = !m_bound.valid;
    const u32 offsets[4] = { (u32)m_constants.offset, (u32)vlayout.offset, (u32)fp_consts.offset, (u32)fp_state.offset };
    if (all || std::memcmp(m_bound.offsets, offsets, sizeof offsets)) {
        std::memcpy(m_bound.offsets, offsets, sizeof offsets);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines.layout(), RSX_SPV_SET_BUFFERS, 1, &m_buffer_set, 4, offsets);
    }
    VkWriteDescriptorSet w[rsx::TEXTURES + rsx::VERTEX_TEXTURES]{};
    u32 nw = 0;
    for (u32 u = 0; u < rsx::TEXTURES + rsx::VERTEX_TEXTURES; u++) {
        VkDescriptorImageInfo& last = m_bound.images[u];
        if (!all && last.sampler == images[u].sampler && last.imageView == images[u].imageView && last.imageLayout == images[u].imageLayout) continue;
        last = images[u];
        VkWriteDescriptorSet& x = w[nw++];
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstBinding = u >= rsx::TEXTURES ? RSX_SPV_BIND_VERTEX_TEXTURE0 + (u - rsx::TEXTURES) : RSX_SPV_BIND_TEXTURE0 + u;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        x.pImageInfo = &last;
    }
    m_bound.valid = true;
    if (all) m_bound.index_type = VK_INDEX_TYPE_MAX_ENUM;
    if (nw) vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines.layout(), RSX_SPV_SET_TEXTURES, nw, w);

    if (subs.front().indexed && m_bound.index_type != index_type) {
        vkCmdBindIndexBuffer(c, m_ring.buffer(), 0, index_type);
        m_bound.index_type = index_type;
    }
    for (const draw_sub& s : subs) {
        if (s.indexed) vkCmdDrawIndexed(c, s.count, 1, s.first, s.vertex_offset, 0);
        else vkCmdDraw(c, s.count, 1, s.first, 0);
    }
    trace_mark(c, id);

    for (u32 k = 0; k < m_fb.count; k++)
        if (m_fb.color[k] && m_fb.color_write[k]) {
            m_fb.color[k]->gpu_dirty = true;
            m_fb.color[k]->version = ++m_content_clock;
        }
    if (m_fb.depth && m_fb.depth_write) m_fb.depth->gpu_dirty = true;
    if (m_fb.depth && m_fb.z_write) m_fb.depth->depth_version = ++m_content_clock;
    m_draw_counter++;
    stats.draws++;
    if (m_trace) {
        u32 n = 0;
        for (const draw_sub& s : subs) n += s.count;
        trace_draw(id, prim, n, subs.front().indexed);
    }
}

}
