#pragma once

#include <cstdint>
#include <vector>

namespace rsx {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

enum : u32 {
    REG_COUNT = 0x2000u / 4u,
    VP_SLOTS = 512u,
    VP_CONSTANTS = 468u,
    ATTRIBS = 16u,
    TEXTURES = 16u,
    VERTEX_TEXTURES = 4u,
};

enum method : u32 {
    M_DMA_COLOR_B = 0x018Cu, M_DMA_COLOR_A = 0x0194u, M_DMA_ZETA = 0x0198u,
    M_DMA_VERTEX_A = 0x019Cu, M_DMA_VERTEX_B = 0x01A0u, M_DMA_COLOR_C = 0x01B4u, M_DMA_COLOR_D = 0x01B8u,
    M_SURFACE_CLIP_H = 0x0200u, M_SURFACE_CLIP_V = 0x0204u, M_SURFACE_FORMAT = 0x0208u,
    M_SURFACE_PITCH_A = 0x020Cu, M_SURFACE_COLOR_A = 0x0210u, M_SURFACE_ZETA = 0x0214u,
    M_SURFACE_COLOR_B = 0x0218u, M_SURFACE_PITCH_B = 0x021Cu, M_SURFACE_TARGET = 0x0220u,
    M_SURFACE_PITCH_Z = 0x022Cu,
    M_SURFACE_PITCH_C = 0x0280u, M_SURFACE_PITCH_D = 0x0284u, M_SURFACE_COLOR_C = 0x0288u, M_SURFACE_COLOR_D = 0x028Cu,
    M_WINDOW_OFFSET = 0x02B8u,
    M_DITHER_ENABLE = 0x0300u, M_ALPHA_TEST_ENABLE = 0x0304u, M_ALPHA_FUNC = 0x0308u, M_ALPHA_REF = 0x030Cu,
    M_BLEND_ENABLE = 0x0310u, M_BLEND_SFACTOR = 0x0314u, M_BLEND_DFACTOR = 0x0318u, M_BLEND_COLOR = 0x031Cu,
    M_BLEND_EQUATION = 0x0320u, M_COLOR_MASK = 0x0324u,
    M_STENCIL_TEST_ENABLE = 0x0328u, M_STENCIL_MASK = 0x032Cu,
    M_STENCIL_FAIL = 0x033Cu,
    M_TWO_SIDED_STENCIL = 0x0348u, M_BACK_STENCIL_MASK = 0x034Cu,
    M_BACK_STENCIL_FAIL = 0x035Cu,
    M_BACK_STENCIL_ZPASS = 0x0364u,
    M_SHADE_MODE = 0x0368u, M_BLEND_ENABLE_MRT = 0x036Cu, M_COLOR_MASK_MRT = 0x0370u,
    M_LOGIC_OP_ENABLE = 0x0374u, M_LOGIC_OP = 0x0378u, M_BLEND_COLOR2 = 0x037Cu,
    M_DEPTH_BOUNDS_ENABLE = 0x0380u, M_DEPTH_BOUNDS_MIN = 0x0384u, M_DEPTH_BOUNDS_MAX = 0x0388u,
    M_CLIP_MIN = 0x0394u, M_CLIP_MAX = 0x0398u, M_CONTROL0 = 0x03B0u, M_LINE_WIDTH = 0x03B8u, M_LINE_SMOOTH = 0x03BCu,
    M_SCISSOR_H = 0x08C0u, M_SCISSOR_V = 0x08C4u, M_FOG_MODE = 0x08CCu, M_FOG_PARAMS = 0x08D0u,
    M_SHADER_PROGRAM = 0x08E4u,
    M_VTEX_OFFSET = 0x0900u,
    M_VIEWPORT_H = 0x0A00u, M_VIEWPORT_V = 0x0A04u, M_POINT_CENTER_MODE = 0x0A0Cu,
    M_VIEWPORT_OFFSET = 0x0A20u, M_VIEWPORT_SCALE = 0x0A30u,
    M_POLY_OFFSET_POINT = 0x0A60u, M_POLY_OFFSET_LINE = 0x0A64u, M_POLY_OFFSET_FILL = 0x0A68u,
    M_DEPTH_FUNC = 0x0A6Cu, M_DEPTH_MASK = 0x0A70u, M_DEPTH_TEST_ENABLE = 0x0A74u,
    M_POLY_OFFSET_SCALE = 0x0A78u, M_POLY_OFFSET_BIAS = 0x0A7Cu,
    M_VERTEX_DATA_SCALED4S = 0x0A80u,
    M_TEXTURE_CONTROL2 = 0x0B00u, M_TEX_COORD_CONTROL = 0x0B40u,
    M_TRANSFORM_PROGRAM = 0x0B80u,
    M_SPECULAR_ENABLE = 0x1428u, M_TWO_SIDE_LIGHT = 0x142Cu,
    M_FLAT_SHADE_OP = 0x1454u,
    M_USER_CLIP_PLANE_CONTROL = 0x1478u, M_POLYGON_STIPPLE = 0x147Cu, M_POLYGON_STIPPLE_PATTERN = 0x1480u,
    M_VERTEX_DATA3F = 0x1500u,
    M_VERTEX_ARRAY_OFFSET = 0x1680u,
    M_VERTEX_BASE_OFFSET = 0x1738u, M_VERTEX_BASE_INDEX = 0x173Cu,
    M_VERTEX_ARRAY_FORMAT = 0x1740u,
    M_CLEAR_REPORT_VALUE = 0x17C8u, M_ZPASS_PIXEL_COUNT_ENABLE = 0x17CCu,
    M_ZCULL_STATS_ENABLE = 0x1804u, M_BEGIN_END = 0x1808u,
    M_ARRAY_ELEMENT16 = 0x180Cu, M_ARRAY_ELEMENT32 = 0x1810u, M_DRAW_ARRAYS = 0x1814u, M_INLINE_ARRAY = 0x1818u,
    M_INDEX_ARRAY_ADDRESS = 0x181Cu, M_INDEX_ARRAY_DMA = 0x1820u, M_DRAW_INDEX_ARRAY = 0x1824u,
    M_FRONT_POLYGON_MODE = 0x1828u, M_CULL_FACE = 0x1830u, M_FRONT_FACE = 0x1834u,
    M_CULL_FACE_ENABLE = 0x183Cu, M_TEXTURE_CONTROL3 = 0x1840u,
    M_VERTEX_DATA2F = 0x1880u, M_VERTEX_DATA2S = 0x1900u, M_VERTEX_DATA4UB = 0x1940u, M_VERTEX_DATA4S = 0x1980u,
    M_TEXTURE_OFFSET = 0x1A00u,
    M_VERTEX_DATA4F = 0x1C00u,
    M_SHADER_CONTROL = 0x1D60u, M_INDEXED_CONSTANT_READ_LIMITS = 0x1D64u,
    M_ZMIN_MAX_CONTROL = 0x1D78u, M_ANTI_ALIASING_CONTROL = 0x1D7Cu, M_SURFACE_COMPRESSION = 0x1D80u,
    M_SHADER_WINDOW = 0x1D88u, M_ZSTENCIL_CLEAR_VALUE = 0x1D8Cu, M_COLOR_CLEAR_VALUE = 0x1D90u,
    M_CLEAR_SURFACE = 0x1D94u,
    M_RESTART_INDEX_ENABLE = 0x1DACu, M_RESTART_INDEX = 0x1DB0u, M_LINE_STIPPLE_PATTERN = 0x1DB8u,
    M_VERTEX_DATA1F = 0x1E40u,
    M_TRANSFORM_EXECUTION_MODE = 0x1E94u, M_RENDER_ENABLE = 0x1E98u,
    M_TRANSFORM_PROGRAM_LOAD = 0x1E9Cu, M_TRANSFORM_PROGRAM_START = 0x1EA0u,
    M_POINT_SIZE = 0x1EE0u, M_POINT_SPRITE_CONTROL = 0x1EE8u,
    M_TRANSFORM_CONSTANT_LOAD = 0x1EFCu, M_TRANSFORM_CONSTANT = 0x1F00u,
    M_FREQUENCY_DIVIDER_OPERATION = 0x1FC0u,
    M_SHADER_PACKER = 0x1FECu,
    M_VERTEX_ATTRIB_INPUT_MASK = 0x1FF0u, M_VERTEX_ATTRIB_OUTPUT_MASK = 0x1FF4u, M_TRANSFORM_BRANCH_BITS = 0x1FF8u,
};

enum dirty : u32 {
    D_SURFACE = 1u << 0,
    D_VIEWPORT = 1u << 1,
    D_RASTER = 1u << 2,
    D_DEPTH_STENCIL = 1u << 3,
    D_BLEND = 1u << 4,
    D_FRAGMENT_PROGRAM = 1u << 5,
    D_FRAGMENT_STATE = 1u << 6,
    D_VERTEX_LAYOUT = 1u << 7,
    D_VERTEX_PROGRAM = 1u << 8,
    D_VERTEX_STATE = 1u << 9,
    D_TEXTURES = 1u << 10,
    D_VERTEX_TEXTURES = 1u << 11,
    D_VERTEX_CONSTANTS = 1u << 12,
    D_FB_STATE = 1u << 13,
    D_ALL = (1u << 14) - 1u,
};

enum class draw_kind : u8 { none, array, indexed, inlined };

enum vertex_type : u32 {
    VT_S1 = 1, VT_F = 2, VT_UB = 4, VT_S32K = 5, VT_CMP = 6, VT_UB256 = 7,
};

struct draw_range {
    u32 first;
    u32 count;
};

struct register_attribute {
    u32 type = VT_F;
    u32 size = 0;
    u32 data[4] = {};
};

struct push_attribute {
    u32 type = VT_F;
    u32 size = 0;
    u32 vertices = 0;
    u32 dwords = 0;
    std::vector<u32> data;
};

struct draw_clause {
    u32 primitive = 0;
    draw_kind kind = draw_kind::none;
    bool immediate = false;
    std::vector<draw_range> ranges;
    std::vector<u32> inline_words;
    std::vector<u32> elements;
};

class frontend {
public:
    u32 reg[REG_COUNT] = {};
    u32 program[VP_SLOTS * 4] = {};
    u32 constants[VP_CONSTANTS * 4] = {};
    u32 const_dirty[(VP_CONSTANTS + 31u) / 32u] = {};
    register_attribute attribute[ATTRIBS];
    push_attribute push[ATTRIBS];
    draw_clause draw;
    u32 dirty = D_ALL;
    u32 dirty_tex = 0xFFFFFu;
    u32 pushed = 0;
    bool vp_code_dirty = true;
    bool in_begin_end = false;

    frontend();
    virtual ~frontend() = default;

    void methods(u32 method, u32 inc, const u32* be, u32 count);
    void reset();

    u32 r(u32 method) const { return reg[method >> 2]; }
    float f(u32 method) const;

protected:
    virtual void on_draw(const draw_clause& clause) = 0;
    virtual void on_clear(u32 mask) = 0;
    virtual void on_report(u32 method, u32 value) = 0;

private:
    u32 program_methods(u32 index, const u32* be, u32 count);
    u32 constant_methods(u32 index, u32 inc, const u32* be, u32 count);
    void vertex_data(u32 method, u32 value_be);
    void store(u32 method, u32 value);
    void begin_end(u32 value);
    void flush_ranges();
    void push_vertex(u32 index, u32 sub, u32 size, u32 type, u32 value);
};

}
