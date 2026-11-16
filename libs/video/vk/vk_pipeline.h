#pragma once

#include "vk_device.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vk {

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
    uint32_t alpha_to_coverage;
    uint32_t alpha_to_one;
    uint32_t provoking_last;
    blend_attachment blend[4];

    bool operator==(const pipeline_key& o) const;
};

struct pipeline_key_hash {
    size_t operator()(const pipeline_key& k) const;
};

struct shader {
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<uint32_t> spirv;
};

class pipelines {
public:
    bool create(vk_device& d, const std::string& cache_path);
    void destroy();
    VkPipelineLayout layout() const { return m_layout; }
    VkPipeline get(const pipeline_key& key, const shader& vs, const shader& fs);
    bool dynamic_blend() const { return m_dynamic_blend; }
    bool dynamic_polygon_mode() const { return m_dynamic_polygon; }
    bool dynamic_logic() const { return m_dynamic_logic; }
    bool dynamic_depth_clamp() const { return m_dynamic_clamp; }
    bool dynamic_depth_clip() const { return m_dynamic_clip; }
    bool dynamic_a2c() const { return m_dynamic_a2c; }
    bool dynamic_provoking() const { return m_dynamic_provoking; }
    pipeline_key normalize(const pipeline_key& key) const;
    void save();

private:
    vk_device* m_d = nullptr;
    VkDescriptorSetLayout m_set = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    std::string m_cache_path;
    std::unordered_map<pipeline_key, VkPipeline, pipeline_key_hash> m_map;
    std::vector<VkDynamicState> m_dynamic;
    bool m_dynamic_blend = false;
    bool m_dynamic_polygon = false;
    bool m_dynamic_logic = false;
    bool m_dynamic_clamp = false;
    bool m_dynamic_clip = false;
    bool m_dynamic_a2c = false;
    bool m_dynamic_provoking = false;
};

}
