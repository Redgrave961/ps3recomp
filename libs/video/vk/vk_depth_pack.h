#pragma once

#include "vk_device.h"

namespace vk {

class depth_pack {
public:
    bool create(vk_device& d);
    void destroy();
    void record(VkCommandBuffer cmd, VkImageView depth, VkImageView stencil, VkBuffer out, VkDeviceSize offset, VkDeviceSize size,
                uint32_t width, uint32_t height, bool d16, bool is_float) const;

private:
    vk_device* m_d = nullptr;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipe[2] = {};
};

}
