#pragma once

#include <volk.h>

#include <cstdint>
#include <vector>

namespace rsx {

struct texture_regs {
    uint32_t offset;
    uint32_t format;
    uint32_t address;
    uint32_t control0;
    uint32_t control1;
    uint32_t filter;
    uint32_t image_rect;
    uint32_t border;
    uint32_t control2;
    uint32_t control3;
};

struct texture_shader_ops {
    uint8_t sign_mask;
    uint8_t gamma_mask;
    uint8_t bx2_mask;
    uint8_t depth_compare;
    uint8_t unnormalized;
};

struct texture_desc {
    uint32_t location;
    uint32_t offset;
    uint32_t gcm_format;
    bool linear;
    bool cube;
    bool compressed;
    bool depth;
    bool border;
    uint32_t dimension;
    uint32_t width;
    uint32_t height;
    uint32_t depth_slices;
    uint32_t levels;
    uint32_t layers;
    uint32_t pitch;
    uint32_t guest_size;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkComponentMapping swizzle;
    texture_shader_ops ops;
};

struct texture_copy {
    uint32_t level;
    uint32_t layer;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint64_t offset;
    uint32_t row_texels;
    uint32_t rows;
};

bool texture_describe(const texture_regs& regs, bool vertex_unit, texture_desc& out);
uint64_t texture_staging_size(const texture_desc& desc);
void texture_convert(const texture_desc& desc, const uint8_t* guest, uint8_t* staging, std::vector<texture_copy>& copies);
void texture_sampler(const texture_regs& regs, bool vertex_unit, const texture_desc& desc, float max_anisotropy,
                     VkSamplerCreateInfo& info, VkSamplerCustomBorderColorCreateInfoEXT& border);

}
