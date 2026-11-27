#pragma once

#include "vk_device.h"

namespace vk {

class smaa {
public:
    struct output {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool create(vk_device& d, VkPipelineCache cache = VK_NULL_HANDLE);
    void destroy();
    void release_targets();

    bool has_staging() const { return m_lut_stage != VK_NULL_HANDLE; }
    void retire_staging(uint64_t completed);

    output record(VkCommandBuffer cmd, VkImageView src_view, VkFormat src_format, uint32_t width, uint32_t height,
                  bool for_sample, uint64_t submit_ticket,
                  VkImage direct_img = VK_NULL_HANDLE, VkImageView direct_view = VK_NULL_HANDLE);

    bool downscale(VkCommandBuffer cmd, VkImageView src_view, uint32_t sw, uint32_t sh,
                   VkImage dst_img, VkImageView dst_view, VkFormat dst_format, VkExtent2D out);

private:
    struct tex {
        VkImage handle = VK_NULL_HANDLE;
        VmaAllocation mem = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool create_image(tex& out, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags view_aspect);
    void destroy_image(tex& t);
    bool ensure_targets(uint32_t w, uint32_t h, VkFormat out_format, bool need_out);
    void destroy_targets();
    bool ensure_blend_pipeline(VkFormat out_format);
    bool ensure_downscale_pipeline(VkFormat dst_format);
    bool upload_luts(VkCommandBuffer cmd, uint64_t submit_ticket);

    vk_device* m_d = nullptr;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkShaderModule m_edge_vs = VK_NULL_HANDLE;
    VkShaderModule m_edge_fs = VK_NULL_HANDLE;
    VkShaderModule m_weight_vs = VK_NULL_HANDLE;
    VkShaderModule m_weight_fs = VK_NULL_HANDLE;
    VkShaderModule m_blend_vs = VK_NULL_HANDLE;
    VkShaderModule m_blend_fs = VK_NULL_HANDLE;
    VkShaderModule m_down_vs = VK_NULL_HANDLE;
    VkShaderModule m_down_fs = VK_NULL_HANDLE;
    VkPipeline m_pipe_edge = VK_NULL_HANDLE;
    VkPipeline m_pipe_weight = VK_NULL_HANDLE;
    VkPipeline m_pipe_blend = VK_NULL_HANDLE;
    VkPipeline m_pipe_down = VK_NULL_HANDLE;
    VkFormat m_blend_format = VK_FORMAT_UNDEFINED;
    VkFormat m_down_format = VK_FORMAT_UNDEFINED;

    VkFormat m_stencil_format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags m_stencil_aspect = VK_IMAGE_ASPECT_STENCIL_BIT;

    tex m_area;
    tex m_search;
    VkBuffer m_lut_stage = VK_NULL_HANDLE;
    VmaAllocation m_lut_stage_mem = nullptr;
    uint64_t m_lut_upload_ticket = 0;
    bool m_luts_uploaded = false;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    VkFormat m_out_format = VK_FORMAT_UNDEFINED;
    tex m_edges;
    tex m_blend;
    tex m_stencil;
    tex m_out;
};

}
