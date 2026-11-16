#include "vk_pipeline.h"
#include "../rsx_spirv.h"

#include <cstdio>
#include <cstring>

namespace vk {

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
    if (FILE* f = std::fopen(m_cache_path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0) {
            blob.resize((size_t)n);
            if (std::fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
        }
        std::fclose(f);
    }
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
        if (e->extendedDynamicState3AlphaToCoverageEnable && e->extendedDynamicState3AlphaToOneEnable &&
            d.features.features.alphaToOne) {
            m_dynamic_a2c = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
            m_dynamic.push_back(VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT);
        }
        if (e->extendedDynamicState3ProvokingVertexMode && d.caps.provoking_vertex) {
            m_dynamic_provoking = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT);
        }
    }
    return true;
}

void pipelines::save()
{
    if (!m_cache) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(m_d->dev, m_cache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<uint8_t> blob(n);
    if (vkGetPipelineCacheData(m_d->dev, m_cache, &n, blob.data()) != VK_SUCCESS) return;
    const std::string tmp = m_cache_path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        const bool ok = std::fwrite(blob.data(), 1, n, f) == n;
        std::fclose(f);
        if (ok) {
            std::remove(m_cache_path.c_str());
            std::rename(tmp.c_str(), m_cache_path.c_str());
        }
    }
}

void pipelines::destroy()
{
    if (!m_d) return;
    save();
    for (auto& [k, p] : m_map) vkDestroyPipeline(m_d->dev, p, nullptr);
    m_map.clear();
    if (m_cache) vkDestroyPipelineCache(m_d->dev, m_cache, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_d->dev, m_layout, nullptr);
    if (m_set) vkDestroyDescriptorSetLayout(m_d->dev, m_set, nullptr);
    m_d = nullptr;
}

pipeline_key pipelines::normalize(const pipeline_key& key) const
{
    pipeline_key k = key;
    if (m_dynamic_blend) std::memset(k.blend, 0, sizeof k.blend);
    if (m_dynamic_polygon) k.polygon_mode = 0;
    if (m_dynamic_clamp) k.depth_clamp = 0;
    if (m_dynamic_clip) k.depth_clip = 0;
    if (m_dynamic_a2c) k.alpha_to_coverage = k.alpha_to_one = 0;
    if (m_dynamic_provoking) k.provoking_last = 0;
    return k;
}

VkPipeline pipelines::get(const pipeline_key& key, const shader& vs, const shader& fs)
{
    const pipeline_key k = normalize(key);
    if (auto it = m_map.find(k); it != m_map.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs.module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = k.topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k.topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = (VkPolygonMode)k.polygon_mode;
    rs.depthClampEnable = k.depth_clamp;
    rs.lineWidth = 1.0f;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT prov{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT };
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

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.alphaToCoverageEnable = k.alpha_to_coverage;
    ms.alphaToOneEnable = k.alpha_to_one;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

    uint32_t count = 0;
    VkFormat formats[4]{};
    VkPipelineColorBlendAttachmentState att[4]{};
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
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.logicOpEnable = k.logic_enable;
    cb.logicOp = (VkLogicOp)k.logic_op;
    cb.attachmentCount = count;
    cb.pAttachments = att;

    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = (uint32_t)m_dynamic.size();
    dy.pDynamicStates = m_dynamic.data();

    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = count;
    ri.pColorAttachmentFormats = formats;
    const VkFormat depth = (VkFormat)k.depth_format;
    ri.depthAttachmentFormat = depth;
    ri.stencilAttachmentFormat = (depth == VK_FORMAT_D32_SFLOAT_S8_UINT || depth == VK_FORMAT_D24_UNORM_S8_UINT) ? depth : VK_FORMAT_UNDEFINED;

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
    if (vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &p) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] pipeline creation failed\n");
        p = VK_NULL_HANDLE;
    }
    m_map.emplace(k, p);
    return p;
}

}
