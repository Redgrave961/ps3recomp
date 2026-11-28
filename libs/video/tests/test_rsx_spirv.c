#define _CRT_SECURE_NO_WARNINGS
#include "rsx_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
#endif

static const char* g_outdir = ".";
static const char* g_val = NULL;
static int g_pass, g_fail, g_modules, g_valid, g_invalid;
static double g_vp_us, g_fp_us;
static int g_vp_n, g_fp_n;
static double g_vp_max, g_fp_max;
static char g_vp_max_name[64], g_fp_max_name[64];

static void check(const char* name, int cond)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static void validate(const char* name, const rsx_spv_blob* b)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", g_outdir, name);
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("[FAIL] cannot write %s\n", path);
        g_fail++;
        return;
    }
    fwrite(b->words, 4, b->count, f);
    fclose(f);
    g_modules++;
    if (!g_val) return;
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"\"%s\" --target-env vulkan1.3 \"%s\"\"", g_val, path);
    int rc = system(cmd);
    if (rc == 0) {
        g_valid++;
    } else {
        g_invalid++;
        printf("[INVALID] %s\n", name);
    }
}

typedef struct vop {
    u32 vec, sca;
    u32 src[3];
    u32 input, cnst;
    int idx_const, idx_input;
    u32 tmp, stmp, out;
    u32 vmask, smask;
    int vec_result;
    u32 cond, ctest, ccswz, ccsel, upd, sat;
    u32 asel, aswz;
    u32 abs;
    int has_target;
    u32 target, bidx, btrue, tex;
    int end;
} vop;

#define SW(x, y, z, w) ((x) | ((y) << 2) | ((z) << 4) | ((w) << 6))
#define SWI SW(0, 1, 2, 3)
#define M_X 1u
#define M_Y 2u
#define M_Z 4u
#define M_W 8u
#define M_ALL 15u

static u32 vsrc(u32 type, u32 reg, u32 swz, u32 neg)
{
    return (type & 3u) | ((reg & 63u) << 2) | (((swz >> 6) & 3u) << 8) | (((swz >> 4) & 3u) << 10) |
           (((swz >> 2) & 3u) << 12) | ((swz & 3u) << 14) | ((neg & 1u) << 16);
}

#define VT(r) vsrc(1, (r), SWI, 0)
#define VI(swz) vsrc(2, 0, (swz), 0)
#define VC(swz) vsrc(3, 0, (swz), 0)

static vop vdef(void)
{
    vop o;
    memset(&o, 0, sizeof o);
    o.src[0] = o.src[1] = o.src[2] = VT(0);
    o.tmp = 0x3F;
    o.stmp = 0x3F;
    o.out = 0x1F;
    o.cond = 7;
    o.ccswz = SWI;
    return o;
}

static void vasm(u32* w, const vop* o)
{
    u32 m0 = 0, m3 = 0;
    u32 cx = o->ccswz & 3u, cy = (o->ccswz >> 2) & 3u, cz = (o->ccswz >> 4) & 3u, cw = (o->ccswz >> 6) & 3u;
    m0 = (o->aswz & 3u) | (cw << 2) | (cz << 4) | (cy << 6) | (cx << 8) | ((o->cond & 7u) << 10) | ((o->ctest & 1u) << 13) |
         ((o->upd & 1u) << 14) | ((o->tmp & 63u) << 15) | ((o->abs & 7u) << 21) | ((o->asel & 1u) << 24) |
         ((o->ccsel & 1u) << 25) | ((o->sat & 1u) << 26) | ((u32)(o->idx_input & 1) << 27) | ((o->upd & 1u) << 29) |
         ((u32)(o->vec_result & 1) << 30);
    u32 d1 = ((o->src[0] >> 9) & 0xFFu) | ((o->input & 15u) << 8) | ((o->cnst & 0x3FFu) << 12) | ((o->vec & 31u) << 22) |
             ((o->sca & 31u) << 27);
    u32 d2 = ((o->src[2] >> 11) & 0x3Fu) | ((o->src[1] & 0x1FFFFu) << 6) | ((o->src[0] & 0x1FFu) << 23);
    if (o->vec == 0x19) d2 = (d2 & ~(3u << 8)) | ((o->tex & 3u) << 8);
    m3 = ((u32)(o->end & 1)) | ((u32)(o->idx_const & 1) << 1) | ((o->out & 31u) << 2) | ((o->stmp & 63u) << 7) |
         (((o->vmask >> 3) & 1u) << 13) | (((o->vmask >> 2) & 1u) << 14) | (((o->vmask >> 1) & 1u) << 15) | ((o->vmask & 1u) << 16) |
         (((o->smask >> 3) & 1u) << 17) | (((o->smask >> 2) & 1u) << 18) | (((o->smask >> 1) & 1u) << 19) | ((o->smask & 1u) << 20) |
         ((o->src[2] & 0x7FFu) << 21);
    if (o->has_target) {
        m0 = (m0 & ~(1u << 23)) | (((o->target >> 9) & 1u) << 23);
        d2 = (d2 & ~0x3Fu) | ((o->target >> 3) & 0x3Fu);
        m3 = (m3 & ~(7u << 29)) | ((o->target & 7u) << 29);
        m3 = (m3 & ~(0x1Fu << 23)) | ((o->bidx & 31u) << 23);
        m3 = (m3 & ~(1u << 28)) | ((o->btrue & 1u) << 28);
    }
    w[0] = m0;
    w[1] = d1;
    w[2] = d2;
    w[3] = m3;
}

static u32 g_vp[RSX_SPV_VP_SLOTS * 4];

static void vp_clear(void) { memset(g_vp, 0, sizeof g_vp); }
static void vp_put(u32 slot, const vop* o) { vasm(&g_vp[slot * 4], o); }

static vop v_mov_out(u32 out, u32 src, u32 mask)
{
    vop o = vdef();
    o.vec = 1;
    o.src[0] = src;
    o.out = out;
    o.vec_result = 1;
    o.vmask = mask;
    return o;
}

static vop v_op_tmp(u32 vec, u32 tmp, u32 s0, u32 s1, u32 s2, u32 mask)
{
    vop o = vdef();
    o.vec = vec;
    o.src[0] = s0;
    o.src[1] = s1;
    o.src[2] = s2;
    o.tmp = tmp;
    o.vmask = mask;
    return o;
}

static vop v_sca_tmp(u32 sca, u32 stmp, u32 s2, u32 mask)
{
    vop o = vdef();
    o.sca = sca;
    o.src[2] = s2;
    o.stmp = stmp;
    o.smask = mask;
    o.vec_result = 1;
    return o;
}

static vop v_branch(u32 sca, u32 target, u32 cond)
{
    vop o = vdef();
    o.sca = sca;
    o.has_target = 1;
    o.target = target;
    o.cond = cond;
    o.vec_result = 1;
    return o;
}

static void vp_transform(u32 at, int end_after_pos)
{
    for (u32 r = 0; r < 4; r++) {
        vop o = vdef();
        o.vec = 7;
        o.src[0] = VI(SWI);
        o.input = 0;
        o.src[1] = VC(SWI);
        o.cnst = r;
        o.out = 0;
        o.vec_result = 1;
        o.vmask = 1u << r;
        o.end = end_after_pos && r == 3;
        vp_put(at + r, &o);
    }
}

static void run_vp(const char* name, u32 start, u32 out_mask, u32 tex_dims)
{
    rsx_spv_vp_key key;
    key.start = start;
    key.output_mask = out_mask;
    key.texture_dims = tex_dims;
    rsx_spv_blob b = { 0 };
    rsx_spv_vp_info info;
    int rc = rsx_spv_vertex(g_vp, &key, &b, &info);
    char cname[160];
    snprintf(cname, sizeof cname, "vp_%s translate", name);
    check(cname, rc == 0);
    if (rc) return;
    int iters = 200;
    double t0 = now_us();
    for (int k = 0; k < iters; k++) rsx_spv_vertex(g_vp, &key, &b, NULL);
    double dt = (now_us() - t0) / iters;
    g_vp_us += dt;
    g_vp_n++;
    if (dt > g_vp_max) {
        g_vp_max = dt;
        snprintf(g_vp_max_name, sizeof g_vp_max_name, "%s", name);
    }
    char vname[160];
    snprintf(vname, sizeof vname, "vp_%s", name);
    validate(vname, &b);
    printf("  vp %-28s %4u instr  %6u words  %7.2f us\n", name, info.instructions, b.count, dt);
    rsx_spv_blob_free(&b);
}

typedef struct fop {
    u32 op;
    u32 dst, fp16, setc, mask, attr, tex, bx2, prec, nodst, sat, end;
    u32 s[3];
    u32 exec, cswz, cmod, creg;
    u32 abs0, abs1, abs2;
    u32 sp[3];
    u32 scale;
    u32 addr, useidx, persp;
    u32 raw1, raw2;
    int use_raw;
} fop;

static u32 fsrc(u32 type, u32 reg, u32 fp16, u32 swz, u32 neg)
{
    return (type & 3u) | ((reg & 63u) << 2) | ((fp16 & 1u) << 8) | ((swz & 0xFFu) << 9) | ((neg & 1u) << 17);
}

#define FR(r) fsrc(0, (r), 0, SWI, 0)
#define FH(r) fsrc(0, (r), 1, SWI, 0)
#define FIN fsrc(1, 0, 0, SWI, 0)
#define FK fsrc(2, 0, 0, SWI, 0)

static fop fdef(u32 op)
{
    fop o;
    memset(&o, 0, sizeof o);
    o.op = op;
    o.mask = M_ALL;
    o.exec = 7;
    o.cswz = SWI;
    o.s[0] = o.s[1] = o.s[2] = FR(0);
    return o;
}

static u32 hsw(u32 x) { return ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8); }

static u32 g_fp[8192];
static u32 g_fpn;

static void fp_begin(void)
{
    memset(g_fp, 0, sizeof g_fp);
    g_fpn = 0;
}

static u32 fp_put(const fop* o)
{
    u32 d = (o->end & 1u) | ((o->dst & 63u) << 1) | ((o->fp16 & 1u) << 7) | ((o->setc & 1u) << 8) | ((o->mask & 15u) << 9) |
            ((o->attr & 15u) << 13) | ((o->tex & 15u) << 17) | ((o->bx2 & 1u) << 21) | ((o->prec & 3u) << 22) |
            ((o->op & 63u) << 24) | ((o->nodst & 1u) << 30) | ((o->sat & 1u) << 31);
    u32 s0 = o->s[0] | ((o->exec & 7u) << 18) | ((o->cswz & 0xFFu) << 21) | ((o->abs0 & 1u) << 29) | ((o->cmod & 1u) << 30) |
             ((o->creg & 1u) << 31);
    u32 s1 = o->s[1] | ((o->abs1 & 1u) << 18) | ((o->sp[0] & 7u) << 19) | ((o->sp[1] & 7u) << 22) | ((o->sp[2] & 7u) << 25) |
             ((o->scale & 7u) << 28) | (((o->op >> 6) & 1u) << 31);
    u32 s2 = o->s[2] | ((o->abs2 & 1u) << 18) | ((o->addr & 0x7FFu) << 19) | ((o->useidx & 1u) << 30) | ((o->persp & 1u) << 31);
    if (o->use_raw) {
        s1 = (o->raw1 & 0x7FFFFFFFu) | (((o->op >> 6) & 1u) << 31);
        s2 = o->raw2;
    }
    u32 at = g_fpn;
    g_fp[g_fpn * 4 + 0] = hsw(d);
    g_fp[g_fpn * 4 + 1] = hsw(s0);
    g_fp[g_fpn * 4 + 2] = hsw(s1);
    g_fp[g_fpn * 4 + 3] = hsw(s2);
    g_fpn++;
    return at;
}

static void fp_const(float a, float b, float c, float d)
{
    float v[4] = { a, b, c, d };
    for (u32 k = 0; k < 4; k++) {
        u32 w;
        memcpy(&w, &v[k], 4);
        g_fp[g_fpn * 4 + k] = hsw(w);
    }
    g_fpn++;
}

static void run_fp_ex(const char* name, u32 control, u32 dims, u32 shadow, u32 outmask, rsx_spv_fp_info* pinfo)
{
    rsx_spv_fp_key key;
    key.control = control;
    key.texture_dims = dims;
    key.texture_shadow = shadow;
    key.output_mask = outmask;
    rsx_spv_blob b = { 0 };
    rsx_spv_fp_info info;
    u32 words = g_fpn * 4u + 64u;
    int rc = rsx_spv_fragment(g_fp, words, &key, &b, &info);
    char cname[160];
    snprintf(cname, sizeof cname, "fp_%s translate", name);
    check(cname, rc == 0);
    if (rc) return;
    rsx_spv_fp_info sinfo;
    int src = rsx_spv_fragment_scan(g_fp, words, &sinfo);
    snprintf(cname, sizeof cname, "fp_%s scan matches", name);
    check(cname, src == 0 && sinfo.constant_count == info.constant_count && sinfo.instructions == info.instructions &&
                     sinfo.texture_mask == info.texture_mask && sinfo.input_mask == info.input_mask &&
                     !memcmp(sinfo.constant_word, info.constant_word, info.constant_count * 4u));
    int iters = 200;
    double t0 = now_us();
    for (int k = 0; k < iters; k++) rsx_spv_fragment(g_fp, words, &key, &b, NULL);
    double dt = (now_us() - t0) / iters;
    g_fp_us += dt;
    g_fp_n++;
    if (dt > g_fp_max) {
        g_fp_max = dt;
        snprintf(g_fp_max_name, sizeof g_fp_max_name, "%s", name);
    }
    char vname[160];
    snprintf(vname, sizeof vname, "fp_%s", name);
    validate(vname, &b);
    printf("  fp %-28s %4u instr %3u const  %6u words  %7.2f us\n", name, info.instructions, info.constant_count, b.count, dt);
    if (pinfo) *pinfo = info;
    rsx_spv_blob_free(&b);
}

static void run_fp(const char* name, u32 control) { run_fp_ex(name, control, 0x55555555u, 0, 1, NULL); }

static void fp_from_host(const u32* host, u32 nwords)
{
    fp_begin();
    for (u32 k = 0; k < nwords; k++) {
        u32 be = (host[k] << 16) | (host[k] >> 16);
        u8 b[4] = { (u8)(be >> 24), (u8)(be >> 16), (u8)(be >> 8), (u8)be };
        memcpy(&g_fp[k], b, 4);
    }
    g_fpn = (nwords + 3u) / 4u;
}

#define T_OPC(x) ((u32)(x) << 24)
#define T_ALL (0xFu << 9)
#define T_END 1u
#define T_OUT(i) ((u32)(i) << 1)
#define T_HALF (1u << 7)
#define T_IN(x) ((u32)(x) << 13)
#define T_TEXU(x) ((u32)(x) << 17)
#define T_SWZ ((0u << 9) | (1u << 11) | (2u << 13) | (3u << 15))
#define T_REG(i) ((u32)(i) << 2)
#define T_EXEC ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define F_BITS(f) f_bits(f)

static u32 f_bits(float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    return w;
}

static void toolkit_samples(void)
{
    printf("toolkit fragment samples:\n");
    {
        const u32 p[] = { T_OPC(0x02) | (3u << 9) | T_OUT(1), T_REG(0) | T_SWZ | T_EXEC, T_REG(0) | T_SWZ, 0,
                          T_OPC(0x01) | T_ALL | T_IN(1) | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 8);
        run_fp("tk_debug_mul_mov", 0x40);
    }
    {
        const u32 p[] = { T_OPC(0x01) | T_ALL | T_IN(1) | T_HALF | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 4);
        run_fp("tk_h0_16bit", 0x00);
        run_fp("tk_h0_32bit", 0x40);
    }
    {
        const u32 p[] = { T_OPC(0x01) | T_ALL | T_IN(1) | T_OUT(0), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x01) | T_ALL | T_IN(2) | T_OUT(2) | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 8);
        run_fp_ex("tk_mrt_32", 0x40, 0x55555555u, 0, 0x3, NULL);
        run_fp_ex("tk_mrt_16", 0x00, 0x55555555u, 0, 0x1, NULL);
    }
    {
        const u32 p[] = { T_OPC(0x01) | T_ALL | T_IN(1) | T_OUT(3) | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 4);
        run_fp_ex("tk_r3_export", 0x40, 0x55555555u, 0, 0x7, NULL);
    }
    {
        const u32 p[] = { T_OPC(0x02), (1u << 20), (1u << 31) | (32u >> 2), 48u >> 2,
                          T_OPC(0x01) | T_ALL | T_IN(1), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x01) | T_ALL | T_IN(2), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x3C) | T_ALL | T_OUT(1) | T_END, T_REG(0) | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 16);
        run_fp("tk_ife_else_lif", 0x40);
    }
    {
        u32 p[] = { T_OPC(0x17) | T_ALL | T_IN(4) | T_TEXU(0) | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 4);
        run_fp_ex("tk_tex_3d", 0x40, 3u, 0, 1, NULL);
        p[0] = T_OPC(0x18) | T_ALL | T_IN(4) | T_TEXU(0) | T_END;
        fp_from_host(p, 4);
        run_fp_ex("tk_txp_3d", 0x40, 3u, 0, 1, NULL);
    }
    {
        static u32 p[256 * 4];
        for (u32 i = 0; i < 256; i++) {
            p[i * 4 + 0] = (i == 0 ? T_OPC(0x01) : T_OPC(0x04)) | T_ALL | T_IN(1) | (1u << 8) | (i == 255 ? T_END : 0u);
            p[i * 4 + 1] = 1u | T_SWZ | (i == 0 ? T_EXEC : (1u << 20));
            p[i * 4 + 2] = T_REG(0) | T_SWZ;
            p[i * 4 + 3] = T_REG(1) | T_SWZ;
        }
        fp_from_host(p, 256 * 4);
        run_fp("tk_256_predicated_cc", 0x40);
    }
    {
        const u32 p[] = { T_OPC(0x01) | T_ALL | T_IN(1) | T_END, 1u | T_SWZ | (1u << 20) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27), 0, 0 };
        fp_from_host(p, 4);
        run_fp("tk_predicated_select", 0x40);
    }
    {
        const u32 p[] = { T_OPC(0x01) | T_ALL | T_OUT(1) | T_IN(4), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x01) | T_ALL | T_IN(1) | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 8);
        run_fp("tk_depth_export", 0x40 | 0x0E);
        run_fp("tk_no_depth_export", 0x40);
    }
    {
        u32 p[] = { T_OPC(0x17) | T_ALL | T_IN(4) | T_TEXU(0) | (1u << 21) | T_END, 1u | T_SWZ | T_EXEC, 0, 0 };
        fp_from_host(p, 4);
        run_fp("tk_tex_bx2", 0x40);
        p[0] = T_OPC(0x17) | T_ALL | T_IN(6) | T_TEXU(0) | T_END;
        p[3] = 1u << 31;
        fp_from_host(p, 4);
        run_fp("tk_tex_persp_tc2", 0x40);
    }
    {
        const u32 p[] = { T_OPC(0x01) | T_ALL | T_IN(0), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x17) | T_ALL | T_OUT(1) | T_IN(6) | T_TEXU(0) | T_END, 1u | T_SWZ | T_EXEC, 0, 1u << 31 };
        fp_from_host(p, 8);
        run_fp("tk_wpos_tex_persp", 0x40);
    }
    for (u32 slot = 0; slot < 3; slot++) {
        for (u32 v = 0; v < 8; v++) {
            const u32 p[] = { T_OPC(0x04) | T_ALL | T_OUT(1) | T_END, T_REG(0) | T_SWZ | T_EXEC, T_REG(0) | T_SWZ | (v << (19 + 3 * slot)), T_REG(0) | T_SWZ };
            fp_from_host(p, 4);
            char nm[64];
            snprintf(nm, sizeof nm, "tk_prec_src%u_%u", slot, v);
            run_fp(nm, 0x40);
        }
    }
    {
        const u32 p[] = { T_OPC(0x02) | T_ALL | T_OUT(1) | T_END, T_REG(0) | T_SWZ | T_EXEC | (1u << 29) | (1u << 17), T_REG(0) | T_SWZ | (3u << 19), 0 };
        fp_from_host(p, 4);
        run_fp("tk_abs_clamp_negate", 0x40);
    }
    {
        const u32 p[] = { T_OPC(0x17) | T_ALL | T_OUT(1) | T_IN(4) | T_TEXU(0), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x17) | T_ALL | T_OUT(5) | T_IN(5) | T_TEXU(1), 1u | T_SWZ | T_EXEC, 0, 0,
                          T_OPC(0x03) | T_ALL | T_OUT(5), T_REG(5) | T_SWZ | T_EXEC, 2u | T_SWZ | (1u << 17), 0,
                          F_BITS(0.25f), F_BITS(0.25f), F_BITS(0.25f), F_BITS(0.25f),
                          T_OPC(0x02) | T_ALL | T_OUT(0) | T_END, T_REG(1) | T_SWZ | T_EXEC, T_REG(5) | T_SWZ | (1u << 18) | (3u << 22), 0 };
        fp_from_host(p, 20);
        rsx_spv_fp_info info;
        run_fp_ex("tk_soft_particle", 0x40, 0x55555555u, 0, 1, &info);
        check("soft particle constant at word 12", info.constant_count == 1 && info.constant_word[0] == 12 && info.ucode_words == 20);
    }
}

static void vertex_tests(void)
{
    printf("vertex programs:\n");

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = v_mov_out(1, VI(SWI), M_ALL);
        o.input = 3;
        vp_put(4, &o);
        o = v_mov_out(7, VI(SWI), M_ALL);
        o.input = 8;
        o.end = 1;
        vp_put(5, &o);
    }
    run_vp("transform", 0, RSX_SPV_VPOUT_FRONTDIFFUSE | RSX_SPV_VPOUT_TEX0, 0);

    vp_clear();
    vp_transform(100, 0);
    {
        vop o = v_mov_out(1, VI(SWI), M_ALL);
        o.input = 3;
        o.end = 1;
        vp_put(104, &o);
    }
    run_vp("transform_offset_start", 100, RSX_SPV_VPOUT_FRONTDIFFUSE, 0);

    vp_clear();
    {
        u32 pc = 0;
        static const u32 vecs[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22 };
        vop o = v_op_tmp(1, 1, VI(SWI), VC(SWI), VT(2), M_ALL);
        o.cnst = 5;
        vp_put(pc++, &o);
        for (u32 k = 0; k < sizeof vecs / sizeof vecs[0]; k++) {
            o = v_op_tmp(vecs[k], 2 + (k % 8), VT(1), vsrc(1, 2, SW(3, 2, 1, 0), 1), VC(SW(0, 0, 0, 0)), M_ALL);
            o.cnst = 7 + k;
            o.abs = k & 7u;
            o.sat = k & 1u;
            vp_put(pc++, &o);
        }
        vp_transform(pc, 0);
        pc += 4;
        o = v_mov_out(7, VT(9), M_X | M_Y);
        o.end = 1;
        vp_put(pc, &o);
    }
    run_vp("all_vec_ops", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    {
        u32 pc = 0;
        static const u32 scas[] = { 1, 2, 3, 4, 5, 6, 7, 13, 14, 15, 16, 19, 20 };
        for (u32 k = 0; k < sizeof scas / sizeof scas[0]; k++) {
            vop o = v_sca_tmp(scas[k], 3 + k, vsrc(2, 0, SW(k & 3u, 1, 2, 3), k & 1u), M_ALL);
            o.input = 1 + (k % 4);
            vp_put(pc++, &o);
        }
        vp_transform(pc, 0);
        pc += 4;
        vop o = v_sca_tmp(1, 0x3F, VT(5), M_ALL);
        o.vec_result = 0;
        o.out = 1;
        o.end = 1;
        vp_put(pc, &o);
    }
    run_vp("all_sca_ops", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    {
        vop o = v_op_tmp(13, 0, VI(SW(0, 1, 2, 3)), VT(0), VT(0), M_X | M_Y);
        o.input = 7;
        vp_put(0, &o);
        for (u32 r = 0; r < 4; r++) {
            o = vdef();
            o.vec = 7;
            o.src[0] = VI(SWI);
            o.src[1] = VC(SWI);
            o.cnst = 20 + r;
            o.idx_const = 1;
            o.asel = 0;
            o.aswz = r & 1u;
            o.out = 0;
            o.vec_result = 1;
            o.vmask = 1u << r;
            vp_put(1 + r, &o);
        }
        o = v_op_tmp(23, 1, VI(SWI), VT(0), VT(0), M_ALL);
        o.input = 6;
        vp_put(5, &o);
        o = v_op_tmp(24, 1, VT(0), VT(0), VT(0), M_X | M_Y);
        o.asel = 1;
        vp_put(6, &o);
        o = v_mov_out(1, VC(SWI), M_ALL);
        o.idx_const = 1;
        o.asel = 1;
        o.aswz = 1;
        o.cnst = 400;
        vp_put(7, &o);
        o = v_mov_out(2, VC(SWI), M_ALL);
        o.cnst = 500;
        vp_put(8, &o);
        o = v_mov_out(7, VI(SWI), M_ALL);
        o.idx_input = 1;
        o.input = 8;
        o.asel = 1;
        o.end = 1;
        vp_put(9, &o);
    }
    run_vp("address_relative", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = v_op_tmp(11, 0x3F, VI(SWI), VC(SWI), VT(0), M_ALL);
        o.input = 3;
        o.cnst = 9;
        o.upd = 1;
        o.ccsel = 1;
        vp_put(4, &o);
        o = v_op_tmp(12, 4, VI(SWI), VC(SWI), VT(0), M_X | M_Z);
        o.input = 3;
        o.cnst = 9;
        o.upd = 1;
        o.sat = 1;
        vp_put(5, &o);
        o = v_mov_out(1, VI(SWI), M_ALL);
        o.input = 3;
        o.ctest = 1;
        o.cond = 1;
        o.ccsel = 1;
        o.ccswz = SW(3, 2, 1, 0);
        vp_put(6, &o);
        o = v_mov_out(1, VT(4), M_X | M_W);
        o.ctest = 1;
        o.cond = 6;
        vp_put(7, &o);
        o = v_sca_tmp(2, 0x3F, VT(4), M_Y);
        o.vec_result = 1;
        o.out = 2;
        o.ctest = 1;
        o.cond = 5;
        vp_put(8, &o);
        o = v_mov_out(2, VT(4), M_ALL);
        o.ctest = 1;
        o.cond = 0;
        o.end = 1;
        vp_put(9, &o);
    }
    run_vp("condition_codes", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = v_op_tmp(12, 0x3F, VI(SWI), VC(SWI), VT(0), M_ALL);
        o.input = 2;
        o.cnst = 3;
        o.upd = 1;
        vp_put(4, &o);
        o = v_branch(9, 8, 1);
        vp_put(5, &o);
        o = v_mov_out(1, VI(SWI), M_ALL);
        o.input = 3;
        vp_put(6, &o);
        o = v_branch(9, 9, 7);
        vp_put(7, &o);
        o = v_mov_out(1, VC(SWI), M_ALL);
        o.cnst = 4;
        vp_put(8, &o);
        o = v_branch(10, 20, 7);
        vp_put(9, &o);
        o = v_branch(18, 30, 7);
        o.bidx = 3;
        o.btrue = 1;
        vp_put(10, &o);
        o = v_branch(17, 13, 7);
        o.bidx = 5;
        o.btrue = 0;
        vp_put(11, &o);
        o = v_mov_out(2, VI(SWI), M_ALL);
        o.input = 4;
        vp_put(12, &o);
        o = v_branch(11, 20, 2);
        vp_put(13, &o);
        o = v_mov_out(7, VI(SWI), M_ALL);
        o.input = 8;
        o.end = 1;
        vp_put(14, &o);
        o = v_op_tmp(3, 6, VT(6), VT(0), VC(SWI), M_ALL);
        o.cnst = 1;
        vp_put(20, &o);
        o = v_branch(12, 0, 7);
        vp_put(21, &o);
        o = v_op_tmp(2, 6, VT(6), VC(SWI), VT(0), M_ALL);
        o.cnst = 2;
        vp_put(30, &o);
        o = v_op_tmp(12, 0x3F, VT(6), VC(SWI), VT(0), M_X);
        o.cnst = 2;
        o.upd = 1;
        vp_put(31, &o);
        o = v_branch(9, 30, 1);
        vp_put(32, &o);
        o = v_branch(12, 0, 4);
        vp_put(33, &o);
        o = v_branch(12, 0, 7);
        vp_put(34, &o);
    }
    run_vp("branches_calls_loops", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = v_op_tmp(13, 0, VI(SWI), VT(0), VT(0), M_X);
        o.input = 5;
        vp_put(4, &o);
        o = v_branch(8, 0, 3);
        vp_put(5, &o);
        o = v_mov_out(1, VI(SWI), M_ALL);
        o.input = 3;
        vp_put(6, &o);
        o = v_mov_out(2, VI(SWI), M_ALL);
        o.input = 4;
        o.end = 1;
        vp_put(7, &o);
    }
    run_vp("bra_indirect", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    for (u32 u = 0; u < 4; u++) {
        vop o = v_op_tmp(25, 1 + u, VI(SWI), VT(0), VT(0), M_ALL);
        o.input = 8 + u;
        o.tex = u;
        vp_put(4 + u, &o);
        o = v_mov_out(7 + u, VT(1 + u), M_ALL);
        o.end = u == 3;
        vp_put(8 + u, &o);
    }
    {
        vop o = v_mov_out(7, VT(1), M_ALL);
        vp_put(8, &o);
        o = v_mov_out(8, VT(2), M_ALL);
        vp_put(9, &o);
        o = v_mov_out(9, VT(3), M_ALL);
        vp_put(10, &o);
        o = v_mov_out(10, VT(4), M_ALL);
        o.end = 1;
        vp_put(11, &o);
    }
    run_vp("vertex_textures_1d_2d_cube_3d", 0, 0xFFFFFFFFu, 0 | (1u << 2) | (2u << 4) | (3u << 6));
    run_vp("vertex_textures_all_2d", 0, 0xFFFFFFFFu, 0x55u);

    vp_clear();
    vp_transform(0, 0);
    {
        u32 pc = 4;
        for (u32 out = 1; out < 16; out++) {
            vop o = v_mov_out(out, VI(SWI), M_ALL);
            o.input = out;
            o.end = out == 15;
            vp_put(pc++, &o);
        }
    }
    run_vp("all_outputs_full_mask", 0, 0xFFFFFFFFu, 0);
    run_vp("all_outputs_partial_mask", 0, RSX_SPV_VPOUT_FRONTDIFFUSE | RSX_SPV_VPOUT_FOG | (RSX_SPV_VPOUT_UC0 << 1) | RSX_SPV_VPOUT_TEX9, 0);
    run_vp("all_outputs_no_mask", 0, 0, 0);

    vp_clear();
    {
        vop o = v_mov_out(1, VI(SWI), M_ALL);
        o.input = 3;
        o.end = 1;
        vp_put(0, &o);
    }
    run_vp("no_position_write", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = v_mov_out(1, VI(SWI), M_ALL);
        o.src[1] = 0;
        vp_put(4, &o);
        o = v_mov_out(2, VI(SWI), M_ALL);
        o.end = 1;
        vp_put(5, &o);
    }
    run_vp("abort_src_check", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = v_mov_out(1, VI(SWI), M_ALL);
        o.vec = 0x1C;
        vp_put(4, &o);
        o = v_mov_out(2, VI(SWI), M_ALL);
        o.end = 1;
        vp_put(5, &o);
    }
    run_vp("unknown_opcode", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    vp_transform(0, 0);
    {
        vop o = vdef();
        o.vec = 2;
        o.sca = 2;
        o.src[0] = VT(1);
        o.src[1] = VI(SWI);
        o.input = 2;
        o.src[2] = VT(1);
        o.tmp = 1;
        o.vmask = M_ALL;
        o.stmp = 2;
        o.smask = M_X;
        o.vec_result = 0;
        o.out = 3;
        vp_put(4, &o);
        o = vdef();
        o.vec = 1;
        o.sca = 4;
        o.src[0] = VT(2);
        o.src[2] = VT(1);
        o.vec_result = 1;
        o.out = 1;
        o.vmask = M_ALL;
        o.stmp = 0x3F;
        o.smask = M_Y;
        o.end = 1;
        vp_put(5, &o);
    }
    run_vp("dual_issue", 0, 0xFFFFFFFFu, 0);

    vp_clear();
    {
        u32 pc = 0;
        for (u32 k = 0; k < 120; k++) {
            vop o = v_op_tmp(4, k % 32, VI(SWI), VC(SWI), VT((k + 1) % 32), M_ALL);
            o.input = k % 16;
            o.cnst = k * 3;
            vp_put(pc++, &o);
            o = v_sca_tmp(4 + (k % 3), 32 + (k % 16), VT(k % 32), M_X);
            vp_put(pc++, &o);
        }
        vp_transform(pc, 0);
        pc += 4;
        for (u32 out = 1; out < 16; out++) {
            vop o = v_mov_out(out, VT(out), M_ALL);
            o.end = out == 15;
            vp_put(pc++, &o);
        }
    }
    run_vp("large_256", 0, 0xFFFFFFFFu, 0);
}

static void fragment_tests(void)
{
    printf("fragment programs:\n");
    rsx_spv_fp_info info;

    fp_begin();
    {
        fop o = fdef(0x01);
        o.attr = 1;
        o.s[0] = FIN;
        o.end = 1;
        fp_put(&o);
    }
    run_fp("mov_col0_fp32", 0x40);
    run_fp("mov_col0_fp16", 0x00);

    fp_begin();
    {
        fop o = fdef(0x02);
        o.mask = M_X | M_Y;
        o.dst = 1;
        fp_put(&o);
        o = fdef(0x01);
        o.attr = 1;
        o.s[0] = FIN;
        o.end = 1;
        fp_put(&o);
    }
    run_fp("legacy_mul_mask", 0x40);

    fp_begin();
    {
        fop o = fdef(0x04);
        o.sat = 1;
        o.s[1] = FK;
        o.end = 1;
        fp_put(&o);
        fp_const(0.5f, 0.25f, 0.0f, 1.0f);
    }
    run_fp_ex("legacy_mad_const_sat", 0x40, 0x55555555u, 0, 1, &info);
    check("constant word offset", info.constant_count == 1 && info.constant_word[0] == 4);

    fp_begin();
    {
        fop o = fdef(0x17);
        o.attr = 4;
        o.tex = 3;
        o.s[0] = FIN;
        o.end = 1;
        fp_put(&o);
    }
    run_fp_ex("legacy_tex_unit3", 0x40, 0x55555555u, 0, 1, &info);
    check("texture mask unit3", info.texture_mask == 8u && (info.input_mask & (1u << 4)));

    fp_begin();
    {
        fop o = fdef(0x01);
        o.s[0] = fsrc(0, 1, 0, SWI, 1);
        o.abs0 = 1;
        o.end = 1;
        fp_put(&o);
    }
    run_fp("legacy_neg_abs", 0x40);

    fp_begin();
    {
        static const u32 ops[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                                   0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21,
                                   0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x35,
                                   0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x30, 0x32, 0x3F };
        fop o = fdef(0x01);
        o.attr = 4;
        o.s[0] = FIN;
        o.dst = 1;
        fp_put(&o);
        for (u32 k = 0; k < sizeof ops / sizeof ops[0]; k++) {
            o = fdef(ops[k]);
            o.dst = 2 + (k % 20);
            o.fp16 = k & 1u;
            o.s[0] = fsrc(0, 1, 0, SW(k & 3u, 1, 2, 3), k & 1u);
            o.s[1] = (k % 5 == 0) ? FK : fsrc(0, 2 + (k % 3), (k >> 1) & 1u, SWI, 0);
            o.s[2] = fsrc(0, 3, 0, SW(3, 2, 1, 0), 0);
            o.prec = k & 3u;
            o.sat = (k % 7) == 0;
            o.scale = k % 8;
            fp_put(&o);
            if (k % 5 == 0) fp_const((float)k, 1.0f, -2.0f, 0.5f);
        }
        o = fdef(0x01);
        o.s[0] = FR(5);
        o.end = 1;
        fp_put(&o);
    }
    run_fp("all_alu_ops", 0x40);
    run_fp("all_alu_ops_fp16out", 0x00);

    for (u32 dim = 0; dim < 4; dim++) {
        for (u32 shadow = 0; shadow < 2; shadow++) {
            fp_begin();
            static const u32 texops[] = { 0x17, 0x18, 0x19, 0x31, 0x2F, 0x33, 0x34 };
            for (u32 k = 0; k < 7; k++) {
                fop o = fdef(texops[k]);
                o.attr = 4 + k;
                o.tex = k;
                o.s[0] = FIN;
                o.s[1] = FR(2);
                o.s[2] = FR(3);
                o.dst = 1 + k;
                o.bx2 = k & 1u;
                fp_put(&o);
            }
            fop o = fdef(0x03);
            o.s[0] = FR(1);
            o.s[1] = FR(4);
            o.end = 1;
            fp_put(&o);
            u32 dims = 0;
            for (u32 u = 0; u < 16; u++) dims |= dim << (u * 2);
            char nm[64];
            snprintf(nm, sizeof nm, "tex_dim%u_shadow%u", dim, shadow);
            run_fp_ex(nm, 0x40, dims, shadow ? 0xFFFFu : 0, 1, &info);
            check("tex mask 7 units", info.texture_mask == 0x7Fu);
        }
    }

    fp_begin();
    {
        fop o = fdef(0x01);
        o.attr = 1;
        o.s[0] = FIN;
        o.setc = 1;
        o.dst = 1;
        fp_put(&o);
        o = fdef(0x42);
        o.use_raw = 1;
        o.exec = 4;
        o.raw1 = 4u << 2;
        o.raw2 = 6u << 2;
        fp_put(&o);
        o = fdef(0x01);
        o.attr = 2;
        o.s[0] = FIN;
        fp_put(&o);
        o = fdef(0x01);
        o.attr = 3;
        o.s[0] = FIN;
        fp_put(&o);
        o = fdef(0x01);
        o.s[0] = FK;
        fp_put(&o);
        fp_const(1, 2, 3, 4);
        o = fdef(0x43);
        o.use_raw = 1;
        o.raw1 = (4u << 2) | (0u << 10) | (1u << 19);
        o.raw2 = 14u << 2;
        fp_put(&o);
        o = fdef(0x03);
        o.s[0] = FR(0);
        o.s[1] = fsrc(1, 0, 0, SWI, 0);
        o.useidx = 1;
        o.addr = 0;
        o.persp = 1;
        fp_put(&o);
        o = fdef(0x0A);
        o.s[0] = FR(0);
        o.s[1] = FK;
        o.nodst = 1;
        o.setc = 1;
        o.mask = M_X;
        fp_put(&o);
        fp_const(0.5f, 0, 0, 0);
        o = fdef(0x40);
        o.exec = 2;
        o.cswz = SW(0, 0, 0, 0);
        fp_put(&o);
        o = fdef(0x44);
        o.use_raw = 1;
        o.raw1 = 3u << 2;
        o.raw2 = 13u << 2;
        fp_put(&o);
        o = fdef(0x02);
        o.s[0] = FR(0);
        o.s[1] = FR(1);
        fp_put(&o);
        o = fdef(0x12);
        o.exec = 1;
        fp_put(&o);
        o = fdef(0x45);
        o.exec = 4;
        o.creg = 1;
        fp_put(&o);
        o = fdef(0x01);
        o.s[0] = FR(1);
        o.dst = 2;
        o.end = 1;
        fp_put(&o);
    }
    run_fp_ex("if_else_loop_rep_brk_kil_ret", 0x40, 0x55555555u, 0, 0xF, &info);
    check("dynamic texcoord inputs", (info.input_mask & 0x3FF0u) == 0x3FF0u);

    fp_begin();
    {
        fop o = fdef(0x41);
        o.use_raw = 1;
        o.raw1 = 3u << 2;
        fp_put(&o);
        o = fdef(0x42);
        o.use_raw = 1;
        o.exec = 0;
        o.raw1 = 6u << 2;
        o.raw2 = 6u << 2;
        fp_put(&o);
        o = fdef(0x01);
        o.dst = 1;
        o.s[0] = FR(1);
        fp_put(&o);
        o = fdef(0x01);
        o.dst = 3;
        o.attr = 5;
        o.s[0] = FIN;
        fp_put(&o);
        o = fdef(0x41);
        o.use_raw = 1;
        o.raw1 = 3u << 2;
        fp_put(&o);
        o = fdef(0x45);
        fp_put(&o);
        o = fdef(0x01);
        o.s[0] = FR(3);
        o.end = 1;
        fp_put(&o);
    }
    run_fp("cal_subroutine", 0x40);

    fp_begin();
    {
        fop o = fdef(0x01);
        o.fp16 = 1;
        o.dst = 0;
        o.attr = 1;
        o.s[0] = FIN;
        o.mask = M_X | M_Z;
        fp_put(&o);
        o = fdef(0x01);
        o.fp16 = 1;
        o.dst = 1;
        o.s[0] = FK;
        o.mask = M_Y | M_W;
        o.exec = 2;
        fp_put(&o);
        fp_const(0.25f, 70000.0f, 1e-6f, -0.0f);
        o = fdef(0x03);
        o.dst = 1;
        o.s[0] = FR(0);
        o.s[1] = FH(2);
        fp_put(&o);
        o = fdef(0x01);
        o.dst = 3;
        o.fp16 = 1;
        o.s[0] = FR(1);
        o.mask = M_X;
        fp_put(&o);
        o = fdef(0x01);
        o.dst = 4;
        o.fp16 = 1;
        o.s[0] = FH(3);
        o.setc = 1;
        o.cmod = 1;
        fp_put(&o);
        o = fdef(0x01);
        o.dst = 0;
        o.s[0] = FR(0);
        o.end = 1;
        fp_put(&o);
    }
    run_fp_ex("half_full_aliasing_depth", 0x40 | 0x0E, 0x55555555u, 0, 0xF, &info);
    check("depth export", info.depth_export == 1);
    {
        rsx_spv_fp_info si;
        rsx_spv_fragment_scan(g_fp, g_fpn * 4, &si);
        check("scan depth write", si.depth_export == 1);
    }
    run_fp_ex("half_full_aliasing_fp16out", 0x00, 0x55555555u, 0, 0x5, NULL);

    fp_begin();
    {
        static const u32 attrs[] = { 0, 1, 2, 3, 4, 13, 14 };
        for (u32 k = 0; k < 7; k++) {
            fop o = fdef(0x03);
            o.dst = 1 + k;
            o.attr = attrs[k];
            o.s[0] = FIN;
            o.s[1] = FR(k);
            o.sp[0] = k % 6;
            o.sp[1] = (k + 2) % 6;
            o.persp = k & 1u;
            fp_put(&o);
        }
        fop o = fdef(0x01);
        o.s[0] = FR(7);
        o.prec = 2;
        o.end = 1;
        fp_put(&o);
    }
    run_fp_ex("inputs_precision", 0x40 | RSX_SPV_FPCTL_FLAT_SHADING, 0x55555555u, 0, 1, &info);
    check("input mask", info.input_mask == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 13) | (1u << 14)));

    fp_begin();
    {
        fop o = fdef(0x01);
        o.s[0] = fsrc(3, 0, 0, SWI, 0);
        o.end = 1;
        fp_put(&o);
    }
    run_fp("invalid_src_type3", 0x40);

    fp_begin();
    {
        fop o = fdef(0x00);
        fp_put(&o);
        fp_put(&o);
        o = fdef(0x01);
        o.s[0] = FK;
        fp_put(&o);
        fp_const(1, 0, 0, 1);
        o = fdef(0x00);
        o.s[1] = FK;
        fp_put(&o);
        fp_const(9, 9, 9, 9);
        o = fdef(0x03);
        o.s[0] = FR(0);
        o.s[2] = FK;
        o.end = 1;
        fp_put(&o);
        fp_const(0, 1, 0, 0);
    }
    run_fp_ex("leading_nops_constants", 0x40, 0x55555555u, 0, 1, &info);
    check("constants skip nop data", info.constant_count == 2 && info.constant_word[0] == 12 && info.constant_word[1] == 28);

    fp_begin();
    {
        fop o = fdef(0x00);
        o.end = 1;
        fp_put(&o);
    }
    run_fp("nop_shader", 0x40);

    fp_begin();
    {
        fop o = fdef(0x01);
        o.s[0] = FIN;
        o.attr = 4;
        o.dst = 0;
        fp_put(&o);
        for (u32 k = 0; k < 400; k++) {
            o = fdef(k % 3 == 0 ? 0x17 : (k % 3 == 1 ? 0x04 : 0x05));
            o.tex = k % 16;
            o.attr = 4 + (k % 10);
            o.s[0] = k % 3 == 0 ? FIN : FR(k % 40);
            o.s[1] = k % 4 == 0 ? FK : FR((k + 1) % 40);
            o.s[2] = FR((k + 2) % 40);
            o.dst = k % 40;
            o.fp16 = (k % 5) == 0;
            fp_put(&o);
            if (k % 4 == 0) fp_const((float)k, 0, 0, 1);
        }
        o = fdef(0x01);
        o.s[0] = FR(3);
        o.end = 1;
        fp_put(&o);
    }
    run_fp("large_400", 0x40);
}

static void run_utility(const char* name, int rc, rsx_spv_blob* b)
{
    char cname[160];
    snprintf(cname, sizeof cname, "%s build", name);
    check(cname, rc == 0);
    if (rc == 0) validate(name, b);
    printf("  util %-26s %6u words\n", name, b->count);
    rsx_spv_blob_free(b);
}

static void utility_shaders(void)
{
    printf("utility shaders:\n");
    rsx_spv_blob b = { 0 };
    run_utility("util_depth_pack_d16", rsx_spv_depth_pack(1, &b), &b);
    run_utility("util_depth_pack_d24s8", rsx_spv_depth_pack(0, &b), &b);
    run_utility("util_box_down", rsx_spv_box_down(&b), &b);
    run_utility("util_box_down_vs", rsx_spv_box_down_vs(&b), &b);
    run_utility("util_box_down_fs2", rsx_spv_box_down_fs(2, &b), &b);
    run_utility("util_box_down_fs3", rsx_spv_box_down_fs(3, &b), &b);
    run_utility("util_box_down_fs4", rsx_spv_box_down_fs(4, &b), &b);
    run_utility("util_overlay_vs", rsx_spv_overlay_vs(&b), &b);
    run_utility("util_overlay_fs", rsx_spv_overlay_fs(&b), &b);
    run_utility("util_motion_vectors", rsx_spv_motion_vectors(&b), &b);
}

int main(int argc, char** argv)
{
    if (argc > 1) g_outdir = argv[1];
    if (argc > 2) g_val = argv[2];
    vertex_tests();
    fragment_tests();
    toolkit_samples();
    utility_shaders();
    printf("\nchecks: %d passed, %d failed\n", g_pass, g_fail);
    printf("modules: %d written", g_modules);
    if (g_val) printf(", %d valid, %d invalid", g_valid, g_invalid);
    printf("\n");
    if (g_vp_n) printf("vertex translate:   avg %.2f us, max %.2f us (%s), %d programs\n", g_vp_us / g_vp_n, g_vp_max, g_vp_max_name, g_vp_n);
    if (g_fp_n) printf("fragment translate: avg %.2f us, max %.2f us (%s), %d programs\n", g_fp_us / g_fp_n, g_fp_max, g_fp_max_name, g_fp_n);
    return (g_fail || g_invalid) ? 1 : 0;
}
