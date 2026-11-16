#include "vk_pipeline.h"
#include "vk_shaders.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace vk {

namespace {

thread_local bool g_replay = false;

constexpr VkPipelineCreateFlags k_library_flags =
    VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0u);
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}

bool pipeline_key::operator==(const pipeline_key& o) const
{
    return std::memcmp(this, &o, sizeof *this) == 0;
}

size_t pipeline_key_hash::operator()(const pipeline_key& k) const
{
    const uint64_t* w = reinterpret_cast<const uint64_t*>(&k);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < sizeof k / 8; i++) {
        h ^= w[i];
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return (size_t)h;
}

static_assert(sizeof(pipeline_key) % 8 == 0);

bool pipelines::create(vk_device& d, const std::string& cache_path)
{
    m_d = &d;
    m_cache_path = cache_path;

    std::vector<VkDescriptorSetLayoutBinding> b;
    auto add = [&](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = binding;
        x.descriptorType = type;
        x.descriptorCount = 1;
        x.stageFlags = stages;
        b.push_back(x);
    };
    add(RSX_SPV_BIND_VP_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
    add(RSX_SPV_BIND_VERTEX_LAYOUT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
    add(RSX_SPV_BIND_VERTEX_DATA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
    add(RSX_SPV_BIND_FP_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    add(RSX_SPV_BIND_FP_STATE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t i = 0; i < RSX_SPV_TEXTURE_UNITS; i++)
        add(RSX_SPV_BIND_TEXTURE0 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t i = 0; i < RSX_SPV_VERTEX_TEXTURE_UNITS; i++)
        add(RSX_SPV_BIND_VERTEX_TEXTURE0 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_VERTEX_BIT);
    VkDescriptorSetLayoutCreateInfo si{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    si.bindingCount = (uint32_t)b.size();
    si.pBindings = b.data();
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &si, nullptr, &m_set));
    VkPipelineLayoutCreateInfo li{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 1;
    li.pSetLayouts = &m_set;
    VK_TRY(vkCreatePipelineLayout(d.dev, &li, nullptr, &m_layout));

    std::vector<uint8_t> blob;
    read_file(m_cache_path, blob);
    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(d.dev, &ci, nullptr, &m_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        VK_TRY(vkCreatePipelineCache(d.dev, &ci, nullptr, &m_cache));
    }

    m_dynamic = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
    };
    if (const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT* e = d.dynamic_state3) {
        if (e->extendedDynamicState3ColorBlendEnable && e->extendedDynamicState3ColorBlendEquation &&
            e->extendedDynamicState3ColorWriteMask) {
            m_dynamic_blend = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
            m_dynamic.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
            m_dynamic.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
        }
        if (e->extendedDynamicState3PolygonMode) {
            m_dynamic_polygon = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
        }
        if (e->extendedDynamicState3DepthClampEnable) {
            m_dynamic_clamp = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT);
        }
        if (e->extendedDynamicState3DepthClipEnable && d.caps.depth_clip_enable) {
            m_dynamic_clip = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT);
        }
        if (e->extendedDynamicState3ProvokingVertexMode && d.caps.provoking_vertex) {
            m_dynamic_provoking = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT);
        }
    }

    if (d.caps.pipeline_library) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &gp };
        vkGetPhysicalDeviceProperties2(d.gpu, &p2);
        m_libraries = gp.graphicsPipelineLibraryFastLinking && m_dynamic_blend && m_dynamic_polygon && m_dynamic_clamp &&
                      (m_dynamic_clip || !d.caps.depth_clip_enable) && (m_dynamic_provoking || !d.caps.provoking_vertex);
    }
    if (m_libraries)
        for (uint32_t c = 0; c < 3; c++)
            if (!(m_input[c] = input_library(c))) m_libraries = false;

    const size_t slash = m_cache_path.find_last_of("/\\");
    m_db = std::fopen((m_cache_path.substr(0, slash == std::string::npos ? 0 : slash + 1) + "vk_shaders.db").c_str(), "ab+");
    const unsigned n = std::max(2u, std::thread::hardware_concurrency() / 2u);
    for (unsigned i = 0; i < n; i++) m_workers.emplace_back(&pipelines::worker, this);
    std::fprintf(stderr, "[vk] pipelines: %s, %u compile threads\n", m_libraries ? "libraries with fast link" : "monolithic", n);
    return true;
}

void pipelines::worker()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> hold(m_work_lock);
            m_work_cv.wait(hold, [this] { return m_stop || !m_tasks.empty(); });
            if (m_tasks.empty()) return;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task();
    }
}

void pipelines::work(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> hold(m_work_lock);
        m_tasks.push_back(std::move(task));
    }
    m_work_cv.notify_one();
}

void pipelines::destroy()
{
    if (!m_d) return;
    m_stopping.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> hold(m_work_lock);
        m_stop = true;
    }
    m_work_cv.notify_all();
    for (std::thread& t : m_workers) t.join();
    m_workers.clear();
    size_t n = 0;
    if (vkGetPipelineCacheData(m_d->dev, m_cache, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<uint8_t> blob(n);
        if (vkGetPipelineCacheData(m_d->dev, m_cache, &n, blob.data()) == VK_SUCCESS)
            if (FILE* f = std::fopen((m_cache_path + ".tmp").c_str(), "wb")) {
                const bool ok = std::fwrite(blob.data(), 1, n, f) == n;
                std::fclose(f);
                if (ok) {
                    std::remove(m_cache_path.c_str());
                    std::rename((m_cache_path + ".tmp").c_str(), m_cache_path.c_str());
                }
            }
    }
    for (auto& [k, e] : m_map) {
        const VkPipeline p = e->pipe.load();
        if (p) vkDestroyPipeline(m_d->dev, p, nullptr);
        if (e->fast && e->fast != p) vkDestroyPipeline(m_d->dev, e->fast, nullptr);
    }
    m_map.clear();
    for (auto& [k, p] : m_output) vkDestroyPipeline(m_d->dev, p, nullptr);
    m_output.clear();
    for (VkPipeline& p : m_input)
        if (p) vkDestroyPipeline(m_d->dev, std::exchange(p, VK_NULL_HANDLE), nullptr);
    if (m_db) std::fclose(std::exchange(m_db, nullptr));
    vkDestroyPipelineCache(m_d->dev, m_cache, nullptr);
    vkDestroyPipelineLayout(m_d->dev, m_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_d->dev, m_set, nullptr);
    m_d = nullptr;
}

pipeline_key pipelines::normalize(const pipeline_key& key) const
{
    pipeline_key k = key;
    if (m_dynamic_blend) std::memset(k.blend, 0, sizeof k.blend);
    if (m_dynamic_polygon) k.polygon_mode = 0;
    if (m_dynamic_clamp) k.depth_clamp = 0;
    if (m_dynamic_clip) k.depth_clip = 0;
    if (m_dynamic_provoking) k.provoking_last = 0;
    return k;
}

void pipelines::record(char type, const void* a, size_t an, const void* b, size_t bn)
{
    if (g_replay || !m_db) return;
    const uint32_t sizes[2] = { (uint32_t)an, (uint32_t)bn };
    std::lock_guard<std::mutex> hold(m_db_lock);
    std::fputc(type, m_db);
    std::fwrite(sizes, sizeof sizes, 1, m_db);
    std::fwrite(a, 1, an, m_db);
    if (bn) std::fwrite(b, 1, bn, m_db);
    std::fflush(m_db);
}

void pipelines::record_vertex(const uint32_t* program, const rsx_spv_vp_key& key)
{
    record('V', &key, sizeof key, program, RSX_SPV_VP_SLOTS * 16u);
}

void pipelines::record_fragment(const uint32_t* ucode, uint32_t words, const rsx_spv_fp_key& key)
{
    record('F', &key, sizeof key, ucode, (size_t)words * 4u);
}

void pipelines::precompile(shader_cache& shaders)
{
    if (!m_db) return;
    std::fflush(m_db);
    std::fseek(m_db, 0, SEEK_END);
    const long size = std::ftell(m_db);
    std::fseek(m_db, 0, SEEK_SET);
    auto data = std::make_shared<std::vector<uint8_t>>(size > 0 ? (size_t)size : 0u);
    if (!data->empty() && std::fread(data->data(), 1, data->size(), m_db) != data->size()) data->clear();
    std::fseek(m_db, 0, SEEK_END);
    work([this, &shaders, data] {
        struct rec { char type; const uint8_t* a; uint32_t an; const uint8_t* b; uint32_t bn; };
        std::vector<rec> recs;
        for (size_t at = 0; at + 9 <= data->size();) {
            rec r{ (char)(*data)[at] };
            std::memcpy(&r.an, data->data() + at + 1, 4);
            std::memcpy(&r.bn, data->data() + at + 5, 4);
            if (at + 9 + (size_t)r.an + r.bn > data->size()) break;
            r.a = data->data() + at + 9;
            r.b = r.a + r.an;
            recs.push_back(r);
            at += 9 + (size_t)r.an + r.bn;
        }
        auto pending = std::make_shared<std::atomic<size_t>>(0);
        auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
        auto run_all = [&](char type, auto&& body) {
            for (const rec& r : recs) {
                if (r.type != type) continue;
                pending->fetch_add(1);
                work([this, r, body, pending, done] {
                    g_replay = true;
                    if (!m_stopping.load(std::memory_order_relaxed)) body(r);
                    g_replay = false;
                    if (pending->fetch_sub(1) == 1) {
                        std::lock_guard<std::mutex> hold(done->first);
                        done->second.notify_all();
                    }
                });
            }
            std::unique_lock<std::mutex> hold(done->first);
            done->second.wait(hold, [&] { return pending->load() == 0; });
        };
        auto shader_body = [&shaders](const rec& r) {
            if (r.type == 'V' && r.an == sizeof(rsx_spv_vp_key) && r.bn == RSX_SPV_VP_SLOTS * 16u) {
                rsx_spv_vp_key key;
                std::memcpy(&key, r.a, sizeof key);
                std::vector<uint32_t> program(RSX_SPV_VP_SLOTS * 4u);
                std::memcpy(program.data(), r.b, r.bn);
                shaders.vertex(program.data(), key, shader_cache::program_hash(program.data()), false);
            } else if (r.type == 'F' && r.an == sizeof(rsx_spv_fp_key) && r.bn % 4u == 0) {
                rsx_spv_fp_key key;
                std::memcpy(&key, r.a, sizeof key);
                std::vector<uint32_t> ucode(r.bn / 4u);
                std::memcpy(ucode.data(), r.b, r.bn);
                shaders.fragment(ucode.data(), (uint32_t)ucode.size(), key, false);
            }
        };
        run_all('V', shader_body);
        run_all('F', shader_body);
        run_all('P', [this, &shaders](const rec& r) {
            if (r.an != sizeof(pipeline_key)) return;
            pipeline_key k;
            std::memcpy(&k, r.a, sizeof k);
            const vertex_program* vs = shaders.find_vertex(k.vs);
            const fragment_program* fs = shaders.find_fragment(k.fs);
            if (vs && fs) get(k, vs->code, fs->code);
        });
        std::fprintf(stderr, "[vk] precompiled %zu shaders and pipelines\n", recs.size());
    });
}

void pipelines::fill_raster(const pipeline_key& k, VkPipelineRasterizationStateCreateInfo& rs, VkPipelineRasterizationDepthClipStateCreateInfoEXT& clip,
                            VkPipelineRasterizationProvokingVertexStateCreateInfoEXT& prov) const
{
    rs.polygonMode = (VkPolygonMode)k.polygon_mode;
    rs.depthClampEnable = k.depth_clamp;
    rs.lineWidth = 1.0f;
    const void** tail = const_cast<const void**>(&rs.pNext);
    if (m_d->caps.depth_clip_enable && !m_dynamic_clip) {
        clip.depthClipEnable = k.depth_clip;
        *tail = &clip;
        tail = const_cast<const void**>(&clip.pNext);
    }
    if (m_d->caps.provoking_vertex && !m_dynamic_provoking) {
        prov.provokingVertexMode = k.provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
        *tail = &prov;
    }
}

void pipelines::fill_output(const pipeline_key& k, VkPipelineColorBlendStateCreateInfo& cb, VkPipelineColorBlendAttachmentState* att,
                            VkPipelineRenderingCreateInfo& ri, VkFormat* formats) const
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < 4; i++) {
        formats[i] = (VkFormat)k.color_format[i];
        if (formats[i] != VK_FORMAT_UNDEFINED) count = i + 1u;
        const blend_attachment& a = k.blend[i];
        att[i].blendEnable = a.enable;
        att[i].srcColorBlendFactor = (VkBlendFactor)a.src_color;
        att[i].dstColorBlendFactor = (VkBlendFactor)a.dst_color;
        att[i].colorBlendOp = (VkBlendOp)a.color_op;
        att[i].srcAlphaBlendFactor = (VkBlendFactor)a.src_alpha;
        att[i].dstAlphaBlendFactor = (VkBlendFactor)a.dst_alpha;
        att[i].alphaBlendOp = (VkBlendOp)a.alpha_op;
        att[i].colorWriteMask = a.write_mask;
    }
    cb.logicOpEnable = k.logic_enable;
    cb.logicOp = (VkLogicOp)k.logic_op;
    cb.attachmentCount = count;
    cb.pAttachments = att;
    ri.colorAttachmentCount = count;
    ri.pColorAttachmentFormats = formats;
    const VkFormat depth = (VkFormat)k.depth_format;
    ri.depthAttachmentFormat = depth;
    ri.stencilAttachmentFormat = (depth == VK_FORMAT_D32_SFLOAT_S8_UINT || depth == VK_FORMAT_D24_UNORM_S8_UINT) ? depth : VK_FORMAT_UNDEFINED;
}

VkPipeline pipelines::monolithic(const pipeline_key& k, const shader& vs, const shader& fs) const
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs.module, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs.module, "main" };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = k.topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k.topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT prov{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT };
    fill_raster(k, rs, clip, prov);
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState att[4]{};
    VkFormat formats[4]{};
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    fill_output(k, cb, att, ri, formats);
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = (uint32_t)m_dynamic.size();
    dy.pDynamicStates = m_dynamic.data();
    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = m_layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

bool pipelines::shader_library(shader& s, VkShaderStageFlagBits stage)
{
    if (!m_libraries) return true;
    const bool vertex = stage == VK_SHADER_STAGE_VERTEX_BIT;
    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineLibraryCreateInfoEXT gl{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &ri };
    gl.flags = vertex ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    const VkPipelineShaderStageCreateInfo st{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, stage, s.module, "main" };
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = (uint32_t)m_dynamic.size();
    dy.pDynamicStates = m_dynamic.data();
    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &gl };
    pi.flags = k_library_flags;
    pi.stageCount = 1;
    pi.pStages = &st;
    if (vertex) {
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
    } else {
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
    }
    pi.pDynamicState = &dy;
    pi.layout = m_layout;
    VK_TRY(vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &s.library));
    return true;
}

VkPipeline pipelines::input_library(uint32_t topology_class)
{
    VkGraphicsPipelineLibraryCreateInfoEXT gl{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT };
    gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                      : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = (uint32_t)m_dynamic.size();
    dy.pDynamicStates = m_dynamic.data();
    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &gl };
    pi.flags = k_library_flags;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pDynamicState = &dy;
    VkPipeline p = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

VkPipeline pipelines::output_library(const pipeline_key& k)
{
    pipeline_key o{};
    std::memcpy(o.color_format, k.color_format, sizeof o.color_format);
    o.depth_format = k.depth_format;
    o.logic_enable = k.logic_enable;
    o.logic_op = k.logic_op;
    std::lock_guard<std::mutex> hold(m_lib_lock);
    if (auto it = m_output.find(o); it != m_output.end()) return it->second;
    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineLibraryCreateInfoEXT gl{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, &ri };
    gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    VkPipelineColorBlendAttachmentState att[4]{};
    VkFormat formats[4]{};
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    fill_output(o, cb, att, ri, formats);
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = (uint32_t)m_dynamic.size();
    dy.pDynamicStates = m_dynamic.data();
    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &gl };
    pi.flags = k_library_flags;
    pi.pColorBlendState = &cb;
    pi.pMultisampleState = &ms;
    pi.pDynamicState = &dy;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    m_output.emplace(o, p);
    return p;
}

VkPipeline pipelines::link(const pipeline_key& k, const shader& vs, const shader& fs, bool optimized)
{
    const VkPipeline out = output_library(k);
    if (!out || !vs.library || !fs.library) return VK_NULL_HANDLE;
    const VkPipeline libs[4] = { m_input[k.topology_class < 3 ? k.topology_class : 2], vs.library, fs.library, out };
    VkPipelineLibraryCreateInfoKHR li{ VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
    li.libraryCount = 4;
    li.pLibraries = libs;
    VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li };
    pi.flags = optimized ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    pi.layout = m_layout;
    VkPipeline p = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

VkPipeline pipelines::get(const pipeline_key& key, const shader& vs, const shader& fs)
{
    const pipeline_key k = normalize(key);
    entry* e;
    bool owner;
    {
        std::lock_guard<std::mutex> hold(m_map_lock);
        auto [it, inserted] = m_map.try_emplace(k);
        if (inserted) it->second = std::make_unique<entry>();
        e = it->second.get();
        owner = inserted;
    }
    if (!owner) {
        if (!e->done.load(std::memory_order_acquire)) {
            if (!g_replay) return VK_NULL_HANDLE;
            e->done.wait(false, std::memory_order_acquire);
        }
        return e->pipe.load(std::memory_order_acquire);
    }
    record('P', &k, sizeof k, nullptr, 0);
    if (!m_libraries && !g_replay) {
        work([this, k, v = vs, f = fs, e] {
            const VkPipeline p = m_stopping.load(std::memory_order_relaxed) ? VK_NULL_HANDLE : monolithic(k, v, f);
            e->pipe.store(p, std::memory_order_release);
            e->done.store(true, std::memory_order_release);
            e->done.notify_all();
        });
        return VK_NULL_HANDLE;
    }
    const bool fast = m_libraries && !g_replay;
    const VkPipeline p = !m_libraries ? monolithic(k, vs, fs) : link(k, vs, fs, !fast);
    if (!p) std::fprintf(stderr, "[vk] pipeline creation failed\n");
    e->fast = fast ? p : VK_NULL_HANDLE;
    e->pipe.store(p, std::memory_order_release);
    e->done.store(true, std::memory_order_release);
    e->done.notify_all();
    if (fast && p)
        work([this, k, v = vs, f = fs, e] {
            if (m_stopping.load(std::memory_order_relaxed)) return;
            if (VkPipeline opt = link(k, v, f, true)) e->pipe.store(opt, std::memory_order_release);
        });
    return p;
}

}