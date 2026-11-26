#pragma once

#include "vk_device.h"

#include <vector>

namespace vk {

class downscale {
public:
    bool create(vk_device& d);
    void destroy();
    void record(VkCommandBuffer cmd, VkImageView src, VkImageView dst, uint32_t width, uint32_t height, uint32_t src_width,
                uint32_t src_height) const;
    bool record_direct(VkCommandBuffer cmd, VkImageView src, VkImageView dst, VkFormat dst_format, uint32_t width, uint32_t height,
                       uint32_t src_width, uint32_t src_height);

private:
    struct gfx_pipe {
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t scale = 0;
        VkPipeline pipe = VK_NULL_HANDLE;
    };
    VkPipeline pipeline_for(VkFormat format, uint32_t scale);

    vk_device* m_d = nullptr;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipe = VK_NULL_HANDLE;

    VkSampler m_linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_gfx_set = VK_NULL_HANDLE;
    VkPipelineLayout m_gfx_layout = VK_NULL_HANDLE;
    VkShaderModule m_vs = VK_NULL_HANDLE;
    VkShaderModule m_fs[3]{}; // 0=2x, 1=3x, 2=4x
    std::vector<gfx_pipe> m_gfx_pipes;
};

}