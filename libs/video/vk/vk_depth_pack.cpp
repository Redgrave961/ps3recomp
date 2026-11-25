#include "vk_depth_pack.h"

#include "../rsx_spirv.h"

namespace vk {

bool depth_pack::create(vk_device& d)
{
    m_d = &d;
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VK_TRY(vkCreateSampler(d.dev, &si, nullptr, &m_sampler));
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        b[i].binding = i;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 3;
    li.pBindings = b;
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &m_set));
    const VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &m_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    VK_TRY(vkCreatePipelineLayout(d.dev, &pi, nullptr, &m_layout));
    for (int v = 0; v < 2; v++) {
        rsx_spv_blob blob{};
        if (rsx_spv_depth_pack(v, &blob) != 0) {
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
        r = vkCreateComputePipelines(d.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipe[v]);
        vkDestroyShaderModule(d.dev, mod, nullptr);
        VK_TRY(r);
    }
    return true;
}

void depth_pack::destroy()
{
    if (!m_d) return;
    for (VkPipeline& p : m_pipe) {
        if (p) vkDestroyPipeline(m_d->dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (m_layout) vkDestroyPipelineLayout(m_d->dev, m_layout, nullptr);
    if (m_set) vkDestroyDescriptorSetLayout(m_d->dev, m_set, nullptr);
    if (m_sampler) vkDestroySampler(m_d->dev, m_sampler, nullptr);
    m_layout = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_d = nullptr;
}

void depth_pack::record(VkCommandBuffer cmd, VkImageView depth, VkImageView stencil, VkImageView out, uint32_t width, uint32_t height,
                        bool d16, bool is_float) const
{
    const VkDescriptorImageInfo images[2] = {
        { m_sampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { m_sampler, stencil, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    const VkDescriptorImageInfo target{ VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[3]{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < (d16 ? 1u : 2u); i++) {
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstBinding = i;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &images[i];
        n++;
    }
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstBinding = 2;
    w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[n].pImageInfo = &target;
    n++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipe[d16 ? 1 : 0]);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, n, w);
    const uint32_t pc[3] = { width, height, is_float ? 1u : 0u };
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
}

}
