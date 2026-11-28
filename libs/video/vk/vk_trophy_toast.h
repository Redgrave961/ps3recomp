#pragma once

#include "vk_device.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace vk {

class trophy_toast {
public:
    bool create(vk_device& d, VkPipelineCache cache = VK_NULL_HANDLE);

    bool record(VkCommandBuffer cmd, VkImage target, VkImageView target_view, VkExtent2D extent, VkFormat format);

private:
    struct pending_item {
        int id = 0;
        char grade = 'B';
        std::string name;
        std::vector<uint8_t> png;
    };

    struct retired_tex {
        VkImage img = VK_NULL_HANDLE;
        VmaAllocation mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer stage = VK_NULL_HANDLE;
        VmaAllocation stage_mem = VK_NULL_HANDLE;
        uint32_t age = 0;
    };

    bool ensure_pipeline(VkFormat format);
    void ensure_font();
    void render_card(const pending_item& item, uint32_t ref_h);
    void upload_card(VkCommandBuffer cmd);
    void retire_current();
    void gc_retired();
    void play_chime();
    void poll_chime();

    vk_device* m_d = nullptr;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    bool m_disabled = false;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkShaderModule m_vs = VK_NULL_HANDLE;
    VkShaderModule m_fs = VK_NULL_HANDLE;
    VkPipeline m_pipe = VK_NULL_HANDLE;
    VkFormat m_pipe_format = VK_FORMAT_UNDEFINED;

    VkImage m_img = VK_NULL_HANDLE;
    VmaAllocation m_mem = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkBuffer m_stage = VK_NULL_HANDLE;
    VmaAllocation m_stage_mem = VK_NULL_HANDLE;
    uint32_t m_tex_w = 0, m_tex_h = 0;
    bool m_needs_upload = false;
    std::vector<uint8_t> m_pixels;

    std::vector<retired_tex> m_retired;
    std::deque<pending_item> m_queue;
    bool m_active = false;
    uint64_t m_start_ns = 0;
    uint64_t m_hold_ns = 4500000000ull;
    uint32_t m_built_ref_h = 0;

    bool m_font_tried = false;
    std::vector<uint8_t> m_font_data;

    SDL_AudioStream* m_chime_stream = nullptr;
    uint64_t m_chime_end_ns = 0;
};

}
