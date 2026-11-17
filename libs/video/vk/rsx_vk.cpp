#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <dxgi1_6.h>
#else
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <time.h>
#endif
#include "rsx_vk.h"
#include "vk_device.h"
#include "vk_renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr uint32_t k_slots = 4;
constexpr uint32_t k_present_frames = 3;
constexpr int64_t k_frame_ns = 1001000000ll / 60ll;

enum class pacing { measuring, vrr, locked, free };

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct slot {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation mem = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
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
    std::vector<VkSemaphore> ready;
};

#ifdef _WIN32
struct display_clock {
    std::mutex lock;
    IDXGIFactory1* factory = nullptr;
    IDXGIOutput* output = nullptr;
    HMONITOR monitor = nullptr;
    uint32_t divider = 0;
};
#endif

struct state {
    std::string title;
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

    slot slots[k_slots];
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
    std::atomic<pacing> mode{ pacing::measuring };
    std::atomic<uint32_t> generation{ 0 };
#ifdef _WIN32
    display_clock clock;
#endif
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

const char* pacing_name(pacing p)
{
    switch (p) {
    case pacing::vrr: return "variable refresh: exact 59.94 Hz";
    case pacing::locked: return "locked to display refresh";
    case pacing::free: return "59.94 Hz clock, newest frame each refresh";
    default: return "measuring the display";
    }
}

void sleep_until(int64_t deadline)
{
    const int64_t left = deadline - now_ns();
    if (left <= 0) return;
#ifdef _WIN32
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(left / 100);
    if (due.QuadPart && timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t target = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + left;
    ts.tv_sec = (time_t)(target / 1000000000ll);
    ts.tv_nsec = (long)(target % 1000000000ll);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0) {}
#endif
}

#ifdef _WIN32
int display_vblank(void*)
{
    IDXGIOutput* out;
    uint32_t divider;
    {
        std::lock_guard<std::mutex> hold(g.clock.lock);
        out = g.clock.output;
        divider = g.clock.divider;
        if (out) out->AddRef();
    }
    if (!out || !divider) {
        if (out) out->Release();
        return 0;
    }
    const int64_t t0 = now_ns();
    bool ok = true;
    for (uint32_t i = 0; i < divider && ok; i++) ok = SUCCEEDED(out->WaitForVBlank());
    out->Release();
    const int64_t elapsed = now_ns() - t0;
    static uint32_t off_beat;
    const float hz = g.refresh.load();
    const int64_t expected = hz > 0.f ? (int64_t)(1e9 * divider / hz) : k_frame_ns;
    if (ok && std::llabs(elapsed - expected) > expected / 4) {
        if (++off_beat >= 8) {
            off_beat = 0;
            g.display_changed = true;
            g.queue_cv.notify_one();
            return 0;
        }
    } else {
        off_beat = 0;
    }
    return ok && elapsed > k_frame_ns / 2;
}

void clock_bind_output()
{
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    const HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    std::lock_guard<std::mutex> hold(g.clock.lock);
    if (monitor == g.clock.monitor && g.clock.output) return;
    if (g.clock.output) g.clock.output->Release();
    g.clock.output = nullptr;
    g.clock.monitor = monitor;
    if (!g.clock.factory && FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&g.clock.factory)))) return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !g.clock.output && g.clock.factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* out = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(out->GetDesc(&desc)) && desc.Monitor == monitor) {
                g.clock.output = out;
                break;
            }
            out->Release();
        }
        adapter->Release();
    }
}
#endif

uint32_t refresh_divider()
{
    const float hz = g.refresh.load();
    if (hz < 50.f) return 0;
    const uint32_t k = (uint32_t)std::lround(hz / 60.f);
    if (!k) return 0;
    const float per = hz / (float)k;
    return per > 59.0f && per < 60.6f ? k : 0;
}

void set_pacing(pacing p)
{
    if (g.mode.exchange(p) == p) return;
#ifdef _WIN32
    if (p == pacing::locked) {
        clock_bind_output();
        {
            std::lock_guard<std::mutex> hold(g.clock.lock);
            g.clock.divider = refresh_divider();
        }
        rsx_core_set_vblank_clock(display_vblank, nullptr);
    } else {
        rsx_core_set_vblank_clock(nullptr, nullptr);
    }
#endif
    g.resized = true;
    std::fprintf(stderr, "[vk] pacing: %s (%.2f Hz display)\n", pacing_name(p), g.refresh.load());
}

pacing fallback_pacing()
{
#ifdef _WIN32
    if (refresh_divider()) return pacing::locked;
#endif
    return pacing::free;
}

#ifdef _WIN32
void measure_display(uint32_t generation)
{
    const uint64_t start = g.presented.load();
    const int64_t give_up = now_ns() + 3000000000ll;
    while (g.presented.load() < start + 30u && now_ns() < give_up) {
        if (generation != g.generation.load()) return;
        sleep_until(now_ns() + 50000000ll);
    }
    if (generation != g.generation.load()) return;
    clock_bind_output();
    IDXGIOutput* out;
    {
        std::lock_guard<std::mutex> hold(g.clock.lock);
        out = g.clock.output;
        if (out) out->AddRef();
    }
    pacing result = fallback_pacing();
    if (out) {
        int64_t intervals[48];
        uint32_t n = 0;
        int64_t last = 0;
        for (uint32_t i = 0; i < 49 && SUCCEEDED(out->WaitForVBlank()); i++) {
            const int64_t t = now_ns();
            if (last) intervals[n++] = t - last;
            last = t;
        }
        out->Release();
        if (n >= 24) {
            std::sort(intervals, intervals + n);
            const int64_t median = intervals[n / 2];
            if (g.refresh.load() > 75.f && std::llabs(median - k_frame_ns) < 2500000) result = pacing::vrr;
        }
    }
    if (generation == g.generation.load()) {
        set_pacing(result);
        g.queue_cv.notify_one();
    }
}
#endif

void restart_measurement()
{
    const uint32_t generation = ++g.generation;
#ifdef _WIN32
    if (g.mode == pacing::measuring) set_pacing(fallback_pacing());
    std::thread(measure_display, generation).detach();
#else
    (void)generation;
    set_pacing(pacing::free);
#endif
}

void read_refresh()
{
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g.window));
    g.refresh = mode ? mode->refresh_rate : 0.f;
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

VkPresentModeKHR pick_mode()
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.d.gpu, g.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> list(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.d.gpu, g.surface, &n, list.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(list.begin(), list.end(), m) != list.end(); };
    if (g.mode == pacing::vrr || g.mode == pacing::measuring) return VK_PRESENT_MODE_FIFO_KHR;
    if (g.d.caps.fifo_latest_ready && has(VK_PRESENT_MODE_FIFO_LATEST_READY_KHR)) return VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
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
    if (g.sc.handle) vkDestroySwapchainKHR(g.d.dev, g.sc.handle, nullptr);
    g.sc.handle = VK_NULL_HANDLE;
    VkSwapchainKHR made;
    VK_TRY(vkCreateSwapchainKHR(g.d.dev, &ci, nullptr, &made));
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
    const char* base = SDL_GetBasePath();
    const std::string cache = std::string(base ? base : "") + "vk_pipelines.bin";
    const char* forced = std::getenv("PS3_RESOLUTION_SCALE");
    const uint32_t requested = forced ? (uint32_t)std::strtoul(forced, nullptr, 10) : 0u;
    g.r.resolution_scale = requested ? std::clamp(requested, 1u, 8u) : std::max(1u, (g.pixel_h.load() + 719u) / 720u);
    std::fprintf(stderr, "[vk] resolution scale %ux (%ux%u for a 1280x720 frame)\n", g.r.resolution_scale, 1280u * g.r.resolution_scale, 720u * g.r.resolution_scale);
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

void record_present(VkCommandBuffer cmd, const slot& s, VkImage target)
{
    constexpr VkPipelineStageFlags2 xfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
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

    const VkExtent2D out = g.sc.extent;
    const float scale = std::min((float)out.width / (float)s.width, (float)out.height / (float)s.height);
    const int32_t dw = std::max(1, (int32_t)std::lround((float)s.width * scale));
    const int32_t dh = std::max(1, (int32_t)std::lround((float)s.height * scale));
    const int32_t x0 = ((int32_t)out.width - dw) / 2, y0 = ((int32_t)out.height - dh) / 2;
    if (dw != (int32_t)out.width || dh != (int32_t)out.height) {
        const VkClearColorValue black{};
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)s.width, (int32_t)s.height, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[0] = { x0, y0, 0 };
    region.dstOffsets[1] = { x0 + dw, y0 + dh, 1 };
    vkCmdBlitImage(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &di);
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

void present_slot(uint32_t i)
{
    slot& s = g.slots[i];
    if (!g.sc.extent.width || !g.sc.extent.height) {
        s.released = 0;
        return;
    }
    present_frame& f = g.frames[g.frame_index];
    timeline_wait(g.present_timeline, f.done);
    uint32_t index = 0;
    const VkResult a = vkAcquireNextImageKHR(g.d.dev, g.sc.handle, 100000000ull, f.acquired, VK_NULL_HANDLE, &index);
    if (a == VK_TIMEOUT || a == VK_NOT_READY) {
        s.released = 0;
        return;
    }
    if (a != VK_SUCCESS && a != VK_SUBOPTIMAL_KHR) {
        g.resized = true;
        s.released = 0;
        return;
    }
    if (a == VK_SUBOPTIMAL_KHR) g.resized = true;
    vkResetCommandPool(g.d.dev, f.pool, 0);
    record_present(f.cmd, s, g.sc.images[index]);

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
                std::fprintf(stderr, "[vk] present interval mean %.3f ms, sd %.3f, min %.3f, max %.3f (%s)\n", mean, std::sqrt(std::max(var, 0.0)),
                             lo / 1e6, hi / 1e6, pacing_name(g.mode));
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
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.resized = true;
    else if (r != VK_SUCCESS) std::fprintf(stderr, "[vk] present failed: %d\n", (int)r);

    static bool first = true;
    if (first) {
        first = false;
        mark("first present");
    }
}

void present_main()
{
    g.resized = false;
    if (!swapchain_build()) std::fprintf(stderr, "[vk] swapchain creation failed\n");
    mark("swapchain");
    uint64_t seen = 0;
    bool rebuilt = false;
    for (;;) {
        uint64_t tick;
        {
            std::unique_lock<std::mutex> hold(g.queue_lock);
            g.queue_cv.wait(hold, [&] { return g.tick != seen || g.resized.load() || g.display_changed.load(); });
            tick = g.tick;
        }
        if (g.display_changed.exchange(false)) {
            read_refresh();
            restart_measurement();
        }
        if (g.resized.exchange(false)) {
            rebuild_swapchain();
            rebuilt = true;
        }
        if (tick == seen) continue;
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
        present_slot((uint32_t)pick);
    }
}

void vk_vblank(void*, u64 count)
{
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
bool slot_reserve(slot& s, uint32_t w, uint32_t h)
{
    if (s.image && s.width == w && s.height == h) return true;
    timeline_wait(g.present_timeline, g.presented.load());
    if (s.image) vmaDestroyImage(g.d.vma, s.image, s.mem);
    s.image = VK_NULL_HANDLE;
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = { w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_TRY(vmaCreateImage(g.d.vma, &ii, &ai, &s.image, &s.mem, nullptr));
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
    if (!vk_instance_create(g.d, ext, 2)) return false;
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
    restart_measurement();
    return true;
}

void window_main()
{
    int ok = -1;
    since_start();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        mark("sdl video");
        const char* windowed = std::getenv("PS3_WINDOWED");
        const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                      ((windowed && *windowed && *windowed != '0') ? 0 : SDL_WINDOW_FULLSCREEN);
        g.window = SDL_CreateWindow(g.title.c_str(), 1280, 720, flags);
        if (g.window) {
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
    while (SDL_WaitEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            std::fflush(nullptr);
            std::_Exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            g.pixel_w = (uint32_t)e.window.data1;
            g.pixel_h = (uint32_t)e.window.data2;
            g.resized = true;
            g.queue_cv.notify_one();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            g.display_changed = true;
            g.queue_cv.notify_one();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                SDL_SetWindowFullscreen(g.window, !(SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN));
            else if (e.key.key == SDLK_F12)
                g.r.trace_request.store(true, std::memory_order_relaxed);
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

std::atomic<int64_t> g_render_ns{ 0 };

struct render_timer {
    const int64_t start = now_ns();
    ~render_timer() { g_render_ns.fetch_add(now_ns() - start, std::memory_order_relaxed); }
};

void vk_methods(void*, u32 reg, u32 inc, const u32* be, u32 count)
{
    render_timer timer;
    g.r.methods(reg, inc, be, count);
}

void vk_flush(void*)
{
    render_timer timer;
    g.r.flush();
}

void vk_reset(void*)
{
    g.r.flip_reset();
}

int vk_report(void*, u32 type, u32 ea)
{
    return g.r.report(type, ea) ? 1 : 0;
}

void vk_read_back(void*, u32 ea, u32 size)
{
    g.r.read_back(ea, size);
}

void vk_wrote(void*, u32 ea, u32 size)
{
    g.r.wrote(ea, size);
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

void vk_present(void*, u32, const rsx_display_buffer* db)
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
    if (!ea) {
        g.r.flush();
        return;
    }

    uint32_t i = 0;
    {
        std::lock_guard<std::mutex> hold(g.queue_lock);
        while ((int)i == g.shown || (int)i == g.latest || (int)i == g.queued) i++;
    }
    slot& s = g.slots[i];
    timeline_wait(g.present_timeline, s.released);
    vk::image* src = nullptr;
    u32 scale = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    g.r.present_source(ea, w, h, pitch, src, scale, cmd);
    const uint32_t sw = std::min(w * scale, src ? src->width : 0u), sh = std::min(h * scale, src ? src->height : 0u);
    if (!src || !slot_reserve(s, sw, sh)) {
        g.r.flush();
        return;
    }
    vk::renderer::barrier(cmd, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    VkImageBlit region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
    vkCmdBlitImage(cmd, src->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_NEAREST);
    b.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &di);
    static const char* capture = std::getenv("PS3_VK_CAPTURE");
    static uint64_t flips;
    const char* capture_path = nullptr;
    if (capture && ++flips == std::strtoull(capture, const_cast<char**>(&capture_path), 10) && *capture_path == ':') capture_path++;
    else capture_path = nullptr;
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
        g.queued = (int)i;
    }
    g.queue_cv.notify_one();

    static uint64_t presents;
    if (stats_on() && ++presents % 600u == 0) {
        const auto& st = g.r.stats;
        std::fprintf(stderr, "[vk] frames %llu draws %llu clears %llu uploads %llu, renderer cpu %.3f ms/frame, %s\n", (unsigned long long)presents,
                     (unsigned long long)st.draws, (unsigned long long)st.clears, (unsigned long long)st.uploads,
                     (double)g_render_ns.exchange(0) / 600.0 / 1e6, pacing_name(g.mode));
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
        nullptr, vk_open, vk_methods, vk_flush, nullptr, vk_reset, vk_report, vk_present, vk_read_back, vk_wrote, vk_vblank, vk_close,
    };
    static std::once_flag started;
    std::call_once(started, [title] {
        g.title = title && *title ? title : "ps3recomp";
        std::thread(window_main).detach();
    });
    return &renderer;
}
