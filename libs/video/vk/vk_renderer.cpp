#include "vk_renderer.h"
#include "../../../runtime/memory/vm_track.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <functional>

namespace vk {

namespace {

enum fb_context : u32 { FB_DRAW = 0, FB_CLEAR_COLOR = 1, FB_CLEAR_DEPTH = 2 };

enum : u32 {
    CLEAR_Z = 0x01u, CLEAR_S = 0x02u,
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

bool scaled_edge(float x, const ratio& s, int32_t& out)
{
    if (x >= 0.f && x == std::floor(x)) {
        out = (int32_t)s.edge((u32)x);
        return true;
    }
    const float v = x * s.value();
    out = (int32_t)std::lround(v);
    return std::fabs(v - (float)out) <= 1e-3f;
}

VkImageUsageFlags surface_usage(bool depth)
{
    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
           (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
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
    m_shaders.init(d, m_pipelines);
    if (!m_ring.create(d, 256ull << 20, 256ull << 20, timeline) || !m_pipelines.create(d, cache_path) || !create_buffer_set() || !create_dummies() ||
        !m_occlusion.create(d, timeline) || !m_depth_pack.create(d) || !m_downscale.create(d))
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
    if (m_trace_pool) vkDestroyQueryPool(m_d->dev, m_trace_pool, nullptr);
    m_trace_pool = VK_NULL_HANDLE;
    m_occlusion.destroy();
    m_depth_pack.destroy();
    m_downscale.destroy();
    for (auto& g : m_scratch_garbage) vmaDestroyBuffer(m_d->vma, g.second.buffer, g.second.mem);
    m_scratch_garbage.clear();
    destroy_image(m_box);
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
    if (m_buffer_pool) vkDestroyDescriptorPool(m_d->dev, std::exchange(m_buffer_pool, VK_NULL_HANDLE), nullptr);
    m_pipelines.destroy();
    m_shaders.destroy();
    m_ring.destroy();
    for (submission& s : m_submissions)
        if (s.pool) vkDestroyCommandPool(m_d->dev, s.pool, nullptr);
    m_d = nullptr;
}

constexpr VkAccessFlags2 k_writes = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                    VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
constexpr u32 k_trace_queries = 8192;

bool renderer::readable(const image& img)
{
    return img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && !(img.access & k_writes);
}

void renderer::trace_begin(VkCommandBuffer c)
{
    if (!m_trace_timed || m_trace_open || !m_trace_pool || (m_trace_spans.size() + 1u) * 2u > k_trace_queries) return;
    vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_trace_pool, (u32)m_trace_spans.size() * 2u);
    m_trace_open = true;
}

void renderer::trace_end(VkCommandBuffer c, const char* fmt, ...)
{
    if (!m_trace_open) return;
    vkCmdWriteTimestamp2(c, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_trace_pool, (u32)m_trace_spans.size() * 2u + 1u);
    char label[192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(label, sizeof label, fmt, args);
    va_end(args);
    m_trace_spans.emplace_back(label);
    m_trace_open = false;
}

trace_span::trace_span(renderer& r, VkCommandBuffer c, const char* fmt, ...)
{
    if (!r.tracing()) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(m_label, sizeof m_label, fmt, args);
    va_end(args);
    m_r = &r;
    m_c = c;
    r.trace_begin(c);
}

trace_span::~trace_span()
{
    if (m_r) m_r->trace_end(m_c, "%s", m_label);
}

void renderer::trace_report()
{
    const u32 n = (u32)m_trace_spans.size();
    if (!n) return;
    flush();
    std::vector<uint64_t> t((size_t)n * 2u);
    if (vkGetQueryPoolResults(m_d->dev, m_trace_pool, 0, n * 2u, t.size() * sizeof(uint64_t), t.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return;
    const double us = (double)m_d->props.limits.timestampPeriod / 1000.0;
    std::vector<std::pair<double, u32>> busy(n);
    uint64_t done = t[0];
    double total = 0;
    for (u32 i = 0; i < n; i++) {
        const uint64_t start = std::max(t[i * 2u], done), end = std::max(t[i * 2u + 1u], start);
        busy[i] = { (double)(end - start) * us, i };
        total += busy[i].first;
        done = end;
        std::fprintf(m_trace, "[gpu] %u %9.1f us at %9.1f us  %s\n", i, busy[i].first, (double)(start - t[0]) * us, m_trace_spans[i].c_str());
    }
    std::fprintf(m_trace, "[gpu] busy %.1f us over %.1f us, %u spans\n", total, (double)(done - t[0]) * us, n);
    std::sort(busy.begin(), busy.end(), std::greater<>());
    for (u32 i = 0; i < n && i < 20u; i++)
        std::fprintf(m_trace, "[gpu] top %2u %9.1f us %5.1f%%  %u %s\n", i + 1u, busy[i].first, total > 0 ? 100.0 * busy[i].first / total : 0.0,
                     busy[i].second, m_trace_spans[busy[i].second].c_str());
}

void renderer::barrier(VkCommandBuffer cmd, image& img, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
{
    constexpr VkAccessFlags2 writes = k_writes;
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

void renderer::memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2 dst_access)
{
    VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = src_stage;
    mb.srcAccessMask = src_access;
    mb.dstStageMask = dst_stage;
    mb.dstAccessMask = dst_access;
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

VkCommandBuffer renderer::cmd()
{
    submission& s = m_submissions[m_submission];
    if (m_recording) return s.cmd;
    if (s.done > m_reached) vkGetSemaphoreCounterValue(m_d->dev, m_timeline, &m_reached);
    if (s.done > m_reached) {
        VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &m_timeline;
        wi.pValues = &s.done;
        vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
        m_reached = s.done;
    }
    vkResetCommandPool(m_d->dev, s.pool, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    m_dynamic.valid = false;
    m_bound.valid = false;
    m_bound.pipeline = VK_NULL_HANDLE;
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

    vkGetSemaphoreCounterValue(m_d->dev, m_timeline, &m_reached);
    release_held();
    auto keep = std::remove_if(m_garbage.begin(), m_garbage.end(), [&](std::pair<uint64_t, image>& g) {
        if (g.first > m_reached) return false;
        destroy_image(g.second);
        return true;
    });
    m_garbage.erase(keep, m_garbage.end());
    auto kept = std::remove_if(m_scratch_garbage.begin(), m_scratch_garbage.end(), [&](std::pair<uint64_t, scratch>& g) {
        if (g.first > m_reached) return false;
        vmaDestroyBuffer(m_d->vma, g.second.buffer, g.second.mem);
        return true;
    });
    m_scratch_garbage.erase(kept, m_scratch_garbage.end());
}

void renderer::flush()
{
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
    img.view = VK_NULL_HANDLE;
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img.handle;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { aspect, 0, 1, 0, 1 };
        VK_TRY(vkCreateImageView(m_d->dev, &vi, nullptr, &img.view));
    }
    img.format = format;
    img.aspect = aspect;
    img.usage = usage;
    img.width = width;
    img.height = height;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.stage = VK_PIPELINE_STAGE_2_NONE;
    img.access = 0;
    return true;
}

bool renderer::create_scratch(scratch& s, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_TRY(vmaCreateBuffer(m_d->vma, &bi, &ai, &s.buffer, &s.mem, nullptr));
    s.size = size;
    return true;
}

void renderer::destroy_image(image& img)
{
    for (auto& v : img.views) vkDestroyImageView(m_d->dev, v.second, nullptr);
    if (img.view) vkDestroyImageView(m_d->dev, img.view, nullptr);
    if (img.handle) vmaDestroyImage(m_d->vma, img.handle, img.mem);
    img = image{};
}

void renderer::discard(image& img)
{
    if (img.handle) m_garbage.emplace_back(*m_submitted + 1u, img);
    img = image{};
}

void renderer::retire(surface* s)
{
    discard(s->img);
    discard(s->snapshot);
    discard(s->as_color);
    discard(s->native);
    auto it = std::find_if(m_surfaces.begin(), m_surfaces.end(), [s](const std::unique_ptr<surface>& p) { return p.get() == s; });
    if (it != m_surfaces.end()) m_surfaces.erase(it);
    m_binding_gen++;
}

ratio renderer::scale_for(u32 width, u32 height) const
{
    rsx_display_buffer db{};
    return rsx_core_display_buffer(0, &db) && width >= db.width && height >= db.height ? resolution_scale : ratio{};
}

surface* renderer::surface_for(u32 ea, u32 pitch, u32 width, u32 height, u32 format, bool depth, u32 aa_u, u32 aa_v)
{
    const ratio scale = scale_for(width, height);
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
    if (depth) {
        vf = format == 1 ? (depth_float ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM) : VK_FORMAT_D32_SFLOAT_S8_UINT;
        aspect = format == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    } else {
        vf = color_format(format);
        aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (!create_image(s->img, vf, scale.edge(s->width * aa_u), scale.edge(s->height * aa_v), surface_usage(depth), aspect)) return nullptr;

    VkCommandBuffer c = cmd();
    if (grow_from) {
        end_rendering();
        const trace_span span(*this, c, "surface grow %ux%u", s->img.width, s->img.height);
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
    m_binding_gen++;
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

bool renderer::scale_depth(VkCommandBuffer c, image& src, image& dst)
{
    constexpr VkPipelineStageFlags2 xfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (const VkImageAspectFlags aspect : { VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT }) {
        if (!(src.aspect & aspect)) continue;
        const bool d16 = src.format == VK_FORMAT_D16_UNORM, stencil = aspect == VK_IMAGE_ASPECT_STENCIL_BIT;
        const VkFormat format = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT;
        const VkDeviceSize texel = stencil ? 1u : d16 ? 2u : 4u;
        image from{}, to{};
        scratch buffer{};
        const bool made = create_scratch(buffer, texel * std::max(src.width * src.height, dst.width * dst.height),
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) &&
                          create_image(from, format, src.width, src.height, usage, VK_IMAGE_ASPECT_COLOR_BIT) &&
                          create_image(to, format, dst.width, dst.height, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        if (made) {
            VkBufferImageCopy region{};
            region.imageSubresource = { aspect, 0, 0, 1 };
            region.imageExtent = { src.width, src.height, 1 };
            barrier(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdCopyImageToBuffer(c, src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
            memory_barrier(c, xfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, xfer, VK_ACCESS_2_TRANSFER_READ_BIT);
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier(c, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(c, buffer.buffer, from.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            scale_blit(c, from, to, VK_FILTER_NEAREST);
            memory_barrier(c, xfer, 0, xfer, 0);
            region.imageExtent = { dst.width, dst.height, 1 };
            barrier(c, to, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            vkCmdCopyImageToBuffer(c, to.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
            memory_barrier(c, xfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, xfer, VK_ACCESS_2_TRANSFER_READ_BIT);
            region.imageSubresource.aspectMask = aspect;
            barrier(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(c, buffer.buffer, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        discard(from);
        discard(to);
        if (buffer.buffer) m_scratch_garbage.emplace_back(*m_submitted + 1u, buffer);
        if (!made) return false;
    }
    return true;
}

void renderer::load_surface(surface& s)
{
    stats.surface_loads++;
    VkCommandBuffer c = cmd();
    end_rendering();
    const trace_span span(*this, c, "surface load %ux%u %s", s.img.width, s.img.height, s.depth ? "depth" : "color");
    const u32 w = s.width * s.aa_u, h = s.height * s.aa_v;
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
            if (!s.scale.unit()) {
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
                discard(native);
            }
            s.state = SURFACE_VALID;
    s.version = ++m_content_clock;
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
    s.version = ++m_content_clock;
}

void renderer::download_surface(surface& s)
{
    if (!s.gpu_dirty || s.depth) return;
    VkCommandBuffer c = cmd();
    end_rendering();
    const u32 w = s.width * s.aa_u, h = s.height * s.aa_v, row = w * s.bpp;
    vk_slice slice = m_ring.alloc((VkDeviceSize)row * h, 16);
    if (!slice.ptr) return;
    image* source = s.scale.unit() ? &s.img : native_image(s, s.img);
    if (!source) return;
    {
        const trace_span span(*this, c, "surface download %ux%u", w, h);
        barrier(c, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = slice.offset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(c, source->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slice.buffer, 1, &region);
        memory_barrier(c, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
    stats.downloads++;
    submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    const uint64_t value = *m_submitted;
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_timeline;
    wi.pValues = &value;
    vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
    await_writes(s.ea, s.pitch * h);
    uint8_t* dst = vm_base + s.ea;
    mark_written(s.ea, s.pitch * h);
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
    vm_track_write(s.ea, s.pitch * h);
    s.gpu_dirty = false;
}

void renderer::end_rendering()
{
    if (!m_rendering) return;
    VkCommandBuffer c = cmd();
    if (m_occlusion.active()) m_occlusion.end(c);
    vkCmdEndRendering(c);
    m_rendering = false;
    if (m_trace_open) {
        const ratio& sc = m_fb.scale;
        trace_end(c, "pass %ux%u color %u fmt %d depth fmt %d draws %u-%u", sc.edge(m_fb.width * m_fb.aa_u), sc.edge(m_fb.height * m_fb.aa_v),
                  m_fb.count, (int)m_fb.color_format[0], (int)m_fb.depth_format, m_trace_pass_first, m_trace_draws);
    }
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
    ri.renderArea = { { 0, 0 }, { m_fb.scale.edge(m_fb.width * m_fb.aa_u), m_fb.scale.edge(m_fb.height * m_fb.aa_v) } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = m_fb.count;
    ri.pColorAttachments = color;
    ri.pDepthAttachment = m_fb.depth ? &depth : nullptr;
    ri.pStencilAttachment = stencil.imageView ? &stencil : nullptr;
    trace_begin(c);
    m_trace_pass_first = m_trace_draws;
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
        s->version = ++m_content_clock;
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
        s->version = ++m_content_clock;
        if (d.aspectMask) count++;
    }
    if (!count) return;
    begin_rendering();
    const ratio& sc = m_fb.scale;
    const u32 rx = sc.edge(x0 * m_fb.aa_u), ry = sc.edge(y0 * m_fb.aa_v);
    const VkClearRect rect{ { { (int32_t)rx, (int32_t)ry }, { sc.edge(x1 * m_fb.aa_u) - rx, sc.edge(y1 * m_fb.aa_v) - ry } }, 0, 1 };
    vkCmdClearAttachments(cmd(), count, att, 1, &rect);
    stats.clears++;
    m_draw_counter++;
}

void renderer::on_draw(const rsx::draw_clause& clause)
{
    const u32 id = m_trace_draws++;
    if (!bind_framebuffer(FB_DRAW)) {
        if (m_trace)
            std::fprintf(m_trace, "[trace] %u dropped: no render target (clip %ux%u format %08X target %X color %08X zeta %08X)\n", id,
                         r(rsx::M_SURFACE_CLIP_H) >> 16, r(rsx::M_SURFACE_CLIP_V) >> 16, r(rsx::M_SURFACE_FORMAT),
                         r(rsx::M_SURFACE_TARGET), r(rsx::M_SURFACE_COLOR_A), r(rsx::M_SURFACE_ZETA));
        return;
    }
    if (!draw_setup()) {
        if (m_trace) std::fprintf(m_trace, "[trace] %u skipped: %s program compiling or failed\n", id, m_vp ? "fragment" : "vertex");
        return;
    }
    draw_execute(clause, id);
}

void renderer::trace_draw(u32 id, u32 prim, u32 count, bool indexed) const
{
    const u32 ch = r(rsx::M_SURFACE_CLIP_H), cv = r(rsx::M_SURFACE_CLIP_V), sh = r(rsx::M_SCISSOR_H), sv = r(rsx::M_SCISSOR_V);
    const u32 wo = r(rsx::M_WINDOW_OFFSET);
    std::fprintf(m_trace,
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
        std::fprintf(m_trace, "[trace] %u   attr%u type %u size %u stride %u offset %08X first %08X %08X\n", id, k, fmt & 0xFu, (fmt >> 4) & 0xFu,
                     (fmt >> 8) & 0xFFu, off, __builtin_bswap32(w[0]), __builtin_bswap32(w[1]));
    }
    if (m_vp)
        for (u32 w = 0; w < std::size(m_vp->info.constants_used); w++)
            for (u32 bits = m_vp->info.constants_used[w]; bits; bits &= bits - 1u) {
                const u32 i = w * 32u + (u32)__builtin_ctz(bits);
                float c[4];
                std::memcpy(c, constants + (size_t)i * 4u, sizeof c);
                std::fprintf(m_trace, "[trace] %u   vpconst%u %.4f %.4f %.4f %.4f\n", id, i, c[0], c[1], c[2], c[3]);
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
            std::fprintf(m_trace, "[trace] %u   fpconst%u %.4f %.4f %.4f %.4f\n", id, i, c[0], c[1], c[2], c[3]);
        }
    }
    const u32 used = m_fp ? m_fp->info.texture_mask : 0u;
    for (u32 u = 0; u < rsx::TEXTURES; u++) {
        if (!((used >> u) & 1u) || !m_units[u].valid) continue;
        const unit_binding& b = m_units[u];
        std::fprintf(m_trace, "[trace] %u   tex%u %s | %08X gcm %02X %ux%u lv %u vk %d %s | filter %08X address %08X ctrl0 %08X remap %08X ctrl3 %08X\n", id, u,
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
        queued();
        break;
    default:
        break;
    }
}

bool renderer::report(u32 type, u32 ea)
{
    const bool counted = type == 1u;
    if (counted && m_occlusion.active()) m_occlusion.end(cmd());
    if (m_occlusion.report(ea, counted) && !hle_window(ea, 16u)) flush();
    queued();
    return true;
}

bool renderer::defer(const rsx_write& w)
{
    if (!m_occlusion.ordered(w)) return false;
    download(w.src, w.src_size);
    m_occlusion.defer(w);
    if (!hle_window(w.ea, w.size) || !hle_window(w.src, w.src_size)) flush();
    queued();
    if (w.barrier) stats.deferred_labels++;
    else stats.deferred_transfers++;
    return true;
}

void renderer::queued()
{
    if (!m_recording) m_occlusion.submitted(*m_submitted);
}

surface* renderer::scanout_surface(u32 ea, u32 pitch) const
{
    for (const auto& p : m_surfaces)
        if (!p->depth && p->ea == ea && p->state == SURFACE_VALID && p->bpp == 4 && p->pitch == pitch) return p.get();
    return nullptr;
}

u64 renderer::content(u32 ea, u32 pitch) const
{
    const surface* s = scanout_surface(ea, pitch);
    return s ? s->version : 0;
}

void renderer::present_source(u32 ea, u32 width, u32 height, u32 pitch, image*& out, ratio& scale, VkCommandBuffer& c)
{
    c = cmd();
    end_rendering();
    out = nullptr;
    scale = {};
    if (surface* s = scanout_surface(ea, pitch)) {
        out = &s->img;
        scale = s->scale;
        if (m_trace) std::fprintf(m_trace, "[trace] scanout surface %08X %ux%u image %ux%u\n", ea, width, height, s->img.width, s->img.height);
        return;
    }
    if (m_trace) std::fprintf(m_trace, "[trace] scanout memory %08X %ux%u pitch %u\n", ea, width, height, pitch);
    if (m_display.width != width || m_display.height != height) {
        discard(m_display);
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
    const trace_span span(*this, c, "scanout upload %ux%u", width, height);
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
    settle(ea, size);
    download(ea, size);
}

void renderer::download(u32 ea, u32 size)
{
    const auto start = std::chrono::steady_clock::now();
    bool stalled = false;
    for (auto& p : m_surfaces)
        if (p->gpu_dirty && ranges_overlap(p->ea, p->bytes(), ea, size)) {
            download_surface(*p);
            stats.surface_stalls++;
            stats.stall_bytes += p->bytes();
            stalled = true;
        }
    if (stalled) stall(start, ea, size);
}

void renderer::settle(u32 ea, u32 size)
{
    if (!m_occlusion.pending(ea, size)) return;
    flush();
    queued();
    await_writes(ea, size);
}

void renderer::await_writes(u32 ea, u32 size)
{
    if (!m_occlusion.pending(ea, size)) return;
    const auto start = std::chrono::steady_clock::now();
    m_occlusion.settle(ea, size);
    stats.write_stalls++;
    stall(start, ea, size);
}

void renderer::drain()
{
    if (m_occlusion.idle()) return;
    flush();
    queued();
    const auto start = std::chrono::steady_clock::now();
    m_occlusion.drain();
    stats.write_stalls++;
    stall(start, 0, 0);
}

void renderer::stall(std::chrono::steady_clock::time_point start, u32 ea, u32 size)
{
    stats.stall_ns += (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    stats.stall_ea = ea;
    stats.stall_size = size;
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

surface* renderer::surface_at(u32 ea, u32 pitch, u32 bpp, u32& x, u32& y)
{
    for (auto& p : m_surfaces) {
        surface* s = p.get();
        if (s->depth || s->pitch != pitch || s->bpp != bpp || s->aa_u != 1u || s->aa_v != 1u || ea < s->ea || ea - s->ea >= s->bytes()) continue;
        const u32 rel = ea - s->ea;
        if ((rel % pitch) % bpp) continue;
        x = (rel % pitch) / bpp;
        y = rel / pitch;
        return s;
    }
    return nullptr;
}

bool renderer::blit(const rsx_blit& b)
{
    u32 sx = 0, sy = 0;
    surface* src = surface_at(b.src_ea, b.src_pitch, b.bpp, sx, sy);
    if (!src || src->state != SURFACE_VALID || (float)sx + b.src_x1 > (float)src->width || (float)sy + b.src_y1 > (float)src->height)
        return false;
    int32_t so[4];
    const float sf[4] = { (float)sx + b.src_x0, (float)sy + b.src_y0, (float)sx + b.src_x1, (float)sy + b.src_y1 };
    for (int i = 0; i < 4; i++)
        if (!scaled_edge(sf[i], src->scale, so[i])) return false;
    const u32 dst_bytes = b.dst_pitch * (b.dst_y + b.dst_h);
    if (ranges_overlap(src->ea, src->bytes(), b.dst_ea, dst_bytes)) return false;
    u32 dx = 0, dy = 0;
    surface* dst = surface_at(b.dst_ea, b.dst_pitch, b.bpp, dx, dy);
    if (dst && (dx + b.dst_x + b.dst_w > dst->width || dy + b.dst_y + b.dst_h > dst->height || dst->img.format != src->img.format)) return false;
    if (!dst) {
        dst = surface_for(b.dst_ea, b.dst_pitch, b.dst_x + b.dst_w, b.dst_y + b.dst_h, src->gcm_format, false, 1, 1);
        if (!dst || dst->img.format != src->img.format) return false;
    }
    dx += b.dst_x;
    dy += b.dst_y;
    if (dst->state != SURFACE_VALID) {
        if (dx || dy || b.dst_w != dst->width || b.dst_h != dst->height) load_surface(*dst);
        dst->state = SURFACE_VALID;
    }
    VkCommandBuffer c = cmd();
    end_rendering();
    const trace_span span(*this, c, "blit %ux%u to %ux%u", b.src_x1 > b.src_x0 ? (u32)(b.src_x1 - b.src_x0) : 0u,
                          b.src_y1 > b.src_y0 ? (u32)(b.src_y1 - b.src_y0) : 0u, b.dst_w, b.dst_h);
    barrier(c, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(c, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const ratio& ds = dst->scale;
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[0] = { so[0], so[1], 0 };
    region.srcOffsets[1] = { so[2], so[3], 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[0] = { (int32_t)ds.edge(dx), (int32_t)ds.edge(dy), 0 };
    region.dstOffsets[1] = { (int32_t)ds.edge(dx + b.dst_w), (int32_t)ds.edge(dy + b.dst_h), 1 };
    vkCmdBlitImage(c, src->img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   b.bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    dst->gpu_dirty = true;
    dst->version = ++m_content_clock;
    stats.blits++;
    return true;
}

void renderer::flip_reset()
{
    end_rendering();
    if (m_trace) {
        std::fprintf(m_trace, "[trace] frame end, %u draws\n", m_trace_draws);
        if (m_trace_timed) trace_report();
        m_trace_timed = false;
        m_trace_spans.clear();
        m_trace_open = false;
        if (trace_frames.fetch_sub(1, std::memory_order_relaxed) <= 1) {
            trace_frames.store(0, std::memory_order_relaxed);
            std::fclose(m_trace);
            m_trace = nullptr;
        } else {
            std::fprintf(m_trace, "[trace] frame %llu\n", (unsigned long long)m_flips);
        }
    }
    m_trace_draws = 0;
    if (!m_trace && trace_frames.load(std::memory_order_relaxed)) {
        char name[48];
        std::snprintf(name, sizeof name, "trace_%llu.txt", (unsigned long long)m_flips);
        m_trace = std::fopen(name, "w");
        if (m_trace) {
            std::setvbuf(m_trace, nullptr, _IOFBF, 1u << 28);
            std::fprintf(m_trace, "[trace] frame %llu\n", (unsigned long long)m_flips);
            std::fprintf(stderr, "[vk] F12 trace -> %s\n", name);
            m_trace_timed = true;
        }
        if (m_trace && !m_trace_pool) {
            VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = k_trace_queries;
            if (vkCreateQueryPool(m_d->dev, &qi, nullptr, &m_trace_pool) != VK_SUCCESS) m_trace_pool = VK_NULL_HANDLE;
        }
        if (m_trace_pool) vkResetQueryPool(m_d->dev, m_trace_pool, 0, k_trace_queries);
    }
    frontend::reset();
    m_fb_valid = false;
    if ((++m_flips & 255u) == 0)
        for (auto it = m_textures.begin(); it != m_textures.end();) {
            if (m_flips - it->second->used <= 1800u) {
                ++it;
                continue;
            }
            discard(it->second->img);
            it = m_textures.erase(it);
            m_binding_gen++;
        }
    if ((m_flips & 63u) == 0) evict(64);
}

void renderer::rescale(ratio scale)
{
    resolution_scale = scale;
    m_binding_gen++;
    VkCommandBuffer c = cmd();
    end_rendering();
    for (auto& p : m_surfaces) {
        surface& s = *p;
        const ratio to = !s.scale.unit() ? scale : scale_for(s.width, s.height);
        if (to == s.scale) continue;
        image img{};
        if (!create_image(img, s.img.format, to.edge(s.width * s.aa_u), to.edge(s.height * s.aa_v), surface_usage(s.depth), s.img.aspect)) {
            discard(img);
            continue;
        }
        if (s.state == SURFACE_VALID) {
            if (!s.depth) scale_blit(c, s.img, img, VK_FILTER_LINEAR);
            else if (!scale_depth(c, s.img, img)) s.state = SURFACE_UNINITIALIZED;
        }
        discard(s.img);
        discard(s.snapshot);
        discard(s.as_color);
        if (to.unit()) discard(s.native);
        s.img = img;
        s.scale = to;
    }
}

}
