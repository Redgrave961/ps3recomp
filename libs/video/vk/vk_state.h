#pragma once

#include "rsx_frontend.h"
#include "vk_pipeline.h"
#include "vk_scale.h"

namespace vk {

struct draw_target {
    uint32_t width;
    uint32_t height;
    uint32_t aa_u;
    uint32_t aa_v;
    ratio scale;
    bool depth;
    bool stencil;
    bool depth_float;
    VkFormat color_format[4];
};

struct dynamic_state {
    struct face {
        uint32_t write, compare, reference;
        VkStencilOp fail, pass, depth_fail;
        VkCompareOp op;
    };
    VkViewport viewport;
    VkRect2D scissor;
    float line_width;
    uint32_t bias_enable;
    float bias[3];
    float blend_constants[4];
    uint32_t depth_test, depth_write, depth_op, bounds_enable;
    float bounds[2];
    uint32_t stencil_test;
    face faces[2];
    uint32_t cull, front, topology, restart;
    uint32_t polygon_mode, depth_clamp, depth_clip, provoking;
    uint32_t blend_count;
    VkBool32 blend_enable[4];
    VkColorBlendEquationEXT equation[4];
    VkColorComponentFlags write_mask[4];
};

struct dynamic_cache {
    dynamic_state last;
    bool valid = false;
};

void fill_pipeline_state(const rsx::frontend& f, const vk_device& d, const draw_target& t, uint32_t prim, pipeline_key& key);
void set_dynamic_state(VkCommandBuffer cmd, const rsx::frontend& f, const vk_device& d, const pipelines& p, const draw_target& t,
                       uint32_t prim, bool restart, const pipeline_key& key, dynamic_cache& cache);

}
