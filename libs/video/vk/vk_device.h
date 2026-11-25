#pragma once

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

#define VK_TRY(expr) \
    do { const VkResult vk_try_r = (expr); if (vk_try_r != VK_SUCCESS) { std::fprintf(stderr, "[vk] %s failed: %d\n", #expr, (int)vk_try_r); return false; } } while (0)

struct vk_caps {
    bool validation;
    bool push_descriptor;
    bool dynamic_state3;
    bool pipeline_library;
    bool custom_border_color;
    bool depth_clip_enable;
    bool provoking_vertex;
    bool maintenance5;
    bool memory_budget;
    bool buffer_address;
    bool fifo_latest_ready;
    bool mirror_clamp_to_edge;
    uint32_t max_custom_border_samplers;
};

struct vk_device {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex queue_lock;
    uint32_t family = 0;
    VmaAllocator vma = nullptr;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures2 features{};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT* dynamic_state3 = nullptr;
    vk_caps caps{};
    std::vector<std::unique_ptr<uint8_t[]>> chain;
};

bool vk_instance_create(vk_device& d, const char* const* extensions, uint32_t count);
bool vk_device_create(vk_device& d, VkSurfaceKHR surface);
