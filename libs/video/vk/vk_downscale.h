#pragma once

#include "vk_device.h"

namespace vk {

class downscale {
public:
    bool create(vk_device& d);
    void destroy();
    void record(VkCommandBuffer cmd, VkImageView src, VkImageView dst, uint32_t width, uint32_t height, uint32_t src_width,
                uint32_t src_height) const;

private:
    vk_device* m_d = nullptr;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipe = VK_NULL_HANDLE;
};

}