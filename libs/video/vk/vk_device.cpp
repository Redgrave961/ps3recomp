#define VMA_IMPLEMENTATION
#include "vk_device.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

struct ext_entry {
    const char* name;
    VkStructureType stype;
    size_t size;
    bool vk_caps::*cap;
};

const ext_entry k_extensions[] = {
    { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VkStructureType(0), 0, &vk_caps::push_descriptor },
    { VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
      sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT), nullptr },
    { VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
      sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT), &vk_caps::dynamic_state3 },
    { VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VkStructureType(0), 0, nullptr },
    { VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
      sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT), &vk_caps::pipeline_library },
    { VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
      sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT), &vk_caps::custom_border_color },
    { VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
      sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT), &vk_caps::depth_clip_enable },
    { VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
      sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT), &vk_caps::provoking_vertex },
    { VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,
      sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT), nullptr },
    { VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT,
      sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT), nullptr },
    { VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
      sizeof(VkPhysicalDeviceMaintenance5FeaturesKHR), &vk_caps::maintenance5 },
    { VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
      sizeof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR), &vk_caps::fifo_latest_ready },
    { VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
      sizeof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR), &vk_caps::fifo_latest_ready },
    { VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, sizeof(VkPhysicalDevicePresentIdFeaturesKHR),
      &vk_caps::present_id },
    { VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, sizeof(VkPhysicalDevicePresentWaitFeaturesKHR),
      &vk_caps::present_wait },
    { VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VkStructureType(0), 0, &vk_caps::memory_budget },
    { VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VkStructureType(0), 0, nullptr },
    { VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME, VkStructureType(0), 0, nullptr },
    { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
      sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR), nullptr },
    { VK_EXT_SHADER_FLOAT8_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,
      sizeof(VkPhysicalDeviceShaderFloat8FeaturesEXT), nullptr },
    { VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,
      sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR), nullptr },
};

VKAPI_ATTR VkBool32 VKAPI_CALL vk_message(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    std::fprintf(stderr, "[vk] %s\n", data->pMessage);
    return VK_FALSE;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const VkExtensionProperties& e : list)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice gpu)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, list.data());
    return list;
}

int find_family(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families.data());
    for (uint32_t i = 0; i < n; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) return (int)i;
    }
    return -1;
}

const char* unsuitable(VkPhysicalDevice gpu, const VkPhysicalDeviceProperties& props, VkSurfaceKHR surface)
{
    if (props.apiVersion < VK_API_VERSION_1_3) return "its driver supports only an older Vulkan version, 1.3 is required";
    const std::vector<VkExtensionProperties> exts = device_extensions(gpu);
    if (!has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return "its driver cannot display Vulkan output";
    if (!has_extension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) return "its driver lacks the push descriptor extension";
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    VkPhysicalDeviceFeatures2 f{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    vkGetPhysicalDeviceFeatures2(gpu, &f);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f13.shaderDemoteToHelperInvocation || !f12.timelineSemaphore || !f12.hostQueryReset)
        return "its driver lacks required Vulkan 1.3 features";
    if (find_family(gpu, surface) < 0) return "it cannot present to this window";
    return nullptr;
}

int rank(VkPhysicalDevice gpu)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
    default: return 1;
    }
}

void* chain_add(vk_device& d, VkStructureType stype, size_t size)
{
    d.chain.emplace_back(new uint8_t[size]());
    auto* s = reinterpret_cast<VkBaseOutStructure*>(d.chain.back().get());
    s->sType = stype;
    s->pNext = reinterpret_cast<VkBaseOutStructure*>(d.features.pNext);
    d.features.pNext = s;
    return s;
}

bool first_feature(const void* s)
{
    VkBool32 v;
    std::memcpy(&v, static_cast<const uint8_t*>(s) + sizeof(VkBaseOutStructure), sizeof v);
    return v != VK_FALSE;
}

}

bool vk_instance_create(vk_device& d, const char* const* extensions, uint32_t count)
{
    if (volkInitialize() != VK_SUCCESS) {
        d.unsupported = "Vulkan is not installed on this PC.\n";
        std::fprintf(stderr, "[vk] %s", d.unsupported.c_str());
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        d.unsupported = "The installed Vulkan runtime is older than 1.3.\n";
        std::fprintf(stderr, "[vk] %s", d.unsupported.c_str());
        return false;
    }
    std::vector<const char*> ext(extensions, extensions + count);
    std::vector<const char*> layers;
    const char* want = std::getenv("PS3_VK_VALIDATION");
    if (want && *want && *want != '0') {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> list(n);
        vkEnumerateInstanceLayerProperties(&n, list.data());
        for (const VkLayerProperties& l : list)
            if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) d.caps.validation = true;
        if (d.caps.validation) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr, "[vk] validation layer not installed\n");
        }
    }
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ps3recomp";
    app.pEngineName = "ps3recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)ext.size();
    ci.ppEnabledExtensionNames = ext.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    VK_TRY(vkCreateInstance(&ci, nullptr, &d.instance));
    volkLoadInstanceOnly(d.instance);
    if (d.caps.validation) {
        VkDebugUtilsMessengerCreateInfoEXT mi{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = vk_message;
        VK_TRY(vkCreateDebugUtilsMessengerEXT(d.instance, &mi, nullptr, &d.messenger));
    }
    return true;
}

bool vk_device_create(vk_device& d, VkSurfaceKHR surface)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(d.instance, &n, gpus.data());
    std::string rejected;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (const char* why = unsuitable(gpu, props, surface)) {
            char line[512];
            std::snprintf(line, sizeof line, "%s (Vulkan %u.%u): %s.\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                          VK_API_VERSION_MINOR(props.apiVersion), why);
            rejected += line;
        } else if (!d.gpu || rank(gpu) > rank(d.gpu)) {
            d.gpu = gpu;
        }
    }
    if (!d.gpu) {
        d.unsupported = gpus.empty() ? "No graphics card with Vulkan support was found.\n" : "No graphics card in this PC can run it:\n" + rejected;
        std::fprintf(stderr, "[vk] %s", d.unsupported.c_str());
        return false;
    }
    vkGetPhysicalDeviceProperties(d.gpu, &d.props);
    d.family = (uint32_t)find_family(d.gpu, surface);

    const std::vector<VkExtensionProperties> available = device_extensions(d.gpu);
    std::vector<const char*> enable{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    d.features = VkPhysicalDeviceFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    std::vector<std::pair<bool vk_caps::*, const void*>> gated;
    for (const ext_entry& e : k_extensions) {
        if (!has_extension(available, e.name)) continue;
        if (!std::strcmp(e.name, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME) &&
            has_extension(available, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME))
            continue;
        enable.push_back(e.name);
        if (e.size) {
            void* s = chain_add(d, e.stype, e.size);
            if (e.stype == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT)
                d.dynamic_state2 = static_cast<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(s);
            else if (e.stype == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT)
                d.dynamic_state3 = static_cast<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*>(s);
            gated.emplace_back(e.cap, s);
        } else if (e.cap) {
            d.caps.*e.cap = true;
        }
    }
    auto* f13 = static_cast<VkPhysicalDeviceVulkan13Features*>(
        chain_add(d, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, sizeof(VkPhysicalDeviceVulkan13Features)));
    auto* f12 = static_cast<VkPhysicalDeviceVulkan12Features*>(
        chain_add(d, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, sizeof(VkPhysicalDeviceVulkan12Features)));
    chain_add(d, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, sizeof(VkPhysicalDeviceVulkan11Features));
    vkGetPhysicalDeviceFeatures2(d.gpu, &d.features);

    d.features.features.robustBufferAccess = VK_FALSE;
    f13->robustImageAccess = VK_FALSE;
    f12->bufferDeviceAddressCaptureReplay = VK_FALSE;
    f12->bufferDeviceAddressMultiDevice = VK_FALSE;
    for (const auto& [cap, s] : gated) {
        if (cap) d.caps.*cap = first_feature(s);
        if (cap == &vk_caps::custom_border_color) {
            const auto* cb = static_cast<const VkPhysicalDeviceCustomBorderColorFeaturesEXT*>(s);
            d.caps.custom_border_without_format = cb->customBorderColorWithoutFormat != VK_FALSE;
        }
    }
    if (d.dynamic_state3 && !d.caps.dynamic_state3) d.caps.dynamic_state3 = d.dynamic_state3->extendedDynamicState3PolygonMode != VK_FALSE;
    d.caps.present_wait = d.caps.present_wait && d.caps.present_id;
    d.caps.buffer_address = f12->bufferDeviceAddress != VK_FALSE;
    d.caps.mirror_clamp_to_edge = f12->samplerMirrorClampToEdge != VK_FALSE;
    if (d.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(d.gpu, VK_FORMAT_R16G16B16A16_UNORM, &fp);
        constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        d.caps.rgba16_unorm = (fp.optimalTilingFeatures & need) == need;
    }
    const auto format_has = [&](VkFormat format, VkFormatFeatureFlags need) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(d.gpu, format, &fp);
        return (fp.optimalTilingFeatures & need) == need;
    };
    constexpr VkFormatFeatureFlags k_texture = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    d.caps.r8_srgb = format_has(VK_FORMAT_R8_SRGB, k_texture);
    d.caps.r8g8_srgb = format_has(VK_FORMAT_R8G8_SRGB, k_texture);
    d.caps.r16_uint_storage = d.features.features.shaderStorageImageExtendedFormats &&
                              format_has(VK_FORMAT_R16_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    {
        VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &d.driver };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        vkGetPhysicalDeviceProperties2(d.gpu, &p2);
        if (idp.deviceLUIDValid) {
            std::memcpy(d.luid, idp.deviceLUID, sizeof d.luid);
            d.luid_valid = true;
        }
    }
    if (d.caps.custom_border_color) {
        VkPhysicalDeviceCustomBorderColorPropertiesEXT cb{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &cb };
        vkGetPhysicalDeviceProperties2(d.gpu, &p2);
        d.caps.max_custom_border_samplers = cb.maxCustomBorderColorSamplers;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = d.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &d.features };
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)enable.size();
    ci.ppEnabledExtensionNames = enable.data();
    VK_TRY(vkCreateDevice(d.gpu, &ci, nullptr, &d.dev));
    volkLoadDevice(d.dev);
    vkGetDeviceQueue(d.dev, d.family, 0, &d.queue);

    VmaVulkanFunctions fn{};
    fn.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fn.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = d.gpu;
    ai.device = d.dev;
    ai.instance = d.instance;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.pVulkanFunctions = &fn;
    if (d.caps.memory_budget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    if (d.caps.buffer_address) ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (d.caps.maintenance5) ai.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
    VK_TRY(vmaCreateAllocator(&ai, &d.vma));

    std::fprintf(stderr, "[vk] %s, driver %s %s, Vulkan %u.%u.%u: push descriptors %d, dynamic state 3 %d, pipeline library %d, custom border %d (%u), fifo latest ready %d, present wait %d, rgba16_unorm %d\n",
                 d.props.deviceName, d.driver.driverName, d.driver.driverInfo, VK_API_VERSION_MAJOR(d.props.apiVersion),
                 VK_API_VERSION_MINOR(d.props.apiVersion), VK_API_VERSION_PATCH(d.props.apiVersion), d.caps.push_descriptor,
                 d.caps.dynamic_state3, d.caps.pipeline_library, d.caps.custom_border_color, d.caps.max_custom_border_samplers,
                 d.caps.fifo_latest_ready, d.caps.present_wait, d.caps.rgba16_unorm);
    return true;
}

void vk_fatal(const vk_device& d, VkResult result, const char* call)
{
    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true))
        std::fprintf(stderr, "[vk] %s failed: %d on %s (vendor 0x%04X device 0x%04X), driver %s %s (id %d, version 0x%08X)\n", call,
                     (int)result, d.props.deviceName, d.props.vendorID, d.props.deviceID, d.driver.driverName, d.driver.driverInfo,
                     (int)d.driver.driverID, d.props.driverVersion);
    std::fflush(stderr);
    d.fatal(result, call);
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

void vk_wait(const vk_device& d, VkSemaphore timeline, uint64_t value)
{
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &value;
    vk_must(d, vkWaitSemaphores(d.dev, &wi, UINT64_MAX), "vkWaitSemaphores");
}

uint64_t vk_reached(const vk_device& d, VkSemaphore timeline)
{
    uint64_t value = 0;
    vk_must(d, vkGetSemaphoreCounterValue(d.dev, timeline, &value), "vkGetSemaphoreCounterValue");
    return value;
}

void vk_submit(vk_device& d, const VkSubmitInfo2& submit)
{
    VkResult r;
    {
        std::lock_guard<std::mutex> hold(d.queue_lock);
        r = vkQueueSubmit2(d.queue, 1, &submit, VK_NULL_HANDLE);
    }
    vk_must(d, r, "vkQueueSubmit2");
}
