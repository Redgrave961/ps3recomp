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
#include "vk_smaa.h"
#include "vk_trophy_toast.h"
#include "../../input/pad_bindings.h"
#include "ps3emu/thread_name.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#if defined(ASURA_PGO_GEN)
extern "C" int __llvm_profile_write_file(void);
#endif

namespace {

constexpr uint32_t k_slots = 4;
constexpr uint32_t k_present_frames = 3;


int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct slot {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation mem = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool fitted = false;
    uint64_t rendered = 0;
    uint64_t released = 0;
};

struct present_frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    uint64_t done = 0;
};

struct swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format{ VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> ready;
};

struct state {
    std::string title;
    std::string cache_path;
    std::string icon_path;
    std::mutex lock;
    std::condition_variable cv;
    int status = 0;
    SDL_Window* window = nullptr;
    std::atomic<uint32_t> pixel_w{ 0 };
    std::atomic<uint32_t> pixel_h{ 0 };
    std::atomic<bool> resized{ false };
    std::atomic<bool> display_changed{ false };
    std::atomic<float> refresh{ 0.f };

    vk_device d;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    swapchain sc;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted = 0;
    vk::renderer r;
    vk::smaa smaa;
    vk::trophy_toast toast;

    slot slots[k_slots];
    std::atomic<uint64_t> fit{ 0 };
    std::mutex queue_lock;
    std::condition_variable queue_cv;
    int queued = -1;
    int latest = -1;
    bool latest_new = false;
    int shown = -1;
    uint64_t tick = 0;

    VkSemaphore present_timeline = VK_NULL_HANDLE;
    std::atomic<uint64_t> presented{ 0 };
    present_frame frames[k_present_frames];
    uint32_t frame_index = 0;
    std::atomic<uint32_t> frame_rate{ 0 };
    std::atomic<bool> replan{ false };
    std::atomic<bool> free_flips{ false };
    std::atomic<rsx_vk_display> display{ RSX_VK_BORDERLESS };
    rsx_vk_display last_fullscreen = RSX_VK_BORDERLESS;
    std::atomic<void (*)(rsx_vk_display)> display_listener{ nullptr };
    std::atomic<uint32_t> display_event{ 0 };
    std::atomic<uint64_t> scale{ (3ull << 32) | 1u };
    std::atomic<rsx_vk_aa> aa{ RSX_VK_AA_SMAA };
    std::mutex modes_lock;
    std::vector<rsx_vk_mode> modes;
    std::atomic<void (*)()> modes_listener{ nullptr };
    std::mutex mouse_lock;
    float mouse_dx = 0.f, mouse_dy = 0.f, mouse_travel = 0.f;
    uint32_t mouse_buttons = 0;
    int mouse_wheel = 0;
};

state g;

bool stats_on()
{
    static const bool on = [] { const char* e = std::getenv("PS3_VK_STATS"); return e && *e && *e != '0'; }();
    return on;
}

double since_start()
{
    static const int64_t t0 = now_ns();
    return (double)(now_ns() - t0) / 1e9;
}

void mark(const char* what)
{
    if (stats_on()) std::fprintf(stderr, "[vk] %.3f s %s\n", since_start(), what);
}

void read_refresh()
{
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g.window));
    g.refresh = mode ? mode->refresh_rate : 0.f;
}

vk::ratio resolution_scale()
{
    const uint64_t v = g.scale.load(std::memory_order_relaxed);
    const vk::ratio r = vk::ratio::of((uint32_t)(v >> 32), (uint32_t)v), top{ 8, 1 };
    return r > top ? top : vk::ratio{} > r ? vk::ratio{} : r;
}

void read_modes()
{
    int n = 0;
    SDL_DisplayMode** list = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(g.window), &n);
    std::vector<rsx_vk_mode> modes;
    modes.reserve((size_t)n);
    for (int i = 0; i < n; i++)
        modes.push_back({ (u32)std::lround(list[i]->w * list[i]->pixel_density), (u32)std::lround(list[i]->h * list[i]->pixel_density), list[i]->refresh_rate });
    SDL_free(list);
    {
        std::lock_guard<std::mutex> hold(g.modes_lock);
        if (modes.size() == g.modes.size() && std::equal(modes.begin(), modes.end(), g.modes.begin(), [](const rsx_vk_mode& a, const rsx_vk_mode& b) {
                return a.width == b.width && a.height == b.height && a.refresh == b.refresh;
            }))
            return;
        g.modes = std::move(modes);
    }
    if (const auto listener = g.modes_listener.load()) listener();
}

void report_scale()
{
    const vk::ratio& s = g.r.resolution_scale;
    std::fprintf(stderr, "[vk] resolution %ux%u for a 1280x720 frame (scale %u/%u)\n", s.edge(1280), s.edge(720), s.num, s.den);
}

bool exclusive_mode(SDL_DisplayMode& out)
{
    const SDL_DisplayID display = SDL_GetDisplayForWindow(g.window);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (!desktop) return false;
    out = *desktop;
    int n = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &n);
    for (int i = 0; i < n; i++)
        if (modes[i]->w == out.w && modes[i]->h == out.h && modes[i]->pixel_density == out.pixel_density && modes[i]->refresh_rate > out.refresh_rate)
            out = *modes[i];
    SDL_free(modes);
    return true;
}

void apply_display()
{
    const rsx_vk_display display = g.display;
    if (display == RSX_VK_WINDOWED) {
        SDL_SetWindowFullscreen(g.window, false);
        return;
    }
    g.last_fullscreen = display;
    SDL_DisplayMode exclusive;
    SDL_SetWindowFullscreenMode(g.window, display == RSX_VK_FULLSCREEN && exclusive_mode(exclusive) ? &exclusive : nullptr);
    SDL_SetWindowFullscreen(g.window, true);
}

VkSurfaceFormatKHR pick_format()
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.d.gpu, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> list(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.d.gpu, g.surface, &n, list.data());
    for (VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
        for (const VkSurfaceFormatKHR& f : list)
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    if (list.empty() || list[0].format == VK_FORMAT_UNDEFINED) return { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    return list[0];
}

bool uncapped() { return g.free_flips && !g.frame_rate.load(); }

VkPresentModeKHR pick_mode()
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.d.gpu, g.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> list(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.d.gpu, g.surface, &n, list.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(list.begin(), list.end(), m) != list.end(); };
    if (!g.free_flips) return VK_PRESENT_MODE_FIFO_KHR;
    if (uncapped() && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    return g.d.caps.fifo_latest_ready && has(VK_PRESENT_MODE_FIFO_LATEST_READY_KHR) ? VK_PRESENT_MODE_FIFO_LATEST_READY_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

void apply_plan()
{
    const float hz = g.refresh.load();
    const bool flips = g.free_flips;
    const uint32_t cap = flips ? g.frame_rate.load() : 30u;
    rsx_core_set_frame_limit(flips ? cap : 0);
    const VkPresentModeKHR mode = pick_mode();
    if (mode != g.sc.mode) g.resized = true;
    std::fprintf(stderr, "[vk] %.2f Hz display, %u fps cap: %s, %s\n", hz, cap,
                 mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR ? "fifo latest ready" : "fifo",
                 !flips ? "59.94 Hz console clock" : !cap ? "uncapped" : "timer limited");
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
    g.fit.store(0, std::memory_order_release);
    if (!extent.width || !extent.height) return true;
    if (g.sc.format.format == VK_FORMAT_UNDEFINED) g.sc.format = pick_format();
    g.sc.mode = pick_mode();
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
    for (VkImageView v : g.sc.views)
        if (v) vkDestroyImageView(g.d.dev, v, nullptr);
    g.sc.views.clear();
    if (g.sc.handle) vkDestroySwapchainKHR(g.d.dev, g.sc.handle, nullptr);
    g.sc.handle = VK_NULL_HANDLE;
    VkSwapchainKHR made;
    VK_TRY(vkCreateSwapchainKHR(g.d.dev, &ci, nullptr, &made));
    g.sc.handle = made;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g.d.dev, made, &n, nullptr);
    g.sc.images.resize(n);
    vkGetSwapchainImagesKHR(g.d.dev, made, &n, g.sc.images.data());
    g.sc.views.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g.sc.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g.sc.format.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(g.d.dev, &vi, nullptr, &g.sc.views[i]);
    }
    while (g.sc.ready.size() < n) {
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore s;
        VK_TRY(vkCreateSemaphore(g.d.dev, &si, nullptr, &s));
        g.sc.ready.push_back(s);
    }
    const VkFormat f = g.sc.format.format;
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM)
        g.fit.store((uint64_t)extent.width << 40 | (uint64_t)extent.height << 20 | (uint64_t)f, std::memory_order_release);
    return true;
}

bool timeline_create(VkSemaphore& out)
{
    VkSemaphoreTypeCreateInfo ti{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti };
    VK_TRY(vkCreateSemaphore(g.d.dev, &ci, nullptr, &out));
    return true;
}

void timeline_wait(VkSemaphore s, uint64_t value)
{
    if (!value) return;
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s;
    wi.pValues = &value;
    vkWaitSemaphores(g.d.dev, &wi, UINT64_MAX);
}

bool presenter_build()
{
    if (!timeline_create(g.timeline) || !timeline_create(g.present_timeline)) return false;
    for (present_frame& f : g.frames) {
        VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = g.d.family;
        VK_TRY(vkCreateCommandPool(g.d.dev, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(g.d.dev, &ai, &f.cmd));
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_TRY(vkCreateSemaphore(g.d.dev, &si, nullptr, &f.acquired));
    }
    g.r.resolution_scale = resolution_scale();
    report_scale();
    if (!g.r.open(g.d, g.timeline, &g.submitted, g.cache_path.c_str())) return false;
    g.smaa.create(g.d);
    g.toast.create(g.d);
    return true;
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
        return ci.surface && make && make(g.d.instance, &ci, nullptr, &g.surface) == VK_SUCCESS;
    }
    VkXlibSurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    ci.dpy = static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    ci.window = (Window)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    auto make = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(g.d.instance, "vkCreateXlibSurfaceKHR"));
    return ci.dpy && make && make(g.d.instance, &ci, nullptr, &g.surface) == VK_SUCCESS;
#endif
}

VkImageMemoryBarrier2 to_transfer_dst(VkImage image, VkPipelineStageFlags2 src_stage, VkPipelineStageFlags2 dst_stage)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return b;
}

void barrier_one(VkCommandBuffer cmd, const VkImageMemoryBarrier2& b)
{
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void blit_fitted(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, VkImage dst, VkExtent2D out)
{
    const float scale = std::min((float)out.width / (float)sw, (float)out.height / (float)sh);
    const int32_t dw = std::max(1, (int32_t)std::lround((float)sw * scale));
    const int32_t dh = std::max(1, (int32_t)std::lround((float)sh * scale));
    const int32_t x0 = ((int32_t)out.width - dw) / 2, y0 = ((int32_t)out.height - dh) / 2;
    if (dw != (int32_t)out.width || dh != (int32_t)out.height) {
        const VkClearColorValue black{};
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
        VkImageMemoryBarrier2 b = to_transfer_dst(dst, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier_one(cmd, b);
    }
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[0] = { x0, y0, 0 };
    region.dstOffsets[1] = { x0 + dw, y0 + dh, 1 };
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
}

void record_present(VkCommandBuffer cmd, const slot& s, VkImage target, VkImageView target_view)
{
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier2 b = to_transfer_dst(target, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    barrier_one(cmd, b);
    const VkExtent2D out = g.sc.extent;
    if (s.fitted && s.width == out.width && s.height == out.height && s.format == g.sc.format.format) {
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { out.width, out.height, 1 };
        vkCmdCopyImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        blit_fitted(cmd, s.image, s.width, s.height, target, out);
    }
    if (!g.toast.record(cmd, target, target_view, out, g.sc.format.format)) {
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier_one(cmd, b);
    }
    vkEndCommandBuffer(cmd);
}

void rebuild_swapchain()
{
    {
        std::lock_guard<std::mutex> hold(g.d.queue_lock);
        vkDeviceWaitIdle(g.d.dev);
    }
    if (!swapchain_build()) std::fprintf(stderr, "[vk] swapchain rebuild failed\n");
}

bool swapchain_extent_changed()
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.d.gpu, g.surface, &caps) != VK_SUCCESS) return true;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(g.pixel_w.load(), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(g.pixel_h.load(), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    return extent.width != g.sc.extent.width || extent.height != g.sc.extent.height;
}

bool present_image(slot& s)
{
    if (!g.sc.extent.width || !g.sc.extent.height) {
        s.released = 0;
        return false;
    }
    present_frame& f = g.frames[g.frame_index];
    timeline_wait(g.present_timeline, f.done);
    uint32_t index = 0;
    const VkResult a = vkAcquireNextImageKHR(g.d.dev, g.sc.handle, 100000000ull, f.acquired, VK_NULL_HANDLE, &index);
    if (a == VK_TIMEOUT || a == VK_NOT_READY) {
        s.released = 0;
        return false;
    }
    if (a != VK_SUCCESS && a != VK_SUBOPTIMAL_KHR) {
        g.resized = true;
        s.released = 0;
        return false;
    }
    if (a == VK_SUBOPTIMAL_KHR && swapchain_extent_changed()) g.resized = true;
    vkResetCommandPool(g.d.dev, f.pool, 0);
    record_present(f.cmd, s, g.sc.images[index], g.sc.views[index]);

    const uint64_t value = g.presented.load() + 1u;
    VkSemaphoreSubmitInfo waits[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    waits[0].semaphore = f.acquired;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    waits[1].semaphore = g.timeline;
    waits[1].value = s.rendered;
    waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    VkSemaphoreSubmitInfo signals[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    signals[0].semaphore = g.sc.ready[index];
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].semaphore = g.present_timeline;
    signals[1].value = value;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cb.commandBuffer = f.cmd;
    VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = 2;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = 2;
    si.pSignalSemaphoreInfos = signals;

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.sc.ready[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.sc.handle;
    pi.pImageIndices = &index;
    VkResult r;
    {
        std::lock_guard<std::mutex> hold(g.d.queue_lock);
        vkQueueSubmit2(g.d.queue, 1, &si, VK_NULL_HANDLE);
        r = vkQueuePresentKHR(g.d.queue, &pi);
    }
    {
        static int64_t last, lo = INT64_MAX, hi, sum, sq;
        static uint32_t n;
        const int64_t now = now_ns();
        if (last && stats_on()) {
            const int64_t d = now - last;
            lo = std::min(lo, d);
            hi = std::max(hi, d);
            sum += d;
            sq += (d / 1000) * (d / 1000);
            static int64_t seq[120];
            static uint32_t sn, dumped;
            if (dumped < 2 && n >= 300) {
                seq[sn++] = d;
                if (sn == 120) {
                    std::string line = "[vk] intervals:";
                    for (uint32_t k = 0; k < 120; k++) line += " " + std::to_string((int)(seq[k] / 100000) / 10.0).substr(0, 4);
                    std::fprintf(stderr, "%s\n", line.c_str());
                    sn = 0;
                    dumped++;
                }
            }
            if (++n == 600) {
                const double mean = (double)sum / n / 1e6, var = (double)sq / n / 1e6 - mean * mean;
                std::fprintf(stderr, "[vk] present interval mean %.3f ms, sd %.3f, min %.3f, max %.3f\n", mean, std::sqrt(std::max(var, 0.0)),
                             lo / 1e6, hi / 1e6);
                n = 0;
                lo = INT64_MAX;
                hi = sum = sq = 0;
            }
        }
        last = now;
    }
    g.presented = value;
    f.done = value;
    s.released = value;
    g.frame_index = (g.frame_index + 1u) % k_present_frames;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && swapchain_extent_changed())) g.resized = true;
    else if (r != VK_SUCCESS) std::fprintf(stderr, "[vk] present failed: %d\n", (int)r);

    static bool first = true;
    if (first) {
        first = false;
        mark("first present");
    }
    return true;
}

void present_main()
{
    ps3_thread_name("vk present");
    g.resized = false;
    if (!swapchain_build()) std::fprintf(stderr, "[vk] swapchain creation failed\n");
    mark("swapchain");
    uint64_t seen = 0;
    bool rebuilt = false;
    for (;;) {
        uint64_t tick;
        {
            std::unique_lock<std::mutex> hold(g.queue_lock);
            g.queue_cv.wait(hold, [&] { return g.tick != seen || g.resized.load() || g.display_changed.load() || g.replan.load(); });
            tick = g.tick;
        }
        if (g.display_changed.exchange(false)) {
            read_refresh();
            g.replan = true;
        }
        if (g.replan.exchange(false)) apply_plan();
        if (g.resized.exchange(false)) {
            rebuild_swapchain();
            rebuilt = true;
        }
        if (tick == seen && !rebuilt) continue;
        seen = tick;
        int pick = -1;
        {
            std::lock_guard<std::mutex> hold(g.queue_lock);
            if (g.latest_new) {
                g.shown = g.latest;
                g.latest_new = false;
                pick = g.shown;
            } else if (rebuilt) {
                pick = g.shown;
            }
        }
        if (pick < 0) continue;
        rebuilt = false;
        present_image(g.slots[pick]);
    }
}

void vk_vblank(void*, u64 count)
{
    if (g.free_flips) return;
    {
        std::lock_guard<std::mutex> hold(g.queue_lock);
        g.tick = count;
        if (g.queued >= 0) {
            g.latest = g.queued;
            g.latest_new = true;
            g.queued = -1;
        }
    }
    g.queue_cv.notify_one();
}
bool slot_reserve(slot& s, uint32_t w, uint32_t h, VkFormat format)
{
    if (s.image && s.width == w && s.height == h && s.format == format) return true;
    timeline_wait(g.present_timeline, g.presented.load());
    if (s.view) vkDestroyImageView(g.d.dev, s.view, nullptr);
    s.view = VK_NULL_HANDLE;
    if (s.image) vmaDestroyImage(g.d.vma, s.image, s.mem);
    s.image = VK_NULL_HANDLE;
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_TRY(vmaCreateImage(g.d.vma, &ii, &ai, &s.image, &s.mem, nullptr));
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = s.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(g.d.dev, &vi, nullptr, &s.view) != VK_SUCCESS) s.view = VK_NULL_HANDLE;
    s.format = format;
    s.width = w;
    s.height = h;
    s.released = 0;
    return true;
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
    if (!vk_instance_create(g.d, ext, (uint32_t)std::size(ext))) return false;
    mark("instance");
    if (!create_surface()) {
        std::fprintf(stderr, "[vk] surface creation failed\n");
        return false;
    }
    if (!vk_device_create(g.d, g.surface)) return false;
    mark("device");
    if (!presenter_build()) return false;
    mark("renderer");
    read_refresh();
    apply_plan();
    return true;
}

void update_hidden()
{
    const bool hidden = (SDL_GetWindowFlags(g.window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) != 0;
    rsx_core_set_hidden_refresh(hidden ? std::max(1u, (u32)std::lround(g.refresh.load())) : 0u);
}

void window_main()
{
    ps3_thread_name("vk window");
    int ok = -1;
    since_start();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        mark("sdl video");
        g.display_event = SDL_RegisterEvents(1);
        g.window = SDL_CreateWindow(g.title.c_str(), 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        if (g.window) {
            if (SDL_Surface* icon = g.icon_path.empty() ? nullptr : SDL_LoadPNG(g.icon_path.c_str())) {
                SDL_SetWindowIcon(g.window, icon);
                SDL_DestroySurface(icon);
            }
            apply_display();
            SDL_HideCursor();
            SDL_ShowWindow(g.window);
            SDL_RaiseWindow(g.window);
            SDL_SyncWindow(g.window);
            read_modes();
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g.window, &w, &h);
            g.pixel_w = (uint32_t)w;
            g.pixel_h = (uint32_t)h;
            if (vulkan_open()) ok = 1;
        }
    }
    if (ok < 0 && !g.d.dev) std::fprintf(stderr, "[vk] window: %s\n", SDL_GetError());
    if (ok > 0) std::thread(present_main).detach();
    {
        std::lock_guard<std::mutex> hold(g.lock);
        g.status = ok;
    }
    g.cv.notify_all();
    if (ok < 0) return;
    SDL_Event e;
    bool relative = false;
    for (;;) {
        const bool got = SDL_WaitEventTimeout(&e, 50);
        const bool look = (SDL_GetWindowFlags(g.window) & SDL_WINDOW_INPUT_FOCUS) && !pad_menu_active();
        if (look != relative) {
            relative = look;
            SDL_SetWindowRelativeMouseMode(g.window, look);
            std::lock_guard<std::mutex> hold(g.mouse_lock);
            g.mouse_dx = g.mouse_dy = 0.f;
        }
        if (!got) continue;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            std::lock_guard<std::mutex> hold(g.mouse_lock);
            g.mouse_buttons = e.motion.state;
            g.mouse_travel += std::fabs(e.motion.xrel) + std::fabs(e.motion.yrel);
            if (relative) {
                g.mouse_dx += e.motion.xrel;
                g.mouse_dy += e.motion.yrel;
            }
            continue;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            std::lock_guard<std::mutex> hold(g.mouse_lock);
            g.mouse_buttons = e.button.down ? g.mouse_buttons | SDL_BUTTON_MASK(e.button.button) : g.mouse_buttons & ~SDL_BUTTON_MASK(e.button.button);
            continue;
        }
        if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            std::lock_guard<std::mutex> hold(g.mouse_lock);
            g.mouse_wheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.integer_y : e.wheel.integer_y;
            continue;
        }
        if (e.type == g.display_event) {
            apply_display();
            continue;
        }
        switch (e.type) {
        case SDL_EVENT_QUIT:
            g.r.save_caches();
#if defined(ASURA_PGO_GEN)
            __llvm_profile_write_file();
#endif
            std::fflush(nullptr);
            std::_Exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            g.pixel_w = (uint32_t)e.window.data1;
            g.pixel_h = (uint32_t)e.window.data2;
            g.resized = true;
            g.queue_cv.notify_one();
            break;
        case SDL_EVENT_DISPLAY_ADDED:
        case SDL_EVENT_DISPLAY_REMOVED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
            read_modes();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            read_modes();
            g.display_changed = true;
            g.resized = true;
            g.queue_cv.notify_one();
            break;
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_OCCLUDED:
        case SDL_EVENT_WINDOW_EXPOSED:
            update_hidden();
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            update_hidden();
            g.display_changed = true;
            g.resized = true;
            g.queue_cv.notify_one();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
                const rsx_vk_display next = g.display == RSX_VK_WINDOWED ? g.last_fullscreen : RSX_VK_WINDOWED;
                g.display = next;
                apply_display();
                if (const auto listener = g.display_listener.load()) listener(next);
            }
            break;
        default:
            break;
        }
    }
}

int vk_open(void*)
{
    std::unique_lock<std::mutex> hold(g.lock);
    g.cv.wait(hold, [] { return g.status != 0; });
    return g.status > 0 ? 0 : -1;
}

std::atomic<int64_t> g_rsx_cpu_ns{ 0 };

int64_t thread_cpu_ns()
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    return (int64_t)((((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) + (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) * 100;
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
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
    if (stats_on()) g_rsx_cpu_ns.store(thread_cpu_ns(), std::memory_order_relaxed);
    g.r.flip_reset();
    if (const vk::ratio scale = resolution_scale(); scale != g.r.resolution_scale) {
        g.r.rescale(scale);
        report_scale();
    }
}

void vk_sync(void*)
{
    g.r.drain();
}

int vk_report(void*, u32 type, u32 ea)
{
    return g.r.report(type, ea) ? 1 : 0;
}

int vk_defer(void*, const rsx_write* w)
{
    return g.r.defer(*w) ? 1 : 0;
}

void vk_read_back(void*, u32 ea, u32 size)
{
    g.r.read_back(ea, size);
}

void vk_settle(void*, u32 ea, u32 size)
{
    g.r.settle(ea, size);
}

void vk_wrote(void*, u32 ea, u32 size)
{
    g.r.wrote(ea, size);
}

int vk_blit(void*, const rsx_blit* b)
{
    return g.r.blit(*b) ? 1 : 0;
}

void write_bmp(const char* path, const uint8_t* bgra, uint32_t w, uint32_t h)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const uint32_t bytes = w * h * 4u;
    uint8_t hdr[54] = { 'B', 'M' };
    const uint32_t fields[] = { 54u + bytes, 0, 54, 40, w, (uint32_t)-(int32_t)h };
    std::memcpy(hdr + 2, &fields[0], 4);
    std::memcpy(hdr + 10, &fields[2], 4);
    std::memcpy(hdr + 14, &fields[3], 4);
    std::memcpy(hdr + 18, &fields[4], 4);
    std::memcpy(hdr + 22, &fields[5], 4);
    hdr[26] = 1;
    hdr[28] = 32;
    std::fwrite(hdr, 1, sizeof hdr, f);
    std::fwrite(bgra, 1, bytes, f);
    std::fclose(f);
}

int vk_present(void*, u32, const rsx_display_buffer* db)
{
    const uint32_t w = db->width, h = db->height, pitch = db->pitch;
    static uint32_t last_w, last_h, last_p;
    if (w != last_w || h != last_h || pitch != last_p) {
        last_w = w;
        last_h = h;
        last_p = pitch;
        std::fprintf(stderr, "[vk] display buffer %ux%u pitch %u offset 0x%08X\n", w, h, pitch, db->offset);
    }
    const u32 ea = (w && h && pitch >= w * 4u) ? rsx_address(RSX_DMA_LOCAL, db->offset, pitch * h) : 0u;
    static u64 shown, shown_fit;
    static rsx_vk_aa shown_aa = RSX_VK_AA_OFF;
    const u64 content = ea ? g.r.content(ea, pitch) : 0;
    const uint64_t fit = g.fit.load(std::memory_order_acquire);
    const rsx_vk_aa aa_mode = g.aa.load(std::memory_order_relaxed);
    if (!ea || (content && content == shown && fit == shown_fit && aa_mode == shown_aa)) {
        g.r.flush();
        return 0;
    }

    uint32_t i = 0;
    {
        std::lock_guard<std::mutex> hold(g.queue_lock);
        while ((int)i == g.shown || (int)i == g.latest || (int)i == g.queued) i++;
    }
    slot& s = g.slots[i];
    timeline_wait(g.present_timeline, s.released);
    vk::image* src = nullptr;
    vk::ratio scale;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    g.r.present_source(ea, w, h, pitch, src, scale, cmd);
    shown = content;
    shown_fit = fit;
    shown_aa = aa_mode;
    const uint32_t sw = std::min(scale.edge(w), src ? src->width : 0u), sh = std::min(scale.edge(h), src ? src->height : 0u);
    if (!src) {
        g.r.flush();
        return 0;
    }
    static const char* capture = std::getenv("PS3_VK_CAPTURE");
    static uint64_t flips;
    ++flips;
    const char* capture_path = nullptr;
    if (capture && flips == std::strtoull(capture, const_cast<char**>(&capture_path), 10) && *capture_path == ':') capture_path++;
    else capture_path = nullptr;
    const VkExtent2D out{ (uint32_t)(fit >> 40), (uint32_t)(fit >> 20) & 0xFFFFFu };
    const VkFormat out_format = (VkFormat)(fit & 0xFFFFFu);
    const bool fitted = fit && !capture_path && src->width == sw && src->height == sh &&
                        (src->format == VK_FORMAT_B8G8R8A8_UNORM || src->format == VK_FORMAT_R8G8B8A8_UNORM);
    if (!(fitted ? slot_reserve(s, out.width, out.height, out_format) : slot_reserve(s, sw, sh, VK_FORMAT_B8G8R8A8_UNORM))) {
        g.r.flush();
        return 0;
    }
    s.fitted = fitted;
    std::optional<vk::trace_span> span;
    if (g.r.tracing()) {
        if (fitted) span.emplace(g.r, cmd, "present scale %ux%u to %ux%u", sw, sh, out.width, out.height);
        else span.emplace(g.r, cmd, "present copy %ux%u", sw, sh);
    }
    if (g.smaa.has_staging()) {
        uint64_t done = 0;
        if (vkGetSemaphoreCounterValue(g.d.dev, g.timeline, &done) == VK_SUCCESS) g.smaa.retire_staging(done);
    }
    const bool aa_ok = aa_mode == RSX_VK_AA_SMAA && src->width == sw && src->height == sh && src->view &&
                       (src->format == VK_FORMAT_B8G8R8A8_UNORM || src->format == VK_FORMAT_R8G8B8A8_UNORM);
    if (aa_mode == RSX_VK_AA_OFF) g.smaa.release_targets();
    const VkExtent2D present_out = fitted ? out : VkExtent2D{ sw, sh };
    const bool want_present_filter = s.view && src->view && src->width == sw && src->height == sh &&
                                     (src->format == VK_FORMAT_B8G8R8A8_UNORM || src->format == VK_FORMAT_R8G8B8A8_UNORM) &&
                                     (s.format == VK_FORMAT_B8G8R8A8_UNORM || s.format == VK_FORMAT_R8G8B8A8_UNORM);

    VkImage blit_src = src->handle;
    VkImageView sample_src = VK_NULL_HANDLE;
    if (aa_ok) {
        vk::renderer::barrier(cmd, *src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        const vk::smaa::output res = g.smaa.record(cmd, src->view, src->format, sw, sh, want_present_filter, g.submitted + 1u);
        if (res.image) {
            blit_src = res.image;
            sample_src = res.view;
        }
    }
    bool filtered = false;
    if (want_present_filter) {
        if (!sample_src) {
            vk::renderer::barrier(cmd, *src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            sample_src = src->view;
        }
        filtered = g.smaa.downscale(cmd, sample_src, sw, sh, s.image, s.view, s.format, present_out);
    }
    if (!filtered) {
        if (blit_src == src->handle)
            vk::renderer::barrier(cmd, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        else if (want_present_filter) {
            VkImageMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            mb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mb.image = blit_src;
            mb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barrier_one(cmd, mb);
        }
        VkImageMemoryBarrier2 b = to_transfer_dst(s.image, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        barrier_one(cmd, b);
        if (fitted) {
            blit_fitted(cmd, blit_src, sw, sh, s.image, out);
        } else {
            VkImageBlit region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
            vkCmdBlitImage(cmd, blit_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                           VK_FILTER_NEAREST);
        }
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier_one(cmd, b);
    }
    span.reset();
    VkBuffer cap_buf = VK_NULL_HANDLE;
    VmaAllocation cap_mem = nullptr;
    VmaAllocationInfo cap_info{};
    if (capture_path) {
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = (VkDeviceSize)sw * sh * 4u;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(g.d.vma, &bi, &ai, &cap_buf, &cap_mem, &cap_info) == VK_SUCCESS) {
            VkBufferImageCopy rg{};
            rg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            rg.imageExtent = { sw, sh, 1 };
            vkCmdCopyImageToBuffer(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cap_buf, 1, &rg);
            VkMemoryBarrier2 mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo hd{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            hd.memoryBarrierCount = 1;
            hd.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &hd);
        }
    }
    g.r.submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    s.rendered = g.submitted;
    if (cap_buf) {
        timeline_wait(g.timeline, s.rendered);
        vmaInvalidateAllocation(g.d.vma, cap_mem, 0, VK_WHOLE_SIZE);
        write_bmp(capture_path, static_cast<const uint8_t*>(cap_info.pMappedData), sw, sh);
        vmaDestroyBuffer(g.d.vma, cap_buf, cap_mem);
        std::fprintf(stderr, "[vk] captured flip %llu to %s\n", (unsigned long long)flips, capture_path);
    }
    {
        std::lock_guard<std::mutex> hold(g.queue_lock);
        if (g.free_flips) {
            g.latest = (int)i;
            g.latest_new = true;
            g.tick++;
        } else {
            g.queued = (int)i;
        }
    }
    g.queue_cv.notify_one();

    struct counters_snapshot { u64 epochs, watch_calls, watch_bytes, copy_bytes, reuse_bytes, scans, write_stalls, surface_stalls, stall_ns, stall_bytes, deferred_transfers, deferred_labels, scan_ns, changed_pages; };
    static uint64_t presents;
    if (stats_on() && ++presents % 600u == 0) {
        static int64_t last_rsx_cpu;
        const int64_t rsx_cpu = g_rsx_cpu_ns.load(std::memory_order_relaxed);
        const auto& st = g.r.stats;
        std::fprintf(stderr, "[vk] frames %llu draws %llu clears %llu uploads %llu gpu blits %llu surface downloads %llu surface loads %llu, rsx thread cpu %.3f ms/frame\n", (unsigned long long)presents,
                     (unsigned long long)st.draws, (unsigned long long)st.clears, (unsigned long long)st.uploads,
                     (unsigned long long)st.blits, (unsigned long long)st.downloads, (unsigned long long)st.surface_loads,
                     (double)(rsx_cpu - last_rsx_cpu) / 600.0 / 1e6);
        static counters_snapshot last;
        std::fprintf(stderr, "[vk] per frame: %.1f epochs, %.1f memory checks over %.2f MB, %.1f write scans, %.2f MB vertex copies, %.2f MB vertex reused\n",
                     (st.epochs - last.epochs) / 600.0, (st.watch_calls - last.watch_calls) / 600.0,
                     (st.watch_bytes - last.watch_bytes) / 600.0 / 1048576.0, (st.scans - last.scans) / 600.0, (st.copy_bytes - last.copy_bytes) / 600.0 / 1048576.0,
                     (st.reuse_bytes - last.reuse_bytes) / 600.0 / 1048576.0);
        std::fprintf(stderr, "[vk] per frame: %.2f deferred transfers, %.2f deferred labels, %.2f pending-write stalls, %.2f surface downloads (%.2f MB), %.3f ms stalled; last at 0x%08X size 0x%X\n",
                     (st.deferred_transfers - last.deferred_transfers) / 600.0, (st.deferred_labels - last.deferred_labels) / 600.0,
                     (st.write_stalls - last.write_stalls) / 600.0, (st.surface_stalls - last.surface_stalls) / 600.0,
                     (st.stall_bytes - last.stall_bytes) / 600.0 / 1048576.0, (st.stall_ns - last.stall_ns) / 600.0 / 1e6, st.stall_ea, st.stall_size);
        std::fprintf(stderr, "[vk] per frame: %.3f ms scanning writes (%.1f us per scan), %.1f pages changed\n",
                     (st.scan_ns - last.scan_ns) / 600.0 / 1e6,
                     st.scans > last.scans ? (st.scan_ns - last.scan_ns) / 1e3 / (double)(st.scans - last.scans) : 0.0,
                     (st.changed_pages - last.changed_pages) / 600.0);
        last = { st.epochs, st.watch_calls, st.watch_bytes, st.copy_bytes, st.reuse_bytes, st.scans, st.write_stalls, st.surface_stalls, st.stall_ns, st.stall_bytes,
                 st.deferred_transfers, st.deferred_labels, st.scan_ns, st.changed_pages };
        last_rsx_cpu = rsx_cpu;
    }
    return 1;
}

void vk_close(void*)
{
    if (!g.d.dev) return;
    g.r.flush();
    {
        std::lock_guard<std::mutex> hold(g.d.queue_lock);
        vkDeviceWaitIdle(g.d.dev);
    }
    g.smaa.destroy();
    g.r.close();
}

}

extern "C" const rsx_renderer* rsx_vk_renderer(const char* title, const char* cache_path, const char* icon_path)
{
    static const rsx_renderer renderer = {
        nullptr, vk_open, vk_methods, vk_flush, vk_sync, vk_reset, vk_report, vk_defer, vk_present, vk_read_back, vk_settle, vk_wrote, vk_blit, vk_vblank, vk_close,
    };
    static std::once_flag started;
    std::call_once(started, [title, cache_path, icon_path] {
        g.title = title && *title ? title : "ps3recomp";
        g.cache_path = cache_path;
        g.icon_path = icon_path;
        std::thread(window_main).detach();
    });
    return &renderer;
}

extern "C" void rsx_vk_set_resolution_scale(u32 num, u32 den)
{
    if (num && den) g.scale.store(((uint64_t)num << 32) | den, std::memory_order_relaxed);
}

extern "C" void rsx_vk_set_aa(enum rsx_vk_aa aa)
{
    g.aa.store(aa, std::memory_order_relaxed);
}

extern "C" void rsx_vk_set_display(rsx_vk_display display)
{
    g.display = display;
    SDL_Event e{};
    e.type = g.display_event;
    if (e.type) SDL_PushEvent(&e);
}

extern "C" void rsx_vk_on_display_toggle(void (*listener)(rsx_vk_display))
{
    g.display_listener = listener;
}

extern "C" u32 rsx_vk_display_modes(rsx_vk_mode* out, u32 capacity)
{
    std::lock_guard<std::mutex> hold(g.modes_lock);
    std::copy_n(g.modes.begin(), std::min<size_t>(capacity, g.modes.size()), out);
    return (u32)g.modes.size();
}

extern "C" void rsx_vk_on_display_modes(void (*listener)(void))
{
    g.modes_listener = listener;
}

extern "C" void rsx_vk_mouse_motion(float* dx, float* dy)
{
    std::lock_guard<std::mutex> hold(g.mouse_lock);
    *dx = g.mouse_dx;
    *dy = g.mouse_dy;
    g.mouse_dx = g.mouse_dy = 0.f;
}

extern "C" u32 rsx_vk_mouse_input(int* wheel, float* travel)
{
    std::lock_guard<std::mutex> hold(g.mouse_lock);
    *wheel = g.mouse_wheel;
    *travel = g.mouse_travel;
    g.mouse_wheel = 0;
    g.mouse_travel = 0.f;
    return g.mouse_buttons;
}

extern "C" void rsx_vk_set_frame_rate(int free_flips, u32 hz)
{
    {
        std::lock_guard<std::mutex> hold(g.queue_lock);
        const bool rate = g.frame_rate.exchange(hz) != hz, flips = g.free_flips.exchange(free_flips != 0) != (free_flips != 0);
        if (!rate && !flips) return;
        if (flips && g.queued >= 0) {
            g.latest = g.queued;
            g.latest_new = true;
            g.queued = -1;
            g.tick++;
        }
        g.replan = true;
    }
    g.queue_cv.notify_one();
}
