#include "vk_renderer.h"


#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace vk {

namespace {

constexpr u32 k_page_shift = 12;
constexpr u32 k_region_shift = 20;

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

#ifdef _WIN32
bool scan_writes(u32 base, u32 bytes, std::vector<u32>& stamps, u32& clock)
{
    thread_local std::vector<PVOID> pages(1u << 16);
    for (;;) {
        ULONG_PTR count = pages.size();
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, vm_base + base, bytes, pages.data(), &count, &granularity) != 0) return false;
        if (count) {
            const u32 s = ++clock;
            for (ULONG_PTR i = 0; i < count; i++) stamps[(u32)(static_cast<uint8_t*>(pages[i]) - vm_base) >> k_page_shift] = s;
        }
        if (count < pages.size()) return true;
    }
}
#else
struct write_tracker {
    int uffd = -1;
    int pagemap = -1;
    bool ok = false;

    write_tracker()
    {
        constexpr uint64_t span = 1ull << 32;
        uffd = (int)syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
        if (uffd < 0) return;
        uffdio_api api{ UFFD_API, UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED, 0 };
        uffdio_register reg{ { (uintptr_t)vm_base, span }, UFFDIO_REGISTER_MODE_WP, 0 };
        uffdio_writeprotect wp{ { (uintptr_t)vm_base, span }, UFFDIO_WRITEPROTECT_MODE_WP };
        if (ioctl(uffd, UFFDIO_API, &api) || ioctl(uffd, UFFDIO_REGISTER, &reg) || ioctl(uffd, UFFDIO_WRITEPROTECT, &wp)) return;
        pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        ok = pagemap >= 0;
    }
};

bool scan_writes(u32 base, u32 bytes, std::vector<u32>& stamps, u32& clock)
{
    static const write_tracker tracker;
    if (!tracker.ok) return false;
    page_region regions[32];
    pm_scan_arg arg{};
    arg.size = sizeof arg;
    arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
    arg.start = (uintptr_t)(vm_base + base);
    arg.end = arg.start + bytes;
    arg.vec = (uintptr_t)regions;
    arg.vec_len = 32;
    arg.category_mask = PAGE_IS_WRITTEN;
    arg.return_mask = PAGE_IS_WRITTEN;
    for (;;) {
        const long n = ioctl(tracker.pagemap, PAGEMAP_SCAN, &arg);
        if (n < 0) return false;
        if (n) {
            const u32 s = ++clock;
            for (long i = 0; i < n; i++)
                for (uint64_t a = regions[i].start; a < regions[i].end; a += 1u << k_page_shift)
                    stamps[(u32)(a - (uintptr_t)vm_base) >> k_page_shift] = s;
        }
        if (arg.walk_end >= arg.end) return true;
        arg.start = arg.walk_end;
    }
}
#endif
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

void renderer::watch(u32 ea, u32 size)
{
    await_writes(ea, size);
    stats.watch_calls++;
    stats.watch_bytes += size;
    if (m_page_stamp.empty()) {
        m_page_stamp.assign(1u << (32 - k_page_shift), 0u);
        m_region_batch.assign(1u << (32 - k_region_shift), ~0ull);
    }
    const u64 batch = rsx_core_batch();
    const u32 last = (u32)(((uint64_t)ea + size - 1u) >> k_region_shift);
    for (u32 r = ea >> k_region_shift; r <= last; r++) {
        if (m_region_batch[r] == batch) continue;
        u32 end = r;
        while (end + 1 <= last && m_region_batch[end + 1] != batch) end++;
        stats.scans++;
        if (!scan_writes(r << k_region_shift, (end - r + 1) << k_region_shift, m_page_stamp, m_stamp)) {
            mark_written(ea, size);
            return;
        }
        for (u32 k = r; k <= end; k++) m_region_batch[k] = batch;
        r = end;
    }
}

bool renderer::memory_changed(u32 ea, u32 size, u32 stamp)
{
    watch(ea, size);
    for (uint64_t p = ea >> k_page_shift; p <= ((uint64_t)ea + size - 1u) >> k_page_shift; p++)
        if (m_page_stamp[p] > stamp) return true;
    return false;
}

u32 renderer::refresh(u32 ea, u32 size, u32 stamp, uint8_t* dst)
{
    if (!stamp) {
        std::memcpy(dst, vm_base + ea, size);
        return size;
    }
    u32 copied = 0;
    const u32 last = (u32)(((uint64_t)ea + size - 1u) >> k_page_shift);
    for (u32 p = ea >> k_page_shift; p <= last; p++) {
        if (m_page_stamp[p] <= stamp) continue;
        u32 q = p;
        while (q < last && m_page_stamp[q + 1u] > stamp) q++;
        const uint64_t lo = std::max<uint64_t>(ea, (uint64_t)p << k_page_shift);
        const uint64_t hi = std::min<uint64_t>((uint64_t)ea + size, (uint64_t)(q + 1u) << k_page_shift);
        std::memcpy(dst + (lo - ea), vm_base + lo, hi - lo);
        copied += (u32)(hi - lo);
        p = q;
    }
    return copied;
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

image* renderer::depth_as_color(surface& s, VkFormat format)
{
    image& dst = s.as_color;
    if (dst.width != s.img.width || dst.height != s.img.height || dst.format != format) {
        discard(dst);
        if (!create_image(dst, format, s.img.width, s.img.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT))
            return nullptr;
        s.as_color_version = ~0ull;
    }
    if (s.as_color_version == s.version) return &dst;
    const bool d16 = s.bpp == 2;
    const u32 w = s.img.width, h = s.img.height, row = d16 ? (w + 1u) & ~1u : w;
    const VkDeviceSize bytes = (VkDeviceSize)row * h * s.bpp;
    if (m_pack.size < bytes) {
        if (m_pack.buffer) m_scratch_garbage.emplace_back(*m_submitted + 1u, m_pack);
        m_pack = scratch{};
        if (!create_scratch(m_pack, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return nullptr;
    }
    const VkComponentMapping identity{};
    const VkImageView dv = view_for(s.img, VK_IMAGE_VIEW_TYPE_2D, s.img.format, VK_IMAGE_ASPECT_DEPTH_BIT, identity);
    const VkImageView sv = d16 ? dv : view_for(s.img, VK_IMAGE_VIEW_TYPE_2D, s.img.format, VK_IMAGE_ASPECT_STENCIL_BIT, identity);
    if (!dv || !sv) return nullptr;
    VkCommandBuffer c = cmd();
    end_rendering();
    barrier(c, s.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_depth_pack.record(c, dv, sv, m_pack.buffer, 0, bytes, w, h, d16, s.depth_float);
    memory_barrier(c, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = row;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(c, m_pack.buffer, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    s.as_color_version = s.version;
    return &dst;
}

image* renderer::native_image(surface& s, image& source)
{
    image& low = s.native;
    const u32 w = s.width * s.aa_u, h = s.height * s.aa_v;
    if (low.width != w || low.height != h || low.format != s.img.format) {
        discard(low);
        if (!create_image(low, s.img.format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT))
            return nullptr;
        s.native_version = ~0ull;
    }
    if (s.native_version == s.version) return &low;
    if (m_box.width < w || m_box.height < h) {
        discard(m_box);
        if (!create_image(m_box, VK_FORMAT_R32G32B32A32_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT))
            return nullptr;
    }
    VkCommandBuffer c = cmd();
    barrier(c, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, m_box, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_downscale.record(c, source.view, m_box.view, w, h, source.width, source.height);
    barrier(c, m_box, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(c, low, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    vkCmdBlitImage(c, m_box.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, low.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    s.native_version = s.version;
    return &low;
}

bool renderer::resolve_texture(u32 unit, bool vertex, VkDescriptorImageInfo& out, rsx_spv_texture_state& state)
{
    const u32 slot = vertex ? rsx::TEXTURES + unit : unit;
    unit_binding& b = m_units[slot];
    const char*& path = m_texture_path[slot];
    path = "dummy: unit disabled";
    if (!b.valid) return false;
    const rsx::texture_desc& d = b.desc;
    const u32 ea = rsx_address(d.location ? RSX_DMA_MAIN : RSX_DMA_LOCAL, d.offset, d.guest_size);
    path = "dummy: address out of range";
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

    const char* skipped = nullptr;
    for (auto& p : m_surfaces) {
        surface* s = p.get();
        if (s->ea != ea) continue;
        if (s->state != SURFACE_VALID) { skipped = "memory: surface not valid"; continue; }
        if (d.cube || d.dimension == 3) { skipped = "memory: surface sampled as cube/3D"; continue; }
        if (s->depth && !d.depth) {
            if (d.compressed || s->bpp != texel_bytes(d.format)) {
                skipped = "memory: depth surface sampled as color of another size";
                continue;
            }
            image* packed = depth_as_color(*s, d.format);
            path = "dummy: depth pack failed";
            if (!packed) return false;
            barrier(c, *packed, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            const VkImageView view = view_for(*packed, VK_IMAGE_VIEW_TYPE_2D, d.format, VK_IMAGE_ASPECT_COLOR_BIT, d.swizzle);
            if (!view) return false;
            path = "depth packed as color";
            out = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            return true;
        }
        if (s->depth != d.depth) { skipped = "memory: color surface sampled as depth"; continue; }
        if (!s->depth && (d.compressed || texel_bytes(s->img.format) == 0 || texel_bytes(s->img.format) != texel_bytes(d.format))) {
            skipped = "memory: surface texel size differs";
            continue;
        }
        bool bound = s == m_fb.depth;
        for (surface* f : m_fb.color) bound |= f == s;
        const VkImageAspectFlags aspect = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        image* source = &s->img;
        end_rendering();
        if (bound) {
            image& snap = s->snapshot;
            path = "dummy: snapshot creation failed";
            if (snap.width != s->img.width || snap.height != s->img.height || snap.format != s->img.format) {
                discard(snap);
                if (!create_image(snap, s->img.format, s->img.width, s->img.height,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s->img.aspect))
                    return false;
                s->snapshot_draw = ~0ull;
            }
            if (s->snapshot_draw != m_draw_counter) {
                barrier(c, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                barrier(c, snap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkImageCopy region{};
                region.srcSubresource = { aspect, 0, 0, 1 };
                region.dstSubresource = { aspect, 0, 0, 1 };
                region.extent = { s->img.width, s->img.height, 1 };
                vkCmdCopyImage(c, s->img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
                s->snapshot_draw = m_draw_counter;
            }
            source = &snap;
        }
        if (!s->depth && s->scale > m_fb.scale) {
            path = "dummy: rescale creation failed";
            source = native_image(*s, *source);
            if (!source) return false;
        }
        barrier(c, *source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const VkImageView view = view_for(*source, VK_IMAGE_VIEW_TYPE_2D, s->depth ? s->img.format : d.format, aspect, d.swizzle);
        path = "dummy: surface view failed";
        if (!view) return false;
        path = source == &s->native ? "surface rescaled" : bound ? "surface snapshot" : "surface";
        out = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        return true;
    }
    path = "dummy: texture creation failed";

    const uint64_t key = texture_key(d, ea);
    auto it = m_textures.find(key);
    texture_entry* e = it != m_textures.end() ? it->second.get() : nullptr;
    if (e) e->used = m_flips;
    bool upload = false;
    if (!e) {
        auto made = std::make_unique<texture_entry>();
        e = made.get();
        e->ea = ea;
        e->used = m_flips;
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
        watch(ea, d.guest_size);
        upload = true;
    } else if (e->checked != m_epoch) {
        e->checked = m_epoch;
        upload = memory_changed(ea, d.guest_size, e->stamp);
    }

    if (upload) {
        stats.uploads++;
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
    path = skipped ? skipped : "memory";
    out = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return true;
}

}
