#include "vk_downscale.h"

#include "../rsx_spirv.h"

#include <cstring>
#include <numeric>

namespace vk {

bool downscale::create(vk_device& d, VkPipelineCache cache)
{
    m_d = &d;
    m_cache = cache;
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VK_TRY(vkCreateSampler(d.dev, &si, nullptr, &m_sampler));
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &m_set));
    const VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 28 };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &m_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    VK_TRY(vkCreatePipelineLayout(d.dev, &pi, nullptr, &m_layout));
    rsx_spv_blob blob{};
    if (rsx_spv_box_down(&blob) != 0) {
        rsx_spv_blob_free(&blob);
        return false;
    }
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)blob.count * 4u;
    mi.pCode = blob.words;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(d.dev, &mi, nullptr, &mod);
    rsx_spv_blob_free(&blob);
    VK_TRY(r);
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = m_layout;
    r = vkCreateComputePipelines(d.dev, m_cache, 1, &ci, nullptr, &m_pipe);
    vkDestroyShaderModule(d.dev, mod, nullptr);
    VK_TRY(r);

    VkSamplerCreateInfo lsi{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    lsi.magFilter = VK_FILTER_LINEAR;
    lsi.minFilter = VK_FILTER_LINEAR;
    lsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    lsi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lsi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    lsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(d.dev, &lsi, nullptr, &m_linear) == VK_SUCCESS) {
        VkDescriptorSetLayoutBinding gb{};
        gb.binding = 0;
        gb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        gb.descriptorCount = 1;
        gb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo gli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        gli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        gli.bindingCount = 1;
        gli.pBindings = &gb;
        const VkPushConstantRange gpc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };
        VkPipelineLayoutCreateInfo gpi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        gpi.setLayoutCount = 1;
        gpi.pSetLayouts = &m_gfx_set;
        gpi.pushConstantRangeCount = 1;
        gpi.pPushConstantRanges = &gpc;
        if (vkCreateDescriptorSetLayout(d.dev, &gli, nullptr, &m_gfx_set) == VK_SUCCESS &&
            vkCreatePipelineLayout(d.dev, &gpi, nullptr, &m_gfx_layout) == VK_SUCCESS) {
            rsx_spv_blob vb{};
            if (rsx_spv_box_down_vs(&vb) == 0) {
                VkShaderModuleCreateInfo vmi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                vmi.codeSize = (size_t)vb.count * 4u;
                vmi.pCode = vb.words;
                vkCreateShaderModule(d.dev, &vmi, nullptr, &m_vs);
            }
            rsx_spv_blob_free(&vb);
            for (int k = 0; k < 3; k++) {
                rsx_spv_blob fb{};
                if (rsx_spv_box_down_fs(k + 2, &fb) == 0) {
                    VkShaderModuleCreateInfo fmi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                    fmi.codeSize = (size_t)fb.count * 4u;
                    fmi.pCode = fb.words;
                    vkCreateShaderModule(d.dev, &fmi, nullptr, &m_fs[k]);
                }
                rsx_spv_blob_free(&fb);
            }
        }
    }
    return true;
}

void downscale::destroy()
{
    if (!m_d) return;
    for (const gfx_pipe& p : m_gfx_pipes)
        if (p.pipe) vkDestroyPipeline(m_d->dev, p.pipe, nullptr);
    m_gfx_pipes.clear();
    for (VkShaderModule& m : m_fs) {
        if (m) vkDestroyShaderModule(m_d->dev, m, nullptr);
        m = VK_NULL_HANDLE;
    }
    if (m_vs) vkDestroyShaderModule(m_d->dev, m_vs, nullptr);
    if (m_gfx_layout) vkDestroyPipelineLayout(m_d->dev, m_gfx_layout, nullptr);
    if (m_gfx_set) vkDestroyDescriptorSetLayout(m_d->dev, m_gfx_set, nullptr);
    if (m_linear) vkDestroySampler(m_d->dev, m_linear, nullptr);
    m_vs = VK_NULL_HANDLE;
    m_gfx_layout = VK_NULL_HANDLE;
    m_gfx_set = VK_NULL_HANDLE;
    m_linear = VK_NULL_HANDLE;
    if (m_pipe) vkDestroyPipeline(m_d->dev, m_pipe, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_d->dev, m_layout, nullptr);
    if (m_set) vkDestroyDescriptorSetLayout(m_d->dev, m_set, nullptr);
    if (m_sampler) vkDestroySampler(m_d->dev, m_sampler, nullptr);
    m_pipe = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_d = nullptr;
}

VkPipeline downscale::pipeline_for(VkFormat format, uint32_t scale)
{
    for (const gfx_pipe& p : m_gfx_pipes)
        if (p.format == format && p.scale == scale) return p.pipe;
    if (!m_d || !m_vs || scale < 2u || scale > 4u || !m_fs[scale - 2u] || !m_gfx_layout) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, m_vs, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m_fs[scale - 2u], "main" };
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
    VkPipelineColorBlendAttachmentState ba{};
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
    ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = m_gfx_layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &gi, nullptr, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
    m_gfx_pipes.push_back({ format, scale, pipe });
    return pipe;
}

bool downscale::record_direct(VkCommandBuffer cmd, VkImageView src, VkImageView dst, VkFormat dst_format, uint32_t width, uint32_t height,
                              uint32_t src_width, uint32_t src_height)
{
    if (!width || !height) return false;
    const uint32_t sx = src_width / width, sy = src_height / height;
    if (sx != sy || sx < 2u || sx > 4u || sx * width != src_width || sy * height != src_height) return false;
    VkPipeline pipe = pipeline_for(dst_format, sx);
    if (!pipe) return false;
    VkRenderingAttachmentInfo ca{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = dst;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { width, height } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    const VkViewport vp{ 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
    const VkRect2D scissor{ { 0, 0 }, { width, height } };
    const VkDescriptorImageInfo img0{ m_linear, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img0;
    const float pc[2] = { 1.f / (float)src_width, 1.f / (float)src_height };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gfx_layout, 0, 1, &w);
    vkCmdPushConstants(cmd, m_gfx_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    return true;
}

void downscale::record(VkCommandBuffer cmd, VkImageView src, VkImageView dst, uint32_t width, uint32_t height, uint32_t src_width,
                       uint32_t src_height) const
{
    const VkDescriptorImageInfo images[2] = {
        { m_sampler, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL },
    };
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &images[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 2, w);
    const uint32_t gx = std::gcd(src_width, width), gy = std::gcd(src_height, height);
    const float weight = 1.f / ((float)(src_width / gx) * (float)(src_height / gy));
    uint32_t pc[7] = { width, height, src_width / gx, width / gx, src_height / gy, height / gy };
    std::memcpy(&pc[6], &weight, sizeof weight);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
}

}