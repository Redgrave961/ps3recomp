#include "vk_smaa.h"
#include "vk_smaa_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vk {

namespace {

VkShaderModule make_module(VkDevice dev, const uint32_t* words, size_t word_count)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = word_count * sizeof(uint32_t);
    ci.pCode = words;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
}

VkImageMemoryBarrier2 make_barrier(VkImage img, VkImageAspectFlags aspect,
                                   VkImageLayout old_layout, VkImageLayout new_layout,
                                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    return b;
}

void img_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                 VkImageLayout old_layout, VkImageLayout new_layout,
                 VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                 VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    const VkImageMemoryBarrier2 b = make_barrier(img, aspect, old_layout, new_layout, src_stage, src_access, dst_stage, dst_access);
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void img_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* barriers, uint32_t count)
{
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkPipeline make_pipeline(VkDevice dev, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs,
                         VkFormat color_fmt, VkFormat stencil_fmt, bool stencil_write, bool stencil_test)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" };

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    if (stencil_write || stencil_test) {
        ds.stencilTestEnable = VK_TRUE;
        VkStencilOpState st{};
        st.failOp = VK_STENCIL_OP_KEEP;
        st.depthFailOp = VK_STENCIL_OP_KEEP;
        st.compareMask = 0xFFu;
        st.reference = 1u;
        if (stencil_write) {
            st.passOp = VK_STENCIL_OP_REPLACE;
            st.compareOp = VK_COMPARE_OP_ALWAYS;
            st.writeMask = 0xFFu;
        } else {
            st.passOp = VK_STENCIL_OP_KEEP;
            st.compareOp = VK_COMPARE_OP_EQUAL;
            st.writeMask = 0x00u;
        }
        ds.front = st;
        ds.back = st;
    }

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_FALSE;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_fmt;
    ri.stencilAttachmentFormat = stencil_fmt;

    VkGraphicsPipelineCreateInfo gi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = layout;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pipe;
}

} // namespace

bool smaa::create_image(tex& out, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags view_aspect)
{
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_TRY(vmaCreateImage(m_d->vma, &ci, &ai, &out.handle, &out.mem, nullptr));

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = out.handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { view_aspect, 0, 1, 0, 1 };
    if (vkCreateImageView(m_d->dev, &vi, nullptr, &out.view) != VK_SUCCESS) {
        vmaDestroyImage(m_d->vma, out.handle, out.mem);
        out = {};
        return false;
    }
    return true;
}

void smaa::destroy_image(tex& t)
{
    if (!m_d) return;
    if (t.view) vkDestroyImageView(m_d->dev, t.view, nullptr);
    if (t.handle) vmaDestroyImage(m_d->vma, t.handle, t.mem);
    t = {};
}

bool smaa::create(vk_device& d)
{
    m_d = &d;

    const VkFormat stencil_candidates[] = {
        VK_FORMAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (VkFormat f : stencil_candidates) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(d.gpu, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_stencil_format = f;
            m_stencil_aspect = (f == VK_FORMAT_S8_UINT) ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                        : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            break;
        }
    }
    if (m_stencil_format == VK_FORMAT_UNDEFINED) {
        destroy();
        return false;
    }

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    VK_TRY(vkCreateSampler(d.dev, &si, nullptr, &m_sampler));

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 3;
    li.pBindings = bindings;
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &m_set));

    const VkPushConstantRange pc{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &m_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    VK_TRY(vkCreatePipelineLayout(d.dev, &pi, nullptr, &m_layout));

    m_edge_vs   = make_module(d.dev, smaa_data::k_smaa_edge_vs,      std::size(smaa_data::k_smaa_edge_vs));
    m_edge_fs   = make_module(d.dev, smaa_data::k_smaa_edge_fs,      std::size(smaa_data::k_smaa_edge_fs));
    m_weight_vs = make_module(d.dev, smaa_data::k_smaa_weight_vs,    std::size(smaa_data::k_smaa_weight_vs));
    m_weight_fs = make_module(d.dev, smaa_data::k_smaa_weight_fs,    std::size(smaa_data::k_smaa_weight_fs));
    m_blend_vs  = make_module(d.dev, smaa_data::k_smaa_blend_vs,     std::size(smaa_data::k_smaa_blend_vs));
    m_blend_fs  = make_module(d.dev, smaa_data::k_smaa_blend_fs,     std::size(smaa_data::k_smaa_blend_fs));
    m_down_vs   = make_module(d.dev, smaa_data::k_smaa_downscale_vs, std::size(smaa_data::k_smaa_downscale_vs));
    m_down_fs   = make_module(d.dev, smaa_data::k_smaa_downscale_fs, std::size(smaa_data::k_smaa_downscale_fs));
    if (!m_edge_vs || !m_edge_fs || !m_weight_vs || !m_weight_fs || !m_blend_vs || !m_blend_fs || !m_down_vs || !m_down_fs) {
        destroy();
        return false;
    }

    m_pipe_edge = make_pipeline(d.dev, m_layout, m_edge_vs, m_edge_fs,
                                VK_FORMAT_R8G8_UNORM, m_stencil_format, true, false);
    m_pipe_weight = make_pipeline(d.dev, m_layout, m_weight_vs, m_weight_fs,
                                  VK_FORMAT_R8G8B8A8_UNORM, m_stencil_format, false, true);
    if (!m_pipe_edge || !m_pipe_weight) {
        destroy();
        return false;
    }

    if (!create_image(m_area, VK_FORMAT_R8G8_UNORM, smaa_data::k_area_width, smaa_data::k_area_height,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(m_search, VK_FORMAT_R8_UNORM, smaa_data::k_search_width, smaa_data::k_search_height,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
        destroy();
        return false;
    }

    const VkDeviceSize stage_size = sizeof(smaa_data::k_area_tex) + sizeof(smaa_data::k_search_tex);
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = stage_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo bai{};
    bai.usage = VMA_MEMORY_USAGE_AUTO;
    bai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(d.vma, &bi, &bai, &m_lut_stage, &m_lut_stage_mem, &info) != VK_SUCCESS || !info.pMappedData) {
        destroy();
        return false;
    }
    auto* dst = static_cast<uint8_t*>(info.pMappedData);
    std::memcpy(dst, smaa_data::k_area_tex, sizeof(smaa_data::k_area_tex));
    std::memcpy(dst + sizeof(smaa_data::k_area_tex), smaa_data::k_search_tex, sizeof(smaa_data::k_search_tex));
    vmaFlushAllocation(d.vma, m_lut_stage_mem, 0, stage_size);
    return true;
}

void smaa::destroy_targets()
{
    destroy_image(m_edges);
    destroy_image(m_blend);
    destroy_image(m_stencil);
    destroy_image(m_out);
    m_width = 0;
    m_height = 0;
    m_out_format = VK_FORMAT_UNDEFINED;
}

void smaa::release_targets()
{
    if (!m_d || !m_out.handle) return;
    {
        std::lock_guard<std::mutex> hold(m_d->queue_lock);
        vkDeviceWaitIdle(m_d->dev);
    }
    destroy_targets();
}

void smaa::retire_staging(uint64_t completed)
{
    if (!m_d || !m_lut_stage || !m_luts_uploaded || completed < m_lut_upload_ticket) return;
    vmaDestroyBuffer(m_d->vma, m_lut_stage, m_lut_stage_mem);
    m_lut_stage = VK_NULL_HANDLE;
    m_lut_stage_mem = nullptr;
}

void smaa::destroy()
{
    if (!m_d) return;
    destroy_targets();
    destroy_image(m_area);
    destroy_image(m_search);
    if (m_lut_stage) vmaDestroyBuffer(m_d->vma, m_lut_stage, m_lut_stage_mem);
    m_lut_stage = VK_NULL_HANDLE;
    m_lut_stage_mem = nullptr;
    m_lut_upload_ticket = 0;
    m_luts_uploaded = false;
    if (m_pipe_down) vkDestroyPipeline(m_d->dev, m_pipe_down, nullptr);
    if (m_pipe_blend) vkDestroyPipeline(m_d->dev, m_pipe_blend, nullptr);
    if (m_pipe_weight) vkDestroyPipeline(m_d->dev, m_pipe_weight, nullptr);
    if (m_pipe_edge) vkDestroyPipeline(m_d->dev, m_pipe_edge, nullptr);
    if (m_down_fs) vkDestroyShaderModule(m_d->dev, m_down_fs, nullptr);
    if (m_down_vs) vkDestroyShaderModule(m_d->dev, m_down_vs, nullptr);
    if (m_blend_fs) vkDestroyShaderModule(m_d->dev, m_blend_fs, nullptr);
    if (m_blend_vs) vkDestroyShaderModule(m_d->dev, m_blend_vs, nullptr);
    if (m_weight_fs) vkDestroyShaderModule(m_d->dev, m_weight_fs, nullptr);
    if (m_weight_vs) vkDestroyShaderModule(m_d->dev, m_weight_vs, nullptr);
    if (m_edge_fs) vkDestroyShaderModule(m_d->dev, m_edge_fs, nullptr);
    if (m_edge_vs) vkDestroyShaderModule(m_d->dev, m_edge_vs, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_d->dev, m_layout, nullptr);
    if (m_set) vkDestroyDescriptorSetLayout(m_d->dev, m_set, nullptr);
    if (m_sampler) vkDestroySampler(m_d->dev, m_sampler, nullptr);
    m_pipe_down = m_pipe_blend = m_pipe_weight = m_pipe_edge = VK_NULL_HANDLE;
    m_down_fs = m_down_vs = m_blend_fs = m_blend_vs = m_weight_fs = m_weight_vs = m_edge_fs = m_edge_vs = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_blend_format = m_down_format = VK_FORMAT_UNDEFINED;
    m_d = nullptr;
}

bool smaa::ensure_blend_pipeline(VkFormat out_format)
{
    if (m_pipe_blend && m_blend_format == out_format) return true;
    if (m_pipe_blend) {
        vkDestroyPipeline(m_d->dev, m_pipe_blend, nullptr);
        m_pipe_blend = VK_NULL_HANDLE;
    }
    m_pipe_blend = make_pipeline(m_d->dev, m_layout, m_blend_vs, m_blend_fs,
                                 out_format, VK_FORMAT_UNDEFINED, false, false);
    if (!m_pipe_blend) return false;
    m_blend_format = out_format;
    return true;
}

bool smaa::ensure_downscale_pipeline(VkFormat dst_format)
{
    if (m_pipe_down && m_down_format == dst_format) return true;
    if (m_pipe_down) {
        vkDestroyPipeline(m_d->dev, m_pipe_down, nullptr);
        m_pipe_down = VK_NULL_HANDLE;
    }
    m_pipe_down = make_pipeline(m_d->dev, m_layout, m_down_vs, m_down_fs,
                                dst_format, VK_FORMAT_UNDEFINED, false, false);
    if (!m_pipe_down) return false;
    m_down_format = dst_format;
    return true;
}

bool smaa::ensure_targets(uint32_t w, uint32_t h, VkFormat out_format)
{
    if (m_width == w && m_height == h && m_out_format == out_format && m_out.handle) return true;
    if (m_out.handle) {
        std::lock_guard<std::mutex> hold(m_d->queue_lock);
        vkDeviceWaitIdle(m_d->dev);
    }
    destroy_targets();
    if (!create_image(m_edges, VK_FORMAT_R8G8_UNORM, w, h,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(m_blend, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(m_stencil, m_stencil_format, w, h,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_STENCIL_BIT) ||
        !create_image(m_out, out_format, w, h,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT)) {
        destroy_targets();
        return false;
    }
    m_width = w;
    m_height = h;
    m_out_format = out_format;
    return true;
}

bool smaa::upload_luts(VkCommandBuffer cmd, uint64_t submit_ticket)
{
    if (m_luts_uploaded) return true;
    img_barrier(cmd, m_area.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    img_barrier(cmd, m_search.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy r_area{};
    r_area.bufferOffset = 0;
    r_area.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r_area.imageExtent = { smaa_data::k_area_width, smaa_data::k_area_height, 1 };
    vkCmdCopyBufferToImage(cmd, m_lut_stage, m_area.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r_area);

    VkBufferImageCopy r_search{};
    r_search.bufferOffset = sizeof(smaa_data::k_area_tex);
    r_search.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r_search.imageExtent = { smaa_data::k_search_width, smaa_data::k_search_height, 1 };
    vkCmdCopyBufferToImage(cmd, m_lut_stage, m_search.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r_search);

    img_barrier(cmd, m_area.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    img_barrier(cmd, m_search.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    m_lut_upload_ticket = submit_ticket;
    m_luts_uploaded = true;
    return true;
}

smaa::output smaa::record(VkCommandBuffer cmd, VkImageView src_view, VkFormat src_format, uint32_t width, uint32_t height,
                          bool for_sample, uint64_t submit_ticket)
{
    if (!m_d || !m_pipe_edge || !width || !height || !src_view) return {};
    if (!ensure_blend_pipeline(src_format) || !ensure_targets(width, height, src_format)) return {};
    upload_luts(cmd, submit_ticket);

    const float rt_metrics[4] = { 1.f / (float)width, 1.f / (float)height, (float)width, (float)height };
    const VkViewport vp{ 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
    const VkRect2D scissor{ { 0, 0 }, { width, height } };

    // Pass 1: Color Edge Detection + Stencil Write
    {
        const VkImageMemoryBarrier2 bs[2] = {
            make_barrier(m_edges.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
            make_barrier(m_stencil.handle, m_stencil_aspect,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
        };
        img_barriers(cmd, bs, 2);
    }

    {
        VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = m_edges.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo sa{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        sa.imageView = m_stencil.view;
        sa.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pStencilAttachment = &sa;

        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe_edge);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rt_metrics), rt_metrics);
        const VkDescriptorImageInfo img0{ m_sampler, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &img0;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &w);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    // Pass 2: Blending Weight Calculation (Stencil-masked to edge pixels)
    {
        const VkImageMemoryBarrier2 bs[3] = {
            make_barrier(m_edges.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            make_barrier(m_stencil.handle, m_stencil_aspect,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT),
            make_barrier(m_blend.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        img_barriers(cmd, bs, 3);
    }

    {
        VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = m_blend.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo sa{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        sa.imageView = m_stencil.view;
        sa.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        sa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pStencilAttachment = &sa;

        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe_weight);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rt_metrics), rt_metrics);
        const VkDescriptorImageInfo imgs[3] = {
            { m_sampler, m_edges.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { m_sampler, m_area.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { m_sampler, m_search.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        };
        VkWriteDescriptorSet w[3]{};
        for (uint32_t i = 0; i < 3; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &imgs[i];
        }
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 3, w);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    // Pass 3: Linear-Space Neighborhood Blending
    {
        const VkImageMemoryBarrier2 bs[2] = {
            make_barrier(m_blend.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
            make_barrier(m_out.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
        };
        img_barriers(cmd, bs, 2);
    }

    {
        VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = m_out.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;

        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe_blend);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rt_metrics), rt_metrics);
        const VkDescriptorImageInfo imgs[2] = {
            { m_sampler, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { m_sampler, m_blend.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        };
        VkWriteDescriptorSet w[2]{};
        for (uint32_t i = 0; i < 2; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &imgs[i];
        }
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, w);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }

    if (for_sample) {
        img_barrier(cmd, m_out.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else {
        img_barrier(cmd, m_out.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    return { m_out.handle, m_out.view };
}

bool smaa::downscale(VkCommandBuffer cmd, VkImageView src_view, uint32_t sw, uint32_t sh,
                     VkImage dst_img, VkImageView dst_view, VkFormat dst_format, VkExtent2D out)
{
    if (!m_d || !m_down_vs || !src_view || !dst_img || !dst_view || !sw || !sh || !out.width || !out.height) return false;
    if (!ensure_downscale_pipeline(dst_format)) return false;

    const float scale = std::min((float)out.width / (float)sw, (float)out.height / (float)sh);
    const uint32_t dw = (uint32_t)std::max(1, (int32_t)std::lround((float)sw * scale));
    const uint32_t dh = (uint32_t)std::max(1, (int32_t)std::lround((float)sh * scale));
    const int32_t x0 = ((int32_t)out.width - (int32_t)dw) / 2;
    const int32_t y0 = ((int32_t)out.height - (int32_t)dh) / 2;
    const bool letterboxed = (dw != out.width || dh != out.height);

    img_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = dst_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = letterboxed ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, out };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;

    const float params[4] = { 1.f / (float)sw, 1.f / (float)sh, (float)sw / (float)dw, (float)sh / (float)dh };
    const VkViewport vp{ (float)x0, (float)y0, (float)dw, (float)dh, 0.f, 1.f };
    const VkRect2D scissor{ { x0, y0 }, { dw, dh } };

    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe_down);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), params);
    const VkDescriptorImageInfo img0{ m_sampler, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img0;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &w);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    img_barrier(cmd, dst_img, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    return true;
}

} // namespace vk
