#pragma once

#include "rsx_frontend.h"
#include "vk_pipeline.h"

namespace vk {

struct draw_target {
    uint32_t width;
    uint32_t height;
    uint32_t aa_u;
    uint32_t aa_v;
    bool depth;
    bool stencil;
    bool depth_float;
    VkFormat color_format[4];
};

void fill_pipeline_state(const rsx::frontend& f, const vk_device& d, const draw_target& t, uint32_t prim, pipeline_key& key);
void set_dynamic_state(VkCommandBuffer cmd, const rsx::frontend& f, const vk_device& d, const pipelines& p, const draw_target& t,
                       uint32_t prim, bool restart, const pipeline_key& key);

}
