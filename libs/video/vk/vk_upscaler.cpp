#include "vk_upscaler.h"
#include "../rsx_spirv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

namespace vk {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* k_ffx_module = "amd_fidelityfx_loader_vk.dll";
constexpr const char* k_dlss_module = "nvngx_dlss.dll";
constexpr const char* k_ngx_init = "NVSDK_NGX_VULKAN_Init_with_ProjectID";
using ngx_init_fn = decltype(&NVSDK_NGX_VULKAN_Init_with_ProjectID);

void* lib_open(const fs::path& p) { return LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); }
void* lib_sym(void* m, const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m), name)); }
void lib_close(void* m) { FreeLibrary(static_cast<HMODULE>(m)); }

fs::path exe_dir()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n && n < MAX_PATH ? fs::path(buf).parent_path() : fs::path(L".");
}
#else
constexpr const char* k_ffx_module = "amd_fidelityfx_upscaler_vk.so";
constexpr const char* k_dlss_module = "libnvidia-ngx-dlss.so";
constexpr const char* k_ngx_init = "NVSDK_NGX_VULKAN_Init_ProjectID_Ext";
using ngx_init_fn = NVSDK_NGX_Result (*)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, VkInstance, VkPhysicalDevice,
                                         VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version,
                                         const NVSDK_NGX_FeatureCommonInfo*);

void* lib_open(const fs::path& p) { return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* lib_sym(void* m, const char* name) { return dlsym(m, name); }
void lib_close(void* m) { dlclose(m); }

fs::path exe_dir()
{
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path(".") : p.parent_path();
}
#endif

fs::path upscaler_dir() { return exe_dir() / "upscalers"; }

template <typename F>
bool load(void* m, const char* name, F& out)
{
    out = reinterpret_cast<F>(lib_sym(m, name));
    return out != nullptr;
}

void* open_bundled(const char* name)
{
#ifdef _WIN32
    SetDllDirectoryW(upscaler_dir().c_str());
#endif
    for (const fs::path& dir : { upscaler_dir(), exe_dir() })
        if (void* m = lib_open(dir / name)) return m;
    return nullptr;
}

bool bundled(const char* prefix)
{
    for (const fs::path& dir : { upscaler_dir(), exe_dir() }) {
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            if (e.path().filename().string().rfind(prefix, 0) == 0) return true;
    }
    return false;
}

void* open_ngx_core()
{
#ifdef _WIN32
    if (void* m = open_bundled("_nvngx.dll")) return m;
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) return m;
    static const wchar_t* const k_keys[3] = {
        L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
        L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
    };
    for (const wchar_t* k : k_keys) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
        for (const wchar_t* v : { L"NGXPath", L"FullPath" }) {
            wchar_t path[MAX_PATH]{};
            DWORD sz = sizeof path;
            if (RegQueryValueExW(hk, v, nullptr, nullptr, reinterpret_cast<LPBYTE>(path), &sz) != ERROR_SUCCESS || !path[0]) continue;
            if (void* m = lib_open(fs::path(path) / L"_nvngx.dll")) {
                RegCloseKey(hk);
                return m;
            }
        }
        RegCloseKey(hk);
    }
    wchar_t sysdir[MAX_PATH]{};
    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) return nullptr;
    std::error_code ec;
    for (const fs::directory_entry& e :
         fs::directory_iterator(fs::path(sysdir) / L"DriverStore" / L"FileRepository", fs::directory_options::skip_permission_denied, ec))
        if (e.path().filename().wstring().rfind(L"nv_", 0) == 0)
            if (void* m = lib_open(e.path() / L"_nvngx.dll")) return m;
    return nullptr;
#else
    return dlopen("libnvidia-ngx.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

struct {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
} g_ffx;

struct {
    ngx_init_fn init = nullptr;
    decltype(&NVSDK_NGX_VULKAN_Shutdown1) shutdown = nullptr;
    decltype(&NVSDK_NGX_VULKAN_GetCapabilityParameters) get_cap_params = nullptr;
    decltype(&NVSDK_NGX_VULKAN_DestroyParameters) destroy_params = nullptr;
    decltype(&NVSDK_NGX_VULKAN_CreateFeature1) create = nullptr;
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release = nullptr;
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature) eval = nullptr;
    PFN_NVSDK_NGX_Parameter_SetUI set_ui = nullptr;
    PFN_NVSDK_NGX_Parameter_SetI set_i = nullptr;
    PFN_NVSDK_NGX_Parameter_SetF set_f = nullptr;
    PFN_NVSDK_NGX_Parameter_SetVoidPointer set_ptr = nullptr;
    PFN_NVSDK_NGX_Parameter_GetI get_i = nullptr;
} g_ngx;

bool load_ngx_params(void* m)
{
#ifdef _WIN32
    return load(m, "NVSDK_NGX_Parameter_SetUI", g_ngx.set_ui) && load(m, "NVSDK_NGX_Parameter_SetI", g_ngx.set_i) &&
           load(m, "NVSDK_NGX_Parameter_SetF", g_ngx.set_f) && load(m, "NVSDK_NGX_Parameter_SetVoidPointer", g_ngx.set_ptr) &&
           load(m, "NVSDK_NGX_Parameter_GetI", g_ngx.get_i);
#else
    (void)m;
    g_ngx.set_ui = [](NVSDK_NGX_Parameter* p, const char* n, unsigned int v) { p->Set(n, v); };
    g_ngx.set_i = [](NVSDK_NGX_Parameter* p, const char* n, int v) { p->Set(n, v); };
    g_ngx.set_f = [](NVSDK_NGX_Parameter* p, const char* n, float v) { p->Set(n, v); };
    g_ngx.set_ptr = [](NVSDK_NGX_Parameter* p, const char* n, void* v) { p->Set(n, v); };
    g_ngx.get_i = [](NVSDK_NGX_Parameter* p, const char* n, int* v) { return p->Get(n, v); };
    return true;
#endif
}

NVSDK_NGX_Result ngx_init(const vk_device& d, const wchar_t* data_dir, const NVSDK_NGX_FeatureCommonInfo* common)
{
    constexpr const char* k_project = "a9f4c2e1-7b38-4d5a-9c12-6e8f0a4b2c7d";
#ifdef _WIN32
    return g_ngx.init(k_project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_dir, d.instance, d.gpu, d.dev, vkGetInstanceProcAddr,
                      vkGetDeviceProcAddr, common, NVSDK_NGX_Version_API);
#else
    return g_ngx.init(k_project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_dir, d.instance, d.gpu, d.dev, vkGetInstanceProcAddr,
                      vkGetDeviceProcAddr, NVSDK_NGX_Version_API, common);
#endif
}

float halton(uint32_t index, uint32_t base)
{
    float f = 1.f, r = 0.f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

float mode_ratio(int mode)
{
    switch (mode) {
    case RSX_VK_UPSCALE_FSR_QUALITY:
    case RSX_VK_UPSCALE_DLSS_QUALITY:
        return 1.5f;
    case RSX_VK_UPSCALE_FSR_BALANCED:
    case RSX_VK_UPSCALE_DLSS_BALANCED:
        return 1.7f;
    case RSX_VK_UPSCALE_FSR_PERFORMANCE:
    case RSX_VK_UPSCALE_DLSS_PERFORMANCE:
        return 2.0f;
    default:
        return 1.0f;
    }
}

bool is_fsr_mode(int mode)
{
    return mode >= RSX_VK_UPSCALE_FSR_NATIVE && mode <= RSX_VK_UPSCALE_FSR_PERFORMANCE;
}

bool is_dlss_mode(int mode)
{
    return mode >= RSX_VK_UPSCALE_DLAA && mode <= RSX_VK_UPSCALE_DLSS_PERFORMANCE;
}

uint32_t fsr_quality_mode(int mode)
{
    switch (mode) {
    case RSX_VK_UPSCALE_FSR_QUALITY: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case RSX_VK_UPSCALE_FSR_BALANCED: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case RSX_VK_UPSCALE_FSR_PERFORMANCE: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    default: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    }
}

NVSDK_NGX_PerfQuality_Value dlss_quality_mode(int mode)
{
    switch (mode) {
    case RSX_VK_UPSCALE_DLSS_QUALITY: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case RSX_VK_UPSCALE_DLSS_BALANCED: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case RSX_VK_UPSCALE_DLSS_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

void mat4_mul_transpose_f64(const float a[16], const float b[16], float out[16])
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += (double)a[r * 4 + k] * (double)b[k * 4 + c];
            out[c * 4 + r] = (float)s;
        }
    }
}

void transition_image(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout old_l, VkImageLayout new_l,
                      VkPipelineStageFlags2 src_s, VkAccessFlags2 src_a,
                      VkPipelineStageFlags2 dst_s, VkAccessFlags2 dst_a)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_s;
    b.srcAccessMask = src_a;
    b.dstStageMask = dst_s;
    b.dstAccessMask = dst_a;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}

bool upscaler::init_mv_pipeline(VkPipelineCache cache)
{
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_TRY(vkCreateSampler(m_d->dev, &si, nullptr, &m_sampler));
    VkDescriptorSetLayoutBinding b[3]{};
    b[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 3;
    li.pBindings = b;
    VK_TRY(vkCreateDescriptorSetLayout(m_d->dev, &li, nullptr, &m_mv_set));
    const VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 96 };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &m_mv_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    VK_TRY(vkCreatePipelineLayout(m_d->dev, &pi, nullptr, &m_mv_layout));
    rsx_spv_blob blob{};
    if (rsx_spv_motion_vectors(&blob) != 0) {
        rsx_spv_blob_free(&blob);
        return false;
    }
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)blob.count * 4u;
    mi.pCode = blob.words;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(m_d->dev, &mi, nullptr, &mod);
    rsx_spv_blob_free(&blob);
    VK_TRY(r);
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main" };
    ci.layout = m_mv_layout;
    r = vkCreateComputePipelines(m_d->dev, cache, 1, &ci, nullptr, &m_mv_pipe);
    vkDestroyShaderModule(m_d->dev, mod, nullptr);
    VK_TRY(r);
    return true;
}

bool upscaler::ensure_mv_image(uint32_t width, uint32_t height)
{
    if (m_mv_img && m_mv_w == width && m_mv_h == height) return true;
    destroy_mv_image();
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16_SFLOAT;
    ii.extent = { width, height, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m_d->vma, &ii, &ai, &m_mv_img, &m_mv_mem, nullptr) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_mv_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_d->dev, &vi, nullptr, &m_mv_view) != VK_SUCCESS) {
        destroy_mv_image();
        return false;
    }
    m_mv_w = width;
    m_mv_h = height;
    return true;
}

void upscaler::destroy_mv_image()
{
    if (!m_d) return;
    if (m_mv_view || m_mv_img) vkDeviceWaitIdle(m_d->dev);
    if (m_mv_view) vkDestroyImageView(m_d->dev, m_mv_view, nullptr);
    if (m_mv_img) vmaDestroyImage(m_d->vma, m_mv_img, m_mv_mem);
    m_mv_view = VK_NULL_HANDLE;
    m_mv_img = VK_NULL_HANDLE;
    m_mv_mem = nullptr;
    m_mv_w = m_mv_h = 0;
}

bool upscaler::init_ffx()
{
    void* mod = open_bundled(k_ffx_module);
    if (!mod) return false;
    VkDeviceContext dev_ctx{ m_d->dev, m_d->gpu, vkGetDeviceProcAddr };
    uint64_t count = 0;
    ffxQueryDescGetVersions qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    qv.device = &dev_ctx;
    qv.outputCount = &count;
    if (!load(mod, "ffxCreateContext", g_ffx.create) || !load(mod, "ffxDestroyContext", g_ffx.destroy) ||
        !load(mod, "ffxQuery", g_ffx.query) || !load(mod, "ffxDispatch", g_ffx.dispatch) ||
        g_ffx.query(nullptr, &qv.header) != FFX_API_RETURN_OK || count == 0) {
        lib_close(mod);
        return false;
    }
    std::vector<uint64_t> ids((size_t)count);
    std::vector<const char*> names((size_t)count);
    qv.versionIds = ids.data();
    qv.versionNames = names.data();
    if (g_ffx.query(nullptr, &qv.header) == FFX_API_RETURN_OK && names[0])
        if (const char* v = std::strpbrk(names[0], "0123456789"))
            std::snprintf(m_fsr_version, sizeof m_fsr_version, "%.*s", (int)std::strcspn(v, " "), v);
    m_ffx_mod = mod;
    return true;
}

void upscaler::destroy_ffx_context()
{
    if (m_ffx_ctx) {
        vkDeviceWaitIdle(m_d->dev);
        ffxContext ctx = m_ffx_ctx;
        g_ffx.destroy(&ctx, nullptr);
    }
    m_ffx_ctx = nullptr;
    m_ffx_rw = m_ffx_rh = m_ffx_dw = m_ffx_dh = 0;
}

bool upscaler::ensure_ffx_context(uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h)
{
    if (m_ffx_ctx && m_ffx_rw == render_w && m_ffx_rh == render_h && m_ffx_dw == display_w && m_ffx_dh == display_h)
        return true;
    destroy_ffx_context();
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = m_d->dev;
    backend.deviceContext.vkPhysicalDevice = m_d->gpu;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.header.pNext = &backend.header;
    ver.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &ver.header;
    desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.fpMessage = [](uint32_t, const wchar_t* message) { std::fprintf(stderr, "[ffx] %ls\n", message); };
    desc.maxRenderSize = { render_w, render_h };
    desc.maxUpscaleSize = { display_w, display_h };

    ffxContext ctx = nullptr;
    ffxReturnCode_t rc = g_ffx.create(&ctx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx) {
        desc.header.pNext = &backend.header;
        rc = g_ffx.create(&ctx, &desc.header, nullptr);
    }
    if (rc != FFX_API_RETURN_OK || !ctx) {
        std::fprintf(stderr, "[vk] FSR context creation failed: code %d, render %ux%u, output %ux%u\n", (int)rc, render_w, render_h,
                     display_w, display_h);
        return false;
    }
    m_ffx_ctx = ctx;
    m_ffx_rw = render_w;
    m_ffx_rh = render_h;
    m_ffx_dw = display_w;
    m_ffx_dh = display_h;
    return true;
}

bool upscaler::init_ngx(const std::string& cache_path)
{
    if (m_d->props.vendorID != 0x10DEu || !bundled(k_dlss_module)) return false;
    void* mod = open_ngx_core();
    if (!mod) return false;
    const std::wstring data_dir = fs::path(reinterpret_cast<const char8_t*>(cache_path.c_str())).parent_path().wstring();
    const std::wstring dirs[2] = { upscaler_dir().wstring(), exe_dir().wstring() };
    const wchar_t* paths[2] = { dirs[0].c_str(), dirs[1].c_str() };
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 2;
    NVSDK_NGX_Parameter* params = nullptr;
    int dlss_avail = 0;
    if (!load(mod, k_ngx_init, g_ngx.init) || !load(mod, "NVSDK_NGX_VULKAN_Shutdown1", g_ngx.shutdown) ||
        !load(mod, "NVSDK_NGX_VULKAN_GetCapabilityParameters", g_ngx.get_cap_params) ||
        !load(mod, "NVSDK_NGX_VULKAN_DestroyParameters", g_ngx.destroy_params) ||
        !load(mod, "NVSDK_NGX_VULKAN_CreateFeature1", g_ngx.create) || !load(mod, "NVSDK_NGX_VULKAN_ReleaseFeature", g_ngx.release) ||
        !load(mod, "NVSDK_NGX_VULKAN_EvaluateFeature", g_ngx.eval) || !load_ngx_params(mod) ||
        NVSDK_NGX_FAILED(ngx_init(*m_d, data_dir.c_str(), &common))) {
        lib_close(mod);
        return false;
    }
    if (NVSDK_NGX_FAILED(g_ngx.get_cap_params(&params)) || !params ||
        NVSDK_NGX_FAILED(g_ngx.get_i(params, NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_avail)) || !dlss_avail) {
        if (params) g_ngx.destroy_params(params);
        g_ngx.shutdown(m_d->dev);
        lib_close(mod);
        return false;
    }
    m_ngx_mod = mod;
    m_ngx_params = params;
    return true;
}

void upscaler::destroy_ngx_feature()
{
    if (m_ngx_handle) {
        vkDeviceWaitIdle(m_d->dev);
        g_ngx.release(static_cast<NVSDK_NGX_Handle*>(m_ngx_handle));
    }
    m_ngx_handle = nullptr;
    m_ngx_mode = 0;
    m_ngx_rw = m_ngx_rh = m_ngx_dw = m_ngx_dh = 0;
}

bool upscaler::ensure_ngx_feature(VkCommandBuffer cmd, int mode, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h)
{
    if (m_ngx_handle && m_ngx_mode == mode && m_ngx_rw == render_w && m_ngx_rh == render_h && m_ngx_dw == display_w && m_ngx_dh == display_h)
        return true;
    destroy_ngx_feature();
    auto* p = static_cast<NVSDK_NGX_Parameter*>(m_ngx_params);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_Width, render_w);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_Height, render_h);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_OutWidth, display_w);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_OutHeight, display_h);
    g_ngx.set_i(p, NVSDK_NGX_Parameter_PerfQualityValue, (int)dlss_quality_mode(mode));
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M);
    g_ngx.set_i(p, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
    g_ngx.set_i(p, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    NVSDK_NGX_Handle* handle = nullptr;
    if (NVSDK_NGX_FAILED(g_ngx.create(m_d->dev, cmd, NVSDK_NGX_Feature_SuperSampling, p, &handle)) || !handle)
        return false;
    m_ngx_handle = handle;
    m_ngx_mode = mode;
    m_ngx_rw = render_w;
    m_ngx_rh = render_h;
    m_ngx_dw = display_w;
    m_ngx_dh = display_h;
    return true;
}

bool upscaler::create(vk_device& d, VkPipelineCache cache, const std::string& cache_path)
{
    m_d = &d;
    init_ffx();
    init_ngx(cache_path);
    std::fprintf(stderr, "[vk] upscalers: FSR %s (%d), DLSS (%d)\n", m_fsr_version, m_ffx_mod != nullptr, m_ngx_mod != nullptr);
    if ((m_ffx_mod || m_ngx_mod) && !init_mv_pipeline(cache)) {
        destroy();
        return false;
    }
    return true;
}

void upscaler::release_ffx()
{
    destroy_ffx_context();
    if (m_ffx_mod) lib_close(m_ffx_mod);
    m_ffx_mod = nullptr;
}

void upscaler::release_ngx()
{
    destroy_ngx_feature();
    if (m_ngx_mod) {
        g_ngx.destroy_params(static_cast<NVSDK_NGX_Parameter*>(m_ngx_params));
        g_ngx.shutdown(m_d->dev);
        lib_close(m_ngx_mod);
    }
    m_ngx_params = nullptr;
    m_ngx_mod = nullptr;
}

void upscaler::destroy()
{
    if (!m_d) return;
    release_ffx();
    release_ngx();
    destroy_mv_image();
    if (m_mv_pipe) vkDestroyPipeline(m_d->dev, m_mv_pipe, nullptr);
    if (m_mv_layout) vkDestroyPipelineLayout(m_d->dev, m_mv_layout, nullptr);
    if (m_mv_set) vkDestroyDescriptorSetLayout(m_d->dev, m_mv_set, nullptr);
    if (m_sampler) vkDestroySampler(m_d->dev, m_sampler, nullptr);
    m_mv_pipe = VK_NULL_HANDLE;
    m_mv_layout = VK_NULL_HANDLE;
    m_mv_set = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_dropped = 0;
    m_d = nullptr;
}

int upscaler::caps() const
{
    return ((m_ffx_mod ? RSX_VK_UPSCALE_CAP_FSR : 0) | (m_ngx_mod ? RSX_VK_UPSCALE_CAP_DLSS : 0)) & ~m_dropped;
}

void upscaler::render_size(uint32_t display_w, uint32_t display_h, int mode, uint32_t& out_w, uint32_t& out_h) const
{
    if (is_fsr_mode(mode) && m_ffx_mod) {
        uint32_t rw = 0, rh = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        q.displayWidth = display_w;
        q.displayHeight = display_h;
        q.qualityMode = fsr_quality_mode(mode);
        q.pOutRenderWidth = &rw;
        q.pOutRenderHeight = &rh;
        if (g_ffx.query(nullptr, &q.header) == FFX_API_RETURN_OK && rw && rh) {
            out_w = rw;
            out_h = rh;
            return;
        }
    }
    const float r = mode_ratio(mode);
    out_w = std::max(1u, (uint32_t)std::lround((double)display_w / r));
    out_h = std::max(1u, (uint32_t)std::lround((double)display_h / r));
}

void upscaler::jitter(int mode, uint32_t render_w, uint32_t display_w, uint32_t frame_index, float& out_jx, float& out_jy) const
{
    if (mode == RSX_VK_UPSCALE_OFF || !render_w || !display_w) {
        out_jx = out_jy = 0.f;
        return;
    }
    if (is_fsr_mode(mode) && m_ffx_mod) {
        int32_t phase_count = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount qp{};
        qp.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        qp.renderWidth = render_w;
        qp.displayWidth = display_w;
        qp.pOutPhaseCount = &phase_count;
        if (g_ffx.query(nullptr, &qp.header) == FFX_API_RETURN_OK && phase_count > 0) {
            float jx = 0.f, jy = 0.f;
            ffxQueryDescUpscaleGetJitterOffset qo{};
            qo.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
            qo.index = (int32_t)(frame_index % (uint32_t)phase_count);
            qo.phaseCount = phase_count;
            qo.pOutX = &jx;
            qo.pOutY = &jy;
            if (g_ffx.query(nullptr, &qo.header) == FFX_API_RETURN_OK) {
                out_jx = jx;
                out_jy = jy;
                return;
            }
        }
    }
    const double ratio = (double)display_w / (double)render_w;
    const uint32_t phases = std::max(8u, (uint32_t)std::ceil(8.0 * ratio * ratio));
    const uint32_t idx = (frame_index % phases) + 1u;
    out_jx = halton(idx, 2u) - 0.5f;
    out_jy = halton(idx, 3u) - 0.5f;
}

bool upscaler::dispatch(VkCommandBuffer cmd, int mode,
                        VkImage color_img, VkImageView color_view, VkFormat color_fmt,
                        VkImage depth_img, VkImageView depth_view, VkFormat depth_fmt,
                        VkImageView vel_view,
                        VkImage out_img, VkImageView out_view, VkFormat out_fmt,
                        uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h,
                        const rsx_vk_upscale_frame& frame)
{
    if (m_dropped & RSX_VK_UPSCALE_CAP_FSR) release_ffx();
    if (m_dropped & RSX_VK_UPSCALE_CAP_DLSS) release_ngx();
    m_dropped = 0;
    const bool fsr = is_fsr_mode(mode) && m_ffx_mod;
    const bool dlss = is_dlss_mode(mode) && m_ngx_mod;
    if (!m_mv_pipe || !(fsr || dlss) || !ensure_mv_image(render_w, render_h)) return false;
    if (fsr ? !ensure_ffx_context(render_w, render_h, display_w, display_h)
            : !ensure_ngx_feature(cmd, mode, render_w, render_h, display_w, display_h)) {
        std::fprintf(stderr, "[vk] %s unavailable on this device, disabling it\n", fsr ? "FSR" : "DLSS");
        if (fsr) release_ffx();
        else release_ngx();
        return false;
    }

    transition_image(cmd, m_mv_img, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    struct mv_pc {
        float reproj[16];
        uint32_t w, h;
        float inv_w, inv_h;
        float vel_sx, vel_sy;
        float djx, djy;
    } pc{};
    mat4_mul_transpose_f64(frame.inv_curr_vp, frame.prev_vp, pc.reproj);
    pc.w = render_w;
    pc.h = render_h;
    pc.inv_w = 1.f / (float)render_w;
    pc.inv_h = 1.f / (float)render_h;
    pc.vel_sx = frame.vel_scale_x;
    pc.vel_sy = frame.vel_scale_y;
    pc.djx = frame.reset ? 0.f : (frame.prev_jitter_x - frame.jitter_x);
    pc.djy = frame.reset ? 0.f : (frame.prev_jitter_y - frame.jitter_y);

    const VkDescriptorImageInfo imgs[3] = {
        { m_sampler, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { VK_NULL_HANDLE, m_mv_view, VK_IMAGE_LAYOUT_GENERAL },
        { m_sampler, vel_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet w[3]{};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgs[0] };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imgs[1] };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgs[2] };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_mv_pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_mv_layout, 0, 3, w);
    vkCmdPushConstants(cmd, m_mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (render_w + 7u) / 8u, (render_h + 7u) / 8u, 1);

    transition_image(cmd, m_mv_img, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    if (fsr) {
        auto make_res = [](VkImage img, VkFormat fmt, uint32_t width, uint32_t height, uint32_t usage, uint32_t state) {
            FfxApiResource r{};
            r.resource = img;
            r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            r.description.format = ffxApiGetSurfaceFormatVK(fmt);
            r.description.width = width;
            r.description.height = height;
            r.description.depth = 1;
            r.description.mipCount = 1;
            r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
            r.description.usage = usage;
            r.state = state;
            return r;
        };
        ffxDispatchDescUpscale dd{};
        dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dd.commandList = cmd;
        dd.color = make_res(color_img, color_fmt, render_w, render_h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        const uint32_t depth_usage = FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_DEPTHTARGET |
                                     (ffxApiIsStencilFormat(depth_fmt) ? FFX_API_RESOURCE_USAGE_STENCILTARGET : 0u);
        dd.depth = make_res(depth_img, depth_fmt, render_w, render_h, depth_usage, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dd.motionVectors = make_res(m_mv_img, VK_FORMAT_R16G16_SFLOAT, render_w, render_h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dd.output = make_res(out_img, out_fmt, display_w, display_h, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dd.jitterOffset = { frame.jitter_x, frame.jitter_y };
        dd.motionVectorScale = { 1.f, 1.f };
        dd.renderSize = { render_w, render_h };
        dd.upscaleSize = { display_w, display_h };
        dd.enableSharpening = frame.sharpness > 0.f;
        dd.sharpness = std::clamp(frame.sharpness, 0.f, 1.f);
        dd.frameTimeDelta = std::max(frame.dt_ms, 0.1f);
        dd.preExposure = 1.f;
        dd.reset = frame.reset != 0;
        dd.cameraNear = std::max(frame.near_clip, 0.01f);
        dd.cameraFar = std::max(frame.far_clip, dd.cameraNear + 1.f);
        dd.cameraFovAngleVertical = frame.fov_y > 0.01f ? frame.fov_y : 1.0f;
        dd.viewSpaceToMetersFactor = 0.02f;
        dd.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        ffxContext ctx = m_ffx_ctx;
        if (g_ffx.dispatch(&ctx, &dd.header) == FFX_API_RETURN_OK) return true;
        std::fprintf(stderr, "[vk] FSR dispatch rejected, disabling it\n");
        m_dropped |= RSX_VK_UPSCALE_CAP_FSR;
        return false;
    }

    const VkImageSubresourceRange color_sr{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    const VkImageSubresourceRange depth_sr{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    NVSDK_NGX_Resource_VK r_color = NVSDK_NGX_Create_ImageView_Resource_VK(color_view, color_img, color_sr, color_fmt, render_w, render_h, false);
    NVSDK_NGX_Resource_VK r_depth = NVSDK_NGX_Create_ImageView_Resource_VK(depth_view, depth_img, depth_sr, depth_fmt, render_w, render_h, false);
    NVSDK_NGX_Resource_VK r_mv = NVSDK_NGX_Create_ImageView_Resource_VK(m_mv_view, m_mv_img, color_sr, VK_FORMAT_R16G16_SFLOAT, render_w, render_h, false);
    NVSDK_NGX_Resource_VK r_out = NVSDK_NGX_Create_ImageView_Resource_VK(out_view, out_img, color_sr, out_fmt, display_w, display_h, true);
    auto* p = static_cast<NVSDK_NGX_Parameter*>(m_ngx_params);
    g_ngx.set_ptr(p, NVSDK_NGX_Parameter_Color, &r_color);
    g_ngx.set_ptr(p, NVSDK_NGX_Parameter_Output, &r_out);
    g_ngx.set_ptr(p, NVSDK_NGX_Parameter_Depth, &r_depth);
    g_ngx.set_ptr(p, NVSDK_NGX_Parameter_MotionVectors, &r_mv);
    g_ngx.set_f(p, NVSDK_NGX_Parameter_Jitter_Offset_X, -frame.jitter_x);
    g_ngx.set_f(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, -frame.jitter_y);
    g_ngx.set_f(p, NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    g_ngx.set_f(p, NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    g_ngx.set_i(p, NVSDK_NGX_Parameter_Reset, frame.reset ? 1 : 0);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render_w);
    g_ngx.set_ui(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render_h);
    g_ngx.set_f(p, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
    g_ngx.set_f(p, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, std::max(frame.dt_ms, 0.1f));
    if (NVSDK_NGX_SUCCEED(g_ngx.eval(cmd, static_cast<NVSDK_NGX_Handle*>(m_ngx_handle), p, nullptr))) return true;
    std::fprintf(stderr, "[vk] DLSS evaluate rejected, disabling it\n");
    m_dropped |= RSX_VK_UPSCALE_CAP_DLSS;
    return false;
}

}
