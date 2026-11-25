#include "vk_downscale.h"

#include "../rsx_spirv.h"

#include <cstring>
#include <numeric>

namespace vk {

bool downscale::create(vk_device& d)
{
    m_d = &d;
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
    r = vkCreateComputePipelines(d.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipe);
    vkDestroyShaderModule(d.dev, mod, nullptr);
    VK_TRY(r);
    return true;
}

void downscale::destroy()
{
    if (!m_d) return;
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