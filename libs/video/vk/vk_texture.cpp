#include "vk_renderer.h"

#include "../rsx_core.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vk {

namespace {

constexpr u32 k_page_shift = 12;

u32 texel_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_SRGB: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_B8G8R8A8_SNORM: case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}
constexpr u32 k_local_scan_bytes = RSX_LOCAL_SIZE_MAX;

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t texture_key(const rsx::texture_desc& d, u32 ea)
{
    uint64_t h = ea;
    h = mix(h, d.gcm_format | ((u32)d.linear << 8) | ((u32)d.cube << 9) | ((u32)d.border << 10) | (d.dimension << 12));
    h = mix(h, ((uint64_t)d.width << 32) | d.height);
    h = mix(h, ((uint64_t)d.depth_slices << 32) | (d.levels << 8) | d.layers);
    h = mix(h, ((uint64_t)d.pitch << 32) | (u32)d.format);
    return h;
}

uint64_t swizzle_key(VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, const VkComponentMapping& s)
{
    return ((uint64_t)type << 56) ^ ((uint64_t)format << 24) ^ ((uint64_t)aspect << 20) ^ ((uint64_t)s.r << 12) ^ ((uint64_t)s.g << 8) ^
           ((uint64_t)s.b << 4) ^ (uint64_t)s.a;
}

}

void renderer::mark_written(u32 ea, u32 size)
{
    if (m_page_stamp.empty() || !size) return;
    const u32 stamp = ++m_stamp;
    for (uint64_t p = ea >> k_page_shift; p <= ((uint64_t)ea + size - 1u) >> k_page_shift && p < m_page_stamp.size(); p++)
        m_page_stamp[p] = stamp;
}

bool renderer::memory_changed(u32 ea, u32 size, u32 stamp)
{
#ifdef _WIN32
    if (m_page_stamp.empty()) m_page_stamp.assign(1u << (32 - k_page_shift), 0u);
    static thread_local std::vector<PVOID> pages(1u << 16);
    auto scan = [&](u32 base, u32 bytes) {
        for (;;) {
            ULONG_PTR count = pages.size();
            DWORD granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, vm_base + base, bytes, pages.data(), &count, &granularity) != 0) return false;
            if (count) {
                const u32 s = ++m_stamp;
                for (ULONG_PTR i = 0; i < count; i++) {
                    const uint64_t page = (uint64_t)(static_cast<uint8_t*>(pages[i]) - vm_base) >> k_page_shift;
                    if (page < m_page_stamp.size()) m_page_stamp[page] = s;
                }
            }
            if (count < pages.size()) return true;
        }
    };
    if (m_scanned_epoch != m_epoch) {
        m_scanned_epoch = m_epoch;
        scan(RSX_LOCAL_BASE, std::min<u32>(k_local_scan_bytes, rsx_core_local_size()));
    }
    if (ea < RSX_LOCAL_BASE) scan(ea & ~((1u << k_page_shift) - 1u), ((ea & ((1u << k_page_shift) - 1u)) + size + (1u << k_page_shift) - 1u) & ~((1u << k_page_shift) - 1u));
    for (uint64_t p = ea >> k_page_shift; p <= ((uint64_t)ea + size - 1u) >> k_page_shift; p++)
        if (m_page_stamp[p] > stamp) return true;
    return false;
#else
    if (m_page_stamp.empty()) return false;
    for (uint64_t p = ea >> k_page_shift; p <= ((uint64_t)ea + size - 1u) >> k_page_shift && p < m_page_stamp.size(); p++)
        if (m_page_stamp[p] > stamp) return true;
    return false;
#endif
}

VkImageView renderer::view_for(image& img, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, const VkComponentMapping& swizzle)
{
    const uint64_t key = swizzle_key(type, format, aspect, swizzle);
    for (auto& [k, v] : img.views)
        if (k == key) return v;
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.handle;
    vi.viewType = type;
    vi.format = format;
    vi.components = swizzle;
    vi.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_d->dev, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    img.views.emplace_back(key, view);
    return view;
}

VkSampler renderer::sampler_for(const VkSamplerCreateInfo& info, const VkSamplerCustomBorderColorCreateInfoEXT* border)
{
    uint64_t h = 0;
    h = mix(h, (uint64_t)info.magFilter | ((uint64_t)info.minFilter << 4) | ((uint64_t)info.mipmapMode << 8) |
                   ((uint64_t)info.addressModeU << 12) | ((uint64_t)info.addressModeV << 16) | ((uint64_t)info.addressModeW << 20) |
                   ((uint64_t)info.anisotropyEnable << 24) | ((uint64_t)info.compareEnable << 25) | ((uint64_t)info.compareOp << 26) |
                   ((uint64_t)info.borderColor << 32) | ((uint64_t)info.unnormalizedCoordinates << 40));
    uint32_t f[4];
    std::memcpy(&f[0], &info.mipLodBias, 4);
    std::memcpy(&f[1], &info.maxAnisotropy, 4);
    std::memcpy(&f[2], &info.minLod, 4);
    std::memcpy(&f[3], &info.maxLod, 4);
    h = mix(h, ((uint64_t)f[0] << 32) | f[1]);
    h = mix(h, ((uint64_t)f[2] << 32) | f[3]);
    if (border) {
        uint64_t b[2];
        std::memcpy(b, border->customBorderColor.uint32, 16);
        h = mix(mix(h, b[0]), b[1]);
    }
    if (auto it = m_samplers.find(h); it != m_samplers.end()) return it->second;
    VkSamplerCreateInfo ci = info;
    ci.pNext = border;
    if (ci.anisotropyEnable && !m_d->features.features.samplerAnisotropy) ci.anisotropyEnable = VK_FALSE;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(m_d->dev, &ci, nullptr, &s);
    m_samplers.emplace(h, s);
    return s;
}

void renderer::describe_units()
{
    for (u32 u = 0; u < rsx::TEXTURES + rsx::VERTEX_TEXTURES; u++) {
        unit_binding& b = m_units[u];
        const bool vertex = u >= rsx::TEXTURES;
        const u32 base = vertex ? rsx::M_VTEX_OFFSET + (u - rsx::TEXTURES) * 0x20u : rsx::M_TEXTURE_OFFSET + u * 0x20u;
        rsx::texture_regs& g = b.regs;
        g.offset = r(base);
        g.format = r(base + 0x04u);
        g.address = r(base + 0x08u);
        g.control0 = r(base + 0x0Cu);
        g.filter = r(base + 0x14u);
        g.image_rect = r(base + 0x18u);
        g.border = r(base + 0x1Cu);
        if (vertex) {
            g.control1 = 0;
            g.control2 = 0;
            g.control3 = r(base + 0x10u);
        } else {
            g.control1 = r(base + 0x10u);
            g.control2 = r(rsx::M_TEXTURE_CONTROL2 + u * 4u);
            g.control3 = r(rsx::M_TEXTURE_CONTROL3 + u * 4u);
        }
        b.valid = rsx::texture_describe(g, vertex, b.desc);
    }
}

bool renderer::resolve_texture(u32 unit, bool vertex, VkDescriptorImageInfo& out, rsx_spv_texture_state& state)
{
    unit_binding& b = m_units[vertex ? rsx::TEXTURES + unit : unit];
    if (!b.valid) return false;
    const rsx::texture_desc& d = b.desc;
    const u32 ea = rsx_address(d.location ? RSX_DMA_MAIN : RSX_DMA_LOCAL, d.offset, d.guest_size);
    if (!ea) return false;

    VkSamplerCustomBorderColorCreateInfoEXT border{ VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT };
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    rsx::texture_sampler(b.regs, vertex, d, 16.0f, si, border);
    const bool custom = si.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || si.borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT;
    if (custom && !m_d->caps.custom_border_color) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const VkSampler sampler = sampler_for(si, custom && m_d->caps.custom_border_color ? &border : nullptr);

    u32 control = (u32)(d.ops.gamma_mask & 0xFu) << RSX_SPV_TEXC_GAMMA_SHIFT;
    control |= (u32)(d.ops.bx2_mask & 0xFu) << RSX_SPV_TEXC_EXPAND_SHIFT;
    control |= (u32)(d.ops.sign_mask & 0xFu) << RSX_SPV_TEXC_SEXT_SHIFT;
    if (!vertex && ((b.regs.control0 >> 2) & 1u)) control |= RSX_SPV_TEXC_ALPHAKILL;
    if (d.gcm_format == 0x91u || d.gcm_format == 0x93u) control |= RSX_SPV_TEXC_DEPTH_FLOAT;
    if (d.gcm_format == 0x94u || d.gcm_format == 0x95u) control |= RSX_SPV_TEXC_CHANNELS16;
    if (d.ops.unnormalized) control |= RSX_SPV_TEXC_UNNORMALIZED;
    state.control = control;
    state.remap = 0;
    state.scale[0] = d.ops.unnormalized ? 1.0f / (float)std::max(d.width, 1u) : 1.0f;
    state.scale[1] = d.ops.unnormalized ? 1.0f / (float)std::max(d.height, 1u) : 1.0f;

    const VkImageViewType type = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.dimension == 3 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    VkCommandBuffer c = cmd();

    static const bool trace = std::getenv("PS3_VK_TEXTRACE") != nullptr;
    for (auto& p : m_surfaces) {
        surface* s = p.get();
        if (s->ea != ea) continue;
        if (trace && (s->state != SURFACE_VALID || s->depth != d.depth || (!s->depth && texel_bytes(s->img.format) != texel_bytes(d.format))))
            std::fprintf(stderr, "[vk] surface 0x%08X skipped: state %d depth %d/%d format %d/%d gcm %02X\n", ea, s->state, s->depth, d.depth, s->img.format, d.format, d.gcm_format);
        if (s->state != SURFACE_VALID || d.cube || d.dimension == 3) continue;
        if (s->depth != d.depth) continue;
        if (!s->depth && (d.compressed || texel_bytes(s->img.format) == 0 || texel_bytes(s->img.format) != texel_bytes(d.format))) continue;
        bool bound = s == m_fb.depth;
        for (surface* f : m_fb.color) bound |= f == s;
        if (bound) {
            if (trace) std::fprintf(stderr, "[vk] surface 0x%08X sampled while bound\n", ea);
            return false;
        }
        end_rendering();
        barrier(c, s->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const VkImageAspectFlags aspect = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const VkImageView view = view_for(s->img, VK_IMAGE_VIEW_TYPE_2D, s->depth ? s->img.format : d.format, aspect, d.swizzle);
        if (!view) return false;
        out = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        return true;
    }

    const uint64_t key = texture_key(d, ea);
    auto it = m_textures.find(key);
    texture_entry* e = it != m_textures.end() ? it->second.get() : nullptr;
    bool upload = false;
    if (!e) {
        auto made = std::make_unique<texture_entry>();
        e = made.get();
        e->ea = ea;
        e->size = d.guest_size;
        e->key = key;
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = d.dimension == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ii.flags = d.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.format = d.format;
        ii.extent = { std::max(d.width, 1u), std::max(d.height, 1u), d.dimension == 3 ? std::max(d.depth_slices, 1u) : 1u };
        ii.mipLevels = std::max(d.levels, 1u);
        ii.arrayLayers = std::max(d.layers, 1u);
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(m_d->vma, &ii, &ai, &e->img.handle, &e->img.mem, nullptr) != VK_SUCCESS) return false;
        e->img.format = d.format;
        e->img.aspect = d.aspect;
        e->img.width = ii.extent.width;
        e->img.height = ii.extent.height;
        m_textures.emplace(key, std::move(made));
        upload = true;
    } else if (e->checked != m_epoch) {
        e->checked = m_epoch;
        upload = memory_changed(ea, d.guest_size, e->stamp);
    }

    if (upload) {
        stats.uploads++;
        if (trace) std::fprintf(stderr, "[vk] upload 0x%08X size %u fmt %02X %ux%u new %d\n", ea, d.guest_size, d.gcm_format, d.width, d.height, it == m_textures.end());
        const uint64_t bytes = rsx::texture_staging_size(d);
        vk_slice slice = m_ring.alloc(bytes, 16);
        if (!slice.ptr) return false;
        m_copies.clear();
        e->stamp = m_stamp;
        e->checked = m_epoch;
        rsx::texture_convert(d, vm_base + ea, slice.ptr, m_copies);
        end_rendering();
        barrier(c, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        std::vector<VkBufferImageCopy> regions(m_copies.size());
        for (size_t i = 0; i < m_copies.size(); i++) {
            const rsx::texture_copy& cp = m_copies[i];
            VkBufferImageCopy& rg = regions[i];
            rg.bufferOffset = slice.offset + cp.offset;
            rg.bufferRowLength = cp.row_texels;
            rg.bufferImageHeight = cp.rows;
            rg.imageSubresource = { d.aspect, cp.level, cp.layer, 1 };
            rg.imageExtent = { cp.width, cp.height, std::max(cp.depth, 1u) };
        }
        if (!regions.empty())
            vkCmdCopyBufferToImage(c, slice.buffer, e->img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (u32)regions.size(), regions.data());
    }
    if (e->img.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        end_rendering();
        barrier(c, e->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    const VkImageView view = view_for(e->img, type, d.format, d.aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : d.aspect, d.swizzle);
    if (!view) return false;
    out = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return true;
}

}
