#pragma once

#include "rsx_frontend.h"
#include "vk_device.h"
#include "vk_ring.h"
#include "vk_pipeline.h"
#include "vk_shaders.h"
#include "vk_state.h"
#include "vk_occlusion.h"
#include "vk_depth_pack.h"
#include "vk_downscale.h"
#include "vk_upscaler.h"
#include "vk_flat_map.h"
#include "vk_scale.h"
#include "rsx_texture.h"
#include "../rsx_core.h"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace vk {

using rsx::u32;
using rsx::u64;

constexpr VkPipelineStageFlags2 k_shader_read_stages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

struct image {
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation mem = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
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
    bool depth_only = false;
    u32 bpp = 0;
    u32 aa_u = 1;
    u32 aa_v = 1;
    scale2d scale;
    surface_state state = SURFACE_UNINITIALIZED;
    bool gpu_dirty = false;
    image img;
    image snapshot;
    u64 snapshot_draw = ~0ull;
    image as_color;
    u64 as_color_version = ~0ull;
    u64 version = 0;
    u64 depth_version = 0;
    image native;
    u64 native_version = ~0ull;
    image alt;
    scale2d alt_scale;
    u64 alt_flip = 0;
    u32 bytes() const { return pitch * height * aa_v; }
};

struct framebuffer {
    u32 width = 0;
    u32 height = 0;
    u32 aa_u = 1;
    u32 aa_v = 1;
    scale2d scale;
    u32 count = 0;
    surface* color[4] = {};
    surface* depth = nullptr;
    bool depth_write = false;
    bool z_write = false;
    bool color_write[4] = {};
    VkFormat color_format[4] = {};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool same_pass(const framebuffer& o) const;
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
    void save_caches() { m_pipelines.save_cache(); }
    VkPipelineCache pipeline_cache() const { return m_pipelines.cache(); }
    void present_source(u32 ea, u32 width, u32 height, u32 pitch, image*& out, scale2d& scale, VkCommandBuffer& cmd);
    u64 content(u32 ea, u32 pitch) const;
    void submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal);
    void read_back(u32 ea, u32 size);
    void settle(u32 ea, u32 size);
    void drain();
    void wrote(u32 ea, u32 size);
    bool blit(const rsx_blit& b);
    void flip_reset();
    void rescale(scale2d scale);
    void invalidate_graphics_state();
    bool report(u32 type, u32 ea);
    bool defer(const rsx_write& w);
    std::atomic<u32> trace_frames{ 0 };
    scale2d resolution_scale;
    scale2d display_scale;
    int upscale_mode = RSX_VK_UPSCALE_OFF;

    int upscaler_caps() const { return m_upscaler.caps(); }
    const char* upscaler_fsr_version() const { return m_upscaler.fsr_version(); }
    void upscale_render_size(u32 dw, u32 dh, int mode, u32& rw, u32& rh) const { m_upscaler.render_size(dw, dh, mode, rw, rh); }
    void upscale_jitter(int mode, u32 rw, u32 dw, u32 frame_index, float& jx, float& jy) const { m_upscaler.jitter(mode, rw, dw, frame_index, jx, jy); }
    void capture_velocity() { if (!m_upscaled_this_frame && m_fb.color[0]) m_scene_velocity = m_fb.color[0]; }
    bool run_upscale(const rsx_vk_upscale_frame& frame);
    bool last_frame_upscaled() const { return m_last_frame_upscaled; }

    bool tracing() const { return m_trace != nullptr; }
    void trace_begin(VkCommandBuffer c);
    void trace_end(VkCommandBuffer c, const char* fmt, ...);

    static void barrier(VkCommandBuffer cmd, image& img, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    static bool readable(const image& img, VkPipelineStageFlags2 stage = k_shader_read_stages);
    static void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                               VkAccessFlags2 dst_access);

protected:
    void on_draw(const rsx::draw_clause& clause) override;
    void on_clear(u32 mask) override;
    void on_report(u32 method, u32 value) override;

private:
    VkCommandBuffer cmd();
    void end_rendering();
    bool bind_framebuffer(u32 context);
    void begin_rendering(u32 clear_color_mask = 0, const VkClearValue* clear_color = nullptr,
                         VkImageAspectFlags clear_ds_mask = 0, const VkClearValue* clear_ds = nullptr);
    surface* surface_at(u32 ea, u32 pitch, u32 bpp, u32& x, u32& y);
    surface* scanout_surface(u32 ea, u32 pitch) const;
    scale2d scale_for(u32 width, u32 height, u32 aa_u, u32 aa_v, bool depth, bool depth_only = false) const;
    void rescale_surface(VkCommandBuffer c, surface& s, scale2d to, bool keep = true);
    surface* surface_for(u32 ea, u32 pitch, u32 width, u32 height, u32 format, bool depth, u32 aa_u, u32 aa_v,
                         const surface* const* keep = nullptr, u32 keep_count = 0, bool depth_only = false);
    bool create_image(image& img, VkFormat format, u32 width, u32 height, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroy_image(image& img);
    void discard(image& img);
    void retire(surface* s);
    void load_surface(surface& s);
    void scale_blit(VkCommandBuffer c, image& src, image& dst, VkFilter filter);
    bool scale_depth(VkCommandBuffer c, image& src, image& dst);
    void download_surface(surface& s);
    void download(u32 ea, u32 size);
    void await_writes(u32 ea, u32 size);
    void queued();
    void stall(std::chrono::steady_clock::time_point start, u32 ea, u32 size);
    image* native_image(surface& s, image& source);

    vk_device* m_d = nullptr;
    vk_ring m_ring;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    uint64_t* m_submitted = nullptr;
    std::array<submission, 64> m_submissions;
    u32 m_submission = 0;
    bool m_recording = false;
    bool m_rendering = false;
    u64 m_draw_counter = 0;
    framebuffer m_fb;
    bool m_fb_valid = false;
    bool m_fb_dirty = true;
    u32 m_fb_context = ~0u;
    u32 m_clear_discard = 0;
    u32 m_dyn_dirty = ~0u;
    u32 m_last_dyn_prim = ~0u;
    bool m_last_dyn_restart = false;
    pipeline_key m_last_pipe_key{};
    std::vector<std::unique_ptr<surface>> m_surfaces;
    std::vector<std::pair<uint64_t, image>> m_garbage;
    struct scratch {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation mem = nullptr;
        VkDeviceSize size = 0;
    };
    bool create_scratch(scratch& s, VkDeviceSize size, VkBufferUsageFlags usage);
    std::vector<std::pair<uint64_t, scratch>> m_scratch_garbage;
    image m_display;
    occlusion m_occlusion;
    depth_pack m_depth_pack;
    downscale m_downscale;
    upscaler m_upscaler;
    bool m_upscaled_this_frame = false;
    bool m_last_frame_upscaled = false;
    bool m_upscale_resync = false;
    enum upscale_result { UP_OK, UP_SKIPPED, UP_BACKOFF, UP_NO_SCENE, UP_VELOCITY, UP_INVALID, UP_DEPTH_SIZE, UP_DISPATCH, UP_COUNT };
    u32 m_up_count[UP_COUNT]{};
    u32 m_up_frames = 0;
    u32 m_up_backoff = 0;
    u32 m_up_in[2]{};
    u32 m_up_out[2]{};
    bool upscale_failed(upscale_result r);
    bool rendering_reduced() const { return m_last_frame_upscaled && !m_upscaled_this_frame; }
    surface* m_scene_color = nullptr;
    surface* m_scene_depth = nullptr;
    surface* m_scene_velocity = nullptr;
    image m_upscale_src;
    image m_upscale_out;
    image m_box;
    image* depth_as_color(surface& s);

    bool draw_setup();
    void draw_execute(const rsx::draw_clause& clause, u32 id);
    void trace_draw(u32 id, u32 prim, u32 count, bool indexed) const;
    std::FILE* m_trace = nullptr;
    bool m_trace_timed = false;
    u32 m_trace_draws = 0;
    void trace_report();
    void trace_mark(VkCommandBuffer c, u32 id);
    struct trace_point {
        u32 id;
        u64 vp, fp;
        u32 width, height;
    };
    VkQueryPool m_trace_pool = VK_NULL_HANDLE;
    std::vector<std::string> m_trace_spans;
    std::vector<trace_point> m_trace_points;
    bool m_trace_open = false;
    u32 m_trace_pass_first = 0;
    bool upload_vertices(const rsx::draw_clause& clause, u32 first, u32 count, u32 vertex_base, VkDescriptorBufferInfo& layout);
    bool create_dummies();
    bool create_buffer_set();

    pipelines m_pipelines;
    shader_cache m_shaders;
    image m_dummy[3];
    VkSampler m_dummy_sampler = VK_NULL_HANDLE;
    u32 m_vertex_dims = 0;
    u32 m_fp_tex_dims = 0;
    u32 m_fp_tex_shadow = 0;
    u32 m_fp_tex_alphakill = 0;
    u32 m_fp_tex_alphakill_mask = 0;
    bool m_fp_tex_valid = false;
    u32 m_tcc = 0;
    bool m_tcc_valid = false;
    uint64_t m_constants_sub = 0;
    u32 m_constants_valid[(rsx::VP_CONSTANTS + 31u) / 32u]{};
    u32 m_constants_offset = 0;
    struct vp_hash_slot {
        u32 start = ~0u;
        uint64_t hash = 0;
    };
    vp_hash_slot m_vp_hashes[64]{};
    struct vp_l1_slot {
        uint64_t program_hash = 0;
        rsx_spv_vp_key key{};
        const vertex_program* vp = nullptr;
    };
    vp_l1_slot m_vp_l1[64]{};
    const vertex_program* m_vp = nullptr;
    const fragment_program* m_fp = nullptr;
    struct fp_const_slot {
        const fragment_program* fp = nullptr;
        u32 ea = 0;
        u64 epoch = 0;
        uint64_t sub = 0;
        VkDescriptorBufferInfo info{};
    };
    fp_const_slot m_fp_const_cache[64]{};
    struct fp_seen_key {
        u32 ea;
        rsx_spv_fp_key key;
        bool operator==(const fp_seen_key& o) const { return !std::memcmp(this, &o, sizeof o); }
    };
    struct fp_template {
        const fragment_program* fp = nullptr;
        u32 count = 0;
        u32 prefix[4]{};
        rsx_spv_fp_key key{};
        std::vector<u32> code;
    };
    struct fp_candidate {
        const fp_template* tmpl = nullptr;
        u64 verified_epoch = 0;
    };
    struct fp_lookup {
        std::array<fp_candidate, 8> slots;
        u32 next = 0;
        u32 last_slot = 0;
    };
    struct fp_l1_entry {
        fp_seen_key key{};
        const fragment_program* fp = nullptr;
        u64 verified_epoch = 0;
        fp_lookup* val = nullptr;
    };
    struct fp_l1_set {
        fp_l1_entry ways[2]{};
        u32 next_way = 0;
    };
    struct fp_sig_slot {
        u32 prefix[4]{};
        rsx_spv_fp_key key{};
        const fp_template* tmpl = nullptr;
    };
    std::deque<fp_template> m_fp_templates;
    flat_map<const fragment_program*, const fp_template*> m_fp_tmpl_by_fp;
    fp_sig_slot m_fp_sig[256]{};
    std::deque<fp_lookup> m_fp_pool;
    flat_map<fp_seen_key, fp_lookup*> m_fp_seen;
    fp_l1_set m_fp_l1[256]{};
    fp_seen_key m_fp_last_key{};
    fp_lookup* m_fp_last = nullptr;
    u64 m_fp_last_epoch = 0;
    u32 m_fp_ea = 0;
    VkDeviceSize m_ubo_align = 256;
    std::vector<uint8_t> m_index_scratch;
    std::vector<uint8_t> m_regbytes;
    struct draw_sub {
        u32 first;
        u32 count;
        int32_t vertex_offset;
        bool indexed;
    };
    std::vector<draw_sub> m_subs;
    struct index_key {
        u32 source, first, count, prim, restart_index, flags;
    };
    struct cached {
        VmaVirtualAllocation alloc = VK_NULL_HANDLE;
        u32 offset = 0;
        u32 stamp = 0;
        u32 checked_stamp = 0;
        u32 count = 0;
        u32 min = 0;
        u32 max = 0;
        uint64_t submission = 0;
        u64 used = 0;
        bool seen = false;
        bool watched = false;
    };
    bool cache_place(cached& e, u32 ea, u32 size, u32 bytes, u32& offset, uint8_t*& out);
    void evict(u64 age);
    void release_held();
    flat_map<uint64_t, cached> m_vertex_cache;
    flat_map<index_key, cached> m_index_cache;
    struct vcache_l1 { uint64_t key = 0; cached* val = nullptr; };
    struct icache_l1 { index_key key{}; cached* val = nullptr; };
    vcache_l1 m_vcache_l1[64]{};
    icache_l1 m_icache_l1[32]{};
    size_t m_vcache_cap = 0;
    size_t m_icache_cap = 0;
    std::vector<std::pair<uint64_t, VmaVirtualAllocation>> m_held_garbage;
    uint64_t m_reached = 0;
    u64 m_progress = 0;
    dynamic_cache m_dynamic;
    VkDescriptorPool m_buffer_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_buffer_set = VK_NULL_HANDLE;
    struct bound_descriptors {
        u32 offsets[4];
        VkDescriptorImageInfo images[rsx::TEXTURES + rsx::VERTEX_TEXTURES];
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkIndexType index_type = VK_INDEX_TYPE_MAX_ENUM;
        bool valid = false;
    };
    bound_descriptors m_bound;
    struct reused_upload {
        std::vector<uint8_t> bytes;
        uint64_t submission = 0;
        VkDescriptorBufferInfo info{};
    };
    bool upload(reused_upload& u, const void* data, u32 size, VkDescriptorBufferInfo& out);
    reused_upload m_layout_upload;
    reused_upload m_fp_constant_upload;
    reused_upload m_fp_state_upload;
    std::vector<u32> m_fp_constants;

    struct texture_entry {
        image img;
        u32 stamp = 0;
        u32 checked_stamp = 0;
        u64 checked = 0;
        u64 used = 0;
        u8 patch_tag = 0;
        u32 variant = 0;
    };
    struct unit_resolved {
        bool valid = false;
        u8 patch_tag = 0;
        u32 variant = 0;
        rsx::texture_regs regs{};
        u32 ea = 0;
        u64 epoch = 0;
        u64 generation = 0;
        texture_entry* entry = nullptr;
        surface* surf = nullptr;
        image* surf_img = nullptr;
        u64 surf_version = 0;
        scale2d fb_scale{};
        VkDescriptorImageInfo info{};
        rsx_spv_texture_state state{};
    };
    struct qte_anchor {
        float cx, cy;
    };
    struct unit_binding {
        bool described = false;
        bool valid = false;
        rsx::texture_regs regs{};
        rsx::texture_desc desc{};
        VkSampler sampler = VK_NULL_HANDLE;
    };

    void describe_units(u32 first, u32 last);
    rsx::texture_regs unit_regs(u32 u) const;
    bool resolve_texture(u32 unit, bool vertex, VkDescriptorImageInfo& out, rsx_spv_texture_state& state, u8* patch_tag = nullptr);
    VkImageView view_for(image& img, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, const VkComponentMapping& swizzle);
    VkSampler sampler_for(const VkSamplerCreateInfo& info, const VkSamplerCustomBorderColorCreateInfoEXT* border, bool allow_lod_bias = true);
    bool memory_changed(u32 ea, u32 size, u32 stamp);
    bool memory_changed_watched(u32 ea, u32 size, u32 stamp, u32& checked_stamp);
    void watch(u32 ea, u32 size);
    void prescan(u64 batch);
    void mark_written(u32 ea, u32 size);
    bool supports_linear_blit(VkFormat format);
    flat_map<VkFormat, bool> m_linear_blit_ok;

    unit_binding m_units[rsx::TEXTURES + rsx::VERTEX_TEXTURES];
    unit_resolved m_resolved[rsx::TEXTURES + rsx::VERTEX_TEXTURES];
    std::vector<qte_anchor> m_qte_rings_cur;
    std::vector<qte_anchor> m_qte_rings_prev;
    u64 m_qte_ring_epoch = 0;
    u64 m_binding_gen = 1;
    const char* m_texture_path[rsx::TEXTURES + rsx::VERTEX_TEXTURES]{};
    flat_map<uint64_t, std::unique_ptr<texture_entry>> m_textures;
    flat_map<uint64_t, VkSampler> m_samplers;
    u32 m_sampler_count = 0;
    u32 m_custom_border_count = 0;
    std::vector<u32> m_page_stamp;
    std::vector<u32> m_chunk_stamp;
    VkBuffer m_readback = VK_NULL_HANDLE;
    VmaAllocation m_readback_alloc = nullptr;
    uint8_t* m_readback_map = nullptr;
    VkDeviceSize m_readback_size = 0;
    u64 m_scan_batch = ~0ull;
    u32 m_stamp = 1;
    u64 m_epoch = 1;
    u64 m_flips = 0;
    u64 m_content_clock = 0;
    std::vector<rsx::texture_copy> m_copies;
    std::vector<VkBufferImageCopy> m_copy_regions;

public:
    struct counters {
        u64 draws, clears, uploads, blits, downloads, surface_loads;
        u64 epochs, watch_calls, watch_bytes, copy_bytes, reuse_bytes, scans, write_stalls, surface_stalls, stall_ns, stall_bytes;
        u64 deferred_transfers, deferred_labels;
        u64 scan_ns, changed_pages;
        u32 stall_ea, stall_size;
    } stats{};
};

class trace_span {
public:
    trace_span(renderer& r, VkCommandBuffer c, const char* fmt, ...);
    ~trace_span();
    trace_span(const trace_span&) = delete;
    trace_span& operator=(const trace_span&) = delete;

private:
    renderer* m_r = nullptr;
    VkCommandBuffer m_c = VK_NULL_HANDLE;
    char m_label[160];
};

}
