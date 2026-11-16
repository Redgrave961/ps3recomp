#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#else
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include "rsx_vk.h"
#include "vk_device.h"
#include "vk_renderer.h"

#include <SDL3/SDL.h>


#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr uint32_t k_frames = 3;

struct frame {
    VkSemaphore acquired = VK_NULL_HANDLE;
    uint64_t done = 0;
};

struct swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format{ VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> ready;
};

struct state {
    std::string title;
    std::mutex lock;
    std::condition_variable cv;
    int status = 0;
    SDL_Window* window = nullptr;
    std::atomic<uint32_t> pixel_w{ 0 };
    std::atomic<uint32_t> pixel_h{ 0 };
    std::atomic<bool> resized{ false };
    vk_device d;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    swapchain sc;
    frame frames[k_frames];
    uint32_t frame_index = 0;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted = 0;
    vk::renderer r;
};

state g;

VkSurfaceFormatKHR pick_format()
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.d.gpu, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> list(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.d.gpu, g.surface, &n, list.data());
    for (VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
        for (const VkSurfaceFormatKHR& f : list)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    if (list.empty() || list[0].format == VK_FORMAT_UNDEFINED)
        return { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    return list[0];
}

VkPresentModeKHR pick_mode()
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.d.gpu, g.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> list(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.d.gpu, g.surface, &n, list.data());
    for (VkPresentModeKHR want : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR })
        if (std::find(list.begin(), list.end(), want) != list.end()) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool swapchain_build()
{
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.d.gpu, g.surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(g.pixel_w.load(), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(g.pixel_h.load(), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    g.sc.extent = extent;
    if (!extent.width || !extent.height) return true;
    if (g.sc.format.format == VK_FORMAT_UNDEFINED) {
        g.sc.format = pick_format();
        g.sc.mode = pick_mode();
    }
    uint32_t count = std::max(caps.minImageCount + 1u, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g.surface;
    ci.minImageCount = count;
    ci.imageFormat = g.sc.format.format;
    ci.imageColorSpace = g.sc.format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) & caps.supportedUsageFlags;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
        : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & (0u - caps.supportedCompositeAlpha));
    ci.presentMode = g.sc.mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.sc.handle;
    VkSwapchainKHR made;
    VK_TRY(vkCreateSwapchainKHR(g.d.dev, &ci, nullptr, &made));
    if (g.sc.handle) vkDestroySwapchainKHR(g.d.dev, g.sc.handle, nullptr);
    g.sc.handle = made;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g.d.dev, made, &n, nullptr);
    g.sc.images.resize(n);
    vkGetSwapchainImagesKHR(g.d.dev, made, &n, g.sc.images.data());
    while (g.sc.ready.size() < n) {
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore s;
        VK_TRY(vkCreateSemaphore(g.d.dev, &si, nullptr, &s));
        g.sc.ready.push_back(s);
    }
    return true;
}

bool frames_build()
{
    VkSemaphoreTypeCreateInfo ti{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti };
    VK_TRY(vkCreateSemaphore(g.d.dev, &tci, nullptr, &g.timeline));
    for (frame& f : g.frames) {
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_TRY(vkCreateSemaphore(g.d.dev, &si, nullptr, &f.acquired));
    }
    const char* base = SDL_GetBasePath();
    const std::string cache = std::string(base ? base : "") + "vk_pipelines.bin";
    return g.r.open(g.d, g.timeline, &g.submitted, cache.c_str());
}

bool create_surface()
{
    const SDL_PropertiesID props = SDL_GetWindowProperties(g.window);
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    ci.hinstance = static_cast<HINSTANCE>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr));
    ci.hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    auto make = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(g.d.instance, "vkCreateWin32SurfaceKHR"));
    return ci.hwnd && make && make(g.d.instance, &ci, nullptr, &g.surface) == VK_SUCCESS;
#else
    if (void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
        VkWaylandSurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
        ci.display = static_cast<wl_display*>(display);
        ci.surface = static_cast<wl_surface*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
        auto make = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(vkGetInstanceProcAddr(g.d.instance, "vkCreateWaylandSurfaceKHR"));
        return make && make(g.d.instance, &ci, nullptr, &g.surface) == VK_SUCCESS;
    }
    VkXlibSurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    ci.dpy = static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    ci.window = (Window)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    auto make = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(g.d.instance, "vkCreateXlibSurfaceKHR"));
    return ci.dpy && make && make(g.d.instance, &ci, nullptr, &g.surface) == VK_SUCCESS;
#endif
}

double since_start()
{
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

bool stats_on()
{
    static const bool on = [] { const char* e = std::getenv("PS3_VK_STATS"); return e && *e && *e != '0'; }();
    return on;
}

void mark(const char* what)
{
    if (stats_on()) std::fprintf(stderr, "[vk] %.3f s %s\n", since_start(), what);
}

bool vulkan_open()
{
#ifdef _WIN32
    const char* ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
#else
    const bool wayland = SDL_GetPointerProperty(SDL_GetWindowProperties(g.window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr) != nullptr;
    const char* ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, wayland ? VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME : VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
#endif
    mark("window");
    if (!vk_instance_create(g.d, ext, 2)) return false;
    mark("instance");
    if (!create_surface()) {
        std::fprintf(stderr, "[vk] surface creation failed\n");
        return false;
    }
    if (!vk_device_create(g.d, g.surface)) return false;
    mark("device");
    if (!frames_build()) return false;
    mark("renderer");
    if (!swapchain_build()) return false;
    mark("swapchain");
    return true;
}

void window_main()
{
    int ok = -1;
    since_start();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        mark("sdl video");
        g.window = SDL_CreateWindow(g.title.c_str(), 1280, 720,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (g.window) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g.window, &w, &h);
            g.pixel_w = (uint32_t)w;
            g.pixel_h = (uint32_t)h;
            if (vulkan_open()) ok = 1;
        }
    }
    if (ok < 0 && !g.d.dev) std::fprintf(stderr, "[vk] window: %s\n", SDL_GetError());
    {
        std::lock_guard<std::mutex> hold(g.lock);
        g.status = ok;
    }
    g.cv.notify_all();
    if (ok < 0) return;
    SDL_Event e;
    while (SDL_WaitEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            std::fflush(nullptr);
            std::_Exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            g.pixel_w = (uint32_t)e.window.data1;
            g.pixel_h = (uint32_t)e.window.data2;
            g.resized = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                SDL_SetWindowFullscreen(g.window, !(SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN));
            break;
        default:
            break;
        }
    }
}

void blit(VkCommandBuffer cmd, vk::image& src, VkImage target, uint32_t w, uint32_t h)
{
    constexpr VkPipelineStageFlags2 xfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    vk::renderer::barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = xfer;
    b.dstStageMask = xfer;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);

    const VkClearColorValue black{};
    const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    b.srcStageMask = xfer;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &di);

    const VkExtent2D out = g.sc.extent;
    const float scale = std::min((float)out.width / (float)w, (float)out.height / (float)h);
    const int32_t dw = std::max(1, (int32_t)((float)w * scale + 0.5f));
    const int32_t dh = std::max(1, (int32_t)((float)h * scale + 0.5f));
    const int32_t x0 = ((int32_t)out.width - dw) / 2;
    const int32_t y0 = ((int32_t)out.height - dh) / 2;
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[0] = { x0, y0, 0 };
    region.dstOffsets[1] = { x0 + dw, y0 + dh, 1 };
    vkCmdBlitImage(cmd, src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);

    b.srcStageMask = xfer;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &di);
}

int vk_open(void*)
{
    std::unique_lock<std::mutex> hold(g.lock);
    g.cv.wait(hold, [] { return g.status != 0; });
    return g.status > 0 ? 0 : -1;
}

void vk_methods(void*, u32 reg, u32 inc, const u32* be, u32 count)
{
    g.r.methods(reg, inc, be, count);
}

void vk_flush(void*)
{
    g.r.flush();
}

void vk_reset(void*)
{
    g.r.flip_reset();
}

void vk_read_back(void*, u32 ea, u32 size)
{
    g.r.read_back(ea, size);
}

void vk_wrote(void*, u32 ea, u32 size)
{
    g.r.wrote(ea, size);
}

void vk_present(void*, u32, const rsx_display_buffer* db)
{
    if (g.resized.exchange(false)) {
        g.r.flush();
        vkDeviceWaitIdle(g.d.dev);
        if (!swapchain_build()) return;
    }
    const uint32_t w = db->width, h = db->height, pitch = db->pitch;
    const u32 ea = (w && h && pitch >= w * 4u) ? rsx_address(RSX_DMA_LOCAL, db->offset, pitch * h) : 0u;
    if (!g.sc.extent.width || !g.sc.extent.height || !ea) {
        g.r.flush();
        return;
    }

    frame& f = g.frames[g.frame_index];
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g.timeline;
    wi.pValues = &f.done;
    vkWaitSemaphores(g.d.dev, &wi, UINT64_MAX);

    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(g.d.dev, g.sc.handle, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g.resized = true;
        g.r.flush();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        g.r.flush();
        return;
    }
    if (r == VK_SUBOPTIMAL_KHR) g.resized = true;

    vk::image* src = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    g.r.present_source(ea, w, h, pitch, src, cmd);
    if (src) {
        blit(cmd, *src, g.sc.images[index], w, h);
    } else {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g.sc.images[index];
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    g.r.submit(f.acquired, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, g.sc.ready[index]);
    f.done = g.submitted;

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.sc.ready[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.sc.handle;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(g.d.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.resized = true;
    g.frame_index = (g.frame_index + 1u) % k_frames;
    static bool first = true;
    if (first) {
        first = false;
        mark("first present");
    }
    static const bool report = [] { const char* e = std::getenv("PS3_VK_STATS"); return e && *e && *e != '0'; }();
    static uint64_t presents = 0;
    if (report && ++presents % 120u == 0) {
        const auto& s = g.r.stats;
        std::fprintf(stderr, "[vk] presents %llu draws %llu clears %llu skip fb/shader/pipe %llu/%llu/%llu uploads %llu present surface/memory %llu/%llu\n",
                     (unsigned long long)presents, (unsigned long long)s.draws, (unsigned long long)s.clears, (unsigned long long)s.skipped_fb,
                     (unsigned long long)s.skipped_shader, (unsigned long long)s.skipped_pipeline, (unsigned long long)s.uploads,
                     (unsigned long long)s.present_surface, (unsigned long long)s.present_memory);
    }
}

void vk_close(void*)
{
    if (!g.d.dev) return;
    g.r.flush();
    g.r.close();
}

}

extern "C" const rsx_renderer* rsx_vk_renderer(const char* title)
{
    static const rsx_renderer renderer = {
        nullptr, vk_open, vk_methods, vk_flush, nullptr, vk_reset, nullptr, vk_present, vk_read_back, vk_wrote, vk_close,
    };
    static std::once_flag started;
    std::call_once(started, [title] {
        g.title = title && *title ? title : "ps3recomp";
        std::thread(window_main).detach();
    });
    return &renderer;
}
