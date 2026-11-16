#pragma once

#include "../rsx_spirv.h"
#include "vk_pipeline.h"

#include <memory>
#include <unordered_map>
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
    void init(vk_device& d) { m_d = &d; }
    void destroy();
    const vertex_program* vertex(const uint32_t* program, const rsx_spv_vp_key& key, uint64_t program_hash);
    const fragment_program* fragment(const uint32_t* ucode, uint32_t max_words, const rsx_spv_fp_key& key);
    static uint64_t hash(const void* data, size_t bytes, uint64_t seed);

private:
    bool make_module(shader& s, const rsx_spv_blob& blob);

    vk_device* m_d = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<vertex_program>> m_vertex;
    std::unordered_map<uint64_t, std::unique_ptr<fragment_program>> m_fragment;
    std::vector<uint32_t> m_scratch;
};

}
