#include "vk_state.h"
#include "vk_geometry.h"

#include <algorithm>
#include <cstring>

namespace vk {

namespace {

using rsx::frontend;

VkCompareOp compare_op(uint32_t v)
{
    return (v >= 0x200u && v <= 0x207u) ? (VkCompareOp)(v & 7u) : VK_COMPARE_OP_ALWAYS;
}

VkStencilOp stencil_op(uint32_t v)
{
    switch (v) {
    case 0x0000u: return VK_STENCIL_OP_ZERO;
    case 0x1E01u: return VK_STENCIL_OP_REPLACE;
    case 0x1E02u: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03u: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150Au: return VK_STENCIL_OP_INVERT;
    case 0x8507u: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508u: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkBlendFactor blend_factor(uint32_t v)
{
    switch (v) {
    case 0x0000u: return VK_BLEND_FACTOR_ZERO;
    case 0x0001u: return VK_BLEND_FACTOR_ONE;
    case 0x0300u: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301u: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302u: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303u: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304u: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305u: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306u: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307u: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308u: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001u: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002u: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003u: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004u: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 0x8007u: return VK_BLEND_OP_MIN;
    case 0x8008u: return VK_BLEND_OP_MAX;
    case 0x800Au: return VK_BLEND_OP_SUBTRACT;
    case 0x800Bu: case 0xF005u: case 0xF007u: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

bool blendable(VkFormat f)
{
    return f != VK_FORMAT_R32_SFLOAT && f != VK_FORMAT_R32G32B32A32_SFLOAT;
}

uint32_t write_mask(const frontend& f, uint32_t k)
{
    uint32_t m = 0;
    if (k == 0) {
        const uint32_t v = f.r(rsx::M_COLOR_MASK);
        if (v & 0x00FF0000u) m |= VK_COLOR_COMPONENT_R_BIT;
        if (v & 0x0000FF00u) m |= VK_COLOR_COMPONENT_G_BIT;
        if (v & 0x000000FFu) m |= VK_COLOR_COMPONENT_B_BIT;
        if (v & 0xFF000000u) m |= VK_COLOR_COMPONENT_A_BIT;
    } else {
        const uint32_t v = f.r(rsx::M_COLOR_MASK_MRT) >> (k * 4u);
        if (v & 2u) m |= VK_COLOR_COMPONENT_R_BIT;
        if (v & 4u) m |= VK_COLOR_COMPONENT_G_BIT;
        if (v & 8u) m |= VK_COLOR_COMPONENT_B_BIT;
        if (v & 1u) m |= VK_COLOR_COMPONENT_A_BIT;
    }
    return m;
}

bool blend_enabled(const frontend& f, uint32_t k)
{
    return k == 0 ? f.r(rsx::M_BLEND_ENABLE) != 0 : ((f.r(rsx::M_BLEND_ENABLE_MRT) >> k) & 1u) != 0;
}

VkPolygonMode polygon_mode(uint32_t v)
{
    switch (v) {
    case 0x1B00u: return VK_POLYGON_MODE_POINT;
    case 0x1B01u: return VK_POLYGON_MODE_LINE;
    default: return VK_POLYGON_MODE_FILL;
    }
}

float half_to_float(uint16_t h)
{
    const uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    uint32_t bits;
    if (e == 0) {
        if (!m) bits = s;
        else {
            uint32_t mm = m, ee = 113;
            while (!(mm & 0x400u)) { mm <<= 1; ee--; }
            bits = s | (ee << 23) | ((mm & 0x3FFu) << 13);
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

bool restart_capable(uint32_t prim)
{
    return prim == PRIM_TRIANGLE_STRIP || prim == PRIM_QUAD_STRIP || prim == PRIM_LINE_STRIP || prim == PRIM_TRIANGLE_FAN;
}

}

void fill_pipeline_state(const frontend& f, const vk_device& d, const draw_target& t, uint32_t prim, pipeline_key& key)
{
    for (uint32_t k = 0; k < 4; k++) key.color_format[k] = t.color_format[k];
    key.topology_class = topology_class(prim);
    key.polygon_mode = d.features.features.fillModeNonSolid ? polygon_mode(f.r(rsx::M_FRONT_POLYGON_MODE)) : VK_POLYGON_MODE_FILL;
    key.logic_enable = d.features.features.logicOp && f.r(rsx::M_LOGIC_OP_ENABLE) != 0;
    key.logic_op = key.logic_enable ? (f.r(rsx::M_LOGIC_OP) - 0x1500u) & 15u : 0u;
    const uint32_t zmm = f.r(rsx::M_ZMIN_MAX_CONTROL);
    const bool clip = (zmm & 0xFu) != 0, clamp = ((zmm >> 4) & 0xFu) != 0;
    key.depth_clamp = d.features.features.depthClamp && (clamp || !clip);
    key.depth_clip = clip;
    key.provoking_last = d.caps.provoking_vertex ? 1u : 0u;
    const uint32_t sf = f.r(rsx::M_BLEND_SFACTOR), df = f.r(rsx::M_BLEND_DFACTOR), eq = f.r(rsx::M_BLEND_EQUATION);
    for (uint32_t k = 0; k < 4; k++) {
        blend_attachment& b = key.blend[k];
        if (t.color_format[k] == VK_FORMAT_UNDEFINED) continue;
        b.enable = blend_enabled(f, k) && blendable(t.color_format[k]);
        b.src_color = blend_factor(sf & 0xFFFFu);
        b.src_alpha = blend_factor(sf >> 16);
        b.dst_color = blend_factor(df & 0xFFFFu);
        b.dst_alpha = blend_factor(df >> 16);
        b.color_op = blend_op(eq & 0xFFFFu);
        b.alpha_op = blend_op(eq >> 16);
        b.write_mask = write_mask(f, k);
    }
}

void set_dynamic_state(VkCommandBuffer cmd, const frontend& f, const vk_device& d, const pipelines& p, const draw_target& t,
                       uint32_t prim, bool restart, const pipeline_key& key)
{
    const float sx = (float)t.aa_u, sy = (float)t.aa_v;
    VkViewport vp{ 0.f, 0.f, (float)t.width * sx, (float)t.height * sy, 0.f, 1.f };
    vkCmdSetViewportWithCount(cmd, 1, &vp);

    const uint32_t sh = f.r(rsx::M_SCISSOR_H), sv = f.r(rsx::M_SCISSOR_V);
    const uint32_t x0 = std::min(sh & 0xFFFFu, t.width), y0 = std::min(sv & 0xFFFFu, t.height);
    const uint32_t x1 = std::min((sh & 0xFFFFu) + (sh >> 16), t.width), y1 = std::min((sv & 0xFFFFu) + (sv >> 16), t.height);
    VkRect2D sc{ { (int32_t)(x0 * t.aa_u), (int32_t)(y0 * t.aa_v) },
                 { x1 > x0 ? (x1 - x0) * t.aa_u : 0u, y1 > y0 ? (y1 - y0) * t.aa_v : 0u } };
    vkCmdSetScissorWithCount(cmd, 1, &sc);

    const uint32_t lw = f.r(rsx::M_LINE_WIDTH);
    float width = (float)(lw >> 3) + (float)(lw & 7u) / 8.f;
    width = d.features.features.wideLines ? std::clamp(width, d.props.limits.lineWidthRange[0], d.props.limits.lineWidthRange[1]) : 1.f;
    vkCmdSetLineWidth(cmd, width);

    const uint32_t cls = topology_class(prim);
    const uint32_t bias_reg = cls == 0 ? rsx::M_POLY_OFFSET_POINT : cls == 1 ? rsx::M_POLY_OFFSET_LINE : rsx::M_POLY_OFFSET_FILL;
    const bool bias = t.depth && f.r(bias_reg) != 0;
    vkCmdSetDepthBiasEnable(cmd, bias);
    if (bias) vkCmdSetDepthBias(cmd, f.f(rsx::M_POLY_OFFSET_BIAS), 0.f, f.f(rsx::M_POLY_OFFSET_SCALE));

    const bool fp_target = t.color_format[0] == VK_FORMAT_R16G16B16A16_SFLOAT;
    float bc[4];
    const uint32_t c1 = f.r(rsx::M_BLEND_COLOR);
    if (fp_target) {
        const uint32_t c2 = f.r(rsx::M_BLEND_COLOR2);
        bc[0] = half_to_float((uint16_t)(c1 & 0xFFFFu));
        bc[1] = half_to_float((uint16_t)(c1 >> 16));
        bc[2] = half_to_float((uint16_t)(c2 & 0xFFFFu));
        bc[3] = half_to_float((uint16_t)(c2 >> 16));
    } else {
        bc[0] = (float)((c1 >> 16) & 0xFFu) / 255.f;
        bc[1] = (float)((c1 >> 8) & 0xFFu) / 255.f;
        bc[2] = (float)(c1 & 0xFFu) / 255.f;
        bc[3] = (float)(c1 >> 24) / 255.f;
    }
    vkCmdSetBlendConstants(cmd, bc);

    const bool depth_test = t.depth && f.r(rsx::M_DEPTH_TEST_ENABLE) != 0;
    vkCmdSetDepthTestEnable(cmd, depth_test);
    vkCmdSetDepthWriteEnable(cmd, depth_test && f.r(rsx::M_DEPTH_MASK) != 0);
    vkCmdSetDepthCompareOp(cmd, compare_op(f.r(rsx::M_DEPTH_FUNC)));
    const bool bounds = t.depth && d.features.features.depthBounds && f.r(rsx::M_DEPTH_BOUNDS_ENABLE) != 0;
    vkCmdSetDepthBoundsTestEnable(cmd, bounds);
    if (bounds) vkCmdSetDepthBounds(cmd, std::clamp(f.f(rsx::M_DEPTH_BOUNDS_MIN), 0.f, 1.f), std::clamp(f.f(rsx::M_DEPTH_BOUNDS_MAX), 0.f, 1.f));

    const bool stencil = t.stencil && f.r(rsx::M_STENCIL_TEST_ENABLE) != 0;
    vkCmdSetStencilTestEnable(cmd, stencil);
    const bool two = f.r(rsx::M_TWO_SIDED_STENCIL) != 0;
    const uint32_t front_base = rsx::M_STENCIL_MASK, back_base = two ? rsx::M_BACK_STENCIL_MASK : rsx::M_STENCIL_MASK;
    for (int side = 0; side < 2; side++) {
        const uint32_t base = side ? back_base : front_base;
        const VkStencilFaceFlags face = side ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilWriteMask(cmd, face, f.r(base) & 0xFFu);
        vkCmdSetStencilCompareMask(cmd, face, f.r(base + 12u) & 0xFFu);
        vkCmdSetStencilReference(cmd, face, f.r(base + 8u) & 0xFFu);
        vkCmdSetStencilOp(cmd, face, stencil_op(f.r(base + 16u)), stencil_op(f.r(base + 24u)), stencil_op(f.r(base + 20u)),
                          compare_op(f.r(base + 4u)));
    }

    const uint32_t cull = f.r(rsx::M_CULL_FACE);
    VkCullModeFlags cm = VK_CULL_MODE_NONE;
    if (f.r(rsx::M_CULL_FACE_ENABLE))
        cm = cull == 0x404u ? VK_CULL_MODE_FRONT_BIT : cull == 0x408u ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
    vkCmdSetCullMode(cmd, cm);
    vkCmdSetFrontFace(cmd, f.r(rsx::M_FRONT_FACE) == 0x901u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    bool emulated;
    vkCmdSetPrimitiveTopology(cmd, primitive_topology(prim, emulated));
    vkCmdSetPrimitiveRestartEnable(cmd, restart && restart_capable(prim));
    vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);

    if (p.dynamic_polygon_mode()) vkCmdSetPolygonModeEXT(cmd, (VkPolygonMode)key.polygon_mode);
    if (p.dynamic_depth_clamp()) vkCmdSetDepthClampEnableEXT(cmd, key.depth_clamp);
    if (p.dynamic_depth_clip()) vkCmdSetDepthClipEnableEXT(cmd, key.depth_clip);
    if (p.dynamic_provoking())
        vkCmdSetProvokingVertexModeEXT(cmd, key.provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT);
    if (p.dynamic_blend()) {
        uint32_t count = 0;
        for (uint32_t k = 0; k < 4; k++)
            if (t.color_format[k] != VK_FORMAT_UNDEFINED) count = k + 1u;
        if (count) {
            VkBool32 en[4];
            VkColorBlendEquationEXT eqs[4];
            VkColorComponentFlags masks[4];
            for (uint32_t k = 0; k < count; k++) {
                const blend_attachment& b = key.blend[k];
                en[k] = b.enable;
                eqs[k] = { (VkBlendFactor)b.src_color, (VkBlendFactor)b.dst_color, (VkBlendOp)b.color_op,
                           (VkBlendFactor)b.src_alpha, (VkBlendFactor)b.dst_alpha, (VkBlendOp)b.alpha_op };
                masks[k] = b.write_mask;
            }
            vkCmdSetColorBlendEnableEXT(cmd, 0, count, en);
            vkCmdSetColorBlendEquationEXT(cmd, 0, count, eqs);
            vkCmdSetColorWriteMaskEXT(cmd, 0, count, masks);
        }
    }
}

}
