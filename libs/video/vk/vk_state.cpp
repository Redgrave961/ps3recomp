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
                       uint32_t prim, bool restart, const pipeline_key& key, dynamic_cache& cache)
{
    dynamic_state s{};
    s.viewport = { 0.f, 0.f, t.scale.exact(t.width * t.aa_u), t.scale.exact(t.height * t.aa_v), 0.f, 1.f };

    const uint32_t sh = f.r(rsx::M_SCISSOR_H), sv = f.r(rsx::M_SCISSOR_V);
    const uint32_t x0 = std::min(sh & 0xFFFFu, t.width), y0 = std::min(sv & 0xFFFFu, t.height);
    const uint32_t x1 = std::min((sh & 0xFFFFu) + (sh >> 16), t.width), y1 = std::min((sv & 0xFFFFu) + (sv >> 16), t.height);
    const uint32_t ex = t.scale.edge(x0 * t.aa_u), ey = t.scale.edge(y0 * t.aa_v);
    s.scissor = { { (int32_t)ex, (int32_t)ey }, { t.scale.edge(x1 * t.aa_u) - ex, t.scale.edge(y1 * t.aa_v) - ey } };

    const uint32_t lw = f.r(rsx::M_LINE_WIDTH);
    const float width = ((float)(lw >> 3) + (float)(lw & 7u) / 8.f) * t.scale.value();
    s.line_width = d.features.features.wideLines ? std::clamp(width, d.props.limits.lineWidthRange[0], d.props.limits.lineWidthRange[1]) : 1.f;

    const uint32_t cls = topology_class(prim);
    const uint32_t bias_reg = cls == 0 ? rsx::M_POLY_OFFSET_POINT : cls == 1 ? rsx::M_POLY_OFFSET_LINE : rsx::M_POLY_OFFSET_FILL;
    s.bias_enable = t.depth && f.r(bias_reg) != 0;
    if (s.bias_enable) {
        s.bias[0] = f.f(rsx::M_POLY_OFFSET_BIAS);
        s.bias[2] = f.f(rsx::M_POLY_OFFSET_SCALE);
    }

    const uint32_t c1 = f.r(rsx::M_BLEND_COLOR);
    if (t.color_format[0] == VK_FORMAT_R16G16B16A16_SFLOAT) {
        const uint32_t c2 = f.r(rsx::M_BLEND_COLOR2);
        s.blend_constants[0] = half_to_float((uint16_t)(c1 & 0xFFFFu));
        s.blend_constants[1] = half_to_float((uint16_t)(c1 >> 16));
        s.blend_constants[2] = half_to_float((uint16_t)(c2 & 0xFFFFu));
        s.blend_constants[3] = half_to_float((uint16_t)(c2 >> 16));
    } else {
        s.blend_constants[0] = (float)((c1 >> 16) & 0xFFu) / 255.f;
        s.blend_constants[1] = (float)((c1 >> 8) & 0xFFu) / 255.f;
        s.blend_constants[2] = (float)(c1 & 0xFFu) / 255.f;
        s.blend_constants[3] = (float)(c1 >> 24) / 255.f;
    }

    s.depth_test = t.depth && f.r(rsx::M_DEPTH_TEST_ENABLE) != 0;
    s.depth_write = s.depth_test && f.r(rsx::M_DEPTH_MASK) != 0;
    s.depth_op = compare_op(f.r(rsx::M_DEPTH_FUNC));
    s.bounds_enable = t.depth && d.features.features.depthBounds && f.r(rsx::M_DEPTH_BOUNDS_ENABLE) != 0;
    if (s.bounds_enable) {
        s.bounds[0] = std::clamp(f.f(rsx::M_DEPTH_BOUNDS_MIN), 0.f, 1.f);
        s.bounds[1] = std::clamp(f.f(rsx::M_DEPTH_BOUNDS_MAX), 0.f, 1.f);
    }

    s.stencil_test = t.stencil && f.r(rsx::M_STENCIL_TEST_ENABLE) != 0;
    const uint32_t back_base = f.r(rsx::M_TWO_SIDED_STENCIL) != 0 ? rsx::M_BACK_STENCIL_MASK : rsx::M_STENCIL_MASK;
    for (int side = 0; side < 2; side++) {
        const uint32_t base = side ? back_base : rsx::M_STENCIL_MASK;
        s.faces[side] = { f.r(base) & 0xFFu, f.r(base + 12u) & 0xFFu, f.r(base + 8u) & 0xFFu, stencil_op(f.r(base + 16u)),
                          stencil_op(f.r(base + 24u)), stencil_op(f.r(base + 20u)), compare_op(f.r(base + 4u)) };
    }

    const uint32_t cull = f.r(rsx::M_CULL_FACE);
    s.cull = VK_CULL_MODE_NONE;
    if (f.r(rsx::M_CULL_FACE_ENABLE)) s.cull = cull == 0x404u ? VK_CULL_MODE_FRONT_BIT : cull == 0x408u ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
    s.front = f.r(rsx::M_FRONT_FACE) == 0x901u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    bool emulated;
    s.topology = primitive_topology(prim, emulated);
    s.restart = restart && restart_capable(prim);
    s.polygon_mode = key.polygon_mode;
    s.depth_clamp = key.depth_clamp;
    s.depth_clip = key.depth_clip;
    s.provoking = key.provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
    if (p.dynamic_blend()) {
        for (uint32_t k = 0; k < 4; k++)
            if (t.color_format[k] != VK_FORMAT_UNDEFINED) s.blend_count = k + 1u;
        for (uint32_t k = 0; k < s.blend_count; k++) {
            const blend_attachment& b = key.blend[k];
            s.blend_enable[k] = b.enable;
            s.equation[k] = { (VkBlendFactor)b.src_color, (VkBlendFactor)b.dst_color, (VkBlendOp)b.color_op,
                              (VkBlendFactor)b.src_alpha, (VkBlendFactor)b.dst_alpha, (VkBlendOp)b.alpha_op };
            s.write_mask[k] = b.write_mask;
        }
    }

    const bool all = !cache.valid;
    const dynamic_state& o = cache.last;
    const auto changed = [all](const auto& a, const auto& b) { return all || std::memcmp(&a, &b, sizeof a) != 0; };
    if (all) vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
    if (changed(s.viewport, o.viewport)) vkCmdSetViewportWithCount(cmd, 1, &s.viewport);
    if (changed(s.scissor, o.scissor)) vkCmdSetScissorWithCount(cmd, 1, &s.scissor);
    if (changed(s.line_width, o.line_width)) vkCmdSetLineWidth(cmd, s.line_width);
    if (changed(s.bias_enable, o.bias_enable)) vkCmdSetDepthBiasEnable(cmd, s.bias_enable);
    if (s.bias_enable && changed(s.bias, o.bias)) vkCmdSetDepthBias(cmd, s.bias[0], s.bias[1], s.bias[2]);
    if (changed(s.blend_constants, o.blend_constants)) vkCmdSetBlendConstants(cmd, s.blend_constants);
    if (changed(s.depth_test, o.depth_test)) vkCmdSetDepthTestEnable(cmd, s.depth_test);
    if (changed(s.depth_write, o.depth_write)) vkCmdSetDepthWriteEnable(cmd, s.depth_write);
    if (changed(s.depth_op, o.depth_op)) vkCmdSetDepthCompareOp(cmd, (VkCompareOp)s.depth_op);
    if (changed(s.bounds_enable, o.bounds_enable)) vkCmdSetDepthBoundsTestEnable(cmd, s.bounds_enable);
    if (s.bounds_enable && changed(s.bounds, o.bounds)) vkCmdSetDepthBounds(cmd, s.bounds[0], s.bounds[1]);
    if (changed(s.stencil_test, o.stencil_test)) vkCmdSetStencilTestEnable(cmd, s.stencil_test);
    for (int side = 0; side < 2; side++) {
        const dynamic_state::face& n = s.faces[side];
        const dynamic_state::face& l = o.faces[side];
        const VkStencilFaceFlags face = side ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        if (changed(n.write, l.write)) vkCmdSetStencilWriteMask(cmd, face, n.write);
        if (changed(n.compare, l.compare)) vkCmdSetStencilCompareMask(cmd, face, n.compare);
        if (changed(n.reference, l.reference)) vkCmdSetStencilReference(cmd, face, n.reference);
        if (all || n.fail != l.fail || n.pass != l.pass || n.depth_fail != l.depth_fail || n.op != l.op)
            vkCmdSetStencilOp(cmd, face, n.fail, n.pass, n.depth_fail, n.op);
    }
    if (changed(s.cull, o.cull)) vkCmdSetCullMode(cmd, s.cull);
    if (changed(s.front, o.front)) vkCmdSetFrontFace(cmd, (VkFrontFace)s.front);
    if (changed(s.topology, o.topology)) vkCmdSetPrimitiveTopology(cmd, (VkPrimitiveTopology)s.topology);
    if (changed(s.restart, o.restart)) vkCmdSetPrimitiveRestartEnable(cmd, s.restart);
    if (p.dynamic_polygon_mode() && changed(s.polygon_mode, o.polygon_mode)) vkCmdSetPolygonModeEXT(cmd, (VkPolygonMode)s.polygon_mode);
    if (p.dynamic_depth_clamp() && changed(s.depth_clamp, o.depth_clamp)) vkCmdSetDepthClampEnableEXT(cmd, s.depth_clamp);
    if (p.dynamic_depth_clip() && changed(s.depth_clip, o.depth_clip)) vkCmdSetDepthClipEnableEXT(cmd, s.depth_clip);
    if (p.dynamic_provoking() && changed(s.provoking, o.provoking)) vkCmdSetProvokingVertexModeEXT(cmd, (VkProvokingVertexModeEXT)s.provoking);
    if (s.blend_count) {
        if (all || s.blend_count != o.blend_count || std::memcmp(s.blend_enable, o.blend_enable, s.blend_count * sizeof s.blend_enable[0]))
            vkCmdSetColorBlendEnableEXT(cmd, 0, s.blend_count, s.blend_enable);
        if (all || s.blend_count != o.blend_count || std::memcmp(s.equation, o.equation, s.blend_count * sizeof s.equation[0]))
            vkCmdSetColorBlendEquationEXT(cmd, 0, s.blend_count, s.equation);
        if (all || s.blend_count != o.blend_count || std::memcmp(s.write_mask, o.write_mask, s.blend_count * sizeof s.write_mask[0]))
            vkCmdSetColorWriteMaskEXT(cmd, 0, s.blend_count, s.write_mask);
    }
    cache.last = s;
    cache.valid = true;
}

}