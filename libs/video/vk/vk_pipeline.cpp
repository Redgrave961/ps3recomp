#include "vk_pipeline.h"
#include "vk_shaders.h"
#include "ps3emu/thread_name.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace vk {

namespace {

thread_local bool g_replay = false;
thread_local unsigned t_compiling = 0;
std::string g_cache_files[2];
std::atomic<bool> g_dropped{ false };

constexpr unsigned k_urgent_workers = 2;

constexpr char k_db_magic[8] = { 'V', 'K', 'S', 'H', 'A', 'D', 'E', 'R' };
constexpr uint32_t k_db_version = 1;

struct db_header {
    char magic[8];
    uint64_t features;
    uint32_t version;
    uint32_t vendor;
    uint32_t device;
    uint32_t driver_version;
    uint32_t driver_id;
    uint32_t api_version;
    uint8_t cache_uuid[VK_UUID_SIZE];
};
static_assert(sizeof(db_header) == 56);

bool format_has(VkPhysicalDevice gpu, VkFormat f, VkFormatFeatureFlags need)
{
    if (f == VK_FORMAT_UNDEFINED) return true;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

void lowest_priority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
    setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
#endif
}

constexpr VkPipelineCreateFlags k_library_flags =
    VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;

FILE* open_utf8(const std::string& path, const char* mode)
{
#ifdef _WIN32
    int nw = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (nw <= 0) return nullptr;
    std::vector<wchar_t> wp((size_t)nw);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), nw);
    wchar_t wm[16] = {};
    for (size_t i = 0; mode[i] && i < 15; i++) wm[i] = (wchar_t)(unsigned char)mode[i];
    return _wfopen(wp.data(), wm);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool replace_file_utf8(const std::string& src, const std::string& dst)
{
#ifdef _WIN32
    auto to_wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> w(n > 0 ? (size_t)n : 1u, 0);
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    auto ws = to_wide(src), wd = to_wide(dst);
    return MoveFileExW(ws.data(), wd.data(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(src.c_str(), dst.c_str()) == 0;
#endif
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = open_utf8(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0u);
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

void truncate_utf8(const std::string& path)
{
#ifdef _WIN32
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w, 2048) <= 0) return;
    const HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, TRUNCATE_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
    const int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    if (fd >= 0) close(fd);
#endif
}

}

compile_guard::compile_guard() { t_compiling++; }

compile_guard::~compile_guard() { t_compiling--; }

void drop_caches_if_compiling()
{
    if (!t_compiling) return;
    g_dropped.store(true, std::memory_order_relaxed);
    for (const std::string& f : g_cache_files)
        if (!f.empty()) truncate_utf8(f);
    std::fprintf(stderr, "[vk] failed while compiling shaders: shader caches discarded, they rebuild on the next launch\n");
}

bool pipelines::create(vk_device& d, const std::string& cache_path)
{
    m_d = &d;
    m_cache_path = cache_path;

    std::vector<VkDescriptorSetLayoutBinding> buffers, textures;
    auto add = [](std::vector<VkDescriptorSetLayoutBinding>& b, uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = binding;
        x.descriptorType = type;
        x.descriptorCount = 1;
        x.stageFlags = stages;
        b.push_back(x);
    };
    add(buffers, RSX_SPV_BIND_VP_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
    add(buffers, RSX_SPV_BIND_VERTEX_LAYOUT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_VERTEX_BIT);
    add(buffers, RSX_SPV_BIND_VERTEX_DATA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
    add(buffers, RSX_SPV_BIND_FP_CONSTANTS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
    add(buffers, RSX_SPV_BIND_FP_STATE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t i = 0; i < RSX_SPV_TEXTURE_UNITS; i++)
        add(textures, RSX_SPV_BIND_TEXTURE0 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t i = 0; i < RSX_SPV_VERTEX_TEXTURE_UNITS; i++)
        add(textures, RSX_SPV_BIND_VERTEX_TEXTURE0 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_VERTEX_BIT);
    VkDescriptorSetLayoutCreateInfo si{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    si.bindingCount = (uint32_t)buffers.size();
    si.pBindings = buffers.data();
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &si, nullptr, &m_sets[RSX_SPV_SET_BUFFERS]));
    si.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    si.bindingCount = (uint32_t)textures.size();
    si.pBindings = textures.data();
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &si, nullptr, &m_sets[RSX_SPV_SET_TEXTURES]));
    VkPipelineLayoutCreateInfo li{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.setLayoutCount = 2;
    li.pSetLayouts = m_sets;
    VK_TRY(vkCreatePipelineLayout(d.dev, &li, nullptr, &m_layout));

    const size_t slash = m_cache_path.find_last_of("/\\");
    m_db_path = m_cache_path.substr(0, slash == std::string::npos ? 0 : slash + 1) + "vk_shaders.db";
    g_cache_files[0] = m_cache_path;
    g_cache_files[1] = m_db_path;

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
        if (e->extendedDynamicState3LogicOpEnable && d.dynamic_state2 && d.dynamic_state2->extendedDynamicState2LogicOp &&
            d.features.features.logicOp) {
            m_dynamic_logic = true;
            m_dynamic.push_back(VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT);
            m_dynamic.push_back(VK_DYNAMIC_STATE_LOGIC_OP_EXT);
        }
    }

    std::vector<uint8_t> blob;
    if (open_db()) read_file(m_cache_path, blob);
    else truncate_utf8(m_cache_path);
    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    {
        const compile_guard guard;
        if (vkCreatePipelineCache(d.dev, &ci, nullptr, &m_cache) != VK_SUCCESS) {
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            VK_TRY(vkCreatePipelineCache(d.dev, &ci, nullptr, &m_cache));
        }
    }

    m_saved = std::chrono::steady_clock::now();

    if (d.caps.pipeline_library) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &gp };
        vkGetPhysicalDeviceProperties2(d.gpu, &p2);
        m_libraries = gp.graphicsPipelineLibraryFastLinking && m_dynamic_blend && m_dynamic_polygon && m_dynamic_clamp &&
                      (m_dynamic_clip || !d.caps.depth_clip_enable);
    }
    if (m_libraries) {
        for (uint32_t c = 0; c < 3; c++)
            if (!(m_input[c] = input_library(c))) m_libraries = false;
    }
    if (m_libraries) {
        const VkFormat colors[] = { VK_FORMAT_UNDEFINED, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R32_SFLOAT,
                                    VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };
        const VkFormat depths[] = { VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT };
        for (VkFormat cf : colors) {
            if (!format_has(d.gpu, cf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) continue;
            for (VkFormat df : depths) {
                if (cf == VK_FORMAT_UNDEFINED && df == VK_FORMAT_UNDEFINED) continue;
                if (!format_has(d.gpu, df, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;
                pipeline_key ok{};
                ok.color_format[0] = cf;
                ok.depth_format = df;
                output_library(ok);
            }
        }
        const VkFormat mrts[][4] = {
            { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED },
            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED },
            { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED },
            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED },
        };
        for (const auto& m : mrts) {
            bool all_ok = true;
            for (VkFormat cf : m) all_ok = all_ok && format_has(d.gpu, cf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
            if (!all_ok) continue;
            for (VkFormat df : depths) {
                if (!format_has(d.gpu, df, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;
                pipeline_key ok{};
                for (uint32_t i = 0; i < 4; i++) ok.color_format[i] = m[i];
                ok.depth_format = df;
                output_library(ok);
            }
        }
    }

    const unsigned n = std::max(k_urgent_workers + 1u, std::thread::hardware_concurrency());
    m_background_limit = d.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2u : n - k_urgent_workers;
    for (unsigned i = 0; i < n; i++)
        std::thread([this, i] {
            const bool urgent_only = i < k_urgent_workers;
            ps3_thread_name(urgent_only ? "vk pipeline urgent %u" : "vk pipeline %u", i);
            if (!urgent_only) lowest_priority();
            worker(urgent_only);
        }).detach();
    std::fprintf(stderr, "[vk] pipelines: %s, %u urgent + %u background compile threads\n", m_libraries ? "libraries with fast link" : "monolithic",
                 k_urgent_workers, n - k_urgent_workers);
    return true;
}

void pipelines::worker(bool urgent_only)
{
    for (;;) {
        std::function<void()> task;
        bool urgent;
        {
            std::unique_lock<std::mutex> hold(m_work_lock);
            const auto overflow = [this] { return !m_urgent.empty() && m_urgent_busy >= k_urgent_workers; };
            if (urgent_only)
                m_urgent_cv.wait(hold, [this] { return !m_urgent.empty(); });
            else
                m_work_cv.wait(hold, [&] { return overflow() || (!m_background.empty() && m_background_running < m_background_limit); });
            urgent = urgent_only || overflow();
            std::deque<std::function<void()>>& q = urgent ? m_urgent : m_background;
            task = std::move(q.front());
            q.pop_front();
            (urgent ? m_urgent_busy : m_background_running)++;
        }
        task();
        if (!urgent_only) save_cache_periodic();
        {
            std::lock_guard<std::mutex> hold(m_work_lock);
            (urgent ? m_urgent_busy : m_background_running)--;
        }
        if (!urgent) m_work_cv.notify_one();
    }
}

void pipelines::work(std::function<void()> task, bool urgent)
{
    bool overflow;
    {
        std::lock_guard<std::mutex> hold(m_work_lock);
        (urgent ? m_urgent : m_background).push_back(std::move(task));
        overflow = urgent && m_urgent_busy >= k_urgent_workers;
    }
    if (urgent) m_urgent_cv.notify_one();
    if (!urgent || overflow) m_work_cv.notify_one();
}

void pipelines::save_cache_periodic()
{
    if (!m_cache_dirty.load(std::memory_order_relaxed)) return;
    {
        std::unique_lock<std::mutex> hold(m_save_lock, std::try_to_lock);
        if (!hold.owns_lock() || std::chrono::steady_clock::now() - m_saved < std::chrono::seconds(30)) return;
    }
    save_cache();
}

void pipelines::save_cache()
{
    if (g_dropped.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> hold(m_save_lock);
    if (m_db) {
        std::lock_guard<std::mutex> db_hold(m_db_lock);
        std::fflush(m_db);
    }
    if (!m_cache_dirty.exchange(false, std::memory_order_relaxed)) return;
    m_saved = std::chrono::steady_clock::now();
    std::vector<uint8_t> blob;
    VkResult res = VK_INCOMPLETE;
    for (int tries = 0; tries < 4 && res == VK_INCOMPLETE; tries++) {
        size_t n = 0;
        if (vkGetPipelineCacheData(m_d->dev, m_cache, &n, nullptr) != VK_SUCCESS || !n) {
            m_cache_dirty.store(true, std::memory_order_relaxed);
            return;
        }
        blob.resize(n);
        res = vkGetPipelineCacheData(m_d->dev, m_cache, &n, blob.data());
        if (res == VK_SUCCESS) blob.resize(n);
    }
    if (res != VK_SUCCESS || blob.empty()) {
        m_cache_dirty.store(true, std::memory_order_relaxed);
        return;
    }
    const std::string tmp = m_cache_path + ".tmp";
    FILE* f = open_utf8(tmp, "wb");
    if (!f) {
        m_cache_dirty.store(true, std::memory_order_relaxed);
        return;
    }
    const bool ok = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    std::fclose(f);
    if (!ok || !replace_file_utf8(tmp, m_cache_path)) {
        m_cache_dirty.store(true, std::memory_order_relaxed);
    }
}

pipeline_key pipelines::normalize(const pipeline_key& key) const{
    pipeline_key k = key;
    if (m_dynamic_blend) {
        std::memset(k.blend, 0, sizeof k.blend);
    } else {
        for (blend_attachment& b : k.blend) {
            if (!b.enable) {
                b.src_color = VK_BLEND_FACTOR_ZERO;
                b.dst_color = VK_BLEND_FACTOR_ZERO;
                b.color_op = VK_BLEND_OP_ADD;
                b.src_alpha = VK_BLEND_FACTOR_ZERO;
                b.dst_alpha = VK_BLEND_FACTOR_ZERO;
                b.alpha_op = VK_BLEND_OP_ADD;
            }
        }
    }
    if (m_dynamic_polygon) k.polygon_mode = 0;
    if (m_dynamic_clamp) k.depth_clamp = 0;
    if (m_dynamic_clip) k.depth_clip = 0;
    if (m_dynamic_logic) {
        k.logic_enable = 0;
        k.logic_op = 0;
    }
    k.pad = 0;
    return k;
}

bool pipelines::open_db()
{
    db_header want{};
    std::memcpy(want.magic, k_db_magic, sizeof want.magic);
    const uint32_t features[] = { m_dynamic_blend, m_dynamic_polygon, m_dynamic_clamp, m_dynamic_clip, m_dynamic_logic,
                                  m_d->caps.depth_clip_enable, m_d->caps.provoking_vertex, m_d->caps.rgba16_unorm,
                                  (uint32_t)sizeof(pipeline_key), (uint32_t)sizeof(rsx_spv_vp_key), (uint32_t)sizeof(rsx_spv_fp_key),
                                  RSX_SPV_VP_SLOTS };
    want.features = shader_cache::hash(features, sizeof features, k_db_version);
    want.version = k_db_version;
    want.vendor = m_d->props.vendorID;
    want.device = m_d->props.deviceID;
    want.driver_version = m_d->props.driverVersion;
    want.driver_id = (uint32_t)m_d->driver.driverID;
    want.api_version = m_d->props.apiVersion;
    std::memcpy(want.cache_uuid, m_d->props.pipelineCacheUUID, VK_UUID_SIZE);
    bool stale = false, kept = false;
    if (FILE* f = open_utf8(m_db_path, "rb")) {
        db_header have{};
        if (std::fread(&have, sizeof have, 1, f) == 1) {
            kept = !std::memcmp(&have, &want, sizeof want);
            stale = !kept;
        }
        std::fclose(f);
    }
    if (!kept) {
        if (stale) std::fprintf(stderr, "[vk] shader cache does not match this GPU, driver or build, rebuilding it\n");
        if (FILE* f = open_utf8(m_db_path, "wb")) {
            std::fwrite(&want, sizeof want, 1, f);
            std::fclose(f);
        }
    }
    m_db = open_utf8(m_db_path, "ab+");
    return kept;
}

bool pipelines::usable(const pipeline_key& k) const
{
    const pipeline_key n = normalize(k);
    if (std::memcmp(&n, &k, sizeof k) || k.topology_class > 2u || k.polygon_mode > (uint32_t)VK_POLYGON_MODE_POINT) return false;
    const VkPhysicalDeviceFeatures& f = m_d->features.features;
    if ((k.polygon_mode != (uint32_t)VK_POLYGON_MODE_FILL && !f.fillModeNonSolid) || (k.depth_clamp && !f.depthClamp) ||
        (k.logic_enable && !f.logicOp))
        return false;
    for (uint32_t i = 0; i < 4; i++) {
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | (k.blend[i].enable ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
        if (!format_has(m_d->gpu, (VkFormat)k.color_format[i], need)) return false;
    }
    return format_has(m_d->gpu, (VkFormat)k.depth_format, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

void pipelines::record(char type, const void* a, size_t an, const void* b, size_t bn)
{
    if (g_replay || !m_db || g_dropped.load(std::memory_order_relaxed)) return;
    const uint32_t sizes[2] = { (uint32_t)an, (uint32_t)bn };
    std::lock_guard<std::mutex> hold(m_db_lock);
    std::fputc(type, m_db);
    std::fwrite(sizes, sizeof sizes, 1, m_db);
    std::fwrite(a, 1, an, m_db);
    if (bn) std::fwrite(b, 1, bn, m_db);
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
    struct rec { char type; const uint8_t* a; uint32_t an; const uint8_t* b; uint32_t bn; };
    auto data = std::make_shared<std::vector<uint8_t>>();
    auto recs = std::make_shared<std::vector<rec>>();
    {
        std::lock_guard<std::mutex> db_hold(m_db_lock);
        std::fflush(m_db);
        std::fseek(m_db, 0, SEEK_END);
        const long size = std::ftell(m_db);
        std::fseek(m_db, 0, SEEK_SET);
        data->resize(size > 0 ? (size_t)size : 0u);
        if (!data->empty() && std::fread(data->data(), 1, data->size(), m_db) != data->size()) data->clear();
        std::fseek(m_db, 0, SEEK_END);
        const auto usable_key = [this](const uint8_t* raw) {
            pipeline_key k;
            std::memcpy(&k, raw, sizeof k);
            return usable(k);
        };
        std::unordered_set<uint64_t> seen;
        size_t total_raw = 0;
        size_t at = sizeof(db_header);
        while (at + 9 <= data->size()) {
            rec r{ (char)(*data)[at] };
            std::memcpy(&r.an, data->data() + at + 1, 4);
            std::memcpy(&r.bn, data->data() + at + 5, 4);
            if (at + 9 + (size_t)r.an + r.bn > data->size()) break;
            r.a = data->data() + at + 9;
            r.b = r.a + r.an;
            at += 9 + (size_t)r.an + r.bn;
            total_raw++;
            const bool valid =
                (r.type == 'V' && r.an == sizeof(rsx_spv_vp_key) && r.bn == RSX_SPV_VP_SLOTS * 16u) ||
                (r.type == 'F' && r.an == sizeof(rsx_spv_fp_key) && r.bn % 4u == 0) ||
                (r.type == 'P' && r.an == sizeof(pipeline_key) && r.bn == 0 && usable_key(r.a));
            if (!valid) continue;
            uint64_t h = shader_cache::hash(r.a, r.an, (uint64_t)(unsigned char)r.type * 0x9E3779B97F4A7C15ull);
            if (r.bn) h = shader_cache::hash(r.b, r.bn, h);
            if (seen.insert(h).second) recs->push_back(r);
        }
        if (recs->size() < total_raw || at < data->size()) {
            const std::string tmp = m_db_path + ".tmp";
            if (FILE* f = open_utf8(tmp, "wb")) {
                bool ok = std::fwrite(data->data(), 1, sizeof(db_header), f) == sizeof(db_header);
                for (const rec& r : *recs) {
                    const uint32_t sizes[2] = { r.an, r.bn };
                    ok = ok && std::fputc(r.type, f) != EOF &&
                         std::fwrite(sizes, sizeof sizes, 1, f) == 1 &&
                         std::fwrite(r.a, 1, r.an, f) == r.an &&
                         (!r.bn || std::fwrite(r.b, 1, r.bn, f) == r.bn);
                }
                std::fclose(f);
                if (ok) {
                    std::fclose(std::exchange(m_db, nullptr));
                    replace_file_utf8(tmp, m_db_path);
                    m_db = open_utf8(m_db_path, "ab+");
                }
            }
        }
    }
    if (recs->empty()) return;
    std::thread([this, &shaders, data, recs] {
        ps3_thread_name("vk precompile");
        auto pending = std::make_shared<std::atomic<size_t>>(0);
        auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
        auto run_matching = [&](auto&& pred, auto&& body) {
            for (const rec& r : *recs) {
                if (!pred(r.type)) continue;
                pending->fetch_add(1);
                work([r, body, pending, done] {
                    g_replay = true;
                    body(r);
                    g_replay = false;
                    if (pending->fetch_sub(1) == 1) {
                        std::lock_guard<std::mutex> hold(done->first);
                        done->second.notify_all();
                    }
                }, false);
            }
            std::unique_lock<std::mutex> hold(done->first);
            done->second.wait(hold, [&] { return pending->load() == 0; });
        };
        auto shader_body = [&shaders](const rec& r) {
            if (r.type == 'V') {
                rsx_spv_vp_key key;
                std::memcpy(&key, r.a, sizeof key);
                std::vector<uint32_t> program(RSX_SPV_VP_SLOTS * 4u);
                std::memcpy(program.data(), r.b, r.bn);
                shaders.vertex(program.data(), key, shader_cache::program_hash(program.data(), key.start), false);
            } else if (r.type == 'F') {
                rsx_spv_fp_key key;
                std::memcpy(&key, r.a, sizeof key);
                std::vector<uint32_t> ucode(r.bn / 4u);
                std::memcpy(ucode.data(), r.b, r.bn);
                shaders.fragment(ucode.data(), (uint32_t)ucode.size(), key, false);
            }
        };
        run_matching([](char t) { return t == 'V' || t == 'F'; }, shader_body);
        run_matching([](char t) { return t == 'P'; }, [this, &shaders](const rec& r) {
            pipeline_key k;
            std::memcpy(&k, r.a, sizeof k);
            const vertex_program* vs = shaders.find_vertex(k.vs);
            const fragment_program* fs = shaders.find_fragment(k.fs);
            if (vs && fs) get(k, vs->code, fs->code);
        });
        std::fprintf(stderr, "[vk] precompiled %zu shaders and pipelines\n", recs->size());
    }).detach();
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
    if (m_d->caps.provoking_vertex) {
        prov.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
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

VkPipeline pipelines::create_pipeline(VkGraphicsPipelineCreateInfo pi) const
{
    VkPipelineCreationFeedback feedback{};
    const VkPipelineCreationFeedbackCreateInfo fi{ VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, pi.pNext, &feedback, 0, nullptr };
    pi.pNext = &fi;
    VkPipeline p = VK_NULL_HANDLE;
    const compile_guard guard;
    const VkResult r = vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &pi, nullptr, &p);
    if (r == VK_ERROR_DEVICE_LOST) vk_fatal(*m_d, r, "vkCreateGraphicsPipelines");
    if (r != VK_SUCCESS) return VK_NULL_HANDLE;
    if (!(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))
        m_cache_dirty.store(true, std::memory_order_relaxed);
    return p;
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
    return create_pipeline(pi);
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
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT prov{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT };
    if (vertex && m_d->caps.provoking_vertex) {
        prov.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
        rs.pNext = &prov;
    }
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
    s.library = create_pipeline(pi);
    return s.library != VK_NULL_HANDLE;
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
    return create_pipeline(pi);
}

VkPipeline pipelines::output_library(const pipeline_key& k)
{
    pipeline_key o{};
    std::memcpy(o.color_format, k.color_format, sizeof o.color_format);
    o.depth_format = k.depth_format;
    o.logic_enable = k.logic_enable;
    o.logic_op = k.logic_op;
    std::lock_guard<std::mutex> hold(m_lib_lock);
    if (const VkPipeline* found = m_output.find(o)) return *found;
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
    const VkPipeline p = create_pipeline(pi);
    m_output[o] = p;
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
    return create_pipeline(pi);
}

VkPipeline pipelines::get(const pipeline_key& key, const shader& vs, const shader& fs, bool sync)
{
    if (!g_replay && m_last_entry && std::memcmp(&m_last_raw_key, &key, sizeof key) == 0 &&
        m_last_entry->done.load(std::memory_order_acquire))
        return m_last_entry->pipe.load(std::memory_order_acquire);
    const pipeline_key k = normalize(key);
    const size_t l1_idx = (size_t)hash_key(k) & (k_l1_size - 1u);
    if (!g_replay) {
        const l1_slot& s = m_l1[l1_idx];
        if (s.e && std::memcmp(&s.key, &k, sizeof k) == 0 && s.e->done.load(std::memory_order_acquire)) {
            m_last_raw_key = key;
            m_last_entry = s.e;
            return s.e->pipe.load(std::memory_order_acquire);
        }
    }
    entry* e;
    bool owner;
    {
        std::lock_guard<std::mutex> hold(m_map_lock);
        std::unique_ptr<entry>& slot = m_map[k];
        owner = !slot;
        if (owner) slot = std::make_unique<entry>();
        e = slot.get();
    }
    if (!owner) {
        if (!e->done.load(std::memory_order_acquire)) {
            if (!g_replay && !sync) return VK_NULL_HANDLE;
            e->done.wait(false, std::memory_order_acquire);
        }
        if (!g_replay) {
            m_l1[l1_idx].key = k;
            m_l1[l1_idx].e = e;
            m_last_raw_key = key;
            m_last_entry = e;
        }
        return e->pipe.load(std::memory_order_acquire);
    }
    record('P', &k, sizeof k, nullptr, 0);
    if (!m_libraries && !g_replay && !sync) {
        work([this, k, v = vs, f = fs, e] {
            const VkPipeline p = monolithic(k, v, f);
            e->pipe.store(p, std::memory_order_release);
            e->done.store(true, std::memory_order_release);
            e->done.notify_all();
        }, true);
        return VK_NULL_HANDLE;
    }
    const bool direct_lto = m_libraries && g_replay;
    const VkPipeline p = !m_libraries ? monolithic(k, vs, fs) : link(k, vs, fs, direct_lto);
    if (!p) std::fprintf(stderr, "[vk] pipeline creation failed\n");
    e->fast = (m_libraries && !direct_lto) ? p : VK_NULL_HANDLE;
    e->pipe.store(p, std::memory_order_release);
    e->done.store(true, std::memory_order_release);
    e->done.notify_all();
    if (m_libraries && !direct_lto && p)
        work([this, k, v = vs, f = fs, e] {
            if (VkPipeline opt = link(k, v, f, true)) e->pipe.store(opt, std::memory_order_release);
        }, false);
    return p;
}

}