#pragma once

#include "vk_device.h"
#include "vk_flat_map.h"
#include "../rsx_spirv.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vk {

class shader_cache;

struct blend_attachment {
    uint32_t enable;
    uint32_t src_color;
    uint32_t dst_color;
    uint32_t color_op;
    uint32_t src_alpha;
    uint32_t dst_alpha;
    uint32_t alpha_op;
    uint32_t write_mask;
};

struct pipeline_key {
    uint64_t vs;
    uint64_t fs;
    uint32_t color_format[4];
    uint32_t depth_format;
    uint32_t topology_class;
    uint32_t polygon_mode;
    uint32_t logic_enable;
    uint32_t logic_op;
    uint32_t depth_clamp;
    uint32_t depth_clip;
    uint32_t pad;
    blend_attachment blend[4];
};

struct shader {
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline library = VK_NULL_HANDLE;
};

class pipelines {
public:
    bool create(vk_device& d, const std::string& cache_path);
    void destroy();
    void save_cache();
    void precompile(shader_cache& shaders);
    VkPipelineLayout layout() const { return m_layout; }
    VkPipelineCache cache() const { return m_cache; }
    VkDescriptorSetLayout buffer_set_layout() const { return m_sets[RSX_SPV_SET_BUFFERS]; }
    VkPipeline get(const pipeline_key& key, const shader& vs, const shader& fs, bool sync = false);
    bool fast_link() const { return m_libraries; }
    bool dynamic_blend() const { return m_dynamic_blend; }
    bool dynamic_polygon_mode() const { return m_dynamic_polygon; }
    bool dynamic_depth_clamp() const { return m_dynamic_clamp; }
    bool dynamic_depth_clip() const { return m_dynamic_clip; }
    bool dynamic_logic() const { return m_dynamic_logic; }
    pipeline_key normalize(const pipeline_key& key) const;

    bool shader_library(shader& s, VkShaderStageFlagBits stage);
    void work(std::function<void()> task, bool urgent);
    void record_vertex(const uint32_t* program, const rsx_spv_vp_key& key);
    void record_fragment(const uint32_t* ucode, uint32_t words, const rsx_spv_fp_key& key);

private:
    struct entry {
        std::atomic<VkPipeline> pipe{ VK_NULL_HANDLE };
        std::atomic<bool> done{ false };
        VkPipeline fast = VK_NULL_HANDLE;
    };

    void worker();
    void record(char type, const void* a, size_t an, const void* b, size_t bn);
    VkPipeline monolithic(const pipeline_key& k, const shader& vs, const shader& fs) const;
    VkPipeline link(const pipeline_key& k, const shader& vs, const shader& fs, bool optimized);
    VkPipeline output_library(const pipeline_key& k);
    VkPipeline input_library(uint32_t topology_class);
    void fill_output(const pipeline_key& k, VkPipelineColorBlendStateCreateInfo& cb, VkPipelineColorBlendAttachmentState* att,
                     VkPipelineRenderingCreateInfo& ri, VkFormat* formats) const;
    void fill_raster(const pipeline_key& k, VkPipelineRasterizationStateCreateInfo& rs, VkPipelineRasterizationDepthClipStateCreateInfoEXT& clip,
                     VkPipelineRasterizationProvokingVertexStateCreateInfoEXT& prov) const;

    vk_device* m_d = nullptr;
    VkDescriptorSetLayout m_sets[2] = {};
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    std::string m_cache_path;
    std::vector<VkDynamicState> m_dynamic;
    bool m_libraries = false;
    bool m_dynamic_blend = false;
    bool m_dynamic_polygon = false;
    bool m_dynamic_clamp = false;
    bool m_dynamic_clip = false;
    bool m_dynamic_logic = false;

    struct l1_slot {
        pipeline_key key{};
        const entry* e = nullptr;
    };
    static constexpr size_t k_l1_size = 256;
    l1_slot m_l1[k_l1_size]{};
    pipeline_key m_last_raw_key{};
    const entry* m_last_entry = nullptr;

    std::mutex m_map_lock;
    flat_map<pipeline_key, std::unique_ptr<entry>> m_map;
    std::mutex m_lib_lock;
    flat_map<pipeline_key, VkPipeline> m_output;
    VkPipeline m_input[3] = {};

    std::mutex m_db_lock;
    std::string m_db_path;
    FILE* m_db = nullptr;

    std::mutex m_work_lock;
    std::condition_variable m_work_cv;
    std::deque<std::function<void()>> m_urgent;
    std::deque<std::function<void()>> m_background;
    unsigned m_background_running = 0;
    unsigned m_background_limit = 1;
    std::vector<std::thread> m_workers;
    std::thread m_precompile_thread;
    bool m_stop = false;
    std::atomic<bool> m_stopping{ false };
    std::atomic<bool> m_cache_dirty{ false };
    std::mutex m_save_lock;
    std::chrono::steady_clock::time_point m_saved{};
    void save_cache_periodic();
};

}
