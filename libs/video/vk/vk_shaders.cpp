#include "vk_shaders.h"

#include <algorithm>
#include <atomic>
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

uint64_t shader_cache::program_hash(const uint32_t* program, uint32_t start)
{
    uint8_t reach[RSX_SPV_VP_SLOTS];
    const int dynamic = rsx_spv_vertex_reach(program, start, reach);
    if (dynamic) return hash(program, RSX_SPV_VP_SLOTS * 16u, 0x5650u);
    uint64_t h = 0x5650u ^ start;
    for (uint32_t base = 0; base < RSX_SPV_VP_SLOTS; base += 8u) {
        uint64_t w;
        std::memcpy(&w, reach + base, 8u);
        if (!w) continue;
        for (uint32_t j = 0; j < 8u; j++) {
            if (reach[base + j]) {
                const uint32_t pc = base + j;
                h = hash(program + pc * 4u, 16u, h ^ pc);
            }
        }
    }
    return h;
}


bool shader_cache::make_module(shader& s, const rsx_spv_blob& blob, VkShaderStageFlagBits stage)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = (size_t)blob.count * 4u;
    ci.pCode = blob.words;
    VK_TRY(vkCreateShaderModule(m_d->dev, &ci, nullptr, &s.module));
    if (!m_pipes->shader_library(s, stage)) {
        vkDestroyShaderModule(m_d->dev, s.module, nullptr);
        s.module = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

template <typename T, typename Job>
const T* shader_cache::resolve(std::unordered_map<uint64_t, std::unique_ptr<T>>& map, uint64_t h, bool async, Job&& job)
{
    auto finish = [this, &map, h](std::unique_ptr<T> made) {
        std::lock_guard<std::mutex> hold(m_lock);
        const T* p = map.emplace(h, std::move(made)).first->second.get();
        m_building.erase(h);
        m_built.notify_all();
        return p;
    };
    {
        std::unique_lock<std::mutex> hold(m_lock);
        if (auto it = map.find(h); it != map.end()) return it->second.get();
        if (async) {
            if (!m_building.insert(h).second) return nullptr;
            hold.unlock();
            m_pipes->work([finish, build = job()]() mutable { finish(build()); }, true);
            return nullptr;
        }
        m_built.wait(hold, [&] { return !m_building.count(h); });
        if (auto it = map.find(h); it != map.end()) return it->second.get();
        m_building.insert(h);
    }
    return finish(job()());
}

const vertex_program* shader_cache::vertex(const uint32_t* program_in, const rsx_spv_vp_key& key, uint64_t program_hash, bool async)
{
    rsx_spv_vp_key mk = key;
    uint32_t written = 0, texmask = 0;
    if (rsx_spv_vertex_scan(program_in, mk.start, &written, &texmask) == 0) {
        uint32_t used_out = 0;
        if (written & ((1u << 1) | (1u << 3))) used_out |= RSX_SPV_VPOUT_FRONTDIFFUSE | RSX_SPV_VPOUT_BACKDIFFUSE;
        if (written & ((1u << 2) | (1u << 4))) used_out |= RSX_SPV_VPOUT_FRONTSPECULAR | RSX_SPV_VPOUT_BACKSPECULAR;
        if (written & (1u << 5)) used_out |= RSX_SPV_VPOUT_FOG | (7u * RSX_SPV_VPOUT_UC0);
        if (written & (1u << 6)) used_out |= RSX_SPV_VPOUT_TEX9 | RSX_SPV_VPOUT_POINTSIZE | (0x38u * RSX_SPV_VPOUT_UC0);
        for (uint32_t t = 0; t < 8u; t++)
            if (written & (1u << (7u + t))) used_out |= RSX_SPV_VPOUT_TEX0 << t;
        if (written & (1u << 15)) used_out |= RSX_SPV_VPOUT_TEX8;
        mk.output_mask &= used_out;
        if (mk.output_mask & (RSX_SPV_VPOUT_FRONTDIFFUSE | RSX_SPV_VPOUT_BACKDIFFUSE))
            mk.output_mask = (mk.output_mask & ~RSX_SPV_VPOUT_BACKDIFFUSE) | RSX_SPV_VPOUT_FRONTDIFFUSE;
        if (mk.output_mask & (RSX_SPV_VPOUT_FRONTSPECULAR | RSX_SPV_VPOUT_BACKSPECULAR))
            mk.output_mask = (mk.output_mask & ~RSX_SPV_VPOUT_BACKSPECULAR) | RSX_SPV_VPOUT_FRONTSPECULAR;
        uint32_t vdim_mask = 0;
        for (uint32_t u = 0; u < 4u; u++)
            if (texmask & (1u << u)) vdim_mask |= 3u << (u * 2u);
        mk.texture_dims &= vdim_mask;
    }
    const uint64_t h = hash(&mk, sizeof mk, program_hash);
    return resolve(m_vertex, h, async, [&] { return [this, h, mk, words = std::vector<uint32_t>(program_in, program_in + RSX_SPV_VP_SLOTS * 4u)]() -> std::unique_ptr<vertex_program> {
        const uint32_t* program = words.data();
        auto vp = std::make_unique<vertex_program>();
        vp->hash = h;
        const compile_guard guard;
        rsx_spv_blob blob{};
        const bool ok = rsx_spv_vertex(program, &mk, &blob, &vp->info) == 0 && make_module(vp->code, blob, VK_SHADER_STAGE_VERTEX_BIT);
        rsx_spv_blob_free(&blob);
        if (!ok) {
            std::fprintf(stderr, "[vk] vertex program at slot %u failed to translate\n", mk.start);
            return nullptr;
        }
        m_pipes->record_vertex(program, mk);
        return vp;
    }; });
}

const fragment_program* shader_cache::fragment(const uint32_t* ucode, uint32_t max_words, const rsx_spv_fp_key& key, bool async,
                                               uint32_t alphakill_units)
{
    rsx_spv_fp_info scan{};
    const uint32_t end = rsx_spv_fragment_scan(ucode, max_words, &scan) == 0 ? std::min(scan.ucode_words, max_words) : 0;
    if (!end) {
        static std::atomic<int> reported{ 0 };
        if (reported.fetch_add(1) < 8 && max_words >= 4)
            std::fprintf(stderr, "[vk] fragment program scan failed: %08X %08X %08X %08X\n", ucode[0], ucode[1], ucode[2], ucode[3]);
        return nullptr;
    }
    rsx_spv_fp_key mk = key;
    const u32 ctl = mk.control;
    mk.control = ((ctl & RSX_SPV_FPCTL_DEPTH_EXPORT) ? RSX_SPV_FPCTL_DEPTH_EXPORT : 0u) |
                 (ctl & RSX_SPV_FPCTL_32BIT_EXPORTS) |
                 ((scan.input_mask & 6u) ? (ctl & RSX_SPV_FPCTL_FLAT_SHADING) : 0u);
    uint32_t dim_mask = 0u;
    for (uint32_t u = 0; u < 16u; u++)
        if (scan.texture_mask & (1u << u)) dim_mask |= 3u << (u * 2u);
    mk.texture_dims &= dim_mask;
    mk.texture_shadow &= scan.texture_mask;
    if ((mk.output_mask & RSX_SPV_FPOUT_ALPHAKILL) && !(alphakill_units & scan.texture_mask))
        mk.output_mask &= ~RSX_SPV_FPOUT_ALPHAKILL;

    thread_local std::vector<uint32_t> scratch;
    scratch.assign(ucode, ucode + end);
    for (uint32_t i = 0; i < scan.constant_count; i++)
        for (uint32_t w = 0; w < 4 && scan.constant_word[i] + w < end; w++) scratch[scan.constant_word[i] + w] = 0;
    const uint64_t h = hash(scratch.data(), scratch.size() * 4u, hash(&mk, sizeof mk, 0x46504650u));
    return resolve(m_fragment, h, async, [&] { return [this, h, mk, words = scratch]() -> std::unique_ptr<fragment_program> {
        const uint32_t end = (uint32_t)words.size();
        auto fp = std::make_unique<fragment_program>();
        fp->hash = h;
        const compile_guard guard;
        rsx_spv_blob blob{};
        const bool ok = rsx_spv_fragment(words.data(), end, &mk, &blob, &fp->info) == 0 && make_module(fp->code, blob, VK_SHADER_STAGE_FRAGMENT_BIT);
        rsx_spv_blob_free(&blob);
        if (!ok) {
            std::fprintf(stderr, "[vk] fragment program failed to translate\n");
            return nullptr;
        }
        m_pipes->record_fragment(words.data(), end, mk);
        return fp;
    }; });
}

const vertex_program* shader_cache::find_vertex(uint64_t hash)
{
    std::unique_lock<std::mutex> hold(m_lock);
    m_built.wait(hold, [&] { return !m_building.count(hash); });
    auto it = m_vertex.find(hash);
    return it != m_vertex.end() ? it->second.get() : nullptr;
}

const fragment_program* shader_cache::find_fragment(uint64_t hash)
{
    std::unique_lock<std::mutex> hold(m_lock);
    m_built.wait(hold, [&] { return !m_building.count(hash); });
    auto it = m_fragment.find(hash);
    return it != m_fragment.end() ? it->second.get() : nullptr;
}

}
