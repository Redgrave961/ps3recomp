#define VMA_IMPLEMENTATION
#include "vk_device.h"

#include <cstdlib>
#include <cstring>

namespace {

struct ext_entry {
    const char* name;
    VkStructureType stype;
    size_t size;
    bool vk_caps::*cap;
};

const ext_entry k_extensions[] = {
    { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VkStructureType(0), 0, &vk_caps::push_descriptor },
    { VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
      sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT), &vk_caps::dynamic_state3 },
    { VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
      sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT), &vk_caps::vertex_input },
    { VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VkStructureType(0), 0, nullptr },
    { VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
      sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT), &vk_caps::pipeline_library },
    { VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
      sizeof(VkPhysicalDeviceShaderObjectFeaturesEXT), &vk_caps::shader_object },
    { VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
      sizeof(VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR), &vk_caps::local_read },
    { VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
      sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT), &vk_caps::conditional_rendering },
    { VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
      sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT), &vk_caps::custom_border_color },
    { VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,
      sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT), &vk_caps::depth_clip_control },
    { VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
      sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT), &vk_caps::depth_clip_enable },
    { VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
      sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT), &vk_caps::provoking_vertex },
    { VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,
      sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT), &vk_caps::list_restart },
    { VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT,
      sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT), &vk_caps::unused_attachments },
    { VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
      sizeof(VkPhysicalDeviceMaintenance5FeaturesKHR), &vk_caps::maintenance5 },
    { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VkStructureType(0), 0, &vk_caps::host_memory },
    { VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VkStructureType(0), 0, &vk_caps::memory_budget },
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

bool suitable(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return false;
    if (!has_extension(device_extensions(gpu), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return false;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    VkPhysicalDeviceFeatures2 f{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    vkGetPhysicalDeviceFeatures2(gpu, &f);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) return false;
    return find_family(gpu, surface) >= 0;
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
    VK_TRY(volkInitialize());
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        std::fprintf(stderr, "[vk] Vulkan 1.3 loader required\n");
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
    for (VkPhysicalDevice gpu : gpus)
        if (suitable(gpu, surface) && (!d.gpu || rank(gpu) > rank(d.gpu))) d.gpu = gpu;
    if (!d.gpu) {
        std::fprintf(stderr, "[vk] no Vulkan 1.3 GPU with dynamic rendering, synchronization2 and timeline semaphores\n");
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
        enable.push_back(e.name);
        if (e.size) {
            void* s = chain_add(d, e.stype, e.size);
            if (e.stype == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT)
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
    for (const auto& [cap, s] : gated)
        if (cap) d.caps.*cap = first_feature(s);
    if (d.dynamic_state3 && !d.caps.dynamic_state3) d.caps.dynamic_state3 = d.dynamic_state3->extendedDynamicState3PolygonMode != VK_FALSE;
    d.caps.buffer_address = f12->bufferDeviceAddress != VK_FALSE;
    d.caps.float16 = f12->shaderFloat16 != VK_FALSE;

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

    std::fprintf(stderr, "[vk] %s, Vulkan %u.%u.%u: push descriptors %d, dynamic state 3 %d, vertex input %d, "
                         "pipeline library %d, shader object %d, local read %d, host memory %d\n",
                 d.props.deviceName, VK_API_VERSION_MAJOR(d.props.apiVersion), VK_API_VERSION_MINOR(d.props.apiVersion),
                 VK_API_VERSION_PATCH(d.props.apiVersion), d.caps.push_descriptor, d.caps.dynamic_state3, d.caps.vertex_input,
                 d.caps.pipeline_library, d.caps.shader_object, d.caps.local_read, d.caps.host_memory);
    return true;
}

void vk_device_destroy(vk_device& d)
{
    if (d.vma) vmaDestroyAllocator(d.vma);
    if (d.dev) vkDestroyDevice(d.dev, nullptr);
    if (d.messenger) vkDestroyDebugUtilsMessengerEXT(d.instance, d.messenger, nullptr);
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    d = vk_device{};
}
