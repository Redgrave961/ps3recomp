#pragma once

#include "rsx_frontend.h"
#include "vk_device.h"
#include "vk_ring.h"
#include "vk_pipeline.h"
#include "vk_shaders.h"
#include "vk_occlusion.h"
#include "vk_depth_pack.h"
#include "vk_downscale.h"
#include "rsx_texture.h"
#include "../rsx_core.h"

#include <array>
#include <cstdio>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vk {

using rsx::u32;
using rsx::u64;

struct image {
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation mem = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    u32 width = 0;
    u32 height = 0;
    std::vector<std::pair<uint64_t, VkImageView>> views;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = 0;
};

enum surface_state : uint8_t {
    SURFACE_UNINITIALIZED,
    SURFACE_VALID,
};

struct surface {
    u32 ea = 0;
    u32 pitch = 0;
    u32 width = 0;
    u32 height = 0;
    u32 gcm_format = 0;
    bool depth = false;
    bool depth_float = false;
    u32 bpp = 0;
    u32 aa_u = 1;
    u32 aa_v = 1;
    u32 scale = 1;
    surface_state state = SURFACE_UNINITIALIZED;
    bool gpu_dirty = false;
    u64 last_use = 0;
    image img;
    image snapshot;
    u64 snapshot_draw = ~0ull;
    image as_color;
    u64 as_color_draw = ~0ull;
    u64 version = 0;
    image native;
    u64 native_version = ~0ull;
    u32 bytes() const { return pitch * height * aa_v; }
};

struct framebuffer {
    u32 width = 0;
    u32 height = 0;
    u32 aa_u = 1;
    u32 aa_v = 1;
    u32 scale = 1;
    u32 count = 0;
    surface* color[4] = {};
    surface* depth = nullptr;
    bool depth_write = false;
    bool color_write[4] = {};
    VkFormat color_format[4] = {};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool operator==(const framebuffer& o) const;
};

struct submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t done = 0;
};

class renderer final : public rsx::frontend {
public:
    bool open(vk_device& d, VkSemaphore timeline, uint64_t* submitted, const char* cache_path);
    void close();

    void flush();
    void present_source(u32 ea, u32 width, u32 height, u32 pitch, image*& out, u32& scale, VkCommandBuffer& cmd);
    u64 content(u32 ea, u32 pitch) const;
    void submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal);
    void read_back(u32 ea, u32 size);
    void wrote(u32 ea, u32 size);
    bool blit(const rsx_blit& b);
    void flip_reset();
    bool report(u32 type, u32 ea);
    std::atomic<bool> trace_request{ false };
    u32 resolution_scale = 1;

    static void barrier(VkCommandBuffer cmd, image& img, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);

protected:
    void on_draw(const rsx::draw_clause& clause) override;
    void on_clear(u32 mask) override;
    void on_report(u32 method, u32 value) override;

private:
    VkCommandBuffer cmd();
    void end_rendering();
    bool bind_framebuffer(u32 context);
    void begin_rendering();
    surface* surface_at(u32 ea, u32 pitch, u32 bpp, u32& x, u32& y);
    surface* scanout_surface(u32 ea, u32 pitch) const;
    surface* surface_for(u32 ea, u32 pitch, u32 width, u32 height, u32 format, bool depth, u32 aa_u, u32 aa_v);
    bool create_image(image& img, VkFormat format, u32 width, u32 height, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroy_image(image& img);
    void retire(surface* s);
    void load_surface(surface& s);
    void scale_blit(VkCommandBuffer c, image& src, image& dst, VkFilter filter);
    void download_surface(surface& s);

    vk_device* m_d = nullptr;
    vk_ring m_ring;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    uint64_t* m_submitted = nullptr;
    std::array<submission, 16> m_submissions;
    u32 m_submission = 0;
    bool m_recording = false;
    bool m_rendering = false;
    u64 m_draw_counter = 0;
    framebuffer m_fb;
    bool m_fb_valid = false;
    std::vector<std::unique_ptr<surface>> m_surfaces;
    std::vector<std::pair<uint64_t, image>> m_garbage;
    image m_display;
    occlusion m_occlusion;
    depth_pack m_depth_pack;
    downscale m_downscale;
    image m_box;
    image* depth_as_color(surface& s, VkFormat format);

    bool draw_setup();
    void draw_execute(const rsx::draw_clause& clause, u32 id);
    void trace_draw(u32 id, u32 prim, u32 count, bool indexed) const;
    std::FILE* m_trace = nullptr;
    u32 m_trace_draws = 0;
    bool upload_vertices(const rsx::draw_clause& clause, u32 first, u32 count, u32 vertex_base, VkDescriptorBufferInfo& data,
                         VkDescriptorBufferInfo& layout);
    bool create_dummies();

    pipelines m_pipelines;
    shader_cache m_shaders;
    image m_dummy[3];
    VkSampler m_dummy_sampler = VK_NULL_HANDLE;
    uint64_t m_program_hash = 0;
    u32 m_vertex_dims = 0;
    const vertex_program* m_constants_vp = nullptr;
    VkDescriptorBufferInfo m_constants{};
    const vertex_program* m_vp = nullptr;
    const fragment_program* m_fp = nullptr;
    struct fp_lookup {
        const fragment_program* fp = nullptr;
        rsx_spv_fp_key key{};
        std::vector<u32> words;
        std::vector<uint8_t> constant;
    };
    std::unordered_map<u32, fp_lookup> m_fp_seen;    u32 m_fp_ea = 0;
    VkDeviceSize m_ubo_align = 256;
    VkDeviceSize m_ssbo_align = 256;
    std::vector<uint8_t> m_index_scratch;
    std::vector<uint8_t> m_regbytes;
    struct draw_sub {
        u32 first;
        u32 count;
        int32_t vertex_offset;
        bool indexed;
    };
    std::vector<draw_sub> m_subs;

    struct texture_entry {
        u32 ea = 0;
        u32 size = 0;
        uint64_t key = 0;
        image img;
        u32 stamp = 0;
        u64 checked = 0;
        u64 used = 0;
    };
    struct unit_binding {
        bool valid = false;
        rsx::texture_regs regs{};
        rsx::texture_desc desc{};
    };

    void describe_units();
    bool resolve_texture(u32 unit, bool vertex, VkDescriptorImageInfo& out, rsx_spv_texture_state& state);
    VkImageView view_for(image& img, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, const VkComponentMapping& swizzle);
    VkSampler sampler_for(const VkSamplerCreateInfo& info, const VkSamplerCustomBorderColorCreateInfoEXT* border);
    bool memory_changed(u32 ea, u32 size, u32 stamp);
    bool watch(u32 ea, u32 size);
    void mark_written(u32 ea, u32 size);

    unit_binding m_units[rsx::TEXTURES + rsx::VERTEX_TEXTURES];
    const char* m_texture_path[rsx::TEXTURES + rsx::VERTEX_TEXTURES]{};
    std::unordered_map<uint64_t, std::unique_ptr<texture_entry>> m_textures;
    std::unordered_map<uint64_t, VkSampler> m_samplers;
    std::vector<u32> m_page_stamp;
    struct watch_range {
        u32 start;
        u32 end;
        u64 scanned;
    };
    std::vector<watch_range> m_watch;
    bool m_watch_dirty = true;
    u32 m_stamp = 1;
    u64 m_epoch = 1;
    u64 m_flips = 0;
    u64 m_content_clock = 0;
    std::vector<rsx::texture_copy> m_copies;

public:
    struct counters {
        u64 draws, clears, skipped_fb, skipped_shader, skipped_pipeline, textures, uploads, present_surface, present_memory, blits, downloads, surface_loads;
    } stats{};
};

}
