#ifndef PS3RECOMP_RSX_SPIRV_H
#define PS3RECOMP_RSX_SPIRV_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_SPV_VP_SLOTS            512u
#define RSX_SPV_VP_CONSTANTS        468u
#define RSX_SPV_VERTEX_INPUTS       16u
#define RSX_SPV_TEXTURE_UNITS       16u
#define RSX_SPV_VERTEX_TEXTURE_UNITS 4u
#define RSX_SPV_FP_MAX_CONSTANTS    512u
#define RSX_SPV_FP_MAX_INSTRUCTIONS 4096u

enum rsx_spv_binding {
    RSX_SPV_BIND_VP_CONSTANTS   = 0,
    RSX_SPV_BIND_VERTEX_LAYOUT  = 1,
    RSX_SPV_BIND_VERTEX_DATA    = 2,
    RSX_SPV_BIND_FP_CONSTANTS   = 3,
    RSX_SPV_BIND_FP_STATE       = 4,
    RSX_SPV_BIND_TEXTURE0       = 8,
    RSX_SPV_BIND_VERTEX_TEXTURE0 = 24
};

enum rsx_spv_varying {
    RSX_SPV_VAR_COL0 = 0,
    RSX_SPV_VAR_COL1 = 1,
    RSX_SPV_VAR_BFC0 = 2,
    RSX_SPV_VAR_BFC1 = 3,
    RSX_SPV_VAR_FOG  = 4,
    RSX_SPV_VAR_TEX0 = 5,
    RSX_SPV_VAR_COUNT = 15
};

enum rsx_spv_tex_dim {
    RSX_SPV_TEX_NONE = 0,
    RSX_SPV_TEX_2D   = 1,
    RSX_SPV_TEX_CUBE = 2,
    RSX_SPV_TEX_3D   = 3
};

enum rsx_spv_vertex_format {
    RSX_SPV_VFMT_S1          = 1,
    RSX_SPV_VFMT_F           = 2,
    RSX_SPV_VFMT_SF          = 3,
    RSX_SPV_VFMT_UB          = 4,
    RSX_SPV_VFMT_S32K        = 5,
    RSX_SPV_VFMT_CMP         = 6,
    RSX_SPV_VFMT_UB256       = 7,
    RSX_SPV_VFMT_TYPE_MASK   = 0x0Fu,
    RSX_SPV_VFMT_SIZE_SHIFT  = 4,
    RSX_SPV_VFMT_SIZE_MASK   = 0xF0u,
    RSX_SPV_VFMT_MODULO      = 0x100u
};

enum rsx_spv_clip_plane_op {
    RSX_SPV_CLIP_LT       = 0,
    RSX_SPV_CLIP_DISABLED = 1,
    RSX_SPV_CLIP_GE       = 2,
    RSX_SPV_CLIP_ALL_DISABLED = 0x555u
};

enum rsx_spv_vp_output_bit {
    RSX_SPV_VPOUT_FRONTDIFFUSE  = 1u << 0,
    RSX_SPV_VPOUT_FRONTSPECULAR = 1u << 1,
    RSX_SPV_VPOUT_BACKDIFFUSE   = 1u << 2,
    RSX_SPV_VPOUT_BACKSPECULAR  = 1u << 3,
    RSX_SPV_VPOUT_FOG           = 1u << 4,
    RSX_SPV_VPOUT_POINTSIZE     = 1u << 5,
    RSX_SPV_VPOUT_UC0           = 1u << 6,
    RSX_SPV_VPOUT_TEX8          = 1u << 12,
    RSX_SPV_VPOUT_TEX9          = 1u << 13,
    RSX_SPV_VPOUT_TEX0          = 1u << 14
};

enum rsx_spv_fp_control_bit {
    RSX_SPV_FPCTL_DEPTH_EXPORT  = 0x0Eu,
    RSX_SPV_FPCTL_32BIT_EXPORTS = 0x40u,
    RSX_SPV_FPCTL_FLAT_SHADING  = 0x800u
};

enum rsx_spv_fp_state_flag {
    RSX_SPV_FPF_ALPHA_TEST           = 1u << 0,
    RSX_SPV_FPF_TWO_SIDED            = 1u << 1,
    RSX_SPV_FPF_PIXEL_CENTER_INTEGER = 1u << 2
};

enum rsx_spv_fog_mode {
    RSX_SPV_FOG_LINEAR     = 0x2601,
    RSX_SPV_FOG_EXP        = 0x0800,
    RSX_SPV_FOG_EXP2       = 0x0801,
    RSX_SPV_FOG_EXP_ABS    = 0x0802,
    RSX_SPV_FOG_EXP2_ABS   = 0x0803,
    RSX_SPV_FOG_LINEAR_ABS = 0x0804
};

enum rsx_spv_texture_control_bit {
    RSX_SPV_TEXC_GAMMA_SHIFT  = 0,
    RSX_SPV_TEXC_EXPAND_SHIFT = 4,
    RSX_SPV_TEXC_SEXT_SHIFT   = 8,
    RSX_SPV_TEXC_ALPHAKILL    = 1u << 12,
    RSX_SPV_TEXC_RENORMALIZE  = 1u << 13,
    RSX_SPV_TEXC_DEPTH_FLOAT  = 1u << 14,
    RSX_SPV_TEXC_UNNORMALIZED = 1u << 15,
    RSX_SPV_TEXC_BX2_CAPABLE  = 1u << 16,
    RSX_SPV_TEXC_CHANNELS16   = 1u << 17,
    RSX_SPV_TEXREMAP_BX2_SHIFT = 16
};

typedef struct rsx_spv_vertex_attrib {
    u32 offset;
    u32 stride;
    u32 format;
    u32 frequency;
} rsx_spv_vertex_attrib;

typedef struct rsx_spv_vertex_layout {
    rsx_spv_vertex_attrib attrib[RSX_SPV_VERTEX_INPUTS];
    float value[RSX_SPV_VERTEX_INPUTS][4];
    float scale_offset[4][4];
    u32 vertex_base;
    u32 instance_base;
    u32 clip_planes;
    u32 flags;
    u32 transform_branch_bits;
    u32 reserved[3];
} rsx_spv_vertex_layout;

typedef struct rsx_spv_texture_state {
    u32 control;
    u32 remap;
    float scale[2];
} rsx_spv_texture_state;

typedef struct rsx_spv_fragment_state {
    u32 flags;
    u32 alpha_func;
    float alpha_ref;
    u32 fog_mode;
    float fog_param[2];
    float wpos_scale;
    float wpos_bias;
    rsx_spv_texture_state texture[RSX_SPV_TEXTURE_UNITS];
    u32 texcoord_control;
    u32 reserved[3];
} rsx_spv_fragment_state;

typedef struct rsx_spv_vp_key {
    u32 start;
    u32 output_mask;
    u32 texture_dims;
} rsx_spv_vp_key;

typedef struct rsx_spv_fp_key {
    u32 control;
    u32 texture_dims;
    u32 texture_shadow;
    u32 output_mask;
} rsx_spv_fp_key;

typedef struct rsx_spv_blob {
    u32* words;
    u32 count;
    u32 capacity;
} rsx_spv_blob;

typedef struct rsx_spv_fp_info {
    u32 instructions;
    u32 constant_count;
    u32 constant_word[RSX_SPV_FP_MAX_CONSTANTS];
    u32 texture_mask;
    u32 input_mask;
    int depth_export;
    u32 ucode_words;
} rsx_spv_fp_info;

typedef struct rsx_spv_vp_info {
    u32 instructions;
    u32 input_mask;
    u32 output_mask;
    u32 texture_mask;
    u32 constants_used[(RSX_SPV_VP_CONSTANTS + 31u) / 32u];
    int constants_relative;
} rsx_spv_vp_info;

int  rsx_spv_vertex(const u32* ucode, const rsx_spv_vp_key* key, rsx_spv_blob* out, rsx_spv_vp_info* info);
int  rsx_spv_fragment(const u32* ucode, u32 max_words, const rsx_spv_fp_key* key, rsx_spv_blob* out, rsx_spv_fp_info* info);
int  rsx_spv_fragment_scan(const u32* ucode, u32 max_words, rsx_spv_fp_info* info);
int  rsx_spv_depth_pack(int d16, rsx_spv_blob* out);
void rsx_spv_blob_free(rsx_spv_blob* blob);

#ifdef __cplusplus
}
#endif

#endif
