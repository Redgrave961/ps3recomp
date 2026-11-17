#include "vk_renderer.h"

#include "../rsx_core.h"

#include <algorithm>
#include <cstring>

namespace vk {

namespace {

enum fb_context : u32 { FB_DRAW = 0, FB_CLEAR_COLOR = 1, FB_CLEAR_DEPTH = 2 };

constexpr u32 k_min_scalable = 16;

enum : u32 {
    CLEAR_Z = 0x01u, CLEAR_S = 0x02u, CLEAR_R = 0x10u, CLEAR_G = 0x20u, CLEAR_B = 0x40u, CLEAR_A = 0x80u,
    CLEAR_RGBA = 0xF0u, CLEAR_ANY = 0xF3u,
};

constexpr u32 k_color_dma[4] = { rsx::M_DMA_COLOR_A, rsx::M_DMA_COLOR_B, rsx::M_DMA_COLOR_C, rsx::M_DMA_COLOR_D };
constexpr u32 k_color_offset[4] = { rsx::M_SURFACE_COLOR_A, rsx::M_SURFACE_COLOR_B, rsx::M_SURFACE_COLOR_C, rsx::M_SURFACE_COLOR_D };
constexpr u32 k_color_pitch[4] = { rsx::M_SURFACE_PITCH_A, rsx::M_SURFACE_PITCH_B, rsx::M_SURFACE_PITCH_C, rsx::M_SURFACE_PITCH_D };

u32 color_bpp(u32 fmt)
{
    switch (fmt) {
    case 1: case 2: case 3: case 10: return 2;
    case 9: return 1;
    case 11: return 8;
    case 12: return 16;
    default: return 4;
    }
}

VkFormat color_format(u32 fmt)
{
    switch (fmt) {
    case 1: case 2: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 3: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 9: return VK_FORMAT_R8_UNORM;
    case 10: return VK_FORMAT_R8G8_UNORM;
    case 11: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 12: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 13: return VK_FORMAT_R32_SFLOAT;
    case 14: case 15: case 16: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

u32 target_indexes(u32 target, u32 out[4])
{
    switch (target) {
    case 0x01: out[0] = 0; return 1;
    case 0x02: out[0] = 1; return 1;
    case 0x13: out[0] = 0; out[1] = 1; return 2;
    case 0x17: out[0] = 0; out[1] = 1; out[2] = 2; return 3;
    case 0x1F: out[0] = 0; out[1] = 1; out[2] = 2; out[3] = 3; return 4;
    default: return 0;
    }
}

bool stencil_writes(const rsx::frontend& f, bool back)
{
    const u32 base = back ? rsx::M_BACK_STENCIL_MASK : rsx::M_STENCIL_MASK;
    const u32 fail = back ? rsx::M_BACK_STENCIL_FAIL : rsx::M_STENCIL_FAIL;
    if (!(f.r(base) & 0xFFu)) return false;
    for (u32 i = 0; i < 3; i++)
        if (f.r(fail + i * 4u) != 0x1E00u) return true;
    return false;
}

bool ranges_overlap(u32 a, u32 an, u32 b, u32 bn)
{
    return a < b + bn && b < a + an;
}

}

bool framebuffer::operator==(const framebuffer& o) const
{
    if (width != o.width || height != o.height || aa_u != o.aa_u || aa_v != o.aa_v || scale != o.scale || count != o.count || depth != o.depth ||
        depth_write != o.depth_write)
        return false;
    for (u32 i = 0; i < 4; i++)
        if (color[i] != o.color[i] || color_write[i] != o.color_write[i]) return false;
    return true;
}

bool renderer::open(vk_device& d, VkSemaphore timeline, uint64_t* submitted, const char* cache_path)
{
    m_d = &d;
    m_timeline = timeline;
    m_submitted = submitted;
    for (submission& s : m_submissions) {
        VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = d.family;
        VK_TRY(vkCreateCommandPool(d.dev, &pi, nullptr, &s.pool));
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(d.dev, &ai, &s.cmd));
    }
    m_ubo_align = std::max<VkDeviceSize>(16, d.props.limits.minUniformBufferOffsetAlignment);
    m_ssbo_align = std::max<VkDeviceSize>(16, d.props.limits.minStorageBufferOffsetAlignment);
    m_shaders.init(d, m_pipelines);
    if (!m_ring.create(d, 256ull << 20, timeline) || !m_pipelines.create(d, cache_path) || !create_dummies() ||
        !m_occlusion.create(d, timeline) || !m_depth_pack.create(d))
        return false;
    m_pipelines.precompile(m_shaders);
    return true;
}

void renderer::close()
{
    if (!m_d) return;
    {
        std::lock_guard<std::mutex> hold(m_d->queue_lock);
        vkQueueWaitIdle(m_d->queue);
    }
    m_occlusion.destroy();
    m_depth_pack.destroy();
    for (auto& s : m_surfaces) {
        destroy_image(s->img);
        destroy_image(s->snapshot);
        destroy_image(s->as_color);
    }
    for (auto& g : m_garbage) destroy_image(g.second);
    destroy_image(m_display);
    m_surfaces.clear();
    m_garbage.clear();
    for (image& i : m_dummy) destroy_image(i);
    for (auto& [k, e] : m_textures) destroy_image(e->img);
    m_textures.clear();
    for (auto& [k, s] : m_samplers) vkDestroySampler(m_d->dev, s, nullptr);
    m_samplers.clear();
    if (m_dummy_sampler) vkDestroySampler(m_d->dev, m_dummy_sampler, nullptr);
    m_pipelines.destroy();
    m_shaders.destroy();
    m_ring.destroy();
    for (submission& s : m_submissions)
        if (s.pool) vkDestroyCommandPool(m_d->dev, s.pool, nullptr);
    m_d = nullptr;
}

void renderer::barrier(VkCommandBuffer cmd, image& img, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
{
    constexpr VkAccessFlags2 writes = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                      VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (img.layout == layout && !(img.access & writes) && !(access & writes)) {
        img.stage |= stage;
        img.access |= access;
        return;
    }
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = img.stage ? img.stage : VK_PIPELINE_STAGE_2_NONE;
    b.srcAccessMask = img.access & writes;
    b.dstStageMask = stage;
    b.dstAccessMask = access;
    b.oldLayout = img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.handle;
    b.subresourceRange = { img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    img.layout = layout;
    img.stage = stage;
    img.access = access;
}

VkCommandBuffer renderer::cmd()
{
    submission& s = m_submissions[m_submission];
    if (m_recording) return s.cmd;
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &s.done;
    vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
    vkResetCommandPool(m_d->dev, s.pool, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    m_recording = true;
    return s.cmd;
}

void renderer::submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal)
{
    if (!m_recording && !wait && !signal) return;
    VkCommandBuffer c = cmd();
    end_rendering();
    vkEndCommandBuffer(c);
    const uint64_t value = ++*m_submitted;
    VkSemaphoreSubmitInfo waits{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    waits.semaphore = wait;
    waits.stageMask = wait_stage;
    VkSemaphoreSubmitInfo signals[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    signals[0].semaphore = m_timeline;
    signals[0].value = value;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].semaphore = signal;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cb.commandBuffer = c;
    VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = wait ? 1u : 0u;
    si.pWaitSemaphoreInfos = &waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = signal ? 2u : 1u;
    si.pSignalSemaphoreInfos = signals;
    {
        std::lock_guard<std::mutex> hold(m_d->queue_lock);
        vkQueueSubmit2(m_d->queue, 1, &si, VK_NULL_HANDLE);
    }
    m_submissions[m_submission].done = value;
    m_submission = (m_submission + 1u) % (u32)m_submissions.size();
    m_recording = false;
    m_ring.close(value);
    m_occlusion.submitted(value);

    uint64_t reached = 0;
    vkGetSemaphoreCounterValue(m_d->dev, m_timeline, &reached);
    auto keep = std::remove_if(m_garbage.begin(), m_garbage.end(), [&](std::pair<uint64_t, image>& g) {
        if (g.first > reached) return false;
        destroy_image(g.second);
        return true;
    });
    m_garbage.erase(keep, m_garbage.end());
}

void renderer::flush()
{
    m_epoch++;
    if (m_recording) submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
}

bool renderer::create_image(image& img, VkFormat format, u32 width, u32 height, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = { width, height, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_TRY(vmaCreateImage(m_d->vma, &ii, &ai, &img.handle, &img.mem, nullptr));
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { aspect, 0, 1, 0, 1 };
    VK_TRY(vkCreateImageView(m_d->dev, &vi, nullptr, &img.view));
    img.format = format;
    img.aspect = aspect;
    img.width = width;
    img.height = height;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.stage = VK_PIPELINE_STAGE_2_NONE;
    img.access = 0;
    return true;
}

void renderer::destroy_image(image& img)
{
    for (auto& v : img.views) vkDestroyImageView(m_d->dev, v.second, nullptr);
    if (img.view) vkDestroyImageView(m_d->dev, img.view, nullptr);
    if (img.handle) vmaDestroyImage(m_d->vma, img.handle, img.mem);
    img = image{};
}

void renderer::retire(surface* s)
{
    m_garbage.emplace_back(*m_submitted + 1u, s->img);
    if (s->snapshot.handle) m_garbage.emplace_back(*m_submitted + 1u, s->snapshot);
    if (s->as_color.handle) m_garbage.emplace_back(*m_submitted + 1u, s->as_color);
    auto it = std::find_if(m_surfaces.begin(), m_surfaces.end(), [s](const std::unique_ptr<surface>& p) { return p.get() == s; });
    if (it != m_surfaces.end()) m_surfaces.erase(it);
}

surface* renderer::surface_for(u32 ea, u32 pitch, u32 width, u32 height, u32 format, bool depth, u32 aa_u, u32 aa_v)
{
    const u32 scale = std::max(width, height) > k_min_scalable ? resolution_scale : 1u;
    surface* grow_from = nullptr;
    for (auto& p : m_surfaces) {
        surface* s = p.get();
        if (s->ea != ea || s->depth != depth) continue;
        if (s->gcm_format == format && s->pitch == pitch && s->aa_u == aa_u && s->aa_v == aa_v && s->scale == scale) {
            if (s->width >= width && s->height >= height) return s;
            grow_from = s;
        }
        break;
    }
    auto made = std::make_unique<surface>();
    surface* s = made.get();
    s->ea = ea;
    s->pitch = pitch;
    s->width = std::max(width, grow_from ? grow_from->width : 0u);
    s->height = std::max(height, grow_from ? grow_from->height : 0u);
    s->gcm_format = format;
    s->depth = depth;
    s->aa_u = aa_u;
    s->aa_v = aa_v;
    s->scale = scale;
    s->bpp = depth ? (format == 1 ? 2u : 4u) : color_bpp(format);
    const bool depth_float = (r(rsx::M_CONTROL0) >> 12) & 1u;
    s->depth_float = depth && depth_float;
    VkFormat vf;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depth) {
        vf = format == 1 ? (depth_float ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM) : VK_FORMAT_D32_SFLOAT_S8_UINT;
        aspect = format == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        vf = color_format(format);
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (!create_image(s->img, vf, s->width * aa_u * scale, s->height * aa_v * scale, usage, aspect)) return nullptr;

    VkCommandBuffer c = cmd();
    if (grow_from) {
        end_rendering();
        barrier(c, grow_from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(c, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = { aspect, 0, 0, 1 };
        region.dstSubresource = { aspect, 0, 0, 1 };
        region.extent = { grow_from->img.width, grow_from->img.height, 1 };
        vkCmdCopyImage(c, grow_from->img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img.handle,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        s->state = grow_from->state;
        s->gpu_dirty = grow_from->gpu_dirty;
        retire(grow_from);
    }

    const u32 bytes = s->bytes();
    for (size_t i = 0; i < m_surfaces.size();) {
        surface* o = m_surfaces[i].get();
        bool bound = o == m_fb.depth;
        for (surface* b : m_fb.color) bound |= o == b;
        if (!bound && ranges_overlap(o->ea, o->bytes(), ea, bytes)) {
            retire(o);
            continue;
        }
        i++;
    }
    m_surfaces.push_back(std::move(made));
    return s;
}

void renderer::scale_blit(VkCommandBuffer c, image& src, image& dst, VkFilter filter)
{
    barrier(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit region{};
    region.srcSubresource = { src.aspect, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)src.width, (int32_t)src.height, 1 };
    region.dstSubresource = { dst.aspect, 0, 0, 1 };
    region.dstOffsets[1] = { (int32_t)dst.width, (int32_t)dst.height, 1 };
    vkCmdBlitImage(c, src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
}

void renderer::load_surface(surface& s)
{
    VkCommandBuffer c = cmd();
    end_rendering();
    const u32 w = s.img.width / s.scale, h = s.img.height / s.scale;
    const u32 row = w * s.bpp;
    if (!s.depth) {
        const uint8_t* src = vm_base + s.ea;
        vk_slice slice = m_ring.alloc((VkDeviceSize)row * h, 16);
        if (slice.ptr) {
            for (u32 y = 0; y < h; y++) {
                const uint8_t* in = src + (size_t)y * s.pitch;
                uint8_t* out = slice.ptr + (size_t)y * row;
                switch (s.bpp) {
                case 1:
                    std::memcpy(out, in, row);
                    break;
                case 2:
                    for (u32 x = 0; x < row; x += 2) {
                        uint16_t v;
                        std::memcpy(&v, in + x, 2);
                        v = __builtin_bswap16(v);
                        std::memcpy(out + x, &v, 2);
                    }
                    break;
                case 8:
                    for (u32 x = 0; x < row; x += 2) {
                        uint16_t v;
                        std::memcpy(&v, in + x, 2);
                        v = __builtin_bswap16(v);
                        std::memcpy(out + x, &v, 2);
                    }
                    break;
                default:
                    for (u32 x = 0; x < row; x += 4) {
                        uint32_t v;
                        std::memcpy(&v, in + x, 4);
                        v = __builtin_bswap32(v);
                        std::memcpy(out + x, &v, 4);
                    }
                    break;
                }
            }
            image native{};
            image* target = &s.img;
            if (s.scale > 1u) {
                if (!create_image(native, s.img.format, w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
                    return;
                target = &native;
            }
            barrier(c, *target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.bufferOffset = slice.offset;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyBufferToImage(c, slice.buffer, target->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            if (target == &native) {
                scale_blit(c, native, s.img, VK_FILTER_LINEAR);
                m_garbage.emplace_back(*m_submitted + 1u, native);
            }
            s.state = SURFACE_VALID;
            return;
        }
    }
    barrier(c, s.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange range{ s.img.aspect, 0, 1, 0, 1 };
    if (s.depth) {
        const VkClearDepthStencilValue v{ 1.0f, 0u };
        vkCmdClearDepthStencilImage(c, s.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    } else {
        const VkClearColorValue v{};
        vkCmdClearColorImage(c, s.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    }
    s.state = SURFACE_VALID;
}

void renderer::download_surface(surface& s)
{
    if (!s.gpu_dirty || s.depth) return;
    VkCommandBuffer c = cmd();
    end_rendering();
    const u32 w = s.img.width / s.scale, h = s.img.height / s.scale, row = w * s.bpp;
    vk_slice slice = m_ring.alloc((VkDeviceSize)row * h, 16);
    if (!slice.ptr) return;
    image native{};
    image* source = &s.img;
    if (s.scale > 1u) {
        if (!create_image(native, s.img.format, w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
            return;
        scale_blit(c, s.img, native, VK_FILTER_LINEAR);
        source = &native;
    }
    barrier(c, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = slice.offset;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { w, h, 1 };
    vkCmdCopyImageToBuffer(c, source->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slice.buffer, 1, &region);
    if (source == &native) m_garbage.emplace_back(*m_submitted + 1u, native);
    VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(c, &di);
    submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    const uint64_t value = *m_submitted;
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &value;
    vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
    uint8_t* dst = vm_base + s.ea;
    for (u32 y = 0; y < h; y++) {
        const uint8_t* in = slice.ptr + (size_t)y * row;
        uint8_t* out = dst + (size_t)y * s.pitch;
        switch (s.bpp) {
        case 1:
            std::memcpy(out, in, row);
            break;
        case 2: case 8:
            for (u32 x = 0; x < row; x += 2) {
                uint16_t v;
                std::memcpy(&v, in + x, 2);
                v = __builtin_bswap16(v);
                std::memcpy(out + x, &v, 2);
            }
            break;
        default:
            for (u32 x = 0; x < row; x += 4) {
                uint32_t v;
                std::memcpy(&v, in + x, 4);
                v = __builtin_bswap32(v);
                std::memcpy(out + x, &v, 4);
            }
            break;
        }
    }
    s.gpu_dirty = false;
}

void renderer::end_rendering()
{
    if (!m_rendering) return;
    VkCommandBuffer c = cmd();
    if (m_occlusion.active()) m_occlusion.end(c);
    vkCmdEndRendering(c);
    m_rendering = false;
}

bool renderer::bind_framebuffer(u32 context)
{
    framebuffer fb{};
    const u32 clip_h = r(rsx::M_SURFACE_CLIP_H), clip_v = r(rsx::M_SURFACE_CLIP_V);
    u32 width = clip_h >> 16, height = clip_v >> 16;
    if (!width || !height) {
        m_fb_valid = false;
        return false;
    }
    const u32 fmt = r(rsx::M_SURFACE_FORMAT);
    const u32 color_fmt = fmt & 0x1Fu, depth_fmt = (fmt >> 5) & 7u, raster = (fmt >> 8) & 0xFu, aa = (fmt >> 12) & 0xFu;
    const u32 aa_u = aa == 0 ? 1u : 2u;
    const u32 aa_v = (aa == 0 || aa == 3) ? 1u : 2u;
    const u32 depth_texel = (depth_fmt == 1 ? 2u : 4u) * aa_u;
    const u32 color_texel = color_bpp(color_fmt) * aa_u;
    const bool stencil_test = depth_fmt == 2 && r(rsx::M_STENCIL_TEST_ENABLE);
    const bool depth_test = r(rsx::M_DEPTH_TEST_ENABLE) != 0;
    const bool depth_write = r(rsx::M_DEPTH_MASK) != 0;

    u32 color_ea[4] = {}, color_pitch[4], idx[4];
    for (u32 i = 0; i < 4; i++) {
        color_ea[i] = rsx_address(r(k_color_dma[i]), r(k_color_offset[i]), 0);
        color_pitch[i] = r(k_color_pitch[i]);
    }
    u32 zeta_ea = rsx_address(r(rsx::M_DMA_ZETA), r(rsx::M_SURFACE_ZETA), 0);
    u32 zeta_pitch = r(rsx::M_SURFACE_PITCH_Z);
    const u32 target = r(rsx::M_SURFACE_TARGET);
    const u32 n = target_indexes(target, idx);

    bool zeta_write = depth_test && depth_write;
    if (!zeta_write && stencil_test) zeta_write = stencil_writes(*this, false) || (r(rsx::M_TWO_SIDED_STENCIL) && stencil_writes(*this, true));

    bool color_write_any = false;
    for (u32 i = 0; i < n; i++) {
        const bool on = i == 0 ? r(rsx::M_COLOR_MASK) != 0 : ((r(rsx::M_COLOR_MASK_MRT) >> (i * 4u)) & 0xFu) != 0;
        fb.color_write[idx[i]] = on;
        color_write_any |= on;
    }

    bool depth_unused = false, color_unused = false;
    if (context == FB_CLEAR_DEPTH) color_unused = true;
    else if (context == FB_CLEAR_COLOR) depth_unused = true;
    else if (context == FB_DRAW) {
        if (!stencil_test) {
            if (!depth_test) depth_unused = true;
            else if (!depth_write) {
                const u32 func = r(rsx::M_DEPTH_FUNC);
                if (func == 0x200u || func == 0x207u) depth_unused = true;
            }
            if (depth_unused && r(rsx::M_DEPTH_BOUNDS_ENABLE) &&
                (f(rsx::M_DEPTH_BOUNDS_MIN) > 0.f || f(rsx::M_DEPTH_BOUNDS_MAX) < 1.f))
                depth_unused = false;
        }
        color_unused = !color_write_any || n == 0;
    }

    const bool packed = raster == 2u;
    if (!packed) {
        const u32 scissor_h = r(rsx::M_SCISSOR_H);
        const u32 write_limit = std::min(width, (scissor_h & 0xFFFFu) + (scissor_h >> 16));
        const u32 min_color = color_texel * write_limit, min_zeta = depth_texel * write_limit;
        u32 corrected = 0xFFFFu;
        bool fix_zeta = false, fix_color[4] = {};
        if (!depth_unused) {
            if (zeta_pitch < min_zeta) {
                if (zeta_pitch > 64u) {
                    corrected = zeta_pitch / depth_texel;
                    fix_zeta = true;
                } else {
                    zeta_ea = 0;
                }
            } else if (width * depth_texel > zeta_pitch) {
                corrected = zeta_pitch / depth_texel;
            }
        }
        if (!color_unused) {
            for (u32 i = 0; i < n; i++) {
                const u32 k = idx[i];
                if (color_pitch[k] < min_color) {
                    if (color_pitch[k] > 64u) {
                        corrected = std::min(corrected, color_pitch[k] / color_texel);
                        fix_color[k] = true;
                    } else {
                        color_ea[k] = 0;
                    }
                    continue;
                }
                if (width * color_texel > color_pitch[k]) corrected = std::min(corrected, color_pitch[k] / color_texel);
            }
        }
        if (corrected != 0xFFFFu) {
            width = corrected;
            if (fix_zeta) zeta_pitch = depth_texel * width;
            for (u32 k = 0; k < 4; k++)
                if (fix_color[k]) color_pitch[k] = color_texel * width;
        }
    }
    if (depth_unused) zeta_ea = 0;
    else if (packed) zeta_pitch = width * depth_texel;

    bool any = false;
    bool use[4] = {};
    for (u32 i = 0; i < n; i++) {
        const u32 k = idx[i];
        if (color_unused || !color_ea[k]) continue;
        if (color_pitch[k] < 64u) continue;
        if (color_ea[k] == zeta_ea) {
            if (depth_test || stencil_test || (!fb.color_write[k] && zeta_write)) continue;
            zeta_ea = 0;
        }
        const u32 pitch = packed ? width * color_texel : color_pitch[k];
        use[k] = true;
        color_pitch[k] = pitch;
        any = true;
    }
    if (!any && !zeta_ea) {
        m_fb_valid = false;
        return false;
    }

    const u32 wox = r(rsx::M_WINDOW_OFFSET) & 0xFFFFu, woy = r(rsx::M_WINDOW_OFFSET) >> 16;
    fb.width = width;
    fb.height = height;
    fb.aa_u = aa_u;
    fb.aa_v = aa_v;
    fb.depth_write = zeta_write;
    for (u32 k = 0; k < 4; k++) {
        if (!use[k]) continue;
        const u32 ea = color_ea[k] + color_pitch[k] * woy + color_texel * wox;
        fb.color[k] = surface_for(ea, color_pitch[k], width, height, color_fmt, false, aa_u, aa_v);
        if (!fb.color[k]) continue;
        fb.color_format[k] = fb.color[k]->img.format;
        fb.scale = fb.color[k]->scale;
        fb.count = k + 1u;
    }
    if (zeta_ea) {
        const u32 ea = zeta_ea + zeta_pitch * woy + depth_texel * wox;
        fb.depth = surface_for(ea, zeta_pitch, width, height, depth_fmt ? depth_fmt : 2u, true, aa_u, aa_v);
        if (fb.depth) {
            fb.depth_format = fb.depth->img.format;
            fb.scale = fb.depth->scale;
        }
    }
    if (!(fb == m_fb) || !m_fb_valid) {
        end_rendering();
        m_fb = fb;
    }
    m_fb_valid = true;
    return true;
}

void renderer::begin_rendering()
{
    if (m_rendering) return;
    VkCommandBuffer c = cmd();
    VkRenderingAttachmentInfo color[4]{};
    for (u32 k = 0; k < m_fb.count; k++) {
        color[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        surface* s = m_fb.color[k];
        if (!s) continue;
        if (s->state == SURFACE_UNINITIALIZED) load_surface(*s);
        barrier(c, s->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        color[k].imageView = s->img.view;
        color[k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color[k].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo stencil{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    if (surface* s = m_fb.depth) {
        if (s->state == SURFACE_UNINITIALIZED) load_surface(*s);
        barrier(c, s->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        depth.imageView = s->img.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (s->img.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = depth;
    }
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { m_fb.width * m_fb.aa_u * m_fb.scale, m_fb.height * m_fb.aa_v * m_fb.scale } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = m_fb.count;
    ri.pColorAttachments = color;
    ri.pDepthAttachment = m_fb.depth ? &depth : nullptr;
    ri.pStencilAttachment = stencil.imageView ? &stencil : nullptr;
    vkCmdBeginRendering(c, &ri);
    m_rendering = true;
}

void renderer::on_clear(u32 mask)
{
    if (!(r(rsx::M_STENCIL_MASK) & 0xFFu)) mask &= ~CLEAR_S;
    if (!(mask & CLEAR_ANY)) return;
    u32 context = 0;
    if (mask & CLEAR_RGBA) context |= FB_CLEAR_COLOR;
    if (mask & (CLEAR_Z | CLEAR_S)) context |= FB_CLEAR_DEPTH;
    if (!bind_framebuffer(context == (FB_CLEAR_COLOR | FB_CLEAR_DEPTH) ? 3u : context)) return;

    const u32 sh = r(rsx::M_SCISSOR_H), sv = r(rsx::M_SCISSOR_V);
    const u32 x0 = std::min(sh & 0xFFFFu, m_fb.width), y0 = std::min(sv & 0xFFFFu, m_fb.height);
    const u32 x1 = std::min((sh & 0xFFFFu) + (sh >> 16), m_fb.width), y1 = std::min((sv & 0xFFFFu) + (sv >> 16), m_fb.height);
    if (x1 <= x0 || y1 <= y0) return;
    const bool full = x0 == 0 && y0 == 0 && x1 == m_fb.width && y1 == m_fb.height;

    const u32 fmt = r(rsx::M_SURFACE_FORMAT);
    const u32 color_fmt = fmt & 0x1Fu, depth_fmt = (fmt >> 5) & 7u;
    u32 color_mask = mask & CLEAR_RGBA;
    if (color_fmt == 11 || color_fmt == 12 || color_fmt == 13) color_mask = 0;
    const bool full_color = color_mask == CLEAR_RGBA;

    VkClearAttachment att[5]{};
    u32 count = 0;
    if (color_mask) {
        const u32 cv = r(rsx::M_COLOR_CLEAR_VALUE);
        VkClearValue value{};
        const float a = (float)(cv >> 24) / 255.f, rr = (float)((cv >> 16) & 0xFFu) / 255.f;
        const float g = (float)((cv >> 8) & 0xFFu) / 255.f, b = (float)(cv & 0xFFu) / 255.f;
        value.color.float32[0] = rr;
        value.color.float32[1] = g;
        value.color.float32[2] = b;
        value.color.float32[3] = a;
        for (u32 k = 0; k < m_fb.count; k++) {
            surface* s = m_fb.color[k];
            if (!s) continue;
            if (full && full_color) s->state = SURFACE_VALID;
            s->gpu_dirty = true;
            att[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att[count].colorAttachment = k;
            att[count].clearValue = value;
            count++;
        }
    }
    if ((mask & (CLEAR_Z | CLEAR_S)) && m_fb.depth) {
        surface* s = m_fb.depth;
        const u32 zv = r(rsx::M_ZSTENCIL_CLEAR_VALUE);
        VkClearAttachment& d = att[count];
        if (mask & CLEAR_Z) {
            d.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            d.clearValue.depthStencil.depth = depth_fmt == 1 ? (float)(zv & 0xFFFFu) / 65535.f : (float)(zv >> 8) / 16777215.f;
        }
        if ((mask & CLEAR_S) && depth_fmt == 2) {
            d.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            d.clearValue.depthStencil.stencil = zv & 0xFFu;
        }
        const bool whole = (d.aspectMask == s->img.aspect) && full;
        if (whole) s->state = SURFACE_VALID;
        s->gpu_dirty = true;
        if (d.aspectMask) count++;
    }
    if (!count) return;
    begin_rendering();
    const u32 hx = m_fb.aa_u * m_fb.scale, hy = m_fb.aa_v * m_fb.scale;
    const VkClearRect rect{ { { (int32_t)(x0 * hx), (int32_t)(y0 * hy) }, { (x1 - x0) * hx, (y1 - y0) * hy } }, 0, 1 };
    vkCmdClearAttachments(cmd(), count, att, 1, &rect);
    stats.clears++;
    m_draw_counter++;
}

void renderer::on_draw(const rsx::draw_clause& clause)
{
    const u32 id = m_trace_draws++;
    if (!bind_framebuffer(FB_DRAW)) {
        stats.skipped_fb++;
        if (m_trace)
            std::fprintf(stderr, "[trace] %u dropped: no render target (clip %ux%u format %08X target %X color %08X zeta %08X)\n", id,
                         r(rsx::M_SURFACE_CLIP_H) >> 16, r(rsx::M_SURFACE_CLIP_V) >> 16, r(rsx::M_SURFACE_FORMAT),
                         r(rsx::M_SURFACE_TARGET), r(rsx::M_SURFACE_COLOR_A), r(rsx::M_SURFACE_ZETA));
        return;
    }
    if (!draw_setup()) {
        stats.skipped_shader++;
        if (m_trace) std::fprintf(stderr, "[trace] %u skipped: %s program compiling or failed\n", id, m_vp ? "fragment" : "vertex");
        return;
    }
    draw_execute(clause, id);
}

void renderer::trace_draw(u32 id, u32 prim, u32 count, bool indexed) const
{
    const u32 ch = r(rsx::M_SURFACE_CLIP_H), cv = r(rsx::M_SURFACE_CLIP_V), sh = r(rsx::M_SCISSOR_H), sv = r(rsx::M_SCISSOR_V);
    const u32 wo = r(rsx::M_WINDOW_OFFSET);
    std::fprintf(stderr,
                 "[trace] %u prim %u %s %u | color %08X fmt %d %ux%u aa %ux%u depth %08X fmt %d | clip %u,%u %ux%u win %u,%u scissor %u,%u %ux%u"
                 " | vp scale %.2f %.2f %.4f offset %.2f %.2f %.4f | z test %u write %u func %X stencil %u | cmask %08X blend %u atest %u func %X ref %X"
                 " | cull %u face %X | vp %016llX fp %016llX\n",
                 id, prim, indexed ? "idx" : "arr", count, m_fb.color[0] ? m_fb.color[0]->ea : 0u, (int)m_fb.color_format[0], m_fb.width,
                 m_fb.height, m_fb.aa_u, m_fb.aa_v, m_fb.depth ? m_fb.depth->ea : 0u, (int)m_fb.depth_format, ch & 0xFFFFu, cv & 0xFFFFu, ch >> 16,
                 cv >> 16, wo & 0xFFFFu, wo >> 16, sh & 0xFFFFu, sv & 0xFFFFu, sh >> 16, sv >> 16, f(rsx::M_VIEWPORT_SCALE),
                 f(rsx::M_VIEWPORT_SCALE + 4u), f(rsx::M_VIEWPORT_SCALE + 8u), f(rsx::M_VIEWPORT_OFFSET), f(rsx::M_VIEWPORT_OFFSET + 4u),
                 f(rsx::M_VIEWPORT_OFFSET + 8u), r(rsx::M_DEPTH_TEST_ENABLE), r(rsx::M_DEPTH_MASK), r(rsx::M_DEPTH_FUNC),
                 r(rsx::M_STENCIL_TEST_ENABLE), r(rsx::M_COLOR_MASK), r(rsx::M_BLEND_ENABLE), r(rsx::M_ALPHA_TEST_ENABLE), r(rsx::M_ALPHA_FUNC),
                 r(rsx::M_ALPHA_REF), r(rsx::M_CULL_FACE_ENABLE), r(rsx::M_CULL_FACE), (unsigned long long)(m_vp ? m_vp->hash : 0),
                 (unsigned long long)(m_fp ? m_fp->hash : 0));
    for (u32 k = 0; k < rsx::ATTRIBS; k++) {
        const u32 fmt = r(rsx::M_VERTEX_ARRAY_FORMAT + k * 4u), off = r(rsx::M_VERTEX_ARRAY_OFFSET + k * 4u);
        if (!((fmt >> 4) & 0xFu)) continue;
        const u32 ea = rsx_address((off >> 31) ? RSX_DMA_MAIN : RSX_DMA_LOCAL, off & 0x7FFFFFFFu, 16);
        u32 w[2] = {};
        if (ea) std::memcpy(w, vm_base + ea, 8);
        std::fprintf(stderr, "[trace] %u   attr%u type %u size %u stride %u offset %08X first %08X %08X\n", id, k, fmt & 0xFu, (fmt >> 4) & 0xFu,
                     (fmt >> 8) & 0xFFu, off, __builtin_bswap32(w[0]), __builtin_bswap32(w[1]));
    }
    if (m_fp) {
        const u32* ucode = reinterpret_cast<const u32*>(vm_base + m_fp_ea);
        for (u32 i = 0; i < m_fp->info.constant_count; i++) {
            float c[4];
            for (u32 w = 0; w < 4; w++) {
                u32 v = __builtin_bswap32(ucode[m_fp->info.constant_word[i] + w]);
                v = (v << 16) | (v >> 16);
                std::memcpy(&c[w], &v, 4);
            }
            std::fprintf(stderr, "[trace] %u   fpconst%u %.4f %.4f %.4f %.4f\n", id, i, c[0], c[1], c[2], c[3]);
        }
    }
    const u32 used = m_fp ? m_fp->info.texture_mask : 0u;
    for (u32 u = 0; u < rsx::TEXTURES; u++) {
        if (!((used >> u) & 1u) || !m_units[u].valid) continue;
        const unit_binding& b = m_units[u];
        std::fprintf(stderr, "[trace] %u   tex%u %s | %08X gcm %02X %ux%u lv %u vk %d %s | filter %08X address %08X ctrl0 %08X remap %08X ctrl3 %08X\n", id, u,
                     m_texture_path[u] ? m_texture_path[u] : "?", b.desc.offset, b.desc.gcm_format, b.desc.width, b.desc.height, b.desc.levels, (int)b.desc.format,
                     b.desc.linear ? "lin" : "swz", b.regs.filter, b.regs.address, b.regs.control0, b.regs.control1, b.regs.control3);
    }
}

void renderer::on_report(u32 method, u32 value)
{
    switch (method) {
    case rsx::M_ZPASS_PIXEL_COUNT_ENABLE:
        if (!value && m_occlusion.active()) m_occlusion.end(cmd());
        m_occlusion.enable(value != 0);
        break;
    case rsx::M_CLEAR_REPORT_VALUE:
        if (value != 1u) break;
        if (m_occlusion.active()) m_occlusion.end(cmd());
        m_occlusion.clear();
        break;
    default:
        break;
    }
}

bool renderer::report(u32 type, u32 ea)
{
    if (type != 1u) return false;
    if (m_occlusion.active()) m_occlusion.end(cmd());
    m_occlusion.report(ea);
    if (!m_recording) m_occlusion.submitted(*m_submitted);
    return true;
}

void renderer::present_source(u32 ea, u32 width, u32 height, u32 pitch, image*& out, u32& scale, VkCommandBuffer& c)
{
    c = cmd();
    end_rendering();
    out = nullptr;
    scale = 1;
    for (auto& p : m_surfaces) {
        surface* s = p.get();
        if (s->depth || s->ea != ea || s->state != SURFACE_VALID) continue;
        if (s->bpp != 4 || s->pitch != pitch) continue;
        out = &s->img;
        scale = s->scale;
        stats.present_surface++;
        if (m_trace) std::fprintf(stderr, "[trace] scanout surface %08X %ux%u image %ux%u\n", ea, width, height, s->img.width, s->img.height);
        return;
    }
    stats.present_memory++;
    if (m_trace) std::fprintf(stderr, "[trace] scanout memory %08X %ux%u pitch %u\n", ea, width, height, pitch);
    if (m_display.width != width || m_display.height != height) {
        if (m_display.handle) m_garbage.emplace_back(*m_submitted + 1u, m_display);
        m_display = image{};
        if (!create_image(m_display, VK_FORMAT_B8G8R8A8_UNORM, width, height,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT))
            return;
    }
    vk_slice slice = m_ring.alloc((VkDeviceSize)width * height * 4u, 16);
    if (!slice.ptr) return;
    const uint8_t* src = vm_base + ea;
    for (u32 y = 0; y < height; y++) {
        const uint32_t* in = reinterpret_cast<const uint32_t*>(src + (size_t)y * pitch);
        uint32_t* o = reinterpret_cast<uint32_t*>(slice.ptr + (size_t)y * width * 4u);
        for (u32 x = 0; x < width; x++) o[x] = __builtin_bswap32(in[x]);
    }
    barrier(c, m_display, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = slice.offset;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(c, slice.buffer, m_display.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    out = &m_display;
}

void renderer::read_back(u32 ea, u32 size)
{
    if (m_occlusion.outstanding(ea, size)) {
        if (m_recording) submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        else if (m_occlusion.unsubmitted()) m_occlusion.submitted(*m_submitted);
        m_occlusion.drain();
    }
    for (auto& p : m_surfaces)
        if (p->gpu_dirty && ranges_overlap(p->ea, p->bytes(), ea, size)) download_surface(*p);
}

void renderer::wrote(u32 ea, u32 size)
{
    mark_written(ea, size);
    for (auto& p : m_surfaces) {
        if (!ranges_overlap(p->ea, p->bytes(), ea, size)) continue;
        if (p.get() == m_fb.depth) continue;
        bool bound = false;
        for (surface* b : m_fb.color) bound |= b == p.get();
        if (bound) continue;
        p->state = SURFACE_UNINITIALIZED;
        p->gpu_dirty = false;
    }
}

void renderer::flip_reset()
{
    if (m_trace) std::fprintf(stderr, "[trace] frame end, %u draws\n", m_trace_draws);
    m_trace = trace_request.exchange(false, std::memory_order_relaxed);
    m_trace_draws = 0;
    if (m_trace) std::fprintf(stderr, "[trace] frame begin\n");
    m_epoch++;
    end_rendering();
    frontend::reset();
    m_fb_valid = false;
    if ((++m_flips & 255u) == 0)
        for (auto it = m_textures.begin(); it != m_textures.end();) {
            if (m_flips - it->second->used <= 1800u) {
                ++it;
                continue;
            }
            m_garbage.emplace_back(*m_submitted + 1u, it->second->img);
            it = m_textures.erase(it);
        }
}

}
