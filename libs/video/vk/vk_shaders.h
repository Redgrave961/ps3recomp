#pragma once

#include "../rsx_spirv.h"
#include "vk_pipeline.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vk {

struct vertex_program {
    uint64_t hash = 0;
    shader code;
    rsx_spv_vp_info info{};
};

struct fragment_program {
    uint64_t hash = 0;
    shader code;
    rsx_spv_fp_info info{};
};

class shader_cache {
public:
    void init(vk_device& d, pipelines& p) { m_d = &d; m_pipes = &p; }
    void destroy();
    const vertex_program* vertex(const uint32_t* program, const rsx_spv_vp_key& key, uint64_t program_hash, bool async);
    const fragment_program* fragment(const uint32_t* ucode, uint32_t max_words, const rsx_spv_fp_key& key, bool async);
    const vertex_program* find_vertex(uint64_t hash);
    const fragment_program* find_fragment(uint64_t hash);
    static uint64_t hash(const void* data, size_t bytes, uint64_t seed);
    static uint64_t program_hash(const uint32_t* program, uint32_t start);

private:
    template <typename T, typename Job>
    const T* resolve(std::unordered_map<uint64_t, std::unique_ptr<T>>& map, uint64_t h, bool async, Job&& job);
    bool make_module(shader& s, const rsx_spv_blob& blob, VkShaderStageFlagBits stage);

    vk_device* m_d = nullptr;
    pipelines* m_pipes = nullptr;
    std::mutex m_lock;
    std::condition_variable m_built;
    std::unordered_set<uint64_t> m_building;
    std::unordered_map<uint64_t, std::unique_ptr<vertex_program>> m_vertex;
    std::unordered_map<uint64_t, std::unique_ptr<fragment_program>> m_fragment;
};

}
