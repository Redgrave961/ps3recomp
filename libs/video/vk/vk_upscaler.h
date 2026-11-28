#pragma once

#include "rsx_vk.h"
#include "vk_device.h"

#include <deque>
#include <functional>
#include <string>
#include <utility>

namespace vk {

struct upscale_image {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width;
    uint32_t height;
};

class upscaler {
public:
    bool create(vk_device& d, VkPipelineCache cache, const std::string& cache_path, const uint64_t* submitted);
    void destroy();
    void collect(uint64_t reached);
    int caps() const;
    const char* fsr_version() const { return m_fsr_version; }
    void render_size(uint32_t display_w, uint32_t display_h, int mode, uint32_t& out_w, uint32_t& out_h) const;
    void jitter(int mode, uint32_t render_w, uint32_t display_w, uint32_t frame_index, float& out_jx, float& out_jy) const;
    bool dispatch(VkCommandBuffer cmd, int mode, const upscale_image& color, const upscale_image& depth, VkImageView velocity,
                  const upscale_image& output, const rsx_vk_upscale_frame& frame);

private:
    void retire(std::function<void()> release);
    bool init_mv_pipeline(VkPipelineCache cache);
    bool ensure_mv_image(uint32_t width, uint32_t height);
    void destroy_mv_image();
    bool init_ffx();
    void destroy_ffx_context();
    bool ensure_ffx_context(uint32_t max_w, uint32_t max_h);
    bool init_ngx(const std::string& cache_path);
    void release_ffx();
    void release_ngx();
    void destroy_ngx_feature();
    bool ensure_ngx_feature(VkCommandBuffer cmd, int mode, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h);

    vk_device* m_d = nullptr;
    const uint64_t* m_submitted = nullptr;
    std::deque<std::pair<uint64_t, std::function<void()>>> m_retired;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_mv_set = VK_NULL_HANDLE;
    VkPipelineLayout m_mv_layout = VK_NULL_HANDLE;
    VkPipeline m_mv_pipe = VK_NULL_HANDLE;
    VkImage m_mv_img = VK_NULL_HANDLE;
    VmaAllocation m_mv_mem = nullptr;
    VkImageView m_mv_view = VK_NULL_HANDLE;
    uint32_t m_mv_w = 0;
    uint32_t m_mv_h = 0;

    void* m_ffx_mod = nullptr;
    void* m_ffx_ctx = nullptr;
    uint32_t m_ffx_dw = 0, m_ffx_dh = 0;
    char m_fsr_version[32] = "3.1";

    void* m_ngx_mod = nullptr;
    void* m_ngx_params = nullptr;
    void* m_ngx_handle = nullptr;
    int m_ngx_mode = 0;
    uint32_t m_ngx_rw = 0, m_ngx_rh = 0, m_ngx_dw = 0, m_ngx_dh = 0;
    int m_dropped = 0;
};

}
