#pragma once

#include "vk_device.h"

#include <cstdint>

namespace vk {

enum primitive : uint32_t {
    PRIM_POINTS = 1, PRIM_LINES, PRIM_LINE_LOOP, PRIM_LINE_STRIP, PRIM_TRIANGLES,
    PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN, PRIM_QUADS, PRIM_QUAD_STRIP, PRIM_POLYGON,
};

struct index_result {
    uint32_t count;
    uint32_t min;
    uint32_t max;
};

struct index_format {
    bool wide;
    bool restart;
    bool sentinel;
    bool promote;
    uint32_t restart_index;
    uint32_t in_size() const { return wide ? 4u : 2u; }
    uint32_t out_size() const { return wide || promote ? 4u : 2u; }
};

VkPrimitiveTopology primitive_topology(uint32_t prim, bool& emulated);
uint32_t topology_class(uint32_t prim);
uint32_t expanded_index_count(uint32_t prim, uint32_t count);
uint32_t index_capacity(uint32_t prim, uint32_t count);
void generate_indices(uint32_t prim, uint32_t count, uint32_t* dst);
index_format index_format_for(uint32_t prim, bool wide, bool restart, uint32_t restart_index);
index_result convert_indices(uint32_t prim, const index_format& format, const uint8_t* src_be, uint32_t count, void* dst);

}
