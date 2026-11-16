#include "rsx_transfer.h"
#include "rsx_core.h"
#include "rsx_tiling.h"
#include "../../runtime/ppu/ppu_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NV3062_SURFACE_R5G6B5    4u
#define NV3062_SURFACE_A8R8G8B8  10u
#define NV3062_SURFACE_Y32       11u
#define NV3089_SCALE_A8R8G8B8    3u
#define NV3089_SCALE_X8R8G8B8    4u
#define NV3089_SCALE_R5G6B5      7u
#define NV3089_OP_SRCCOPY        3u
#define NV3089_ORIGIN_CENTER     1u
#define NV3089_INTERP_FOH        1u
#define NV3089_SURFACE2D         0x313371C3u
#define NV3089_SWIZZLE2D         0x31337A73u

static struct {
    u32 m2mf_in_dma, m2mf_out_dma, m2mf_in_off, m2mf_out_off;
    u32 m2mf_in_pitch, m2mf_out_pitch, m2mf_line_length, m2mf_line_count, m2mf_format;
    u32 surf_dst_dma, surf_format, surf_pitch, surf_dst_off;
    u32 swz_dma, swz_format, swz_off;
    u32 ifc_point, ifc_size_out;
    u32 sifm_dma, sifm_surface, sifm_format, sifm_operation;
    u32 sifm_clip_point, sifm_clip_size, sifm_out_point, sifm_out_size;
    u32 sifm_ds_dx, sifm_dt_dy, sifm_in_size, sifm_in_format, sifm_in_offset, sifm_in_point;
} x;

static u8*    s_scratch[3];
static size_t s_scratch_cap[3];

static u8* scratch(u32 slot, size_t n)
{
    if (s_scratch_cap[slot] < n) {
        u8* p = (u8*)realloc(s_scratch[slot], n);
        if (!p) return NULL;
        s_scratch[slot] = p;
        s_scratch_cap[slot] = n;
    }
    return s_scratch[slot];
}

static u8* tiled_ptr(void* user, u32 location, u32 offset, u32 len)
{
    (void)user;
    const u32 ea = rsx_address(location == RSX_LOCATION_MAIN ? RSX_DMA_MAIN : RSX_DMA_LOCAL, offset, len);
    return ea ? vm_base + ea : NULL;
}

static void tiled_store(void* user, u8* dst, const u8* src, u32 len)
{
    (void)user;
    rsx_mem_store((u32)(dst - vm_base), src, len);
}

static const rsx_tiled_mem s_tiled = { tiled_ptr, tiled_store, NULL };

typedef struct span {
    u32 offset;
    u32 ea;
    u32 size;
    int tiled;
} span;

static int span_open(span* sp, u32 dma, u32 offset, u32 size)
{
    sp->offset = offset;
    sp->size = size;
    sp->ea = rsx_address(dma, offset, size);
    if (!sp->ea) return 0;
    rsx_tile t;
    sp->tiled = dma == RSX_DMA_MAIN && rsx_tiling_active() && rsx_tiling_find(RSX_LOCATION_MAIN, offset, size, &t);
    return 1;
}

static const u8* span_read(const span* sp, u32 rel, u32 len, u8* tmp)
{
    if (!sp->tiled) return vm_base + sp->ea + rel;
    rsx_tiled_read(&s_tiled, RSX_LOCATION_MAIN, sp->offset + rel, tmp, len);
    return tmp;
}

static void span_write(const span* sp, u32 rel, const u8* src, u32 len)
{
    if (!sp->tiled) rsx_mem_store(sp->ea + rel, src, len);
    else rsx_tiled_write(&s_tiled, RSX_LOCATION_MAIN, sp->offset + rel, src, len);
}

static int spans_overlap(const span* a, const span* b)
{
    return a->ea < b->ea + b->size && b->ea < a->ea + a->size;
}

static void transfer_fault(const char* what, u32 a, u32 b)
{
    fprintf(stderr, "[rsx] transfer: %s (0x%08X, 0x%08X)\n", what, a, b);
}

static void m2mf_notify(void)
{
    const s32 in_pitch = (s32)x.m2mf_in_pitch, out_pitch = (s32)x.m2mf_out_pitch;
    const u32 cols = x.m2mf_line_length, rows = x.m2mf_line_count;
    const u32 in_stride = x.m2mf_format & 0xFFu, out_stride = (x.m2mf_format >> 8) & 0xFFu;
    if (!cols || !rows) return;
    if (in_stride < 1u || in_stride > 4u || out_stride < 1u || out_stride > 4u) {
        transfer_fault("NV0039 stride", in_stride, out_stride);
        return;
    }
    const u32 in_width = cols * in_stride, out_width = cols * out_stride;
    const s64 in_lo = in_pitch < 0 ? (s64)in_pitch * (rows - 1u) : 0;
    const s64 out_lo = out_pitch < 0 ? (s64)out_pitch * (rows - 1u) : 0;
    const u32 in_len = (u32)((u64)(in_pitch < 0 ? -(s64)in_pitch : in_pitch) * (rows - 1u) + in_width);
    const u32 out_len = (u32)((u64)(out_pitch < 0 ? -(s64)out_pitch : out_pitch) * (rows - 1u) + out_width);
    span src, dst;
    if (!span_open(&src, x.m2mf_in_dma, (u32)(x.m2mf_in_off + in_lo), in_len)) {
        transfer_fault("NV0039 source", x.m2mf_in_dma, x.m2mf_in_off);
        return;
    }
    if (!span_open(&dst, x.m2mf_out_dma, (u32)(x.m2mf_out_off + out_lo), out_len)) {
        transfer_fault("NV0039 destination", x.m2mf_out_dma, x.m2mf_out_off);
        return;
    }
    rsx_core_read_back(src.ea, src.size);
    const u32 in_base = (u32)(-in_lo), out_base = (u32)(-out_lo);
    if (in_stride == 1u && out_stride == 1u && !src.tiled && !dst.tiled &&
        (u32)in_pitch == in_width && (u32)out_pitch == out_width) {
        rsx_mem_store(dst.ea, vm_base + src.ea, in_len);
        rsx_core_wrote(dst.ea, dst.size);
        return;
    }
    u8* rows_in = NULL;
    if (spans_overlap(&src, &dst)) {
        rows_in = scratch(0, (size_t)in_width * rows);
        if (!rows_in) return;
        for (u32 r = 0; r < rows; r++)
            memcpy(rows_in + (size_t)r * in_width,
                   span_read(&src, in_base + (u32)((s64)in_pitch * r), in_width, scratch(1, in_width)),
                   in_width);
    }
    u8* tmp = scratch(1, in_width);
    u8* out = scratch(2, out_width);
    if (!tmp || !out) return;
    for (u32 r = 0; r < rows; r++) {
        const u8* row = rows_in ? rows_in + (size_t)r * in_width
                                : span_read(&src, in_base + (u32)((s64)in_pitch * r), in_width, tmp);
        const u32 drel = out_base + (u32)((s64)out_pitch * r);
        if (in_stride == 1u && out_stride == 1u) {
            span_write(&dst, drel, row, out_width);
            continue;
        }
        memcpy(out, span_read(&dst, drel, out_width, out), out_width);
        for (u32 c = 0; c < cols; c++) out[c * out_stride] = row[c * in_stride];
        span_write(&dst, drel, out, out_width);
    }
    rsx_core_wrote(dst.ea, dst.size);
}

static u16 argb_to_565(u32 argb)
{
    const u32 r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static u32 rgb565_to_argb(u16 p)
{
    const u32 r = (p >> 11) & 0x1Fu, g = (p >> 5) & 0x3Fu, b = p & 0x1Fu;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static u32 ifc_color(u32 index, u32 inc, const u32* be, u32 n)
{
    const u32 out_x_max = x.ifc_size_out & 0xFFFFu;
    if (index >= out_x_max) return 1;
    u32 count = n;
    if (inc) {
        if (count > 0x700u - index) count = 0x700u - index;
        if (count > out_x_max - index) count = out_x_max - index;
    }
    const u32 fmt = x.surf_format;
    const u32 bpp = fmt == NV3062_SURFACE_R5G6B5 ? 2u : 4u;
    if (fmt != NV3062_SURFACE_R5G6B5 && fmt != NV3062_SURFACE_A8R8G8B8 && fmt != NV3062_SURFACE_Y32) {
        transfer_fault("NV308A destination format", fmt, 0);
        return count;
    }
    const u32 px = (x.ifc_point & 0xFFFFu) + index, py = x.ifc_point >> 16;
    const u32 words = inc ? count : 1u;
    const u32* data = inc ? be : be + (count - 1u);
    const u32 off = x.surf_dst_off + px * bpp + (x.surf_pitch >> 16) * py;
    span dst;
    if (!span_open(&dst, x.surf_dst_dma, off, words * bpp)) {
        transfer_fault("NV308A destination", x.surf_dst_dma, off);
        return count;
    }
    if (bpp == 4u) {
        span_write(&dst, 0, (const u8*)data, words * 4u);
    } else {
        u8* out = scratch(2, (size_t)words * 2u);
        if (!out) return count;
        for (u32 i = 0; i < words; i++) {
            const u16 p = argb_to_565(__builtin_bswap32(data[i]));
            out[i * 2u] = (u8)(p >> 8);
            out[i * 2u + 1u] = (u8)p;
        }
        span_write(&dst, 0, out, words * 2u);
    }
    rsx_core_wrote(dst.ea, dst.size);
    return count;
}

static float sifm_scale(u32 v)
{
    if (!v) return 0.f;
    return 1048576.f / (float)(s32)v;
}

static u32 next_pow2(u32 v)
{
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}

static u32 swizzle_index(u32 px, u32 py, u32 log2w, u32 log2h)
{
    const u32 n = log2w < log2h ? log2w : log2h;
    u32 off = 0;
    for (u32 i = 0; i < n; i++)
        off |= ((px >> i) & 1u) << (2u * i) | ((py >> i) & 1u) << (2u * i + 1u);
    if (log2w > log2h) off |= (px >> n) << (2u * n);
    else if (log2h > log2w) off |= (py >> n) << (2u * n);
    return off;
}

static u32 log2u(u32 v)
{
    u32 l = 0;
    while ((1u << l) < v) l++;
    return l;
}

typedef struct sifm_src {
    const u8* rows;
    u32 pitch;
    u32 width;
    u32 height;
    u32 bpp;
    int flip_x;
    int flip_y;
} sifm_src;

static u32 sifm_texel(const sifm_src* sr, u32 sx, u32 sy)
{
    if (sr->flip_x) sx = sr->width - 1u - sx;
    if (sr->flip_y) sy = sr->height - 1u - sy;
    const u8* p = sr->rows + (size_t)sy * sr->pitch + (size_t)sx * sr->bpp;
    if (sr->bpp == 2u) return rgb565_to_argb((u16)(p[0] << 8 | p[1]));
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u32 sifm_sample(const sifm_src* sr, float fx, float fy, int bilinear)
{
    if (!bilinear) {
        u32 sx = fx < 0.f ? 0u : (u32)fx, sy = fy < 0.f ? 0u : (u32)fy;
        if (sx >= sr->width) sx = sr->width - 1u;
        if (sy >= sr->height) sy = sr->height - 1u;
        return sifm_texel(sr, sx, sy);
    }
    fx -= 0.5f;
    fy -= 0.5f;
    if (fx < 0.f) fx = 0.f;
    if (fy < 0.f) fy = 0.f;
    u32 x0 = (u32)fx, y0 = (u32)fy;
    if (x0 >= sr->width) x0 = sr->width - 1u;
    if (y0 >= sr->height) y0 = sr->height - 1u;
    const u32 x1 = x0 + 1u < sr->width ? x0 + 1u : x0, y1 = y0 + 1u < sr->height ? y0 + 1u : y0;
    const float wx = fx - (float)x0 > 1.f ? 1.f : fx - (float)x0, wy = fy - (float)y0 > 1.f ? 1.f : fy - (float)y0;
    const u32 c00 = sifm_texel(sr, x0, y0), c10 = sifm_texel(sr, x1, y0);
    const u32 c01 = sifm_texel(sr, x0, y1), c11 = sifm_texel(sr, x1, y1);
    u32 out = 0;
    for (u32 sh = 0; sh < 32u; sh += 8u) {
        const float a = (float)((c00 >> sh) & 0xFFu) * (1.f - wx) + (float)((c10 >> sh) & 0xFFu) * wx;
        const float b = (float)((c01 >> sh) & 0xFFu) * (1.f - wx) + (float)((c11 >> sh) & 0xFFu) * wx;
        out |= (u32)(a * (1.f - wy) + b * wy + 0.5f) << sh;
    }
    return out;
}

static void sifm_put(u8* dst, u32 bpp, u32 argb)
{
    if (bpp == 2u) {
        const u16 p = argb_to_565(argb);
        dst[0] = (u8)(p >> 8);
        dst[1] = (u8)p;
        return;
    }
    dst[0] = (u8)(argb >> 24);
    dst[1] = (u8)(argb >> 16);
    dst[2] = (u8)(argb >> 8);
    dst[3] = (u8)argb;
}

static void sifm_image_in(void)
{
    if (x.sifm_operation != NV3089_OP_SRCCOPY) {
        transfer_fault("NV3089 operation", x.sifm_operation, 0);
        return;
    }
    const u32 out_x = x.sifm_out_point & 0xFFFFu, out_y = x.sifm_out_point >> 16;
    const u32 out_w = x.sifm_out_size & 0xFFFFu, out_h = x.sifm_out_size >> 16;
    const u32 in_w = x.sifm_in_size & 0xFFFFu, in_h = x.sifm_in_size >> 16;
    const float scale_x = sifm_scale(x.sifm_ds_dx), scale_y = sifm_scale(x.sifm_dt_dy);
    u32 clip_w = x.sifm_clip_size & 0xFFFFu, clip_h = x.sifm_clip_size >> 16;
    if (clip_w > out_w) clip_w = out_w;
    if (clip_h > out_h) clip_h = out_h;
    if (!clip_w || !clip_h) return;
    if (!in_w || !in_h) {
        transfer_fault("NV3089 input size", x.sifm_in_size, 0);
        return;
    }
    u32 clip_x = x.sifm_clip_point & 0xFFFFu, clip_y = x.sifm_clip_point >> 16;
    if (clip_x && out_x + clip_x + clip_w > out_w) clip_x = 0;
    if (clip_y && out_y + clip_y + clip_h > out_h) clip_y = 0;

    u32 src_fmt = x.sifm_format;
    if (src_fmt == NV3089_SCALE_X8R8G8B8) src_fmt = NV3089_SCALE_A8R8G8B8;
    if (src_fmt != NV3089_SCALE_A8R8G8B8 && src_fmt != NV3089_SCALE_R5G6B5) {
        transfer_fault("NV3089 source format", x.sifm_format, 0);
        return;
    }
    const int swizzled = x.sifm_surface == NV3089_SWIZZLE2D;
    if (!swizzled && x.sifm_surface != NV3089_SURFACE2D) {
        transfer_fault("NV3089 context surface", x.sifm_surface, 0);
        return;
    }
    const u32 dst_dma = swizzled ? x.swz_dma : x.surf_dst_dma;
    const u32 dst_off = swizzled ? x.swz_off : x.surf_dst_off;
    const u32 dst_fmt = swizzled ? (x.swz_format & 0xFFFFu) : x.surf_format;
    if (dst_fmt != NV3062_SURFACE_R5G6B5 && dst_fmt != NV3062_SURFACE_A8R8G8B8) {
        transfer_fault("NV3089 destination format", dst_fmt, swizzled);
        return;
    }
    const u32 in_bpp = src_fmt == NV3089_SCALE_R5G6B5 ? 2u : 4u;
    const u32 out_bpp = dst_fmt == NV3062_SURFACE_R5G6B5 ? 2u : 4u;
    u32 out_pitch = swizzled ? 0u : x.surf_pitch >> 16;
    u32 in_pitch = x.sifm_in_format & 0xFFFFu;
    if (!out_pitch) out_pitch = out_bpp * out_w;
    if (!in_pitch) in_pitch = in_bpp * in_w;
    const int center = ((x.sifm_in_format >> 16) & 0xFFu) == NV3089_ORIGIN_CENTER;
    const int bilinear = ((x.sifm_in_format >> 24) & 0xFFu) == NV3089_INTERP_FOH;

    float fx = (float)(x.sifm_in_point & 0xFFFFu) / 16.f, fy = (float)(x.sifm_in_point >> 16) / 16.f;
    if (center) {
        fx = fx < 0.5f ? 0.5f : fx;
        fy = fy < 0.5f ? 0.5f : fy;
        fx -= 0.5f;
        fy -= 0.5f;
    }
    u32 in_x = (u32)floorf(fx), in_y = (u32)floorf(fy);
    if (scale_x > 0.f && scale_x < 1.f) in_x = (u32)(floorf((float)in_x * scale_x) / scale_x);
    if (scale_y > 0.f && scale_y < 1.f) in_y = (u32)(floorf((float)in_y * scale_y) / scale_y);
    const float ax = fabsf(scale_x), ay = fabsf(scale_y);
    const u32 conv_w = (u32)(ax * (float)in_w), conv_h = (u32)(ay * (float)in_h);
    if (!conv_w || !conv_h) {
        transfer_fault("NV3089 scale", x.sifm_ds_dx, x.sifm_dt_dy);
        return;
    }

    span src, dst;
    const u32 src_len = in_pitch * (in_h - 1u) + in_w * in_bpp;
    if (!span_open(&src, x.sifm_dma, x.sifm_in_offset + in_x * in_bpp + in_pitch * in_y, src_len)) {
        transfer_fault("NV3089 source", x.sifm_dma, x.sifm_in_offset);
        return;
    }
    const u32 sw_w = next_pow2(out_w), sw_h = next_pow2(out_h);
    const u32 dst_len = swizzled ? sw_w * sw_h * out_bpp : out_pitch * (clip_h - 1u) + clip_w * out_bpp;
    if (!span_open(&dst, dst_dma, dst_off + out_x * out_bpp + out_pitch * out_y, dst_len)) {
        transfer_fault("NV3089 destination", dst_dma, dst_off);
        return;
    }
    if (src.ea == dst.ea && in_w == clip_w && in_h == clip_h && in_pitch == out_pitch &&
        scale_x == 1.f && scale_y == 1.f)
        return;
    rsx_core_read_back(src.ea, src.size);

    u8* rows = scratch(0, (size_t)in_pitch * (in_h - 1u) + (size_t)in_w * in_bpp);
    if (!rows) return;
    if (src.tiled) {
        for (u32 r = 0; r < in_h; r++)
            span_read(&src, r * in_pitch, in_w * in_bpp, rows + (size_t)r * in_pitch);
    } else {
        memcpy(rows, vm_base + src.ea, src_len);
    }
    const sifm_src sr = { rows, in_pitch, in_w, in_h, in_bpp, scale_x < 0.f, scale_y < 0.f };
    const int direct = !bilinear && ax == 1.f && ay == 1.f && in_bpp == out_bpp && !sr.flip_x && !sr.flip_y;

    const u32 img_w = swizzled ? (clip_w < out_w ? clip_w : out_w) : clip_w;
    const u32 img_h = swizzled ? (clip_h < out_h ? clip_h : out_h) : clip_h;
    const u32 line = (swizzled ? sw_w : clip_w) * out_bpp;
    u8* img = scratch(1, (size_t)line * (swizzled ? sw_h : 1u));
    if (!img) return;
    if (swizzled) memset(img, 0, (size_t)line * sw_h);

    for (u32 j = 0; j < img_h; j++) {
        u8* row = swizzled ? img + (size_t)j * line : img;
        const u32 cy = clip_y + j;
        for (u32 i = 0; i < img_w; i++) {
            const u32 cx = clip_x + i;
            u8* px = row + (size_t)i * out_bpp;
            if (cx >= conv_w || cy >= conv_h) {
                memset(px, 0, out_bpp);
                continue;
            }
            if (direct) {
                memcpy(px, rows + (size_t)cy * in_pitch + (size_t)cx * in_bpp, out_bpp);
                continue;
            }
            sifm_put(px, out_bpp, sifm_sample(&sr, ((float)cx + 0.5f) / ax, ((float)cy + 0.5f) / ay, bilinear));
        }
        if (!swizzled) span_write(&dst, j * out_pitch, row, clip_w * out_bpp);
    }

    if (swizzled) {
        u8* out = scratch(2, (size_t)sw_w * sw_h * out_bpp);
        if (!out) return;
        const u32 lw = log2u(sw_w), lh = log2u(sw_h);
        for (u32 py = 0; py < sw_h; py++)
            for (u32 pxi = 0; pxi < sw_w; pxi++)
                memcpy(out + (size_t)swizzle_index(pxi, py, lw, lh) * out_bpp,
                       img + (size_t)py * line + (size_t)pxi * out_bpp, out_bpp);
        span_write(&dst, 0, out, sw_w * sw_h * out_bpp);
    }
    rsx_core_wrote(dst.ea, dst.size);
}

void rsx_transfer_reset(void)
{
    memset(&x, 0, sizeof x);
    x.m2mf_in_dma = x.m2mf_out_dma = x.surf_dst_dma = x.swz_dma = x.sifm_dma = RSX_DMA_LOCAL;
}

u32 rsx_transfer_methods(u32 reg, u32 inc, const u32* be, u32 n)
{
    if (reg >= 0xA400u && reg < 0xC000u) return ifc_color((reg - 0xA400u) >> 2, inc, be, n);
    const u32 v = __builtin_bswap32(be[0]);
    switch (reg) {
    case 0x2184u: x.m2mf_in_dma = v; break;
    case 0x2188u: x.m2mf_out_dma = v; break;
    case 0x230Cu: x.m2mf_in_off = v; break;
    case 0x2310u: x.m2mf_out_off = v; break;
    case 0x2314u: x.m2mf_in_pitch = v; break;
    case 0x2318u: x.m2mf_out_pitch = v; break;
    case 0x231Cu: x.m2mf_line_length = v; break;
    case 0x2320u: x.m2mf_line_count = v; break;
    case 0x2324u: x.m2mf_format = v; break;
    case 0x2328u: m2mf_notify(); break;
    case 0x6188u: x.surf_dst_dma = v; break;
    case 0x6300u: x.surf_format = v; break;
    case 0x6304u: x.surf_pitch = v; break;
    case 0x630Cu: x.surf_dst_off = v; break;
    case 0x8184u: x.swz_dma = v; break;
    case 0x8300u: x.swz_format = v; break;
    case 0x8304u: x.swz_off = v; break;
    case 0xA304u: x.ifc_point = v; break;
    case 0xA308u: x.ifc_size_out = v; break;
    case 0xC184u: x.sifm_dma = v; break;
    case 0xC198u: x.sifm_surface = v; break;
    case 0xC300u: x.sifm_format = v; break;
    case 0xC304u: x.sifm_operation = v; break;
    case 0xC308u: x.sifm_clip_point = v; break;
    case 0xC30Cu: x.sifm_clip_size = v; break;
    case 0xC310u: x.sifm_out_point = v; break;
    case 0xC314u: x.sifm_out_size = v; break;
    case 0xC318u: x.sifm_ds_dx = v; break;
    case 0xC31Cu: x.sifm_dt_dy = v; break;
    case 0xC400u: x.sifm_in_size = v; break;
    case 0xC404u: x.sifm_in_format = v; break;
    case 0xC408u: x.sifm_in_offset = v; break;
    case 0xC40Cu: x.sifm_in_point = v; sifm_image_in(); break;
    default: break;
    }
    return 1;
}
