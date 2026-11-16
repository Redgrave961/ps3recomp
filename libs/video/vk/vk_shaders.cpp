#include "vk_shaders.h"

#include <algorithm>
#include <cstring>

namespace vk {

uint64_t shader_cache::hash(const void* data, size_t bytes, uint64_t seed)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed ^ (bytes * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        w *= 0x87C37B91114253D5ull;
        w = (w << 31) | (w >> 33);
        h ^= w * 0x4CF5AD432745937Full;
        h = ((h << 27) | (h >> 37)) * 5u + 0x52DCE729u;
    }
    for (; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

bool shader_cache::make_module(shader& s, const rsx_spv_blob& blob)
{
    s.spirv.assign(blob.words, blob.words + blob.count);
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = s.spirv.size() * 4u;
    ci.pCode = s.spirv.data();
    VK_TRY(vkCreateShaderModule(m_d->dev, &ci, nullptr, &s.module));
    return true;
}

const vertex_program* shader_cache::vertex(const uint32_t* program, const rsx_spv_vp_key& key, uint64_t program_hash)
{
    const uint64_t h = hash(&key, sizeof key, program_hash);
    if (auto it = m_vertex.find(h); it != m_vertex.end()) return it->second.get();
    auto vp = std::make_unique<vertex_program>();
    vp->hash = h;
    rsx_spv_blob blob{};
    const bool ok = rsx_spv_vertex(program, &key, &blob, &vp->info) == 0 && make_module(vp->code, blob);
    rsx_spv_blob_free(&blob);
    if (!ok) {
        std::fprintf(stderr, "[vk] vertex program at slot %u failed to translate\n", key.start);
        m_vertex.emplace(h, nullptr);
        return nullptr;
    }
    return m_vertex.emplace(h, std::move(vp)).first->second.get();
}

const fragment_program* shader_cache::fragment(const uint32_t* ucode, uint32_t max_words, const rsx_spv_fp_key& key)
{
    rsx_spv_fp_info scan{};
    if (rsx_spv_fragment_scan(ucode, max_words, &scan) != 0) return nullptr;
    uint32_t end = scan.instructions * 4u;
    for (uint32_t i = 0; i < scan.constant_count; i++) end = std::max(end, scan.constant_word[i] + 4u);
    end = std::min(end, max_words);
    m_scratch.assign(ucode, ucode + end);
    for (uint32_t i = 0; i < scan.constant_count; i++)
        for (uint32_t w = 0; w < 4 && scan.constant_word[i] + w < end; w++) m_scratch[scan.constant_word[i] + w] = 0;
    const uint64_t h = hash(m_scratch.data(), m_scratch.size() * 4u, hash(&key, sizeof key, 0x46504650u));
    if (auto it = m_fragment.find(h); it != m_fragment.end()) return it->second.get();
    auto fp = std::make_unique<fragment_program>();
    fp->hash = h;
    rsx_spv_blob blob{};
    const bool ok = rsx_spv_fragment(ucode, max_words, &key, &blob, &fp->info) == 0 && make_module(fp->code, blob);
    rsx_spv_blob_free(&blob);
    if (!ok) {
        std::fprintf(stderr, "[vk] fragment program failed to translate\n");
        m_fragment.emplace(h, nullptr);
        return nullptr;
    }
    return m_fragment.emplace(h, std::move(fp)).first->second.get();
}

void shader_cache::destroy()
{
    if (!m_d) return;
    for (auto& [h, p] : m_vertex)
        if (p && p->code.module) vkDestroyShaderModule(m_d->dev, p->code.module, nullptr);
    for (auto& [h, p] : m_fragment)
        if (p && p->code.module) vkDestroyShaderModule(m_d->dev, p->code.module, nullptr);
    m_vertex.clear();
    m_fragment.clear();
    m_d = nullptr;
}

}
