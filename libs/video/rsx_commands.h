/*
 * ps3recomp - RSX Command Buffer Processor
 *
 * The PS3's RSX (Reality Synthesizer) is an NV47-class GPU that receives
 * rendering commands through a ring buffer. Games write NV47xx method
 * calls to this buffer via cellGcm, and the RSX processes them.
 *
 * In recompilation, we intercept the command buffer and translate
 * NV47xx methods to the host graphics API (D3D12 on Windows, Vulkan
 * on Linux/macOS).
 *
 * Architecture:
 *   Game code → cellGcmSys (state tracking) → command buffer → RSX processor
 *                                                                  ↓
 *                                                          Host GPU backend
 *
 * Reference: NV47xx method registers documented in envytools/rnndb
 * and RPCS3's RSX module (rpcs3/Emu/RSX/).
 */

#ifndef PS3RECOMP_RSX_COMMANDS_H
#define PS3RECOMP_RSX_COMMANDS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * NV47xx Method Register Addresses
 *
 * These are the GPU method addresses that games write to the command buffer.
 * Each method has a 32-bit address and one or more 32-bit data words.
 *
 * Naming: NV4097_SET_* for 3D methods, NV3062_SET_* for 2D methods
 * -----------------------------------------------------------------------*/

/* Surface / render target configuration (real NV4097 numbering -- RPCS3 /
 * nouveau. The original table here was shuffled: only COLOR_AOFFSET was
 * right, so SET_SURFACE_COLOR_TARGET (MRT selection) was never decoded and
 * clip dims read the format register). */
/* Which context DMA a colour surface's offset is an offset INTO: handle
 * 0xFEED0000 is RSX local memory and 0xFEED0001 is IO-mapped main memory.
 * RSX offsets are two overlapping number spaces, so this is what tells a
 * surface at offset X from a texture at offset X in the other one. Decoded
 * by rsx_dispatch (rsx_dsp_surface.color_location). */
#define NV4097_SET_CONTEXT_DMA_COLOR_A         0x00000194
/* ...and the same for colour target B, which sits BELOW A in the register
 * file rather than after it (C and D are at 0x1B4 and 0x1B8). An MRT set
 * whose members disagree about which memory they live in is one the draw
 * engine cannot match a texture unit against, since it keys surfaces by
 * (location, offset). */
#define NV4097_SET_CONTEXT_DMA_COLOR_B         0x0000018C
/* ...and the same question for the depth buffer, which a texture unit coming
 * back to sample it has to agree with. */
#define NV4097_SET_CONTEXT_DMA_ZETA            0x00000198
#define CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER 0xFEED0000
#define CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER  0xFEED0001

#define NV4097_SET_SURFACE_CLIP_HORIZONTAL     0x00000200
#define NV4097_SET_SURFACE_CLIP_VERTICAL       0x00000204
#define NV4097_SET_SURFACE_FORMAT              0x00000208
#define NV4097_SET_SURFACE_PITCH_A             0x0000020C
#define NV4097_SET_SURFACE_COLOR_AOFFSET       0x00000210
#define NV4097_SET_SURFACE_ZETA_OFFSET         0x00000214
#define NV4097_SET_SURFACE_COLOR_BOFFSET       0x00000218
#define NV4097_SET_SURFACE_PITCH_B             0x0000021C
#define NV4097_SET_SURFACE_COLOR_TARGET        0x00000220
#define NV4097_SET_SURFACE_PITCH_Z             0x0000022C
#define NV4097_SET_SURFACE_PITCH_C             0x00000280
#define NV4097_SET_SURFACE_PITCH_D             0x00000284
#define NV4097_SET_SURFACE_COLOR_COFFSET       0x00000288
#define NV4097_SET_SURFACE_COLOR_DOFFSET       0x0000028C

/* SET_SURFACE_COLOR_TARGET values */
#define CELL_GCM_SURFACE_TARGET_NONE  0x00
#define CELL_GCM_SURFACE_TARGET_0     0x01
#define CELL_GCM_SURFACE_TARGET_1     0x02
#define CELL_GCM_SURFACE_TARGET_MRT1  0x13   /* A + B     */
#define CELL_GCM_SURFACE_TARGET_MRT2  0x17   /* A + B + C */
#define CELL_GCM_SURFACE_TARGET_MRT3  0x1F   /* A+B+C+D   */

/* Viewport (real NV4097 numbering -- the old 0x300/0x304 values actually
 * belong to the dither/alpha-test block, so viewport rects read garbage). */
#define NV4097_SET_VIEWPORT_HORIZONTAL         0x00000A00
#define NV4097_SET_VIEWPORT_VERTICAL           0x00000A04
#define NV4097_SET_CLIP_MIN                    0x00000394
#define NV4097_SET_CLIP_MAX                    0x00000398

/* Scissor */
#define NV4097_SET_SCISSOR_HORIZONTAL          0x000008C0
#define NV4097_SET_SCISSOR_VERTICAL            0x000008C4

/* Clear */
#define NV4097_SET_COLOR_CLEAR_VALUE           0x00001D90
#define NV4097_SET_ZSTENCIL_CLEAR_VALUE        0x00001D8C
#define NV4097_CLEAR_SURFACE                   0x00001D94

/* Draw commands */
#define NV4097_SET_BEGIN_END                    0x00001808
#define NV4097_DRAW_ARRAYS                     0x00001814
#define NV4097_SET_INDEX_ARRAY_ADDRESS         0x0000181C
#define NV4097_SET_INDEX_ARRAY_DMA             0x00001820
#define NV4097_DRAW_INDEX_ARRAY                0x00001824

/* Vertex attributes */
#define NV4097_SET_VERTEX_DATA_ARRAY_FORMAT     0x00001740
#define NV4097_SET_VERTEX_DATA_ARRAY_OFFSET     0x00001680

/* Texture */
#define NV4097_SET_TEXTURE_OFFSET               0x00001A00
#define NV4097_SET_TEXTURE_FORMAT               0x00001A04
#define NV4097_SET_TEXTURE_ADDRESS              0x00001A08
#define NV4097_SET_TEXTURE_CONTROL0             0x00001A0C
#define NV4097_SET_TEXTURE_CONTROL1             0x00001A10
#define NV4097_SET_TEXTURE_FILTER               0x00001A14
#define NV4097_SET_TEXTURE_IMAGE_RECT           0x00001A18
#define NV4097_SET_TEXTURE_BORDER_COLOR         0x00001A1C
/* CONTROL3 sits in its own block, 4 bytes per unit rather than 0x20, because
 * it was added to the register file after the per-unit blocks were laid out.
 * Its low 20 bits are the row pitch of a linear texture and the rest is the
 * depth of a 3D one. */
#define NV4097_SET_TEXTURE_CONTROL3             0x00001840

/* Vertex textures: four units of eight words at 0x0900 + unit*0x20. The block
 * is laid out like a fragment unit's with one substitution -- CONTROL3 sits
 * where CONTROL1 does, so a vertex unit has a row pitch and no component
 * crossbar. A transform program's TXL instruction samples these. */
#define NV4097_SET_VERTEX_TEXTURE_OFFSET        0x00000900
#define NV4097_SET_VERTEX_TEXTURE_FORMAT        0x00000904
#define NV4097_SET_VERTEX_TEXTURE_ADDRESS       0x00000908
#define NV4097_SET_VERTEX_TEXTURE_CONTROL0      0x0000090C
#define NV4097_SET_VERTEX_TEXTURE_CONTROL3      0x00000910
#define NV4097_SET_VERTEX_TEXTURE_FILTER        0x00000914
#define NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT    0x00000918
#define NV4097_SET_VERTEX_TEXTURE_BORDER_COLOR  0x0000091C

/* Shader programs */
#define NV4097_SET_SHADER_PROGRAM               0x000008E4
#define NV4097_SET_SHADER_CONTROL               0x00001D60
/* shader_control bit: fragment colour exports come from r0/r2/r3/r4 (32-bit)
 * instead of h0/h4/h6/h8 (half). */
#define CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS 0x00000040
/* shader_control bits: the program's r1.z replaces the fragment depth. */
#define CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT    0x0000000E
#define NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK    0x00001FF4
#define NV4097_SET_TRANSFORM_PROGRAM_LOAD       0x00001E9C
/* Which INSTRUCTION the vertex program starts executing at. Separate from
 * _LOAD, which only says where following microcode words are written. A title
 * that keeps several programs resident in the 512-instruction store selects
 * between them with this, so ignoring it runs whichever program happens to sit
 * at instruction 0 for every draw. */
#define NV4097_SET_TRANSFORM_PROGRAM_START      0x00001EA0
#define NV4097_SET_TRANSFORM_PROGRAM            0x00000B80
#define NV4097_SET_TRANSFORM_CONSTANT_LOAD      0x00001EFC
/* Vertex constants are written as up to 32 dwords (8 vec4s) per command,
 * 0x1F00..0x1F7C. Slot = transform_constant_load + (reg/4); lane = reg%4.
 * 0x1F80..0x1FFC are other registers (below), not more constants.
 * The hardware supports 512 vec4 constants total in the vertex register file. */
#define NV4097_SET_TRANSFORM_CONSTANT           0x00001F00
#define NV4097_SET_TRANSFORM_CONSTANT_DWORDS    32
#define NV4097_INVALIDATE_L2                    0x00001FD8
#define NV4097_SET_VERTEX_ATTRIB_INPUT_MASK     0x00001FF0
#define NV4097_SET_TRANSFORM_BRANCH_BITS        0x00001FF8
#define RSX_MAX_VERTEX_CONSTANTS                512

/* Color mask */
#define NV4097_SET_COLOR_MASK                   0x00000324

/* Alpha test */
#define NV4097_SET_ALPHA_TEST_ENABLE            0x00000304
#define NV4097_SET_ALPHA_FUNC                   0x00000308
#define NV4097_SET_ALPHA_REF                    0x0000030C

/* Blending */
#define NV4097_SET_BLEND_ENABLE                 0x00000310
#define NV4097_SET_BLEND_FUNC_SFACTOR           0x00000314
#define NV4097_SET_BLEND_FUNC_DFACTOR           0x00000318
#define NV4097_SET_BLEND_EQUATION               0x00000320
#define NV4097_SET_BLEND_COLOR                  0x0000031C

/* Depth / stencil */
#define NV4097_SET_DEPTH_TEST_ENABLE            0x00000A74
#define NV4097_SET_DEPTH_FUNC                   0x00000A6C
#define NV4097_SET_DEPTH_MASK                   0x00000A70
#define NV4097_SET_STENCIL_TEST_ENABLE          0x00000328
#define NV4097_SET_STENCIL_FUNC                 0x00000330
#define NV4097_SET_STENCIL_FUNC_REF             0x00000334
#define NV4097_SET_STENCIL_FUNC_MASK            0x00000338
#define NV4097_SET_STENCIL_OP_FAIL              0x0000033C
#define NV4097_SET_STENCIL_OP_ZFAIL             0x00000340
#define NV4097_SET_STENCIL_OP_ZPASS             0x00000344
#define NV4097_SET_STENCIL_MASK                 0x0000032C  /* stencil write mask */
#define NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE 0x00000348
#define NV4097_SET_BACK_STENCIL_MASK            0x0000034C
#define NV4097_SET_BACK_STENCIL_FUNC            0x00000350
#define NV4097_SET_BACK_STENCIL_FUNC_REF        0x00000354
#define NV4097_SET_BACK_STENCIL_FUNC_MASK       0x00000358
#define NV4097_SET_BACK_STENCIL_OP_FAIL         0x0000035C
#define NV4097_SET_BACK_STENCIL_OP_ZFAIL        0x00000360
#define NV4097_SET_BACK_STENCIL_OP_ZPASS        0x00000364
#define NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE     0x00000380
#define NV4097_SET_DEPTH_BOUNDS_MIN             0x00000384  /* float */
#define NV4097_SET_DEPTH_BOUNDS_MAX             0x00000388  /* float */

/* Culling */
#define NV4097_SET_CULL_FACE_ENABLE             0x0000183C
#define NV4097_SET_CULL_FACE                    0x00001830
#define NV4097_SET_FRONT_FACE                   0x00001834

/* Primitive types */
#define RSX_PRIMITIVE_POINTS             1
#define RSX_PRIMITIVE_LINES              2
#define RSX_PRIMITIVE_LINE_LOOP          3
#define RSX_PRIMITIVE_LINE_STRIP         4
#define RSX_PRIMITIVE_TRIANGLES          5
#define RSX_PRIMITIVE_TRIANGLE_STRIP     6
#define RSX_PRIMITIVE_TRIANGLE_FAN       7
#define RSX_PRIMITIVE_QUADS              8
#define RSX_PRIMITIVE_QUAD_STRIP         9
#define RSX_PRIMITIVE_POLYGON            10

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_COMMANDS_H */
