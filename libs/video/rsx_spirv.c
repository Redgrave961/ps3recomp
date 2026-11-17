#include "rsx_spirv.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(rsx_spv_vertex_attrib) == 16, "rsx_spv_vertex_attrib layout");
_Static_assert(offsetof(rsx_spv_vertex_layout, value) == 256, "rsx_spv_vertex_layout.value");
_Static_assert(offsetof(rsx_spv_vertex_layout, scale_offset) == 512, "rsx_spv_vertex_layout.scale_offset");
_Static_assert(offsetof(rsx_spv_vertex_layout, vertex_base) == 576, "rsx_spv_vertex_layout.vertex_base");
_Static_assert(offsetof(rsx_spv_vertex_layout, instance_base) == 580, "rsx_spv_vertex_layout.instance_base");
_Static_assert(offsetof(rsx_spv_vertex_layout, clip_planes) == 584, "rsx_spv_vertex_layout.clip_planes");
_Static_assert(offsetof(rsx_spv_vertex_layout, flags) == 588, "rsx_spv_vertex_layout.flags");
_Static_assert(offsetof(rsx_spv_vertex_layout, transform_branch_bits) == 592, "rsx_spv_vertex_layout.transform_branch_bits");
_Static_assert(sizeof(rsx_spv_texture_state) == 16, "rsx_spv_texture_state layout");
_Static_assert(offsetof(rsx_spv_fragment_state, fog_param) == 16, "rsx_spv_fragment_state.fog_param");
_Static_assert(offsetof(rsx_spv_fragment_state, wpos_scale) == 24, "rsx_spv_fragment_state.wpos_scale");
_Static_assert(offsetof(rsx_spv_fragment_state, wpos_bias) == 28, "rsx_spv_fragment_state.wpos_bias");
_Static_assert(offsetof(rsx_spv_fragment_state, texture) == 32, "rsx_spv_fragment_state.texture");
_Static_assert(offsetof(rsx_spv_fragment_state, texcoord_control) == 288, "rsx_spv_fragment_state.texcoord_control");

enum {
    OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33,
    OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61,
    OpStore = 62, OpAccessChain = 65, OpArrayLength = 68, OpDecorate = 71, OpMemberDecorate = 72,
    OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageSampleDrefImplicitLod = 89,
    OpImageFetch = 95, OpImageWrite = 99, OpImage = 100, OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112, OpBitcast = 124,
    OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpISub = 130, OpFSub = 131, OpIMul = 132,
    OpFMul = 133, OpUDiv = 134, OpSDiv = 135, OpFDiv = 136, OpUMod = 137, OpVectorTimesScalar = 142, OpDot = 148,
    OpAny = 154, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169,
    OpIEqual = 170, OpINotEqual = 171, OpUGreaterThan = 172, OpSGreaterThan = 173,
    OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175, OpULessThan = 176, OpSLessThan = 177,
    OpFOrdEqual = 180, OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186,
    OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190, OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseAnd = 199,
    OpNot = 200, OpBitFieldSExtract = 202, OpDPdx = 207, OpDPdy = 208, OpLoopMerge = 246,
    OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251,
    OpReturn = 253, OpTerminateInvocation = 4416, OpDemoteToHelperInvocation = 5380
};

enum {
    SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Private = 6,
    SC_Function = 7, SC_PushConstant = 9, SC_StorageBuffer = 12
};

enum {
    DEC_Block = 2, DEC_ArrayStride = 6, DEC_BuiltIn = 11, DEC_Flat = 14, DEC_NonWritable = 24,
    DEC_Location = 30, DEC_Binding = 33, DEC_DescriptorSet = 34, DEC_Offset = 35
};

enum {
    BI_Position = 0, BI_PointSize = 1, BI_ClipDistance = 3, BI_FragCoord = 15, BI_PointCoord = 16,
    BI_FrontFacing = 17, BI_FragDepth = 22, BI_GlobalInvocationId = 28, BI_VertexIndex = 42
};

enum {
    GL_Trunc = 3, GL_FAbs = 4, GL_FSign = 6, GL_Floor = 8, GL_Fract = 10, GL_Sin = 13, GL_Cos = 14,
    GL_Pow = 26, GL_Exp = 27, GL_Exp2 = 29, GL_Log2 = 30, GL_Sqrt = 31, GL_FMin = 37, GL_UMin = 38,
    GL_SMin = 39, GL_FMax = 40, GL_UMax = 41, GL_SMax = 42, GL_FClamp = 43, GL_FMix = 46, GL_Fma = 50,
    GL_PackSnorm4x8 = 54, GL_PackUnorm4x8 = 55, GL_PackUnorm2x16 = 57, GL_PackHalf2x16 = 58,
    GL_UnpackUnorm2x16 = 61, GL_UnpackHalf2x16 = 62, GL_UnpackSnorm4x8 = 63, GL_UnpackUnorm4x8 = 64,
    GL_Length = 66, GL_Normalize = 69, GL_Reflect = 71
};

enum { IMG_Bias = 1, IMG_Lod = 2, IMG_Grad = 4 };

typedef struct sb {
    u32* w;
    u32 n;
    u32 cap;
} sb;

typedef struct hent {
    u32 hash;
    u32 gen;
    u32 off;
    u32 len;
    u32 id;
} hent;

#define SPV_HT 4096u
#define SPV_FRAMES 12

typedef struct frame {
    sb locals;
    sb body;
    u32 id;
    u32 ret;
    u32 fty;
    u32 np;
    u32 pty[4];
    u32 pid[4];
} frame;

typedef struct spv {
    sb tvc;
    sb deco;
    sb funcs;
    sb iface;
    sb keys;
    sb out;
    frame fr[SPV_FRAMES];
    int depth;
    u32 gen;
    u32 id;
    u32 glsl;
    int err;
    u32 tvoid, tbool, tf, tu, ti, tv2, tv3, tv4, tu2, tu4, ti4, tb4, tfn;
    hent ht[SPV_HT];
    u32 scratch[16384];
} spv;

static _Thread_local spv* g_spv;

static u32* sb_grow(spv* s, sb* b, u32 n)
{
    if (b->n + n > b->cap) {
        u32 c = b->cap ? b->cap : 1024u;
        while (c < b->n + n) c *= 2u;
        u32* p = (u32*)realloc(b->w, (size_t)c * sizeof(u32));
        if (!p) {
            s->err = 1;
            return s->scratch;
        }
        b->w = p;
        b->cap = c;
    }
    u32* r = b->w + b->n;
    b->n += n;
    return r;
}

static void sb_ins(spv* s, sb* b, u32 op, const u32* a, u32 n)
{
    u32* p = sb_grow(s, b, n + 1u);
    p[0] = ((n + 1u) << 16) | op;
    for (u32 i = 0; i < n; i++) p[i + 1] = a[i];
}

static void sb_cat(spv* s, sb* d, const sb* src)
{
    if (!src->n) return;
    u32* p = sb_grow(s, d, src->n);
    if (p != s->scratch) memcpy(p, src->w, (size_t)src->n * sizeof(u32));
}

#define BODY(s) (&(s)->fr[(s)->depth].body)

static u32 ins_r(spv* s, u32 op, u32 ty, const u32* a, u32 n)
{
    u32 r = s->id++;
    u32* p = sb_grow(s, BODY(s), n + 3u);
    p[0] = ((n + 3u) << 16) | op;
    p[1] = ty;
    p[2] = r;
    for (u32 i = 0; i < n; i++) p[i + 3] = a[i];
    return r;
}

static void ins_v(spv* s, u32 op, const u32* a, u32 n)
{
    sb_ins(s, BODY(s), op, a, n);
}

#define SPV_EXP(x) x
#define SPV_NARG_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define SPV_NARG(...) SPV_EXP(SPV_NARG_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#define I(s, op, ty, ...) ins_r((s), (op), (ty), (const u32[]){ __VA_ARGS__ }, (u32)SPV_NARG(__VA_ARGS__))
#define V(s, op, ...) ins_v((s), (op), (const u32[]){ __VA_ARGS__ }, (u32)SPV_NARG(__VA_ARGS__))
#define X(s, ty, f, ...) I((s), OpExtInst, (ty), (s)->glsl, (f), __VA_ARGS__)

static u32 hash_words(const u32* k, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) {
        h ^= k[i];
        h *= 16777619u;
        h ^= h >> 15;
    }
    return h;
}

static u32 intern(spv* s, u32 op, u32 ty, const u32* a, u32 n, int unique)
{
    u32 key[48];
    key[0] = op;
    key[1] = ty;
    for (u32 i = 0; i < n; i++) key[i + 2] = a[i];
    u32 kn = n + 2u;
    u32 h = hash_words(key, kn);
    u32 slot = h & (SPV_HT - 1u);
    if (!unique) {
        for (;;) {
            hent* e = &s->ht[slot];
            if (e->gen != s->gen) break;
            if (e->hash == h && e->len == kn && !memcmp(s->keys.w + e->off, key, (size_t)kn * sizeof(u32)))
                return e->id;
            slot = (slot + 1u) & (SPV_HT - 1u);
        }
    }
    u32 r = s->id++;
    u32 has_ty = (op >= OpConstantTrue && op <= OpConstantComposite) ? 1u : 0u;
    u32* p = sb_grow(s, &s->tvc, n + 2u + has_ty);
    p[0] = ((n + 2u + has_ty) << 16) | op;
    if (has_ty) {
        p[1] = ty;
        p[2] = r;
    } else {
        p[1] = r;
    }
    for (u32 i = 0; i < n; i++) p[2 + has_ty + i] = a[i];
    if (!unique) {
        u32* kp = sb_grow(s, &s->keys, kn);
        memcpy(kp, key, (size_t)kn * sizeof(u32));
        hent* e = &s->ht[slot];
        e->hash = h;
        e->gen = s->gen;
        e->off = s->keys.n - kn;
        e->len = kn;
        e->id = r;
    }
    return r;
}

static const u32 k_none[1] = { 0 };

static u32 tvoid(spv* s) { return s->tvoid ? s->tvoid : (s->tvoid = intern(s, OpTypeVoid, 0, k_none, 0, 0)); }
static u32 tbool(spv* s) { return s->tbool ? s->tbool : (s->tbool = intern(s, OpTypeBool, 0, k_none, 0, 0)); }
static u32 tf(spv* s) { return s->tf ? s->tf : (s->tf = intern(s, OpTypeFloat, 0, (const u32[]){ 32 }, 1, 0)); }
static u32 tu(spv* s) { return s->tu ? s->tu : (s->tu = intern(s, OpTypeInt, 0, (const u32[]){ 32, 0 }, 2, 0)); }
static u32 ti(spv* s) { return s->ti ? s->ti : (s->ti = intern(s, OpTypeInt, 0, (const u32[]){ 32, 1 }, 2, 0)); }
static u32 tvec(spv* s, u32 c, u32 n) { return intern(s, OpTypeVector, 0, (const u32[]){ c, n }, 2, 0); }
static u32 tv2(spv* s) { return s->tv2 ? s->tv2 : (s->tv2 = tvec(s, tf(s), 2)); }
static u32 tv3(spv* s) { return s->tv3 ? s->tv3 : (s->tv3 = tvec(s, tf(s), 3)); }
static u32 tv4(spv* s) { return s->tv4 ? s->tv4 : (s->tv4 = tvec(s, tf(s), 4)); }
static u32 tu2(spv* s) { return s->tu2 ? s->tu2 : (s->tu2 = tvec(s, tu(s), 2)); }
static u32 tu4(spv* s) { return s->tu4 ? s->tu4 : (s->tu4 = tvec(s, tu(s), 4)); }
static u32 ti4(spv* s) { return s->ti4 ? s->ti4 : (s->ti4 = tvec(s, ti(s), 4)); }
static u32 tb4(spv* s) { return s->tb4 ? s->tb4 : (s->tb4 = tvec(s, tbool(s), 4)); }
static u32 tptr(spv* s, u32 sc, u32 ty) { return intern(s, OpTypePointer, 0, (const u32[]){ sc, ty }, 2, 0); }
static u32 tfn(spv* s) { return s->tfn ? s->tfn : (s->tfn = intern(s, OpTypeFunction, 0, (const u32[]){ tvoid(s) }, 1, 0)); }
static u32 tarr(spv* s, u32 el, u32 len, int unique);

static u32 cu(spv* s, u32 v) { return intern(s, OpConstant, tu(s), &v, 1, 0); }
static u32 ci(spv* s, s32 v)
{
    u32 w = (u32)v;
    return intern(s, OpConstant, ti(s), &w, 1, 0);
}
static u32 cf(spv* s, float f)
{
    u32 w;
    memcpy(&w, &f, sizeof w);
    return intern(s, OpConstant, tf(s), &w, 1, 0);
}
static u32 cb(spv* s, int b) { return intern(s, b ? OpConstantTrue : OpConstantFalse, tbool(s), k_none, 0, 0); }
static u32 cv4(spv* s, float a, float b, float c, float d)
{
    u32 m[4] = { cf(s, a), cf(s, b), cf(s, c), cf(s, d) };
    return intern(s, OpConstantComposite, tv4(s), m, 4, 0);
}
static u32 cvs(spv* s, float a) { return cv4(s, a, a, a, a); }
static u32 cv2(spv* s, float a, float b)
{
    u32 m[2] = { cf(s, a), cf(s, b) };
    return intern(s, OpConstantComposite, tv2(s), m, 2, 0);
}
static u32 cu4(spv* s, u32 a, u32 b, u32 c, u32 d)
{
    u32 m[4] = { cu(s, a), cu(s, b), cu(s, c), cu(s, d) };
    return intern(s, OpConstantComposite, tu4(s), m, 4, 0);
}
static u32 cus(spv* s, u32 a) { return cu4(s, a, a, a, a); }
static u32 ci4z(spv* s)
{
    u32 z = ci(s, 0);
    u32 m[4] = { z, z, z, z };
    return intern(s, OpConstantComposite, ti4(s), m, 4, 0);
}
static u32 cu4z(spv* s) { return cus(s, 0); }

static u32 tarr(spv* s, u32 el, u32 len, int unique)
{
    return intern(s, OpTypeArray, 0, (const u32[]){ el, cu(s, len) }, 2, unique);
}

static void deco(spv* s, u32 id, u32 d)
{
    sb_ins(s, &s->deco, OpDecorate, (const u32[]){ id, d }, 2);
}

static void deco1(spv* s, u32 id, u32 d, u32 v)
{
    sb_ins(s, &s->deco, OpDecorate, (const u32[]){ id, d, v }, 3);
}

static void mdeco(spv* s, u32 id, u32 m, u32 d, u32 v)
{
    sb_ins(s, &s->deco, OpMemberDecorate, (const u32[]){ id, m, d, v }, 4);
}

static u32 gvar(spv* s, u32 ty, u32 sc, u32 init)
{
    u32 pt = tptr(s, sc, ty);
    u32 r = s->id++;
    if (init)
        sb_ins(s, &s->tvc, OpVariable, (const u32[]){ pt, r, sc, init }, 4);
    else
        sb_ins(s, &s->tvc, OpVariable, (const u32[]){ pt, r, sc }, 3);
    *sb_grow(s, &s->iface, 1) = r;
    return r;
}

static u32 lvar(spv* s, u32 ty)
{
    u32 pt = tptr(s, SC_Function, ty);
    u32 r = s->id++;
    sb_ins(s, &s->fr[s->depth].locals, OpVariable, (const u32[]){ pt, r, SC_Function }, 3);
    return r;
}

static u32 newl(spv* s) { return s->id++; }

static void label(spv* s, u32 l) { sb_ins(s, BODY(s), OpLabel, &l, 1); }

static void fn_begin_t(spv* s, u32 id, u32 ret, u32 np, const u32* pty)
{
    u32 key[5];
    key[0] = ret;
    for (u32 k = 0; k < np; k++) key[k + 1] = pty[k];
    u32 fty = np ? intern(s, OpTypeFunction, 0, key, np + 1u, 0) : tfn(s);
    s->depth++;
    frame* f = &s->fr[s->depth];
    f->locals.n = 0;
    f->body.n = 0;
    f->id = id;
    f->ret = ret;
    f->fty = fty;
    f->np = np;
    for (u32 k = 0; k < np; k++) {
        f->pty[k] = pty[k];
        f->pid[k] = s->id++;
    }
}

static void fn_begin(spv* s, u32 id) { fn_begin_t(s, id, tvoid(s), 0, k_none); }

static void fn_end(spv* s)
{
    frame* f = &s->fr[s->depth];
    u32 l = s->id++;
    sb_ins(s, &s->funcs, OpFunction, (const u32[]){ f->ret, f->id, 0, f->fty }, 4);
    for (u32 k = 0; k < f->np; k++) sb_ins(s, &s->funcs, 55, (const u32[]){ f->pty[k], f->pid[k] }, 2);
    sb_ins(s, &s->funcs, OpLabel, &l, 1);
    sb_cat(s, &s->funcs, &f->locals);
    sb_cat(s, &s->funcs, &f->body);
    sb_ins(s, &s->funcs, OpFunctionEnd, k_none, 0);
    s->depth--;
}

static spv* spv_begin(void)
{
    spv* s = g_spv;
    if (!s) {
        s = (spv*)calloc(1, sizeof(spv));
        if (!s) return NULL;
        g_spv = s;
    }
    s->tvc.n = s->deco.n = s->funcs.n = s->iface.n = s->keys.n = s->out.n = 0;
    s->depth = -1;
    s->gen++;
    if (!s->gen) {
        memset(s->ht, 0, sizeof s->ht);
        s->gen = 1;
    }
    s->id = 1;
    s->err = 0;
    s->tvoid = s->tbool = s->tf = s->tu = s->ti = s->tv2 = s->tv3 = s->tv4 = 0;
    s->tu2 = s->tu4 = s->ti4 = s->tb4 = s->tfn = 0;
    s->glsl = s->id++;
    return s;
}

static void put_str(spv* s, sb* b, const char* str)
{
    u32 len = (u32)strlen(str) + 1u;
    u32 nw = (len + 3u) / 4u;
    u32* p = sb_grow(s, b, nw);
    if (p == s->scratch) return;
    memset(p, 0, (size_t)nw * 4u);
    memcpy(p, str, len);
}

static int spv_finish(spv* s, u32 model, const u32* caps, u32 ncaps, u32 entry, const u32* modes, u32 nmodes, rsx_spv_blob* out)
{
    sb* o = &s->out;
    o->n = 0;
    u32* h = sb_grow(s, o, 5);
    h[0] = 0x07230203u;
    h[1] = 0x00010600u;
    h[2] = 0;
    h[3] = s->id;
    h[4] = 0;
    for (u32 i = 0; i < ncaps; i++) sb_ins(s, o, OpCapability, &caps[i], 1);
    {
        u32 at = o->n;
        sb_grow(s, o, 2);
        put_str(s, o, "GLSL.std.450");
        if (!s->err) {
            o->w[at] = ((o->n - at) << 16) | OpExtInstImport;
            o->w[at + 1] = s->glsl;
        }
    }
    sb_ins(s, o, OpMemoryModel, (const u32[]){ 0, 1 }, 2);
    {
        u32 at = o->n;
        sb_grow(s, o, 3);
        put_str(s, o, "main");
        sb_cat(s, o, &s->iface);
        if (!s->err) {
            o->w[at] = ((o->n - at) << 16) | OpEntryPoint;
            o->w[at + 1] = model;
            o->w[at + 2] = entry;
        }
    }
    for (u32 i = 0; i < nmodes; i++) sb_ins(s, o, OpExecutionMode, (const u32[]){ entry, modes[i] }, 2);
    if (model == 5) sb_ins(s, o, OpExecutionMode, (const u32[]){ entry, 17, 8, 8, 1 }, 5);
    sb_cat(s, o, &s->deco);
    sb_cat(s, o, &s->tvc);
    sb_cat(s, o, &s->funcs);
    if (o->w) o->w[3] = s->id;
    if (s->err) return -1;
    if (!out) return 0;
    if (out->capacity < o->n || !out->words) {
        u32* p = (u32*)realloc(out->words, (size_t)o->n * sizeof(u32));
        if (!p) return -1;
        out->words = p;
        out->capacity = o->n;
    }
    memcpy(out->words, o->w, (size_t)o->n * sizeof(u32));
    out->count = o->n;
    return 0;
}

static inline u32 bf(u32 v, u32 off, u32 n) { return (v >> off) & ((1u << n) - 1u); }

static u32 ld(spv* s, u32 ty, u32 p) { return I(s, OpLoad, ty, p); }
static void st(spv* s, u32 p, u32 v) { V(s, OpStore, p, v); }
static u32 ex(spv* s, u32 ty, u32 v, u32 i) { return I(s, OpCompositeExtract, ty, v, i); }
static u32 exf(spv* s, u32 v, u32 i) { return ex(s, tf(s), v, i); }
static u32 shuf(spv* s, u32 ty, u32 a, u32 b, u32 x, u32 y, u32 z, u32 w) { return I(s, OpVectorShuffle, ty, a, b, x, y, z, w); }
static u32 splat(spv* s, u32 x) { return I(s, OpCompositeConstruct, tv4(s), x, x, x, x); }
static u32 vec4c(spv* s, u32 a, u32 b, u32 c, u32 d) { return I(s, OpCompositeConstruct, tv4(s), a, b, c, d); }
static u32 fadd(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpFAdd, ty, a, b); }
static u32 fsub(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpFSub, ty, a, b); }
static u32 fmul(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpFMul, ty, a, b); }
static u32 fdiv(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpFDiv, ty, a, b); }
static u32 sel(spv* s, u32 ty, u32 c, u32 a, u32 b) { return I(s, OpSelect, ty, c, a, b); }
static u32 bc(spv* s, u32 ty, u32 v) { return I(s, OpBitcast, ty, v); }
static u32 uand(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpBitwiseAnd, ty, a, b); }
static u32 uor(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpBitwiseOr, ty, a, b); }
static u32 ushr(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpShiftRightLogical, ty, a, b); }
static u32 ushl(spv* s, u32 ty, u32 a, u32 b) { return I(s, OpShiftLeftLogical, ty, a, b); }
static u32 dot4(spv* s, u32 a, u32 b) { return I(s, OpDot, tf(s), a, b); }
static u32 bnot(spv* s, u32 a) { return I(s, OpLogicalNot, tbool(s), a); }
static u32 band(spv* s, u32 a, u32 b) { return I(s, OpLogicalAnd, tbool(s), a, b); }
static u32 bor(spv* s, u32 a, u32 b) { return I(s, OpLogicalOr, tbool(s), a, b); }
static u32 bitset(spv* s, u32 v, u32 bit)
{
    return I(s, OpINotEqual, tbool(s), uand(s, tu(s), v, cu(s, 1u << bit)), cu(s, 0));
}
static u32 mask4(spv* s, u32 old, u32 nv, u32 ty, u32 m)
{
    if (m == 0xFu) return nv;
    return shuf(s, ty, old, nv, (m & 1u) ? 4u : 0u, (m & 2u) ? 5u : 1u, (m & 4u) ? 6u : 2u, (m & 8u) ? 7u : 3u);
}
static u32 bitmask4(spv* s, u32 v, u32 shift)
{
    u32 sh = cu4(s, shift, shift + 1u, shift + 2u, shift + 3u);
    u32 b = uand(s, tu4(s), ushr(s, tu4(s), I(s, OpCompositeConstruct, tu4(s), v, v, v, v), sh), cus(s, 1));
    return I(s, OpINotEqual, tb4(s), b, cu4z(s));
}

static void sel_if(spv* s, u32 cond, u32* merge, u32* then_l)
{
    *merge = newl(s);
    *then_l = newl(s);
    V(s, OpSelectionMerge, *merge, 0);
    V(s, OpBranchConditional, cond, *then_l, *merge);
    label(s, *then_l);
}

static void sel_end(spv* s, u32 merge)
{
    V(s, OpBranch, merge);
    label(s, merge);
}

static void demote_if(spv* s, u32 cond)
{
    u32 m, t;
    sel_if(s, cond, &m, &t);
    sb_ins(s, BODY(s), OpDemoteToHelperInvocation, k_none, 0);
    sel_end(s, m);
}

static u32 sampler_var(spv* s, u32 dim, int depth, u32 binding)
{
    static const u32 dims[4] = { 0, 1, 3, 2 };
    u32 img = intern(s, OpTypeImage, 0, (const u32[]){ tf(s), dims[dim & 3u], depth ? 1u : 0u, 0, 0, 1, 0 }, 7, 0);
    u32 si = intern(s, OpTypeSampledImage, 0, &img, 1, 0);
    u32 v = gvar(s, si, SC_UniformConstant, 0);
    deco1(s, v, DEC_DescriptorSet, 0);
    deco1(s, v, DEC_Binding, binding);
    return v;
}

static u32 sampler_type(spv* s, u32 dim, int depth)
{
    static const u32 dims[4] = { 0, 1, 3, 2 };
    u32 img = intern(s, OpTypeImage, 0, (const u32[]){ tf(s), dims[dim & 3u], depth ? 1u : 0u, 0, 0, 1, 0 }, 7, 0);
    return intern(s, OpTypeSampledImage, 0, &img, 1, 0);
}

static u32 coord_for_dim(spv* s, u32 v, u32 dim)
{
    if (dim == 0) return exf(s, v, 0);
    if (dim == 1) return I(s, OpVectorShuffle, tv2(s), v, v, 0, 1);
    return I(s, OpVectorShuffle, tv3(s), v, v, 0, 1, 2);
}

static u32 cmp_op(u32 c)
{
    switch (c) {
    case 1: return OpFOrdLessThan;
    case 2: return OpFOrdEqual;
    case 3: return OpFOrdLessThanEqual;
    case 4: return OpFOrdGreaterThan;
    case 5: return OpFUnordNotEqual;
    default: return OpFOrdGreaterThanEqual;
    }
}

static u32 set_op(spv* s, u32 opc, u32 a, u32 b)
{
    u32 c = I(s, opc, tb4(s), a, b);
    return sel(s, tv4(s), c, cvs(s, 1.0f), cvs(s, 0.0f));
}

static u32 lit_op(spv* s, u32 v)
{
    u32 x = X(s, tf(s), GL_FMax, exf(s, v, 0), cf(s, 0.0f));
    u32 y = X(s, tf(s), GL_FMax, exf(s, v, 1), cf(s, 1e-10f));
    u32 w = exf(s, v, 3);
    u32 p = X(s, tf(s), GL_Exp2, fmul(s, tf(s), w, X(s, tf(s), GL_Log2, y)));
    u32 z = sel(s, tf(s), I(s, OpFOrdGreaterThan, tbool(s), x, cf(s, 0.0f)), p, cf(s, 0.0f));
    return vec4c(s, cf(s, 1.0f), x, z, cf(s, 1.0f));
}

enum { VP_SLOTS = 512 };

enum {
    VEC_NOP = 0, VEC_MOV, VEC_MUL, VEC_ADD, VEC_MAD, VEC_DP3, VEC_DPH, VEC_DP4, VEC_DST, VEC_MIN, VEC_MAX,
    VEC_SLT, VEC_SGE, VEC_ARL, VEC_FRC, VEC_FLR, VEC_SEQ, VEC_SFL, VEC_SGT, VEC_SLE, VEC_SNE, VEC_STR,
    VEC_SSG, VEC_ARR, VEC_ARA, VEC_TXL
};

enum {
    SCA_NOP = 0, SCA_MOV, SCA_RCP, SCA_RCC, SCA_RSQ, SCA_EXP, SCA_LOG, SCA_LIT, SCA_BRA, SCA_BRI, SCA_CAL,
    SCA_CLI, SCA_RET, SCA_LG2, SCA_EX2, SCA_SIN, SCA_COS, SCA_BRB, SCA_CLB, SCA_PSH, SCA_POP
};

typedef struct vpi {
    u32 d0, d1, d2, d3;
    u32 src[3];
} vpi;

typedef struct vpc {
    spv* s;
    const u32* uc;
    const rsx_spv_vp_key* key;
    u8 reach[VP_SLOTS];
    u8 leader[VP_SLOTS];
    u32 label[VP_SLOTS];
    u32 r[64];
    u32 o[16];
    u32 a[2];
    u32 cc[2];
    u32 in[16];
    u32 inarr;
    u32 ubo_c;
    u32 ubo_l;
    u32 ssbo;
    u32 vidx;
    u32 vtex[4];
    u32 fetchfn;
    u32 pcv, spv_, stk;
    u32 written;
    u32 inputs;
    u32 texmask;
    u32 count;
    int indexed_in;
    int has_flow;
    int has_bra;
    u32 cused[(RSX_SPV_VP_CONSTANTS + 31u) / 32u];
    int crel;
} vpc;

static void vp_decode(const u32* uc, u32 pc, vpi* i)
{
    const u32* p = uc + pc * 4u;
    i->d0 = p[0];
    i->d1 = p[1];
    i->d2 = p[2];
    i->d3 = p[3];
    i->src[0] = bf(i->d2, 23, 9) | (bf(i->d1, 0, 8) << 9);
    i->src[1] = bf(i->d2, 6, 17);
    i->src[2] = bf(i->d3, 21, 11) | (bf(i->d2, 0, 6) << 11);
}

static u32 vp_target(const vpi* i) { return (bf(i->d0, 23, 1) << 9) | (bf(i->d2, 0, 6) << 3) | bf(i->d3, 29, 3); }
static int vp_srcs_ok(const vpi* i) { return (i->src[0] & 3u) && (i->src[1] & 3u) && (i->src[2] & 3u); }
static u32 vp_vmask(const vpi* i) { return bf(i->d3, 16, 1) | (bf(i->d3, 15, 1) << 1) | (bf(i->d3, 14, 1) << 2) | (bf(i->d3, 13, 1) << 3); }
static u32 vp_smask(const vpi* i) { return bf(i->d3, 20, 1) | (bf(i->d3, 19, 1) << 1) | (bf(i->d3, 18, 1) << 2) | (bf(i->d3, 17, 1) << 3); }

static u32 vp_vec_uses(u32 op)
{
    switch (op) {
    case VEC_MOV: case VEC_ARL: case VEC_ARR: case VEC_FRC: case VEC_FLR: case VEC_SSG: case VEC_TXL: return 1u;
    case VEC_ADD: return 5u;
    case VEC_MAD: return 7u;
    case VEC_MUL: case VEC_DP3: case VEC_DPH: case VEC_DP4: case VEC_DST: case VEC_MIN: case VEC_MAX:
    case VEC_SLT: case VEC_SGE: case VEC_SEQ: case VEC_SGT: case VEC_SLE: case VEC_SNE: return 3u;
    default: return 0;
    }
}

static int vp_sca_alu(u32 op)
{
    switch (op) {
    case SCA_MOV: case SCA_RCP: case SCA_RCC: case SCA_RSQ: case SCA_EXP: case SCA_LOG: case SCA_LIT:
    case SCA_LG2: case SCA_EX2: case SCA_SIN: case SCA_COS: return 1;
    default: return 0;
    }
}

static int vp_is_addr_op(u32 op) { return op == VEC_ARL || op == VEC_ARR || op == VEC_ARA; }

static void vp_walk(vpc* c, u32 start)
{
    u16 stack[VP_SLOTS * 2 + 4];
    u32 sp = 0;
    stack[sp++] = (u16)start;
    c->leader[start] = 1;
    while (sp) {
        u32 pc = stack[--sp];
        while (pc < VP_SLOTS && !c->reach[pc]) {
            vpi i;
            c->reach[pc] = 1;
            c->count++;
            vp_decode(c->uc, pc, &i);
            if (!vp_srcs_ok(&i)) break;
            u32 vec = bf(i.d1, 22, 5), sca = bf(i.d1, 27, 5), cond = bf(i.d0, 10, 3);
            int stop = vec > VEC_TXL || sca > SCA_POP;
            int fall = 1;
            u32 t = vp_target(&i);
            int push_t = 0, flow = 1;
            switch (sca) {
            case SCA_BRA:
                c->has_bra = 1;
                if (cond == 7) fall = 0;
                break;
            case SCA_BRI:
                push_t = cond != 0;
                if (cond == 7) fall = 0;
                break;
            case SCA_CAL: case SCA_CLI:
                push_t = cond != 0;
                break;
            case SCA_CLB: case SCA_BRB:
                push_t = 1;
                break;
            case SCA_RET:
                if (cond == 7) fall = 0;
                break;
            default:
                flow = 0;
                break;
            }
            if (flow) {
                c->has_flow = 1;
                if (pc + 1u < VP_SLOTS) c->leader[pc + 1u] = 1;
                if (push_t && t < VP_SLOTS) {
                    c->leader[t] = 1;
                    if (!c->reach[t] && sp < VP_SLOTS * 2) stack[sp++] = (u16)t;
                }
            }
            if ((i.d3 & 1u) || stop || !fall) break;
            pc++;
        }
    }
}

int rsx_spv_vertex_reach(const u32* ucode, u32 start, u8* reach)
{
    if (start >= VP_SLOTS) return -1;
    vpc* c = (vpc*)calloc(1, sizeof(vpc));
    if (!c) return -1;
    c->uc = ucode;
    vp_walk(c, start);
    memcpy(reach, c->reach, VP_SLOTS);
    const int dynamic = c->has_bra;
    free(c);
    return dynamic;
}

static void vp_prescan(vpc* c)
{
    for (u32 pc = 0; pc < VP_SLOTS; pc++) {
        if (!c->reach[pc]) continue;
        vpi i;
        vp_decode(c->uc, pc, &i);
        if (!vp_srcs_ok(&i)) continue;
        u32 vec = bf(i.d1, 22, 5), sca = bf(i.d1, 27, 5), cond = bf(i.d0, 10, 3);
        if (vec > VEC_TXL) vec = 0;
        if (sca > SCA_POP) sca = 0;
        u32 use = vp_vec_uses(vec);
        if (vp_sca_alu(sca)) use |= 4u;
        for (u32 n = 0; n < 3; n++) {
            if ((use & (1u << n)) && (i.src[n] & 3u) == 2u) {
                if (bf(i.d0, 27, 1)) c->indexed_in = 1;
                else c->inputs |= 1u << bf(i.d1, 8, 4);
            }
        }
        if (vec == VEC_TXL) c->texmask |= 1u << bf(i.d2, 8, 2);
        if (cond == 0) continue;
        u32 dst = bf(i.d3, 2, 5);
        int vr = (int)bf(i.d0, 30, 1);
        if (vec != VEC_NOP && !vp_is_addr_op(vec) && vp_vmask(&i) && vr && dst < 16) c->written |= 1u << dst;
        if (vp_sca_alu(sca) && vp_smask(&i) && !vr && dst < 16) c->written |= 1u << dst;
    }
    if (c->indexed_in) c->inputs = 0xFFFFu;
}

static u32 vp_rvar(vpc* c, u32 n)
{
    if (!c->r[n]) c->r[n] = gvar(c->s, tv4(c->s), SC_Private, cvs(c->s, 0.0f));
    return c->r[n];
}

static u32 vp_ovar(vpc* c, u32 n)
{
    if (!c->o[n]) c->o[n] = gvar(c->s, tv4(c->s), SC_Private, cv4(c->s, 0.0f, 0.0f, 0.0f, 1.0f));
    return c->o[n];
}

static u32 vp_avar(vpc* c, u32 n)
{
    if (!c->a[n]) c->a[n] = gvar(c->s, ti4(c->s), SC_Private, ci4z(c->s));
    return c->a[n];
}

static u32 vp_ccvar(vpc* c, u32 n)
{
    if (!c->cc[n]) c->cc[n] = gvar(c->s, tv4(c->s), SC_Private, cvs(c->s, 0.0f));
    return c->cc[n];
}

static u32 vp_layout(vpc* c)
{
    spv* s = c->s;
    if (c->ubo_l) return c->ubo_l;
    u32 au = tarr(s, tu4(s), 16, 1);
    u32 av = tarr(s, tv4(s), 16, 1);
    u32 as = tarr(s, tv4(s), 4, 1);
    deco1(s, au, DEC_ArrayStride, 16);
    deco1(s, av, DEC_ArrayStride, 16);
    deco1(s, as, DEC_ArrayStride, 16);
    u32 u = tu(s);
    u32 st_ = intern(s, OpTypeStruct, 0, (const u32[]){ au, av, as, u, u, u, u, u, tf(s), tf(s) }, 10, 1);
    static const u32 offs[10] = { 0, 256, 512, 576, 580, 584, 588, 592, 596, 600 };
    for (u32 m = 0; m < 10; m++) mdeco(s, st_, m, DEC_Offset, offs[m]);
    deco(s, st_, DEC_Block);
    c->ubo_l = gvar(s, st_, SC_Uniform, 0);
    deco1(s, c->ubo_l, DEC_DescriptorSet, 0);
    deco1(s, c->ubo_l, DEC_Binding, RSX_SPV_BIND_VERTEX_LAYOUT);
    return c->ubo_l;
}

static u32 vp_layout_u32(vpc* c, u32 member)
{
    spv* s = c->s;
    u32 p = I(s, OpAccessChain, tptr(s, SC_Uniform, tu(s)), vp_layout(c), ci(s, (s32)member));
    return ld(s, tu(s), p);
}

static u32 vp_consts(vpc* c)
{
    spv* s = c->s;
    if (c->ubo_c) return c->ubo_c;
    u32 arr = tarr(s, tv4(s), RSX_SPV_VP_CONSTANTS, 1);
    deco1(s, arr, DEC_ArrayStride, 16);
    u32 st_ = intern(s, OpTypeStruct, 0, &arr, 1, 1);
    mdeco(s, st_, 0, DEC_Offset, 0);
    deco(s, st_, DEC_Block);
    c->ubo_c = gvar(s, st_, SC_Uniform, 0);
    deco1(s, c->ubo_c, DEC_DescriptorSet, 0);
    deco1(s, c->ubo_c, DEC_Binding, RSX_SPV_BIND_VP_CONSTANTS);
    return c->ubo_c;
}

static u32 vp_data(vpc* c)
{
    spv* s = c->s;
    if (c->ssbo) return c->ssbo;
    u32 ra = intern(s, OpTypeRuntimeArray, 0, (const u32[]){ tu(s) }, 1, 1);
    deco1(s, ra, DEC_ArrayStride, 4);
    u32 st_ = intern(s, OpTypeStruct, 0, &ra, 1, 1);
    mdeco(s, st_, 0, DEC_Offset, 0);
    sb_ins(s, &s->deco, OpMemberDecorate, (const u32[]){ st_, 0, DEC_NonWritable }, 3);
    deco(s, st_, DEC_Block);
    c->ssbo = gvar(s, st_, SC_StorageBuffer, 0);
    deco1(s, c->ssbo, DEC_DescriptorSet, 0);
    deco1(s, c->ssbo, DEC_Binding, RSX_SPV_BIND_VERTEX_DATA);
    return c->ssbo;
}

static u32 bswap32(spv* s, u32 x)
{
    u32 t = tu(s);
    u32 a = ushl(s, t, x, cu(s, 24));
    u32 b = ushl(s, t, uand(s, t, x, cu(s, 0xFF00u)), cu(s, 8));
    u32 d = uand(s, t, ushr(s, t, x, cu(s, 8)), cu(s, 0xFF00u));
    u32 e = ushr(s, t, x, cu(s, 24));
    return uor(s, t, uor(s, t, a, b), uor(s, t, d, e));
}

static u32 vp_fetch_bits(vpc* c, u32 addr, u32 size, u32 esz, u32 ncomp, u32 lenm1)
{
    spv* s = c->s;
    u32 t = tu(s);
    u32 data = vp_data(c);
    u32 pt = tptr(s, SC_StorageBuffer, t);
    u32 comp[4];
    for (u32 n = 0; n < 4; n++) {
        if (n >= ncomp) {
            comp[n] = cu(s, 0);
            continue;
        }
        u32 ea = n ? I(s, OpIAdd, t, addr, cu(s, n * esz)) : addr;
        u32 w = ushr(s, t, ea, cu(s, 2));
        u32 sh = ushl(s, t, uand(s, t, ea, cu(s, 3)), cu(s, 3));
        u32 w0 = X(s, t, GL_UMin, w, lenm1);
        u32 lo = ushr(s, t, ld(s, t, I(s, OpAccessChain, pt, data, ci(s, 0), w0)), sh);
        u32 le = lo;
        if (esz > 1) {
            u32 w1 = X(s, t, GL_UMin, I(s, OpIAdd, t, w, cu(s, 1)), lenm1);
            u32 hv = ld(s, t, I(s, OpAccessChain, pt, data, ci(s, 0), w1));
            u32 hi = ushl(s, t, ushl(s, t, hv, cu(s, 1)), I(s, OpISub, t, cu(s, 31), sh));
            le = uor(s, t, lo, hi);
        }
        u32 v;
        if (esz == 4) v = bswap32(s, le);
        else if (esz == 2) v = uor(s, t, ushl(s, t, uand(s, t, le, cu(s, 0xFFu)), cu(s, 8)), uand(s, t, ushr(s, t, le, cu(s, 8)), cu(s, 0xFFu)));
        else v = uand(s, t, le, cu(s, 0xFFu));
        if (ncomp > 1) v = sel(s, t, I(s, OpULessThan, tbool(s), cu(s, n), size), v, cu(s, 0));
        comp[n] = v;
    }
    return I(s, OpCompositeConstruct, tu4(s), comp[0], comp[1], comp[2], comp[3]);
}

static u32 vp_finalize(vpc* c, u32 v, u32 size)
{
    spv* s = c->s;
    u32 sz = I(s, OpCompositeConstruct, tu4(s), size, size, size, size);
    u32 keep = I(s, OpULessThan, tb4(s), cu4(s, 0, 1, 2, 3), sz);
    u32 r = sel(s, tv4(s), keep, v, cvs(s, 0.0f));
    u32 w = sel(s, tf(s), I(s, OpULessThan, tbool(s), size, cu(s, 4)), cf(s, 1.0f), exf(s, r, 3));
    return I(s, OpCompositeInsert, tv4(s), w, r, 3);
}

static u32 vp_fetch_body(vpc* c, u32 k)
{
    spv* s = c->s;
    u32 t = tu(s), it = ti(s);
    u32 L = vp_layout(c);
    u32 attr = ld(s, tu4(s), I(s, OpAccessChain, tptr(s, SC_Uniform, tu4(s)), L, ci(s, 0), k));
    u32 off = ex(s, t, attr, 0), stride = ex(s, t, attr, 1), fmt = ex(s, t, attr, 2), freq = ex(s, t, attr, 3);
    u32 typ = uand(s, t, fmt, cu(s, 15));
    u32 size = uand(s, t, ushr(s, t, fmt, cu(s, 4)), cu(s, 15));
    u32 modu = I(s, OpLogicalAnd, tbool(s), bitset(s, fmt, 8), I(s, OpINotEqual, tbool(s), freq, cu(s, 0)));
    if (!c->vidx) {
        c->vidx = gvar(s, it, SC_Input, 0);
        deco1(s, c->vidx, DEC_BuiltIn, BI_VertexIndex);
    }
    u32 vid = ld(s, it, c->vidx);
    u32 vb = bc(s, it, vp_layout_u32(c, 3));
    u32 ib = vp_layout_u32(c, 4);
    u32 f1 = X(s, t, GL_UMax, freq, cu(s, 1));
    u32 per = I(s, OpISub, it, vid, vb);
    u32 dv = bc(s, t, I(s, OpSDiv, it, per, bc(s, it, f1)));
    u32 md = I(s, OpUMod, t, I(s, OpIAdd, t, bc(s, t, vid), ib), f1);
    u32 idx = sel(s, t, modu, md, dv);
    u32 addr = I(s, OpIAdd, t, I(s, OpIMul, t, idx, stride), off);
    u32 dflt = ld(s, tv4(s), I(s, OpAccessChain, tptr(s, SC_Uniform, tv4(s)), L, ci(s, 1), k));
    u32 res = lvar(s, tv4(s));
    st(s, res, dflt);
    u32 eff = sel(s, t, I(s, OpIEqual, tbool(s), size, cu(s, 0)), cu(s, 0), typ);
    u32 len = I(s, OpArrayLength, t, vp_data(c), 0);
    u32 lenm1 = I(s, OpISub, t, len, cu(s, 1));
    u32 merge = newl(s), l16 = newl(s), l32 = newl(s), lh = newl(s), l8 = newl(s), lc = newl(s);
    V(s, OpSelectionMerge, merge, 0);
    V(s, OpSwitch, eff, merge, 1, l16, 5, l16, 2, l32, 3, lh, 4, l8, 7, l8, 6, lc);

    label(s, l16);
    {
        u32 b = vp_fetch_bits(c, addr, size, 2, 4, lenm1);
        u32 sx = I(s, OpBitFieldSExtract, ti4(s), bc(s, ti4(s), b), ci(s, 0), ci(s, 16));
        u32 f = I(s, OpConvertSToF, tv4(s), sx);
        u32 is1 = I(s, OpIEqual, tbool(s), typ, cu(s, 1));
        u32 nrm = fdiv(s, tv4(s), fadd(s, tv4(s), f, cvs(s, 0.5f)), cvs(s, 32767.5f));
        st(s, res, vp_finalize(c, sel(s, tv4(s), is1, nrm, f), size));
        V(s, OpBranch, merge);
    }
    label(s, l32);
    {
        u32 b = vp_fetch_bits(c, addr, size, 4, 4, lenm1);
        st(s, res, vp_finalize(c, bc(s, tv4(s), b), size));
        V(s, OpBranch, merge);
    }
    label(s, lh);
    {
        u32 b = vp_fetch_bits(c, addr, size, 2, 4, lenm1);
        u32 h0 = uor(s, t, ex(s, t, b, 0), ushl(s, t, ex(s, t, b, 1), cu(s, 16)));
        u32 h1 = uor(s, t, ex(s, t, b, 2), ushl(s, t, ex(s, t, b, 3), cu(s, 16)));
        u32 a = X(s, tv2(s), GL_UnpackHalf2x16, h0);
        u32 bb = X(s, tv2(s), GL_UnpackHalf2x16, h1);
        st(s, res, vp_finalize(c, shuf(s, tv4(s), a, bb, 0, 1, 2, 3), size));
        V(s, OpBranch, merge);
    }
    label(s, l8);
    {
        u32 b = vp_fetch_bits(c, addr, size, 1, 4, lenm1);
        u32 f = I(s, OpConvertUToF, tv4(s), b);
        u32 is4 = I(s, OpIEqual, tbool(s), typ, cu(s, 4));
        st(s, res, vp_finalize(c, sel(s, tv4(s), is4, fdiv(s, tv4(s), f, cvs(s, 255.0f)), f), size));
        V(s, OpBranch, merge);
    }
    label(s, lc);
    {
        u32 b = vp_fetch_bits(c, addr, size, 4, 1, lenm1);
        u32 bi = bc(s, it, ex(s, t, b, 0));
        u32 x = I(s, OpBitFieldSExtract, it, bi, ci(s, 0), ci(s, 11));
        u32 y = I(s, OpBitFieldSExtract, it, bi, ci(s, 11), ci(s, 11));
        u32 z = I(s, OpBitFieldSExtract, it, bi, ci(s, 22), ci(s, 10));
        u32 fx = fmul(s, tf(s), I(s, OpConvertSToF, tf(s), x), cf(s, 32.0f));
        u32 fy = fmul(s, tf(s), I(s, OpConvertSToF, tf(s), y), cf(s, 32.0f));
        u32 fz = fmul(s, tf(s), I(s, OpConvertSToF, tf(s), z), cf(s, 64.0f));
        u32 v = fdiv(s, tv4(s), vec4c(s, fx, fy, fz, cf(s, 32767.0f)), cvs(s, 32767.0f));
        st(s, res, v);
        V(s, OpBranch, merge);
    }
    label(s, merge);
    return ld(s, tv4(s), res);
}

static u32 vp_fetch(vpc* c, u32 k)
{
    spv* s = c->s;
    if (!c->fetchfn) {
        c->fetchfn = s->id++;
        u32 pt = ti(s);
        fn_begin_t(s, c->fetchfn, tv4(s), 1, &pt);
        u32 r = vp_fetch_body(c, s->fr[s->depth].pid[0]);
        V(s, 254, r);
        fn_end(s);
    }
    return I(s, OpFunctionCall, tv4(s), c->fetchfn, ci(s, (s32)k));
}

static u32 vp_addr(vpc* c, const vpi* i)
{
    spv* s = c->s;
    u32 a = ld(s, ti4(s), vp_avar(c, bf(i->d0, 24, 1)));
    return ex(s, ti(s), a, bf(i->d0, 0, 2));
}

static u32 vp_const(vpc* c, const vpi* i)
{
    spv* s = c->s;
    u32 cs = bf(i->d1, 12, 10);
    u32 pt = tptr(s, SC_Uniform, tv4(s));
    if (bf(i->d3, 1, 1)) {
        c->crel = 1;
        u32 a = vp_addr(c, i);
        u32 idx = uand(s, ti(s), I(s, OpIAdd, ti(s), a, ci(s, (s32)cs)), ci(s, 511));
        u32 inb = I(s, OpSLessThan, tbool(s), idx, ci(s, (s32)RSX_SPV_VP_CONSTANTS));
        u32 cl = X(s, ti(s), GL_SMin, idx, ci(s, (s32)RSX_SPV_VP_CONSTANTS - 1));
        u32 v = ld(s, tv4(s), I(s, OpAccessChain, pt, vp_consts(c), ci(s, 0), cl));
        return sel(s, tv4(s), inb, v, cvs(s, 0.0f));
    }
    if (cs >= RSX_SPV_VP_CONSTANTS) return cvs(s, 0.0f);
    c->cused[cs >> 5] |= 1u << (cs & 31u);
    return ld(s, tv4(s), I(s, OpAccessChain, pt, vp_consts(c), ci(s, 0), ci(s, (s32)cs)));
}

static u32 vp_src(vpc* c, const vpi* i, u32 n)
{
    spv* s = c->s;
    u32 sr = i->src[n];
    u32 v;
    switch (sr & 3u) {
    case 1:
        v = ld(s, tv4(s), vp_rvar(c, bf(sr, 2, 6)));
        break;
    case 2:
        if (bf(i->d0, 27, 1)) {
            u32 a = vp_addr(c, i);
            u32 idx = uand(s, ti(s), I(s, OpIAdd, ti(s), a, ci(s, (s32)bf(i->d1, 8, 4))), ci(s, 15));
            v = ld(s, tv4(s), I(s, OpAccessChain, tptr(s, SC_Private, tv4(s)), c->inarr, idx));
        } else {
            v = c->in[bf(i->d1, 8, 4)];
        }
        break;
    default:
        v = vp_const(c, i);
        break;
    }
    u32 sx = bf(sr, 14, 2), sy = bf(sr, 12, 2), sz = bf(sr, 10, 2), sw = bf(sr, 8, 2);
    if (sx != 0 || sy != 1 || sz != 2 || sw != 3) v = shuf(s, tv4(s), v, v, sx, sy, sz, sw);
    if (bf(i->d0, 21 + n, 1)) v = X(s, tv4(s), GL_FAbs, v);
    if (bf(sr, 16, 1)) v = I(s, OpFNegate, tv4(s), v);
    return v;
}

static u32 vp_ccvec(vpc* c, const vpi* i)
{
    spv* s = c->s;
    u32 cond = bf(i->d0, 10, 3);
    u32 cc = ld(s, tv4(s), vp_ccvar(c, bf(i->d0, 25, 1)));
    u32 sw = shuf(s, tv4(s), cc, cc, bf(i->d0, 8, 2), bf(i->d0, 6, 2), bf(i->d0, 4, 2), bf(i->d0, 2, 2));
    return I(s, cmp_op(cond), tb4(s), sw, cvs(s, 0.0f));
}

static u32 vp_cond_any(vpc* c, const vpi* i, u32 ccv)
{
    spv* s = c->s;
    u32 cond = bf(i->d0, 10, 3);
    if (cond == 0) return cb(s, 0);
    if (cond == 7) return cb(s, 1);
    return I(s, OpAny, tbool(s), ccv ? ccv : vp_ccvec(c, i));
}

static void vp_store(vpc* c, u32 var, u32 ty, u32 val, u32 mask, u32 condv)
{
    spv* s = c->s;
    if (mask == 0xFu && !condv) {
        st(s, var, val);
        return;
    }
    u32 old = ld(s, ty, var);
    u32 nv = condv ? sel(s, ty, condv, val, old) : val;
    st(s, var, mask4(s, old, nv, ty, mask));
}

static void vp_write(vpc* c, const vpi* i, int is_sca, u32 val, u32 condv)
{
    spv* s = c->s;
    u32 mask = is_sca ? vp_smask(i) : vp_vmask(i);
    if (!mask) return;
    if (bf(i->d0, 26, 1)) val = X(s, tv4(s), GL_FClamp, val, cvs(s, 0.0f), cvs(s, 1.0f));
    u32 dst = bf(i->d3, 2, 5);
    int vr = (int)bf(i->d0, 30, 1);
    int is_result = is_sca ? !vr : vr;
    u32 tmp = is_sca ? bf(i->d3, 7, 6) : bf(i->d0, 15, 6);
    int out = is_result && dst != 0x1Fu;
    int wtmp = tmp != 0x3Fu;
    int upd = bf(i->d0, 14, 1) || bf(i->d0, 29, 1);
    int wcc = upd || (dst != 0x1Fu && !is_result && !wtmp);
    if (out && dst < 16u) vp_store(c, vp_ovar(c, dst), tv4(s), val, mask, condv);
    if (wtmp) vp_store(c, vp_rvar(c, tmp), tv4(s), val, mask, condv);
    if (wcc) vp_store(c, vp_ccvar(c, bf(i->d0, 25, 1)), tv4(s), val, mask, condv);
}

static u32 vp_txl(vpc* c, const vpi* i, u32 s0)
{
    spv* s = c->s;
    u32 unit = bf(i->d2, 8, 2);
    u32 dim = (c->key->texture_dims >> (unit * 2u)) & 3u;
    if (!c->vtex[unit]) c->vtex[unit] = sampler_var(s, dim, 0, RSX_SPV_BIND_VERTEX_TEXTURE0 + unit);
    u32 img = ld(s, sampler_type(s, dim, 0), c->vtex[unit]);
    u32 coord = coord_for_dim(s, s0, dim == 2 ? 2u : dim);
    return I(s, OpImageSampleExplicitLod, tv4(s), img, coord, IMG_Lod, exf(s, s0, 3));
}

static u32 vp_ins(vpc* c, u32 pc)
{
    spv* s = c->s;
    vpi i;
    vp_decode(c->uc, pc, &i);
    u32 exitv = ci(s, -1);
    if (!vp_srcs_ok(&i)) return exitv;
    u32 vec = bf(i.d1, 22, 5), sca = bf(i.d1, 27, 5), cond = bf(i.d0, 10, 3);
    int unknown = vec > VEC_TXL || sca > SCA_POP;
    if (vec > VEC_TXL) vec = VEC_NOP;
    if (sca > SCA_POP) sca = SCA_NOP;
    u32 vuse = vp_vec_uses(vec);
    int sca_alu = vp_sca_alu(sca);
    u32 sv[3] = { 0, 0, 0 };
    for (u32 n = 0; n < 3; n++)
        if ((vuse & (1u << n)) || (n == 2 && sca_alu)) sv[n] = vp_src(c, &i, n);
    u32 T4 = tv4(s), TF = tf(s);
    u32 vv = 0, vav = 0;
    switch (vec) {
    case VEC_MOV: vv = sv[0]; break;
    case VEC_MUL: vv = fmul(s, T4, sv[0], sv[1]); break;
    case VEC_ADD: vv = fadd(s, T4, sv[0], sv[2]); break;
    case VEC_MAD: vv = X(s, T4, GL_Fma, sv[0], sv[1], sv[2]); break;
    case VEC_DP3: {
        u32 a = I(s, OpVectorShuffle, tv3(s), sv[0], sv[0], 0, 1, 2);
        u32 b = I(s, OpVectorShuffle, tv3(s), sv[1], sv[1], 0, 1, 2);
        vv = splat(s, I(s, OpDot, TF, a, b));
        break;
    }
    case VEC_DPH: vv = splat(s, dot4(s, shuf(s, T4, sv[0], cvs(s, 1.0f), 0, 1, 2, 7), sv[1])); break;
    case VEC_DP4: vv = splat(s, dot4(s, sv[0], sv[1])); break;
    case VEC_DST:
        vv = vec4c(s, cf(s, 1.0f), fmul(s, TF, exf(s, sv[0], 1), exf(s, sv[1], 1)), exf(s, sv[0], 2), exf(s, sv[1], 3));
        break;
    case VEC_MIN: vv = X(s, T4, GL_FMin, sv[0], sv[1]); break;
    case VEC_MAX: vv = X(s, T4, GL_FMax, sv[0], sv[1]); break;
    case VEC_SLT: vv = set_op(s, OpFOrdLessThan, sv[0], sv[1]); break;
    case VEC_SGE: vv = set_op(s, OpFOrdGreaterThanEqual, sv[0], sv[1]); break;
    case VEC_SEQ: vv = set_op(s, OpFOrdEqual, sv[0], sv[1]); break;
    case VEC_SGT: vv = set_op(s, OpFOrdGreaterThan, sv[0], sv[1]); break;
    case VEC_SLE: vv = set_op(s, OpFOrdLessThanEqual, sv[0], sv[1]); break;
    case VEC_SNE: vv = set_op(s, OpFUnordNotEqual, sv[0], sv[1]); break;
    case VEC_SFL: vv = cvs(s, 0.0f); break;
    case VEC_STR: vv = cvs(s, 1.0f); break;
    case VEC_FRC: vv = X(s, T4, GL_Fract, sv[0]); break;
    case VEC_FLR: vv = X(s, T4, GL_Floor, sv[0]); break;
    case VEC_SSG: vv = X(s, T4, GL_FSign, sv[0]); break;
    case VEC_ARL: vav = I(s, OpConvertFToS, ti4(s), X(s, T4, GL_Floor, sv[0])); break;
    case VEC_ARR: vav = I(s, OpConvertFToS, ti4(s), X(s, T4, GL_Floor, fadd(s, T4, sv[0], cvs(s, 0.5f)))); break;
    case VEC_ARA: {
        u32 a = ld(s, ti4(s), vp_avar(c, bf(i.d0, 24, 1)));
        u32 xy = I(s, OpIAdd, ti4(s), a, I(s, OpVectorShuffle, ti4(s), a, a, 2, 3, 0, 1));
        vav = I(s, OpVectorShuffle, ti4(s), xy, xy, 0, 1, 0, 1);
        break;
    }
    case VEC_TXL: vv = vp_txl(c, &i, sv[0]); break;
    default: break;
    }
    u32 sres = 0;
    if (sca_alu) {
        u32 x = sv[2];
        switch (sca) {
        case SCA_MOV: sres = x; break;
        case SCA_RCP: sres = fdiv(s, T4, cvs(s, 1.0f), x); break;
        case SCA_RCC: {
            u32 r = fdiv(s, T4, cvs(s, 1.0f), x);
            u32 m = X(s, T4, GL_FClamp, X(s, T4, GL_FAbs, r), cvs(s, 5.42101086e-20f), cvs(s, 1.84467441e19f));
            u32 sign = uand(s, tu4(s), bc(s, tu4(s), r), cus(s, 0x80000000u));
            sres = bc(s, T4, uor(s, tu4(s), bc(s, tu4(s), m), sign));
            break;
        }
        case SCA_RSQ:
            sres = splat(s, fdiv(s, TF, cf(s, 1.0f), X(s, TF, GL_Sqrt, X(s, TF, GL_FMax, exf(s, x, 0), cf(s, 1e-10f)))));
            break;
        case SCA_EXP: {
            u32 t = exf(s, x, 0);
            u32 fl = X(s, TF, GL_Floor, t);
            sres = vec4c(s, X(s, TF, GL_Exp2, fl), fsub(s, TF, t, fl), X(s, TF, GL_Exp2, t), cf(s, 1.0f));
            break;
        }
        case SCA_LOG: {
            u32 t = X(s, TF, GL_FAbs, exf(s, x, 0));
            u32 l = X(s, TF, GL_Log2, t);
            u32 e = X(s, TF, GL_Floor, l);
            u32 m = fdiv(s, TF, t, X(s, TF, GL_Exp2, e));
            u32 z = I(s, OpFOrdEqual, tbool(s), t, cf(s, 0.0f));
            sres = vec4c(s, e, sel(s, TF, z, cf(s, 1.0f), m), l, cf(s, 1.0f));
            break;
        }
        case SCA_LIT: sres = lit_op(s, x); break;
        case SCA_LG2: sres = X(s, T4, GL_Log2, X(s, T4, GL_FMax, x, cvs(s, 1e-10f))); break;
        case SCA_EX2: sres = X(s, T4, GL_Exp2, x); break;
        case SCA_SIN: sres = X(s, T4, GL_Sin, x); break;
        case SCA_COS: sres = X(s, T4, GL_Cos, x); break;
        default: break;
        }
    }
    u32 condv = 0;
    int ctest = bf(i.d0, 13, 1) && cond != 7 && cond != 0;
    int is_flow = sca == SCA_BRA || sca == SCA_BRI || sca == SCA_CAL || sca == SCA_CLI || sca == SCA_RET;
    if (ctest || (is_flow && cond != 0 && cond != 7)) condv = vp_ccvec(c, &i);
    u32 wcond = ctest ? condv : 0;
    if (cond != 0) {
        if (vav) {
            u32 tmp = bf(i.d0, 15, 6);
            u32 m = vp_vmask(&i);
            if (tmp < 2u && m) vp_store(c, vp_avar(c, tmp), ti4(s), vav, m, wcond);
        } else if (vv) {
            vp_write(c, &i, 0, vv, wcond);
        }
        if (sres) vp_write(c, &i, 1, sres, wcond);
    }
    int end = (int)(i.d3 & 1u) || unknown;
    u32 nxt = pc + 1u < VP_SLOTS ? ci(s, (s32)(pc + 1u)) : exitv;
    u32 fallv = end ? exitv : nxt;
    u32 t = vp_target(&i);
    u32 tgt = t < VP_SLOTS ? ci(s, (s32)t) : exitv;
    switch (sca) {
    case SCA_BRA: {
        u32 taken = vp_cond_any(c, &i, condv);
        return sel(s, ti(s), taken, vp_addr(c, &i), fallv);
    }
    case SCA_BRI:
        if (cond == 7) return tgt;
        if (cond == 0) return end || pc + 1u >= VP_SLOTS ? exitv : 0;
        return sel(s, ti(s), vp_cond_any(c, &i, condv), tgt, fallv);
    case SCA_BRB: case SCA_CLB: case SCA_CAL: case SCA_CLI: {
        u32 taken;
        if (sca == SCA_BRB || sca == SCA_CLB) {
            u32 bits = vp_layout_u32(c, 7);
            u32 b = bitset(s, ushr(s, tu(s), bits, cu(s, bf(i.d3, 23, 5))), 0);
            taken = bf(i.d3, 28, 1) ? b : bnot(s, b);
        } else {
            taken = vp_cond_any(c, &i, condv);
        }
        if (sca != SCA_BRB) {
            u32 sp = ld(s, ti(s), c->spv_);
            u32 slot = X(s, ti(s), GL_SMin, sp, ci(s, 7));
            u32 pe = I(s, OpAccessChain, tptr(s, SC_Function, ti(s)), c->stk, slot);
            u32 old = ld(s, ti(s), pe);
            st(s, pe, sel(s, ti(s), taken, nxt, old));
            u32 inc = X(s, ti(s), GL_SMin, I(s, OpIAdd, ti(s), sp, ci(s, 1)), ci(s, 8));
            st(s, c->spv_, sel(s, ti(s), taken, inc, sp));
        }
        return sel(s, ti(s), taken, tgt, fallv);
    }
    case SCA_RET: {
        u32 taken = vp_cond_any(c, &i, condv);
        u32 sp = ld(s, ti(s), c->spv_);
        u32 has = I(s, OpSGreaterThan, tbool(s), sp, ci(s, 0));
        u32 slot = X(s, ti(s), GL_SMax, I(s, OpISub, ti(s), sp, ci(s, 1)), ci(s, 0));
        u32 ret = ld(s, ti(s), I(s, OpAccessChain, tptr(s, SC_Function, ti(s)), c->stk, slot));
        u32 pop = band(s, taken, has);
        st(s, c->spv_, sel(s, ti(s), pop, slot, sp));
        return sel(s, ti(s), taken, sel(s, ti(s), has, ret, exitv), fallv);
    }
    default:
        break;
    }
    if (end || pc + 1u >= VP_SLOTS) return exitv;
    return 0;
}

static void vp_epilogue(vpc* c)
{
    spv* s = c->s;
    u32 T4 = tv4(s);
    u32 mask = c->key->output_mask;
    u32 dflt = cv4(s, 0.0f, 0.0f, 0.0f, 1.0f);
    u32 pos = (c->written & 1u) ? ld(s, T4, c->o[0]) : dflt;
    u32 L = vp_layout(c);
    u32 pt = tptr(s, SC_Uniform, T4);
    u32 d[4];
    for (u32 r = 0; r < 4; r++) {
        u32 row = ld(s, T4, I(s, OpAccessChain, pt, L, ci(s, 2), ci(s, (s32)r)));
        d[r] = dot4(s, row, pos);
    }
    u32 posv = gvar(s, T4, SC_Output, 0);
    deco1(s, posv, DEC_BuiltIn, BI_Position);
    st(s, posv, vec4c(s, d[0], d[1], d[2], d[3]));
    static const u32 src_reg[RSX_SPV_VAR_COUNT] = { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 6 };
    static const u32 out_bits[RSX_SPV_VAR_COUNT] = {
        RSX_SPV_VPOUT_FRONTDIFFUSE | RSX_SPV_VPOUT_BACKDIFFUSE, RSX_SPV_VPOUT_FRONTSPECULAR | RSX_SPV_VPOUT_BACKSPECULAR,
        RSX_SPV_VPOUT_FRONTDIFFUSE | RSX_SPV_VPOUT_BACKDIFFUSE, RSX_SPV_VPOUT_FRONTSPECULAR | RSX_SPV_VPOUT_BACKSPECULAR,
        RSX_SPV_VPOUT_FOG,
        RSX_SPV_VPOUT_TEX0, RSX_SPV_VPOUT_TEX0 << 1, RSX_SPV_VPOUT_TEX0 << 2, RSX_SPV_VPOUT_TEX0 << 3,
        RSX_SPV_VPOUT_TEX0 << 4, RSX_SPV_VPOUT_TEX0 << 5, RSX_SPV_VPOUT_TEX0 << 6, RSX_SPV_VPOUT_TEX0 << 7,
        RSX_SPV_VPOUT_TEX8, RSX_SPV_VPOUT_TEX9
    };
    for (u32 v = 0; v < RSX_SPV_VAR_COUNT; v++) {
        u32 ov = gvar(s, T4, SC_Output, 0);
        deco1(s, ov, DEC_Location, v);
        u32 reg = src_reg[v];
        u32 val = dflt;
        if ((mask & out_bits[v]) && (c->written & (1u << reg))) {
            val = ld(s, T4, c->o[reg]);
            if (v == RSX_SPV_VAR_FOG) val = splat(s, exf(s, val, 0));
        }
        st(s, ov, val);
    }
    {
        u32 ps = gvar(s, tf(s), SC_Output, 0);
        deco1(s, ps, DEC_BuiltIn, BI_PointSize);
        u32 pf = tptr(s, SC_Uniform, tf(s));
        u32 size = ((mask & RSX_SPV_VPOUT_POINTSIZE) && (c->written & (1u << 6))) ? exf(s, ld(s, T4, c->o[6]), 0)
                                                                                    : ld(s, tf(s), I(s, OpAccessChain, pf, vp_layout(c), ci(s, 8)));
        st(s, ps, fmul(s, tf(s), size, ld(s, tf(s), I(s, OpAccessChain, pf, vp_layout(c), ci(s, 9)))));
    }
    u32 qual = 0;
    for (u32 p = 0; p < 6; p++) {
        u32 reg = p < 3 ? 5u : 6u;
        if ((mask & (RSX_SPV_VPOUT_UC0 << p)) && (c->written & (1u << reg))) qual |= 1u << p;
    }
    if (qual) {
        u32 arr = tarr(s, tf(s), 6, 0);
        u32 cd = gvar(s, arr, SC_Output, 0);
        deco1(s, cd, DEC_BuiltIn, BI_ClipDistance);
        u32 cfg = vp_layout_u32(c, 5);
        u32 r5 = (qual & 7u) ? ld(s, T4, c->o[5]) : 0;
        u32 r6 = (qual & 0x38u) ? ld(s, T4, c->o[6]) : 0;
        u32 pf = tptr(s, SC_Output, tf(s));
        for (u32 p = 0; p < 6; p++) {
            u32 val = cf(s, 0.5f);
            if (qual & (1u << p)) {
                u32 comp = exf(s, p < 3 ? r5 : r6, (p % 3u) + 1u);
                u32 bits = uand(s, tu(s), ushr(s, tu(s), cfg, cu(s, p * 2u)), cu(s, 3));
                u32 en = I(s, OpINotEqual, tbool(s), bits, cu(s, RSX_SPV_CLIP_DISABLED));
                u32 fct = fsub(s, tf(s), I(s, OpConvertUToF, tf(s), bits), cf(s, 1.0f));
                val = sel(s, tf(s), en, fmul(s, tf(s), comp, fct), cf(s, 0.5f));
            }
            st(s, I(s, OpAccessChain, pf, cd, ci(s, (s32)p)), val);
        }
    }
}

int rsx_spv_vertex(const u32* ucode, const rsx_spv_vp_key* key, rsx_spv_blob* out, rsx_spv_vp_info* info)
{
    if (!ucode || !key || key->start >= VP_SLOTS) return -1;
    spv* s = spv_begin();
    if (!s) return -1;
    vpc* c = (vpc*)calloc(1, sizeof(vpc));
    if (!c) return -1;
    c->s = s;
    c->uc = ucode;
    c->key = key;
    vp_walk(c, key->start);
    vp_prescan(c);
    int has_body = (c->written & 1u) != 0;
    if (!has_body) c->written = 0;
    u32 mainid = s->id++;
    fn_begin(s, mainid);
    if (has_body) {
        if (c->indexed_in) {
            u32 arr = tarr(s, tv4(s), 16, 0);
            c->inarr = gvar(s, arr, SC_Private, 0);
        }
        for (u32 k = 0; k < 16; k++) {
            if (!(c->inputs & (1u << k))) continue;
            c->in[k] = vp_fetch(c, k);
            if (c->inarr) st(s, I(s, OpAccessChain, tptr(s, SC_Private, tv4(s)), c->inarr, ci(s, (s32)k)), c->in[k]);
        }
        for (u32 n = 0; n < 16; n++)
            if (c->written & (1u << n)) vp_ovar(c, n);
        if (!c->has_flow && !c->has_bra) {
            u32 pc = key->start;
            while (pc < VP_SLOTS && c->reach[pc]) {
                u32 nx = vp_ins(c, pc);
                if (nx) break;
                pc++;
            }
        } else {
            c->pcv = lvar(s, ti(s));
            c->spv_ = lvar(s, ti(s));
            c->stk = lvar(s, tarr(s, ti(s), 8, 0));
            u32 itv = lvar(s, ti(s));
            st(s, c->pcv, ci(s, (s32)key->start));
            st(s, c->spv_, ci(s, 0));
            st(s, itv, ci(s, 0));
            if (c->has_bra)
                for (u32 pc = 0; pc < VP_SLOTS; pc++)
                    if (c->reach[pc]) c->leader[pc] = 1;
            u32 hdr = newl(s), body = newl(s), cont = newl(s), lmerge = newl(s), smerge = newl(s), dfl = newl(s);
            V(s, OpBranch, hdr);
            label(s, hdr);
            V(s, OpLoopMerge, lmerge, cont, 0);
            V(s, OpBranch, body);
            label(s, body);
            u32 pcval = ld(s, ti(s), c->pcv);
            V(s, OpSelectionMerge, smerge, 0);
            {
                u32 ncase = 0;
                for (u32 pc = 0; pc < VP_SLOTS; pc++)
                    if (c->reach[pc] && c->leader[pc]) ncase++;
                u32* w = sb_grow(s, BODY(s), 3u + ncase * 2u);
                w[0] = ((3u + ncase * 2u) << 16) | OpSwitch;
                w[1] = pcval;
                w[2] = dfl;
                u32 k = 3;
                for (u32 pc = 0; pc < VP_SLOTS; pc++) {
                    if (!(c->reach[pc] && c->leader[pc])) continue;
                    c->label[pc] = newl(s);
                    if (w != s->scratch || k < 16380u) {
                        w[k++] = pc;
                        w[k++] = c->label[pc];
                    }
                }
            }
            label(s, dfl);
            st(s, c->pcv, ci(s, -1));
            V(s, OpBranch, smerge);
            int open = 0;
            for (u32 pc = 0; pc < VP_SLOTS; pc++) {
                if (!c->reach[pc]) {
                    if (open) {
                        st(s, c->pcv, ci(s, -1));
                        V(s, OpBranch, smerge);
                        open = 0;
                    }
                    continue;
                }
                if (c->leader[pc]) {
                    if (open) {
                        st(s, c->pcv, ci(s, (s32)pc));
                        V(s, OpBranch, smerge);
                    }
                    label(s, c->label[pc]);
                    open = 1;
                }
                if (!open) continue;
                u32 nx = vp_ins(c, pc);
                if (nx) {
                    st(s, c->pcv, nx);
                    V(s, OpBranch, smerge);
                    open = 0;
                }
            }
            if (open) {
                st(s, c->pcv, ci(s, -1));
                V(s, OpBranch, smerge);
            }
            label(s, smerge);
            V(s, OpBranch, cont);
            label(s, cont);
            u32 it = I(s, OpIAdd, ti(s), ld(s, ti(s), itv), ci(s, 1));
            st(s, itv, it);
            u32 p2 = ld(s, ti(s), c->pcv);
            u32 ok = band(s, I(s, OpSGreaterThanEqual, tbool(s), p2, ci(s, 0)), I(s, OpSLessThan, tbool(s), it, ci(s, 65536)));
            V(s, OpBranchConditional, ok, hdr, lmerge);
            label(s, lmerge);
        }
    }
    vp_epilogue(c);
    sb_ins(s, BODY(s), OpReturn, k_none, 0);
    fn_end(s);
    u32 caps[3];
    u32 ncaps = 0;
    caps[ncaps++] = 1;
    for (u32 u = 0; u < 4; u++)
        if (c->vtex[u] && ((key->texture_dims >> (u * 2u)) & 3u) == 0) {
            caps[ncaps++] = 43;
            break;
        }
    if ((key->output_mask & (0x3Fu * RSX_SPV_VPOUT_UC0)) && (c->written & 0x60u)) caps[ncaps++] = 32;
    int rc = spv_finish(s, 0, caps, ncaps, mainid, NULL, 0, out);
    if (info) {
        info->instructions = c->count;
        info->input_mask = c->inputs | 1u;
        info->texture_mask = c->texmask;
        memcpy(info->constants_used, c->cused, sizeof info->constants_used);
        info->constants_relative = c->crel;
    }
    free(c);
    return rc;
}

enum {
    FP_NOP = 0x00, FP_MOV = 0x01, FP_MUL = 0x02, FP_ADD = 0x03, FP_MAD = 0x04, FP_DP3 = 0x05, FP_DP4 = 0x06,
    FP_DST = 0x07, FP_MIN = 0x08, FP_MAX = 0x09, FP_SLT = 0x0A, FP_SGE = 0x0B, FP_SLE = 0x0C, FP_SGT = 0x0D,
    FP_SNE = 0x0E, FP_SEQ = 0x0F, FP_FRC = 0x10, FP_FLR = 0x11, FP_KIL = 0x12, FP_PK4 = 0x13, FP_UP4 = 0x14,
    FP_DDX = 0x15, FP_DDY = 0x16, FP_TEX = 0x17, FP_TXP = 0x18, FP_TXD = 0x19, FP_RCP = 0x1A, FP_RSQ = 0x1B,
    FP_EX2 = 0x1C, FP_LG2 = 0x1D, FP_LIT = 0x1E, FP_LRP = 0x1F, FP_STR = 0x20, FP_SFL = 0x21, FP_COS = 0x22,
    FP_SIN = 0x23, FP_PK2 = 0x24, FP_UP2 = 0x25, FP_POW = 0x26, FP_PKB = 0x27, FP_UPB = 0x28, FP_PK16 = 0x29,
    FP_UP16 = 0x2A, FP_BEM = 0x2B, FP_PKG = 0x2C, FP_UPG = 0x2D, FP_DP2A = 0x2E, FP_TXL = 0x2F, FP_TXB = 0x31,
    FP_TEXBEM = 0x33, FP_TXPBEM = 0x34, FP_BEMLUM = 0x35, FP_REFL = 0x36, FP_TIMESWTEX = 0x37, FP_DP2 = 0x38,
    FP_NRM = 0x39, FP_DIV = 0x3A, FP_DIVSQ = 0x3B, FP_LIF = 0x3C, FP_FENCT = 0x3D, FP_FENCB = 0x3E,
    FP_BRK = 0x40, FP_CAL = 0x41, FP_IFE = 0x42, FP_LOOP = 0x43, FP_REP = 0x44, FP_RET = 0x45
};

#define FP_MAX_SLOTS 8192u
#define FP_MAX_SUBS 32

typedef struct fpi {
    u32 d, s0, s1, s2;
    u32 op;
} fpi;

typedef struct fpsub {
    u32 tgt;
    u32 id;
    int busy;
} fpsub;

typedef struct fpc {
    spv* s;
    const u32* uc;
    u32 nslots;
    u32 start;
    u32 nprog;
    const rsx_spv_fp_key* key;
    u16 cmap[FP_MAX_SLOTS + 2];
    u32 nconst;
    u32 cword[RSX_SPV_FP_MAX_CONSTANTS];
    int invalid;
    int nop_shader;
    int depth_write;
    u32 inputs;
    u32 texmask;
    u32 count;
    u32 words;
    u32 r[64];
    u32 cc[2];
    u32 vin[RSX_SPV_VAR_COUNT];
    u32 fragcoord, frontfacing, pointcoord;
    u32 fogv;
    u32 ubo_s, ubo_c;
    u32 tex[16];
    u32 tcfn, texelfn;
    u32 loopk[64];
    u32 lmerge[64];
    int ldepth;
    fpsub subs[FP_MAX_SUBS];
    int nsubs;
} fpc;

static inline u32 hswap(u32 x) { return ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8); }

static void fp_decode(const fpc* c, u32 pc, fpi* i)
{
    const u32* p = c->uc + (size_t)(c->start + pc) * 4u;
    i->d = hswap(p[0]);
    i->s0 = hswap(p[1]);
    i->s1 = hswap(p[2]);
    i->s2 = hswap(p[3]);
    i->op = bf(i->d, 24, 6) | (bf(i->s1, 31, 1) << 6);
}

static int fp_is_flow(u32 op) { return op >= FP_BRK || op == FP_FENCT || op == FP_FENCB; }

static int fp_has_const(const fpi* i)
{
    if (fp_is_flow(i->op)) return 0;
    return (i->s0 & 3u) == 2u || (i->s1 & 3u) == 2u || (i->s2 & 3u) == 2u;
}

static u32 fp_nsrc(u32 op)
{
    switch (op) {
    case FP_NOP: case FP_KIL: case FP_STR: case FP_SFL: case FP_BEMLUM: case FP_TIMESWTEX:
    case FP_FENCT: case FP_FENCB:
        return 0;
    case FP_MUL: case FP_ADD: case FP_DP3: case FP_DP4: case FP_DST: case FP_MIN: case FP_MAX: case FP_SLT:
    case FP_SGE: case FP_SLE: case FP_SGT: case FP_SNE: case FP_SEQ: case FP_POW: case FP_TXL: case FP_TXB:
    case FP_REFL: case FP_DP2: case FP_DIV: case FP_DIVSQ:
        return 2;
    case FP_MAD: case FP_TXD: case FP_LRP: case FP_BEM: case FP_DP2A: case FP_TEXBEM: case FP_TXPBEM:
        return 3;
    default:
        return op >= FP_BRK ? 0u : 1u;
    }
}

static int fp_is_tex(u32 op)
{
    return op == FP_TEX || op == FP_TXP || op == FP_TXD || op == FP_TXB || op == FP_TXL || op == FP_TEXBEM || op == FP_TXPBEM;
}

static int fp_scan(fpc* c, const u32* ucode, u32 max_words)
{
    c->uc = ucode;
    c->nslots = max_words / 4u;
    if (c->nslots > FP_MAX_SLOTS) return -1;
    u32 i0 = 0;
    for (;;) {
        if (i0 >= c->nslots) {
            c->nop_shader = 1;
            c->start = 0;
            c->nprog = 0;
            c->words = max_words & ~3u;
            return 0;
        }
        u32 d = hswap(ucode[i0 * 4u]);
        if (bf(d, 24, 6)) break;
        if (d & 1u) {
            c->nop_shader = 1;
            c->start = i0;
            c->nprog = 1;
            c->count = 1;
            c->words = (i0 + 1u) * 4u;
            return 0;
        }
        i0++;
    }
    c->start = i0;
    u32 lim = c->nslots - c->start;
    u32 lstack[64];
    u32 lsp = 0;
    u32 pc = 0;
    int ended = 0;
    while (pc < lim) {
        fpi i;
        fp_decode(c, pc, &i);
        c->count++;
        while (lsp && pc >= lstack[lsp - 1]) lsp--;
        if (i.op == FP_LOOP || i.op == FP_REP) {
            u32 e = i.s2 >> 2;
            if (e > pc + 1u && lsp < 64) lstack[lsp++] = e;
        }
        if (!fp_is_flow(i.op) && i.op != FP_NOP && bf(i.s0, 18, 3)) {
            u32 ns = fp_nsrc(i.op);
            const u32 srcs[3] = { i.s0, i.s1, i.s2 };
            for (u32 n = 0; n < ns; n++) {
                u32 t = srcs[n] & 3u;
                if (t == 3u) c->invalid = 1;
                if (t != 1u) continue;
                int idx = (int)bf(i.s2, 30, 1);
                u32 reg = idx ? bf(i.s2, 19, 11) + 4u : bf(i.d, 13, 4);
                if (reg >= 15u) c->invalid = 1;
                else if (idx && lsp) c->inputs |= 0x3FF0u;
                else c->inputs |= 1u << reg;
            }
            if (fp_is_tex(i.op)) c->texmask |= 1u << bf(i.d, 17, 4);
            if (!bf(i.d, 30, 1)) {
                u32 m = bf(i.d, 9, 4);
                u32 reg = bf(i.d, 1, 6);
                if (bf(i.d, 7, 1) ? (reg == 3u && (m & 3u)) : (reg == 1u && (m & 4u))) c->depth_write = 1;
            }
        }
        u32 step = 1;
        if (fp_has_const(&i)) {
            if (i.op != FP_NOP) {
                if (c->nconst >= RSX_SPV_FP_MAX_CONSTANTS) return -1;
                c->cword[c->nconst] = (c->start + pc + 1u) * 4u;
                c->cmap[pc] = (u16)(c->nconst + 1u);
                c->nconst++;
            }
            step = 2;
        }
        pc += step;
        if (i.d & 1u) {
            ended = 1;
            break;
        }
    }
    (void)ended;
    if (pc > lim) pc = lim;
    c->nprog = pc;
    c->words = (c->start + pc) * 4u;
    return 0;
}

static void fp_fill_info(const fpc* c, rsx_spv_fp_info* info, int depth_export)
{
    if (!info) return;
    info->instructions = c->count;
    info->constant_count = c->nconst;
    memcpy(info->constant_word, c->cword, (size_t)c->nconst * sizeof(u32));
    info->texture_mask = c->texmask;
    info->input_mask = c->inputs;
    info->depth_export = depth_export;
    info->ucode_words = c->words;
}

int rsx_spv_fragment_scan(const u32* ucode, u32 max_words, rsx_spv_fp_info* info)
{
    if (!ucode) return -1;
    fpc* c = (fpc*)calloc(1, sizeof(fpc));
    if (!c) return -1;
    int rc = fp_scan(c, ucode, max_words);
    if (!rc) fp_fill_info(c, info, c->depth_write);
    free(c);
    return rc;
}

static u32 fp_state(fpc* c)
{
    spv* s = c->s;
    if (c->ubo_s) return c->ubo_s;
    u32 at = tarr(s, tu4(s), RSX_SPV_TEXTURE_UNITS, 1);
    deco1(s, at, DEC_ArrayStride, 16);
    u32 u = tu(s), f = tf(s);
    u32 st_ = intern(s, OpTypeStruct, 0, (const u32[]){ u, u, f, u, f, f, f, f, at, u }, 10, 1);
    static const u32 offs[10] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 288 };
    for (u32 m = 0; m < 10; m++) mdeco(s, st_, m, DEC_Offset, offs[m]);
    deco(s, st_, DEC_Block);
    c->ubo_s = gvar(s, st_, SC_Uniform, 0);
    deco1(s, c->ubo_s, DEC_DescriptorSet, 0);
    deco1(s, c->ubo_s, DEC_Binding, RSX_SPV_BIND_FP_STATE);
    return c->ubo_s;
}

static u32 fp_state_u(fpc* c, u32 m)
{
    spv* s = c->s;
    return ld(s, tu(s), I(s, OpAccessChain, tptr(s, SC_Uniform, tu(s)), fp_state(c), ci(s, (s32)m)));
}

static u32 fp_state_f(fpc* c, u32 m)
{
    spv* s = c->s;
    return ld(s, tf(s), I(s, OpAccessChain, tptr(s, SC_Uniform, tf(s)), fp_state(c), ci(s, (s32)m)));
}

static u32 fp_texstate(fpc* c, u32 unit)
{
    spv* s = c->s;
    return ld(s, tu4(s), I(s, OpAccessChain, tptr(s, SC_Uniform, tu4(s)), fp_state(c), ci(s, 8), ci(s, (s32)unit)));
}

static u32 fp_rvar(fpc* c, u32 n)
{
    if (!c->r[n]) c->r[n] = gvar(c->s, tu4(c->s), SC_Private, cu4z(c->s));
    return c->r[n];
}

static u32 fp_ccvar(fpc* c, u32 n)
{
    if (!c->cc[n]) c->cc[n] = gvar(c->s, tv4(c->s), SC_Private, cvs(c->s, 0.0f));
    return c->cc[n];
}

static u32 fp_readR(fpc* c, u32 n)
{
    spv* s = c->s;
    return bc(s, tv4(s), ld(s, tu4(s), fp_rvar(c, n)));
}

static u32 fp_readH(fpc* c, u32 m)
{
    spv* s = c->s;
    u32 v = ld(s, tu4(s), fp_rvar(c, m >> 1));
    u32 b = (m & 1u) * 2u;
    u32 lo = X(s, tv2(s), GL_UnpackHalf2x16, ex(s, tu(s), v, b));
    u32 hi = X(s, tv2(s), GL_UnpackHalf2x16, ex(s, tu(s), v, b + 1u));
    return shuf(s, tv4(s), lo, hi, 0, 1, 2, 3);
}

static u32 f16_quantize(spv* s, u32 v)
{
    u32 raw = bc(s, tu4(s), v);
    u32 normal = bc(s, tv4(s), uand(s, tu4(s), raw, cus(s, 0xFFFFE000u)));
    u32 tr = fmul(s, tv4(s), X(s, tv4(s), GL_Trunc, fmul(s, tv4(s), normal, cvs(s, 16777216.0f))), cvs(s, 1.0f / 16777216.0f));
    u32 ovf = I(s, OpUGreaterThanEqual, tb4(s), uand(s, tu4(s), raw, cus(s, 0x7FFFFFFFu)), cus(s, 0x47800000u));
    return sel(s, tv4(s), ovf, fmul(s, tv4(s), v, cvs(s, 1e35f)), tr);
}

static u32 f16_32_clamp(spv* s, u32 v)
{
    u32 raw = bc(s, tu4(s), v);
    u32 den = I(s, OpIEqual, tb4(s), uand(s, tu4(s), raw, cus(s, 0x7F800000u)), cu4z(s));
    u32 fl = sel(s, tv4(s), den, cvs(s, 0.0f), v);
    u32 ovf = I(s, OpFOrdGreaterThanEqual, tb4(s), X(s, tv4(s), GL_FAbs, v), cvs(s, 65536.0f));
    return sel(s, tv4(s), ovf, fmul(s, tv4(s), v, cvs(s, 1e35f)), fl);
}

static u32 precision_clamp(spv* s, u32 v, float lo, float hi)
{
    u32 m = uand(s, tu4(s), bc(s, tu4(s), v), cus(s, 0x7FFFFFFFu));
    u32 bad = I(s, OpUGreaterThan, tb4(s), I(s, OpISub, tu4(s), m, cus(s, 0x00800000u)), cus(s, 0x7F000000u));
    u32 f = sel(s, tv4(s), bad, cvs(s, 0.0f), v);
    return X(s, tv4(s), GL_FClamp, f, cvs(s, lo), cvs(s, hi));
}

static u32 clamp_prec(spv* s, u32 v, u32 prec)
{
    switch (prec) {
    case 1: return f16_quantize(s, v);
    case 2: return precision_clamp(s, v, -2.0f, 2.0f);
    case 3: return precision_clamp(s, v, -1.0f, 1.0f);
    case 4: return precision_clamp(s, v, 0.0f, 1.0f);
    default: return v;
    }
}

static void fp_writeR(fpc* c, u32 n, u32 v, u32 mask, u32 condv)
{
    spv* s = c->s;
    u32 var = fp_rvar(c, n);
    u32 nb = bc(s, tu4(s), v);
    if (mask == 0xFu && !condv) {
        st(s, var, nb);
        return;
    }
    u32 old = ld(s, tu4(s), var);
    if (condv) nb = sel(s, tu4(s), condv, nb, old);
    st(s, var, mask4(s, old, nb, tu4(s), mask));
}

static void fp_writeH(fpc* c, u32 m, u32 v, u32 mask, u32 condv)
{
    spv* s = c->s;
    u32 q = f16_quantize(s, v);
    u32 w[2];
    w[0] = X(s, tu(s), GL_PackHalf2x16, I(s, OpVectorShuffle, tv2(s), q, q, 0, 1));
    w[1] = X(s, tu(s), GL_PackHalf2x16, I(s, OpVectorShuffle, tv2(s), q, q, 2, 3));
    u32 var = fp_rvar(c, m >> 1);
    u32 old = ld(s, tu4(s), var);
    u32 base = (m & 1u) * 2u;
    u32 nv = old;
    for (u32 k = 0; k < 2; k++) {
        u32 lm = (mask >> (k * 2u)) & 3u;
        if (!lm) continue;
        u32 o = ex(s, tu(s), old, base + k);
        u32 msk;
        if (condv) {
            u32 m0 = (lm & 1u) ? sel(s, tu(s), ex(s, tbool(s), condv, k * 2u), cu(s, 0x0000FFFFu), cu(s, 0)) : cu(s, 0);
            u32 m1 = (lm & 2u) ? sel(s, tu(s), ex(s, tbool(s), condv, k * 2u + 1u), cu(s, 0xFFFF0000u), cu(s, 0)) : cu(s, 0);
            msk = uor(s, tu(s), m0, m1);
        } else {
            u32 cm = ((lm & 1u) ? 0x0000FFFFu : 0u) | ((lm & 2u) ? 0xFFFF0000u : 0u);
            if (cm == 0xFFFFFFFFu) {
                nv = I(s, OpCompositeInsert, tu4(s), w[k], nv, base + k);
                continue;
            }
            msk = cu(s, cm);
        }
        u32 keep = uand(s, tu(s), o, I(s, OpNot, tu(s), msk));
        u32 put = uand(s, tu(s), w[k], msk);
        nv = I(s, OpCompositeInsert, tu4(s), uor(s, tu(s), keep, put), nv, base + k);
    }
    st(s, var, nv);
}

static u32 fp_ccvec(fpc* c, const fpi* i)
{
    spv* s = c->s;
    u32 cc = ld(s, tv4(s), fp_ccvar(c, bf(i->s0, 31, 1)));
    u32 sw = shuf(s, tv4(s), cc, cc, bf(i->s0, 21, 2), bf(i->s0, 23, 2), bf(i->s0, 25, 2), bf(i->s0, 27, 2));
    u32 lt = bf(i->s0, 18, 1), eq = bf(i->s0, 19, 1), gr = bf(i->s0, 20, 1);
    u32 op;
    if (gr && eq) op = OpFOrdGreaterThanEqual;
    else if (lt && eq) op = OpFOrdLessThanEqual;
    else if (gr && lt) op = OpFUnordNotEqual;
    else if (gr) op = OpFOrdGreaterThan;
    else if (lt) op = OpFOrdLessThan;
    else op = OpFOrdEqual;
    return I(s, op, tb4(s), sw, cvs(s, 0.0f));
}

static u32 fp_cond_any(fpc* c, const fpi* i)
{
    spv* s = c->s;
    u32 e = bf(i->s0, 18, 3);
    if (e == 7u) return cb(s, 1);
    if (e == 0u) return cb(s, 0);
    return I(s, OpAny, tbool(s), fp_ccvec(c, i));
}

static u32 fp_invar(fpc* c, u32 loc)
{
    spv* s = c->s;
    if (!c->vin[loc]) {
        c->vin[loc] = gvar(s, tv4(s), SC_Input, 0);
        deco1(s, c->vin[loc], DEC_Location, loc);
        if (loc <= RSX_SPV_VAR_BFC1 && (c->key->control & RSX_SPV_FPCTL_FLAT_SHADING)) deco(s, c->vin[loc], DEC_Flat);
    }
    return ld(s, tv4(s), c->vin[loc]);
}

static u32 fp_fragcoord(fpc* c)
{
    spv* s = c->s;
    if (!c->fragcoord) {
        c->fragcoord = gvar(s, tv4(s), SC_Input, 0);
        deco1(s, c->fragcoord, DEC_BuiltIn, BI_FragCoord);
    }
    return ld(s, tv4(s), c->fragcoord);
}

static u32 fp_front(fpc* c)
{
    spv* s = c->s;
    if (!c->frontfacing) {
        c->frontfacing = gvar(s, tbool(s), SC_Input, 0);
        deco1(s, c->frontfacing, DEC_BuiltIn, BI_FrontFacing);
    }
    return ld(s, tbool(s), c->frontfacing);
}

static u32 fp_pointcoord(fpc* c)
{
    spv* s = c->s;
    if (!c->pointcoord) {
        c->pointcoord = gvar(s, tv2(s), SC_Input, 0);
        deco1(s, c->pointcoord, DEC_BuiltIn, BI_PointCoord);
    }
    return ld(s, tv2(s), c->pointcoord);
}

static u32 fp_color(fpc* c, u32 n)
{
    spv* s = c->s;
    u32 front = fp_invar(c, RSX_SPV_VAR_COL0 + n);
    u32 back = fp_invar(c, RSX_SPV_VAR_BFC0 + n);
    u32 two = bitset(s, fp_state_u(c, 0), 1);
    u32 pick = band(s, two, bnot(s, fp_front(c)));
    u32 v = sel(s, tv4(s), pick, back, front);
    return X(s, tv4(s), GL_FClamp, v, cvs(s, 0.0f), cvs(s, 1.0f));
}

static u32 fp_wpos(fpc* c)
{
    spv* s = c->s;
    u32 fc = fp_fragcoord(c);
    u32 sc = fp_state_f(c, 6);
    u32 bias = fp_state_f(c, 7);
    u32 pci = bitset(s, fp_state_u(c, 0), 2);
    u32 bx = sel(s, tf(s), pci, cf(s, -0.5f), cf(s, 0.0f));
    u32 mul = vec4c(s, X(s, tf(s), GL_FAbs, sc), sc, cf(s, 1.0f), cf(s, 1.0f));
    u32 add = vec4c(s, bx, bias, cf(s, 0.0f), cf(s, 0.0f));
    return fadd(s, tv4(s), fmul(s, tv4(s), fc, mul), add);
}

static u32 fp_fog_compute(fpc* c)
{
    spv* s = c->s;
    u32 TF = tf(s);
    u32 x = exf(s, fp_invar(c, RSX_SPV_VAR_FOG), 0);
    u32 ax = X(s, TF, GL_FAbs, x);
    u32 p0 = fp_state_f(c, 4), p1 = fp_state_f(c, 5);
    u32 mode = fp_state_u(c, 3);
    u32 lin = fadd(s, TF, fmul(s, TF, p1, x), fsub(s, TF, p0, cf(s, 1.0f)));
    u32 lina = fadd(s, TF, fmul(s, TF, p1, ax), fsub(s, TF, p0, cf(s, 1.0f)));
    u32 e1 = fsub(s, TF, fadd(s, TF, fmul(s, TF, p1, x), p0), cf(s, 1.5f));
    u32 e1a = fsub(s, TF, fadd(s, TF, fmul(s, TF, p1, ax), p0), cf(s, 1.5f));
    u32 ex1 = X(s, TF, GL_Exp, fmul(s, TF, cf(s, 11.084f), e1));
    u32 ex1a = X(s, TF, GL_Exp, fmul(s, TF, cf(s, 11.084f), e1a));
    u32 t2 = fmul(s, TF, cf(s, 4.709f), e1);
    u32 t2a = fmul(s, TF, cf(s, 4.709f), e1a);
    u32 ex2 = X(s, TF, GL_Exp, I(s, OpFNegate, TF, fmul(s, TF, t2, t2)));
    u32 ex2a = X(s, TF, GL_Exp, I(s, OpFNegate, TF, fmul(s, TF, t2a, t2a)));
    u32 y = cf(s, 0.0f);
    u32 known = cb(s, 0);
    const u32 modes[6] = { RSX_SPV_FOG_LINEAR, RSX_SPV_FOG_EXP, RSX_SPV_FOG_EXP2, RSX_SPV_FOG_EXP_ABS, RSX_SPV_FOG_EXP2_ABS, RSX_SPV_FOG_LINEAR_ABS };
    const u32 vals[6] = { lin, ex1, ex2, ex1a, ex2a, lina };
    for (u32 k = 0; k < 6; k++) {
        u32 is = I(s, OpIEqual, tbool(s), mode, cu(s, modes[k]));
        y = sel(s, TF, is, vals[k], y);
        known = bor(s, known, is);
    }
    u32 yc = X(s, TF, GL_FClamp, y, cf(s, 0.0f), cf(s, 1.0f));
    y = sel(s, TF, known, yc, cf(s, 0.0f));
    return vec4c(s, x, y, cf(s, 0.0f), cf(s, 0.0f));
}

static u32 bitrt(spv* s, u32 v, u32 bit)
{
    return I(s, OpINotEqual, tbool(s), uand(s, tu(s), ushr(s, tu(s), v, bit), cu(s, 1)), cu(s, 0));
}

static u32 fp_texcoord_body(fpc* c, u32 v, u32 t, u32 persp)
{
    spv* s = c->s;
    u32 T4 = tv4(s);
    u32 fc = fp_fragcoord(c);
    u32 fw = exf(s, fc, 3);
    u32 ctl = fp_state_u(c, 9);
    u32 is2d = bitrt(s, ctl, t);
    u32 ispc = bitrt(s, ctl, I(s, OpIAdd, tu(s), t, cu(s, 16)));
    u32 in_w = fdiv(s, tf(s), cf(s, 1.0f), fw);
    u32 pcd = fp_pointcoord(c);
    u32 pcx = exf(s, pcd, 0), pcy = exf(s, pcd, 1);
    u32 pc2 = vec4c(s, pcx, pcy, cf(s, 0.0f), in_w);
    u32 pc1 = vec4c(s, pcx, pcy, cf(s, 1.0f), cf(s, 0.0f));
    u32 pcv = sel(s, T4, is2d, pc2, pc1);
    u32 m = I(s, OpVectorTimesScalar, T4, v, fw);
    u32 pd2 = vec4c(s, exf(s, m, 0), exf(s, m, 1), cf(s, 0.0f), cf(s, 1.0f));
    u32 np = sel(s, T4, is2d, pd2, m);
    u32 d2 = vec4c(s, exf(s, v, 0), exf(s, v, 1), cf(s, 0.0f), in_w);
    u32 nn = sel(s, T4, is2d, d2, v);
    return sel(s, T4, ispc, pcv, sel(s, T4, persp, np, nn));
}

static u32 fp_texcoord(fpc* c, u32 t, int persp)
{
    spv* s = c->s;
    if (!c->tcfn) {
        c->tcfn = s->id++;
        const u32 pt[3] = { tv4(s), tu(s), tbool(s) };
        fn_begin_t(s, c->tcfn, tv4(s), 3, pt);
        frame* f = &s->fr[s->depth];
        u32 r = fp_texcoord_body(c, f->pid[0], f->pid[1], f->pid[2]);
        V(s, 254, r);
        fn_end(s);
    }
    return I(s, OpFunctionCall, tv4(s), c->tcfn, fp_invar(c, RSX_SPV_VAR_TEX0 + t), cu(s, t), cb(s, persp));
}

static u32 fp_input(fpc* c, const fpi* i, u32* prec)
{
    spv* s = c->s;
    int idx = (int)bf(i->s2, 30, 1);
    u32 addr = bf(i->s2, 19, 11);
    u32 reg = idx ? addr + 4u : bf(i->d, 13, 4);
    int persp = (int)bf(i->s2, 31, 1);
    if (idx && c->ldepth > 0 && reg < 15u) {
        u32 k = ld(s, ti(s), c->loopk[c->ldepth - 1]);
        u32 ix = I(s, OpIAdd, ti(s), k, ci(s, (s32)addr));
        u32 r = cv4(s, 0.0f, 0.0f, 0.0f, 1.0f);
        for (u32 t = 0; t < 10; t++)
            r = sel(s, tv4(s), I(s, OpIEqual, tbool(s), ix, ci(s, (s32)t)), fp_invar(c, RSX_SPV_VAR_TEX0 + t), r);
        if (persp) r = I(s, OpVectorTimesScalar, tv4(s), r, exf(s, fp_fragcoord(c), 3));
        return r;
    }
    switch (reg) {
    case 0: return fp_wpos(c);
    case 1: case 2:
        *prec = 0;
        return fp_color(c, reg - 1u);
    case 3: return c->fogv ? ld(s, tv4(s), c->fogv) : fp_fog_compute(c);
    case 14:
        *prec = 0;
        return sel(s, tv4(s), fp_front(c), cvs(s, 1.0f), cvs(s, -1.0f));
    default:
        if (reg >= 4u && reg <= 13u) return fp_texcoord(c, reg - 4u, persp);
        return cvs(s, 0.0f);
    }
}

static u32 fp_consts(fpc* c)
{
    spv* s = c->s;
    if (c->ubo_c) return c->ubo_c;
    u32 arr = tarr(s, tv4(s), c->nconst ? c->nconst : 1u, 1);
    deco1(s, arr, DEC_ArrayStride, 16);
    u32 st_ = intern(s, OpTypeStruct, 0, &arr, 1, 1);
    mdeco(s, st_, 0, DEC_Offset, 0);
    deco(s, st_, DEC_Block);
    c->ubo_c = gvar(s, st_, SC_Uniform, 0);
    deco1(s, c->ubo_c, DEC_DescriptorSet, 0);
    deco1(s, c->ubo_c, DEC_Binding, RSX_SPV_BIND_FP_CONSTANTS);
    return c->ubo_c;
}

static u32 fp_src(fpc* c, const fpi* i, u32 n, u32 pc)
{
    spv* s = c->s;
    u32 sr = n == 0 ? i->s0 : n == 1 ? i->s1 : i->s2;
    u32 prec = bf(i->s1, 19 + n * 3u, 3);
    u32 v;
    switch (sr & 3u) {
    case 0: {
        u32 fp16 = bf(sr, 8, 1);
        u32 idx = bf(sr, 2, 6);
        if (fp16 && prec == 1u) prec = 0;
        v = fp16 ? fp_readH(c, idx) : fp_readR(c, idx);
        break;
    }
    case 1:
        v = fp_input(c, i, &prec);
        break;
    case 2: {
        u32 k = c->cmap[pc];
        if (!k) return cvs(s, 0.0f);
        v = ld(s, tv4(s), I(s, OpAccessChain, tptr(s, SC_Uniform, tv4(s)), fp_consts(c), ci(s, 0), ci(s, (s32)(k - 1u))));
        break;
    }
    default:
        return cvs(s, 0.0f);
    }
    u32 sx = bf(sr, 9, 2), sy = bf(sr, 11, 2), sz = bf(sr, 13, 2), sw = bf(sr, 15, 2);
    if (sx != 0 || sy != 1 || sz != 2 || sw != 3) v = shuf(s, tv4(s), v, v, sx, sy, sz, sw);
    u32 ab = n == 0 ? bf(sr, 29, 1) : bf(sr, 18, 1);
    if (prec == 4u) v = clamp_prec(s, v, prec);
    if (ab) v = X(s, tv4(s), GL_FAbs, v);
    if (prec && prec != 4u) v = clamp_prec(s, v, prec);
    if (bf(sr, 17, 1)) v = I(s, OpFNegate, tv4(s), v);
    return v;
}

static u32 srgb_to_linear(spv* s, u32 cs)
{
    u32 T4 = tv4(s);
    u32 a = fdiv(s, T4, cs, cvs(s, 12.92f));
    u32 b = X(s, T4, GL_Pow, fdiv(s, T4, fadd(s, T4, cs, cvs(s, 0.055f)), cvs(s, 1.055f)), cvs(s, 2.4f));
    return sel(s, T4, I(s, OpFOrdGreaterThan, tb4(s), cs, cvs(s, 0.04045f)), b, a);
}

static u32 linear_to_srgb(spv* s, u32 cl)
{
    u32 T4 = tv4(s);
    u32 low = fmul(s, T4, cl, cvs(s, 12.92f));
    u32 high = fsub(s, T4, fmul(s, T4, cvs(s, 1.055f), X(s, T4, GL_Pow, cl, cvs(s, 1.0f / 2.4f))), cvs(s, 0.055f));
    u32 lt = I(s, OpFOrdLessThan, tb4(s), cl, cvs(s, 0.0031308f));
    return X(s, T4, GL_FClamp, sel(s, T4, lt, low, high), cvs(s, 0.0f), cvs(s, 1.0f));
}

static u32 sext_norm(spv* s, u32 x, int bits16)
{
    float scale = bits16 ? 65535.0f : 255.0f;
    u32 shift = bits16 ? 16u : 24u;
    u32 ub = I(s, OpConvertFToS, ti4(s), X(s, tv4(s), GL_Floor, X(s, tv4(s), GL_Fma, x, cvs(s, scale), cvs(s, 0.5f))));
    u32 sh = I(s, OpCompositeConstruct, ti4(s), ci(s, (s32)shift), ci(s, (s32)shift), ci(s, (s32)shift), ci(s, (s32)shift));
    u32 ib = I(s, OpShiftRightArithmetic, ti4(s), I(s, OpShiftLeftLogical, ti4(s), ub, sh), sh);
    return fdiv(s, tv4(s), I(s, OpConvertSToF, tv4(s), ib), cvs(s, bits16 ? 32767.0f : 127.0f));
}

static u32 fp_process_texel_body(fpc* c, u32 rgba, u32 ctl, u32 bx2)
{
    spv* s = c->s;
    u32 T4 = tv4(s);
    u32 ak = I(s, OpLogicalAnd, tbool(s), bitset(s, ctl, 12), I(s, OpFOrdLessThan, tbool(s), exf(s, rgba, 3), cf(s, 0.000001f)));
    demote_if(s, ak);
    u32 ren = bitset(s, ctl, 13);
    rgba = sel(s, T4, ren, fdiv(s, T4, X(s, T4, GL_Floor, fmul(s, T4, rgba, cvs(s, 255.0f))), cvs(s, 255.0f)), rgba);
    u32 is16 = bitset(s, ctl, 17);
    u32 sx = bitmask4(s, ctl, RSX_SPV_TEXC_SEXT_SHIFT);
    u32 conv = sel(s, T4, is16, sext_norm(s, rgba, 1), sext_norm(s, rgba, 0));
    rgba = sel(s, T4, sx, conv, rgba);
    u32 nsx = I(s, OpLogicalNot, tb4(s), sx);
    u32 gm = I(s, OpLogicalAnd, tb4(s), bitmask4(s, ctl, RSX_SPV_TEXC_GAMMA_SHIFT), nsx);
    rgba = sel(s, T4, gm, srgb_to_linear(s, rgba), rgba);
    u32 ch = I(s, OpLogicalAnd, tb4(s), nsx, I(s, OpLogicalNot, tb4(s), gm));
    u32 exm = I(s, OpLogicalAnd, tb4(s), bitmask4(s, ctl, RSX_SPV_TEXC_EXPAND_SHIFT), ch);
    u32 e16 = fdiv(s, T4, fsub(s, T4, X(s, T4, GL_Floor, X(s, T4, GL_Fma, rgba, cvs(s, 65535.0f), cvs(s, 0.5f))), cvs(s, 32768.0f)), cvs(s, 32767.0f));
    u32 e8 = fdiv(s, T4, fsub(s, T4, X(s, T4, GL_Floor, X(s, T4, GL_Fma, rgba, cvs(s, 255.0f), cvs(s, 0.5f))), cvs(s, 128.0f)), cvs(s, 127.0f));
    u32 eb = fsub(s, T4, fmul(s, T4, rgba, cvs(s, 2.0f)), cvs(s, 1.0f));
    u32 e = sel(s, T4, bx2, eb, sel(s, T4, is16, e16, e8));
    return sel(s, T4, exm, e, rgba);
}

static u32 fp_process_texel(fpc* c, u32 rgba, u32 ctl, u32 bx2)
{
    spv* s = c->s;
    if (!c->texelfn) {
        c->texelfn = s->id++;
        const u32 pt[3] = { tv4(s), tu(s), tbool(s) };
        fn_begin_t(s, c->texelfn, tv4(s), 3, pt);
        frame* f = &s->fr[s->depth];
        u32 r = fp_process_texel_body(c, f->pid[0], f->pid[1], f->pid[2]);
        V(s, 254, r);
        fn_end(s);
    }
    return I(s, OpFunctionCall, tv4(s), c->texelfn, rgba, ctl, bx2 ? bx2 : cb(s, 0));
}

static u32 fp_bem(spv* s, u32 a, u32 b, u32 c)
{
    u32 T4 = tv4(s);
    u32 t0 = shuf(s, T4, a, a, 0, 1, 0, 1);
    u32 t1 = fmul(s, T4, shuf(s, T4, b, b, 0, 0, 0, 0), shuf(s, T4, c, c, 0, 2, 0, 2));
    u32 t2 = fmul(s, T4, shuf(s, T4, b, b, 1, 1, 1, 1), shuf(s, T4, c, c, 1, 3, 1, 3));
    return fadd(s, T4, fadd(s, T4, t0, t1), t2);
}

static u32 fp_tex(fpc* c, const fpi* i, u32 op, u32 s0, u32 s1, u32 s2)
{
    spv* s = c->s;
    u32 T4 = tv4(s), TF = tf(s);
    u32 unit = bf(i->d, 17, 4);
    u32 dim = (c->key->texture_dims >> (unit * 2u)) & 3u;
    int shadow = (int)((c->key->texture_shadow >> unit) & 1u);
    u32 coord = s0;
    if (op == FP_TEXBEM || op == FP_TXPBEM) {
        coord = fp_bem(s, s0, s1, s2);
        op = op == FP_TEXBEM ? FP_TEX : FP_TXP;
    }
    int proj = op == FP_TXP;
    if (shadow && (dim == 0 || dim == 3 || (dim == 2 && proj))) return cvs(s, 0.0f);
    if (!c->tex[unit]) c->tex[unit] = sampler_var(s, dim, shadow, RSX_SPV_BIND_TEXTURE0 + unit);
    u32 img = ld(s, sampler_type(s, dim, shadow), c->tex[unit]);
    u32 ts = fp_texstate(c, unit);
    u32 ctl = ex(s, tu(s), ts, 0);
    u32 w = exf(s, coord, 3);
    if (proj) coord = I(s, OpVectorTimesScalar, T4, coord, fdiv(s, TF, cf(s, 1.0f), w));
    u32 unn = bitset(s, ctl, 15);
    u32 sc = bc(s, tv2(s), I(s, OpVectorShuffle, tu2(s), ts, ts, 2, 3));
    u32 fct = sel(s, tv2(s), unn, sc, cv2(s, 1.0f, 1.0f));
    if (shadow) {
        u32 res;
        if (dim == 1) {
            u32 xy = fmul(s, tv2(s), I(s, OpVectorShuffle, tv2(s), coord, coord, 0, 1), fct);
            u32 ref = proj ? fmul(s, TF, exf(s, s0, 2), fdiv(s, TF, cf(s, 1.0f), w)) : exf(s, coord, 2);
            ref = sel(s, TF, bitset(s, ctl, 14), ref, X(s, TF, GL_FMin, ref, cf(s, 1.0f)));
            res = I(s, OpImageSampleDrefImplicitLod, TF, img, xy, ref);
        } else {
            u32 xyz = I(s, OpVectorShuffle, tv3(s), coord, coord, 0, 1, 2);
            u32 ref = exf(s, coord, 3);
            ref = sel(s, TF, bitset(s, ctl, 14), ref, X(s, TF, GL_FMin, ref, cf(s, 1.0f)));
            res = I(s, OpImageSampleDrefImplicitLod, TF, img, xyz, ref);
        }
        return splat(s, res);
    }
    u32 cd;
    if (dim == 0) cd = fmul(s, TF, exf(s, coord, 0), exf(s, fct, 0));
    else if (dim == 1) cd = fmul(s, tv2(s), I(s, OpVectorShuffle, tv2(s), coord, coord, 0, 1), fct);
    else if (dim == 2) cd = I(s, OpVectorShuffle, tv3(s), coord, coord, 0, 1, 2);
    else {
        u32 f3 = I(s, OpCompositeConstruct, tv3(s), exf(s, fct, 0), exf(s, fct, 1), cf(s, 1.0f));
        cd = fmul(s, tv3(s), I(s, OpVectorShuffle, tv3(s), coord, coord, 0, 1, 2), f3);
    }
    u32 res;
    switch (op) {
    case FP_TXB:
        res = I(s, OpImageSampleImplicitLod, T4, img, cd, IMG_Bias, exf(s, s1, 0));
        break;
    case FP_TXL:
        res = I(s, OpImageSampleExplicitLod, T4, img, cd, IMG_Lod, exf(s, s1, 0));
        break;
    case FP_TXD: {
        u32 gx = coord_for_dim(s, s1, dim == 3 ? 2u : dim);
        u32 gy = coord_for_dim(s, s2, dim == 3 ? 2u : dim);
        res = I(s, OpImageSampleExplicitLod, T4, img, cd, IMG_Grad, gx, gy);
        break;
    }
    default:
        res = I(s, OpImageSampleImplicitLod, T4, img, cd);
        break;
    }
    if (bf(i->d, 21, 1)) {
        u32 cap = bitset(s, ctl, 16);
        u32 remap = ex(s, tu(s), ts, 1);
        u32 ov = ushl(s, tu(s), uand(s, tu(s), ushr(s, tu(s), remap, cu(s, RSX_SPV_TEXREMAP_BX2_SHIFT)), cu(s, 15)), cu(s, RSX_SPV_TEXC_EXPAND_SHIFT));
        u32 nc = uor(s, tu(s), uand(s, tu(s), ctl, cu(s, ~0xFu)), ov);
        ctl = sel(s, tu(s), cap, nc, ctl);
        return fp_process_texel(c, res, ctl, cap);
    }
    return fp_process_texel(c, res, ctl, 0);
}

static int fp_exempt(u32 op)
{
    switch (op) {
    case FP_NRM: case FP_MAX: case FP_MIN: case FP_COS: case FP_SIN: case FP_REFL: case FP_FRC: case FP_LIT:
    case FP_LIF: case FP_LG2:
        return 1;
    default:
        return 0;
    }
}

static void fp_setdst(fpc* c, const fpi* i, u32 v)
{
    spv* s = c->s;
    u32 T4 = tv4(s);
    switch (bf(i->s1, 28, 3)) {
    case 1: v = fmul(s, T4, v, cvs(s, 2.0f)); break;
    case 2: v = fmul(s, T4, v, cvs(s, 4.0f)); break;
    case 3: v = fmul(s, T4, v, cvs(s, 8.0f)); break;
    case 5: v = fdiv(s, T4, v, cvs(s, 2.0f)); break;
    case 6: v = fdiv(s, T4, v, cvs(s, 4.0f)); break;
    case 7: v = fdiv(s, T4, v, cvs(s, 8.0f)); break;
    default: break;
    }
    u32 exec = bf(i->s0, 18, 3);
    u32 mask = bf(i->d, 9, 4);
    u32 fp16 = bf(i->d, 7, 1);
    u32 dreg = bf(i->d, 1, 6);
    int setc = (int)bf(i->d, 8, 1);
    u32 ccw = bf(i->s0, 30, 1);
    if (bf(i->d, 30, 1)) {
        if (!setc || !mask) return;
        u32 var = fp_ccvar(c, ccw);
        u32 old = ld(s, T4, var);
        u32 nv = mask4(s, old, v, T4, mask);
        if (exec != 7u) nv = sel(s, T4, I(s, OpAny, tbool(s), fp_ccvec(c, i)), nv, old);
        st(s, var, nv);
        return;
    }
    if (bf(i->d, 31, 1)) {
        v = precision_clamp(s, v, 0.0f, 1.0f);
    } else {
        u32 prec = bf(i->d, 22, 2);
        if (prec || fp16) {
            u32 op = i->op;
            u32 outp = prec ? prec : (fp16 ? 1u : 0u);
            int ex_ = fp_exempt(op);
            if (op == FP_MOV && fp16 && bf(i->s0, 8, 1) && (i->s0 & 3u) == 0u) ex_ = 1;
            if (!ex_) {
                if (outp == 1u && op == FP_UP2) {
                } else if (outp == 1u && !fp16) {
                    v = f16_32_clamp(s, v);
                } else if (outp != 1u) {
                    v = clamp_prec(s, v, outp);
                }
            }
        }
    }
    if (!mask) return;
    u32 condv = exec != 7u ? fp_ccvec(c, i) : 0;
    if (fp16) fp_writeH(c, dreg, v, mask, condv);
    else fp_writeR(c, dreg, v, mask, condv);
    if (setc) {
        u32 back = fp16 ? fp_readH(c, dreg) : fp_readR(c, dreg);
        u32 var = fp_ccvar(c, ccw);
        u32 old = ld(s, T4, var);
        st(s, var, mask4(s, old, back, T4, mask));
    }
}

static u32 fp_bits_x(spv* s, u32 v) { return bc(s, tu(s), exf(s, v, 0)); }

static void fp_alu(fpc* c, const fpi* i, u32 pc)
{
    spv* s = c->s;
    if (!bf(i->s0, 18, 3)) return;
    u32 op = i->op;
    if (op == FP_BEMLUM || op == FP_TIMESWTEX || op == 0x30 || op == 0x32 || op == 0x3F) return;
    u32 ns = fp_nsrc(op);
    u32 a = ns > 0 ? fp_src(c, i, 0, pc) : 0;
    u32 b = ns > 1 ? fp_src(c, i, 1, pc) : 0;
    u32 d = ns > 2 ? fp_src(c, i, 2, pc) : 0;
    u32 T4 = tv4(s), TF = tf(s);
    u32 v = 0;
    switch (op) {
    case FP_MOV: v = a; break;
    case FP_MUL: v = fmul(s, T4, a, b); break;
    case FP_ADD: v = fadd(s, T4, a, b); break;
    case FP_MAD: v = X(s, T4, GL_Fma, a, b, d); break;
    case FP_DP3: {
        u32 x = I(s, OpVectorShuffle, tv3(s), a, a, 0, 1, 2);
        u32 y = I(s, OpVectorShuffle, tv3(s), b, b, 0, 1, 2);
        v = splat(s, I(s, OpDot, TF, x, y));
        break;
    }
    case FP_DP4: v = splat(s, dot4(s, a, b)); break;
    case FP_DP2: case FP_DP2A: {
        u32 x = I(s, OpVectorShuffle, tv2(s), a, a, 0, 1);
        u32 y = I(s, OpVectorShuffle, tv2(s), b, b, 0, 1);
        u32 dp = I(s, OpDot, TF, x, y);
        if (op == FP_DP2A) dp = fadd(s, TF, dp, exf(s, d, 0));
        v = splat(s, dp);
        break;
    }
    case FP_DST: v = vec4c(s, cf(s, 1.0f), fmul(s, TF, exf(s, a, 1), exf(s, b, 1)), exf(s, a, 2), exf(s, b, 3)); break;
    case FP_MIN: v = X(s, T4, GL_FMin, a, b); break;
    case FP_MAX: v = X(s, T4, GL_FMax, a, b); break;
    case FP_SLT: v = set_op(s, OpFOrdLessThan, a, b); break;
    case FP_SGE: v = set_op(s, OpFOrdGreaterThanEqual, a, b); break;
    case FP_SLE: v = set_op(s, OpFOrdLessThanEqual, a, b); break;
    case FP_SGT: v = set_op(s, OpFOrdGreaterThan, a, b); break;
    case FP_SNE: v = set_op(s, OpFUnordNotEqual, a, b); break;
    case FP_SEQ: v = set_op(s, OpFOrdEqual, a, b); break;
    case FP_FRC: v = X(s, T4, GL_Fract, a); break;
    case FP_FLR: v = X(s, T4, GL_Floor, a); break;
    case FP_PK4: v = splat(s, bc(s, TF, X(s, tu(s), GL_PackSnorm4x8, a))); break;
    case FP_UP4: v = X(s, T4, GL_UnpackSnorm4x8, fp_bits_x(s, a)); break;
    case FP_DDX: v = I(s, OpDPdx, T4, a); break;
    case FP_DDY: v = I(s, OpDPdy, T4, a); break;
    case FP_TEX: case FP_TXP: case FP_TXD: case FP_TXB: case FP_TXL: case FP_TEXBEM: case FP_TXPBEM:
        v = fp_tex(c, i, op, a, b, d);
        break;
    case FP_RCP: v = splat(s, fdiv(s, TF, cf(s, 1.0f), exf(s, a, 0))); break;
    case FP_RSQ: v = splat(s, fdiv(s, TF, cf(s, 1.0f), X(s, TF, GL_Sqrt, X(s, TF, GL_FAbs, exf(s, a, 0))))); break;
    case FP_EX2: v = splat(s, X(s, TF, GL_Exp2, exf(s, a, 0))); break;
    case FP_LG2: v = splat(s, X(s, TF, GL_Log2, exf(s, a, 0))); break;
    case FP_LIT: v = lit_op(s, a); break;
    case FP_LIF: {
        u32 y = exf(s, a, 1);
        u32 z = sel(s, TF, I(s, OpFOrdGreaterThan, tbool(s), y, cf(s, 0.0f)), X(s, TF, GL_Exp2, exf(s, a, 3)), cf(s, 0.0f));
        v = vec4c(s, cf(s, 1.0f), y, z, cf(s, 1.0f));
        break;
    }
    case FP_LRP: v = X(s, T4, GL_FMix, d, b, a); break;
    case FP_STR: v = cvs(s, 1.0f); break;
    case FP_SFL: v = cvs(s, 0.0f); break;
    case FP_COS: v = splat(s, X(s, TF, GL_Cos, exf(s, a, 0))); break;
    case FP_SIN: v = splat(s, X(s, TF, GL_Sin, exf(s, a, 0))); break;
    case FP_PK2: {
        u32 src = ((i->s0 & 3u) == 0u && bf(i->s0, 8, 1)) ? a : f16_quantize(s, a);
        v = splat(s, bc(s, TF, X(s, tu(s), GL_PackHalf2x16, I(s, OpVectorShuffle, tv2(s), src, src, 0, 1))));
        break;
    }
    case FP_UP2: {
        u32 h = X(s, tv2(s), GL_UnpackHalf2x16, fp_bits_x(s, a));
        v = shuf(s, T4, h, h, 0, 1, 0, 1);
        break;
    }
    case FP_POW: v = splat(s, X(s, TF, GL_Pow, exf(s, a, 0), exf(s, b, 0))); break;
    case FP_PKB: v = splat(s, bc(s, TF, X(s, tu(s), GL_PackUnorm4x8, a))); break;
    case FP_UPB: v = X(s, T4, GL_UnpackUnorm4x8, fp_bits_x(s, a)); break;
    case FP_PK16: v = splat(s, bc(s, TF, X(s, tu(s), GL_PackUnorm2x16, I(s, OpVectorShuffle, tv2(s), a, a, 0, 1)))); break;
    case FP_UP16: {
        u32 h = X(s, tv2(s), GL_UnpackUnorm2x16, fp_bits_x(s, a));
        v = shuf(s, T4, h, h, 0, 1, 0, 1);
        break;
    }
    case FP_BEM: v = fp_bem(s, a, b, d); break;
    case FP_PKG: v = splat(s, bc(s, TF, X(s, tu(s), GL_PackUnorm4x8, linear_to_srgb(s, a)))); break;
    case FP_UPG: v = srgb_to_linear(s, X(s, T4, GL_UnpackUnorm4x8, fp_bits_x(s, a))); break;
    case FP_REFL: v = X(s, T4, GL_Reflect, a, b); break;
    case FP_NRM: {
        u32 x = I(s, OpVectorShuffle, tv3(s), a, a, 0, 1, 2);
        u32 len = X(s, TF, GL_Length, x);
        u32 nz = I(s, OpFOrdGreaterThan, tbool(s), len, cf(s, 0.0f));
        u32 n = sel(s, tv3(s), nz, X(s, tv3(s), GL_Normalize, x), x);
        v = shuf(s, T4, n, n, 0, 1, 2, 2);
        break;
    }
    case FP_DIV: v = fdiv(s, T4, a, splat(s, exf(s, b, 0))); break;
    case FP_DIVSQ: {
        u32 q = fdiv(s, T4, a, splat(s, X(s, TF, GL_Sqrt, X(s, TF, GL_FAbs, exf(s, b, 0)))));
        v = sel(s, T4, I(s, OpFOrdGreaterThan, tb4(s), X(s, T4, GL_FAbs, a), cvs(s, 0.0f)), q, a);
        break;
    }
    default:
        return;
    }
    fp_setdst(c, i, v);
}

static void fp_range(fpc* c, u32 pc, u32 end, int* ended);

static u32 fp_sub(fpc* c, u32 tgt)
{
    spv* s = c->s;
    for (int k = 0; k < c->nsubs; k++) {
        if (c->subs[k].tgt == tgt) return c->subs[k].busy ? 0 : c->subs[k].id;
    }
    if (c->nsubs >= FP_MAX_SUBS || s->depth + 1 >= SPV_FRAMES) return 0;
    int k = c->nsubs++;
    c->subs[k].tgt = tgt;
    c->subs[k].id = s->id++;
    c->subs[k].busy = 1;
    int saved = c->ldepth;
    c->ldepth = 0;
    fn_begin(s, c->subs[k].id);
    int ended = 0;
    fp_range(c, tgt, 0xFFFFFFFFu, &ended);
    sb_ins(s, BODY(s), OpReturn, k_none, 0);
    fn_end(s);
    c->ldepth = saved;
    c->subs[k].busy = 0;
    return c->subs[k].id;
}

static void fp_range(fpc* c, u32 pc, u32 end, int* ended)
{
    spv* s = c->s;
    u32 lim = c->nprog;
    while (!*ended && pc < end && pc < lim) {
        fpi i;
        fp_decode(c, pc, &i);
        u32 next = pc + 1u;
        switch (i.op) {
        case FP_NOP:
            if (fp_has_const(&i)) next = pc + 2u;
            break;
        case FP_IFE: {
            u32 e = i.s2 >> 2;
            u32 el = (i.s1 & 0x7FFFFFFFu) >> 2;
            if (e <= pc + 1u) break;
            u32 cond = fp_cond_any(c, &i);
            int has_else = el >= pc + 1u && el < e;
            u32 te = has_else ? el : e;
            u32 merge = newl(s), lt = newl(s), lf = has_else ? newl(s) : merge;
            V(s, OpSelectionMerge, merge, 0);
            V(s, OpBranchConditional, cond, lt, lf);
            label(s, lt);
            fp_range(c, pc + 1u, te, ended);
            V(s, OpBranch, merge);
            if (has_else) {
                label(s, lf);
                fp_range(c, el, e, ended);
                V(s, OpBranch, merge);
            }
            label(s, merge);
            next = e;
            break;
        }
        case FP_LOOP: case FP_REP: {
            u32 e = i.s2 >> 2;
            if (e <= pc + 1u) break;
            if (c->ldepth >= 64) break;
            u32 cond = fp_cond_any(c, &i);
            u32 rep = bf(i.s1, 2, 8);
            u32 smerge = newl(s), pre = newl(s), hdr = newl(s), chk = newl(s), body = newl(s), lc = newl(s), lm = newl(s);
            u32 k = lvar(s, ti(s));
            V(s, OpSelectionMerge, smerge, 0);
            V(s, OpBranchConditional, cond, pre, smerge);
            label(s, pre);
            st(s, k, ci(s, 0));
            V(s, OpBranch, hdr);
            label(s, hdr);
            V(s, OpLoopMerge, lm, lc, 0);
            V(s, OpBranch, chk);
            label(s, chk);
            u32 kv = ld(s, ti(s), k);
            V(s, OpBranchConditional, I(s, OpSLessThan, tbool(s), kv, ci(s, (s32)rep)), body, lm);
            label(s, body);
            c->loopk[c->ldepth] = k;
            c->lmerge[c->ldepth] = lm;
            c->ldepth++;
            fp_range(c, pc + 1u, e, ended);
            c->ldepth--;
            V(s, OpBranch, lc);
            label(s, lc);
            st(s, k, I(s, OpIAdd, ti(s), ld(s, ti(s), k), ci(s, 1)));
            V(s, OpBranch, hdr);
            label(s, lm);
            V(s, OpBranch, smerge);
            label(s, smerge);
            next = e;
            break;
        }
        case FP_BRK:
            if (c->ldepth > 0 && bf(i.s0, 18, 3)) {
                u32 m, t;
                sel_if(s, fp_cond_any(c, &i), &m, &t);
                V(s, OpBranch, c->lmerge[c->ldepth - 1]);
                label(s, m);
            }
            break;
        case FP_RET:
            if (bf(i.s0, 18, 3)) {
                u32 m, t;
                sel_if(s, fp_cond_any(c, &i), &m, &t);
                sb_ins(s, BODY(s), OpReturn, k_none, 0);
                label(s, m);
            }
            break;
        case FP_CAL:
            if (bf(i.s0, 18, 3)) {
                u32 tgt = (i.s1 & 0x7FFFFFFFu) >> 2;
                u32 cond = fp_cond_any(c, &i);
                u32 fid = tgt < lim ? fp_sub(c, tgt) : 0;
                if (fid) {
                    u32 m, t;
                    sel_if(s, cond, &m, &t);
                    I(s, OpFunctionCall, tvoid(s), fid);
                    sel_end(s, m);
                }
            }
            break;
        case FP_KIL:
            if (bf(i.s0, 18, 3)) demote_if(s, fp_cond_any(c, &i));
            if (c->cmap[pc]) next = pc + 2u;
            break;
        case FP_FENCT: case FP_FENCB:
            break;
        default:
            fp_alu(c, &i, pc);
            if (c->cmap[pc]) next = pc + 2u;
            break;
        }
        if (i.d & 1u) *ended = 1;
        pc = next;
    }
}

int rsx_spv_fragment(const u32* ucode, u32 max_words, const rsx_spv_fp_key* key, rsx_spv_blob* out, rsx_spv_fp_info* info)
{
    if (!ucode || !key) return -1;
    fpc* c = (fpc*)calloc(1, sizeof(fpc));
    if (!c) return -1;
    if (fp_scan(c, ucode, max_words)) {
        free(c);
        return -1;
    }
    spv* s = spv_begin();
    if (!s) {
        free(c);
        return -1;
    }
    c->s = s;
    c->key = key;
    u32 mainid = s->id++;
    int depth = (key->control & RSX_SPV_FPCTL_DEPTH_EXPORT) != 0;
    u32 modes[2] = { 7, 12 };
    u32 nmodes = 1;
    fn_begin(s, mainid);
    if (c->invalid) {
        sb_ins(s, BODY(s), OpTerminateInvocation, k_none, 0);
    } else {
        fp_state(c);
        if (c->inputs & (1u << 3)) {
            c->fogv = gvar(s, tv4(s), SC_Private, 0);
            st(s, c->fogv, fp_fog_compute(c));
        }
        if (!c->nop_shader) {
            u32 body = s->id++;
            fn_begin(s, body);
            int ended = 0;
            fp_range(c, 0, 0xFFFFFFFFu, &ended);
            sb_ins(s, BODY(s), OpReturn, k_none, 0);
            fn_end(s);
            I(s, OpFunctionCall, tvoid(s), body);
        }
        int fp32 = (key->control & RSX_SPV_FPCTL_32BIT_EXPORTS) != 0;
        static const u32 rregs[4] = { 0, 2, 3, 4 };
        static const u32 hregs[4] = { 0, 4, 6, 8 };
        u32 col[4] = { 0, 0, 0, 0 };
        for (u32 k = 0; k < 4; k++) {
            if (k && !(key->output_mask & (1u << k))) continue;
            col[k] = fp32 ? fp_readR(c, rregs[k]) : fp_readH(c, hregs[k]);
        }
        {
            u32 flags = fp_state_u(c, 0);
            u32 a = exf(s, col[0], 3);
            u32 ref = fp_state_f(c, 2);
            u32 fn = uand(s, tu(s), fp_state_u(c, 1), cu(s, 7));
            static const u32 cmps[6] = { OpFOrdLessThan, OpFOrdEqual, OpFOrdLessThanEqual, OpFOrdGreaterThan, OpFUnordNotEqual, OpFOrdGreaterThanEqual };
            u32 pass = I(s, OpIEqual, tbool(s), fn, cu(s, 7));
            for (u32 k = 0; k < 6; k++)
                pass = sel(s, tbool(s), I(s, OpIEqual, tbool(s), fn, cu(s, k + 1u)), I(s, cmps[k], tbool(s), a, ref), pass);
            demote_if(s, band(s, bitset(s, flags, 0), bnot(s, pass)));
        }
        for (u32 k = 0; k < 4; k++) {
            if (!(key->output_mask & (1u << k))) continue;
            u32 ov = gvar(s, tv4(s), SC_Output, 0);
            deco1(s, ov, DEC_Location, k);
            st(s, ov, col[k]);
        }
        if (depth) {
            u32 fd = gvar(s, tf(s), SC_Output, 0);
            deco1(s, fd, DEC_BuiltIn, BI_FragDepth);
            u32 r1 = ld(s, tu4(s), fp_rvar(c, 1));
            st(s, fd, bc(s, tf(s), ex(s, tu(s), r1, 2)));
            nmodes = 2;
        }
        sb_ins(s, BODY(s), OpReturn, k_none, 0);
    }
    fn_end(s);
    u32 caps[3];
    u32 ncaps = 0;
    caps[ncaps++] = 1;
    caps[ncaps++] = 5379;
    for (u32 u = 0; u < 16; u++)
        if (c->tex[u] && ((key->texture_dims >> (u * 2u)) & 3u) == 0) {
            caps[ncaps++] = 43;
            break;
        }
    int rc = spv_finish(s, 4, caps, ncaps, mainid, modes, nmodes, out);
    fp_fill_info(c, info, depth);
    free(c);
    return rc;
}

static u32 depth_fetch(spv* s, u32 img, u32 x, u32 y)
{
    u32 coord = I(s, OpCompositeConstruct, tvec(s, ti(s), 2), bc(s, ti(s), x), bc(s, ti(s), y));
    return exf(s, I(s, OpImageFetch, tv4(s), img, coord, IMG_Lod, ci(s, 0)), 0);
}

static u32 depth_unorm(spv* s, u32 z, float scale)
{
    u32 c = X(s, tf(s), GL_FClamp, z, cf(s, 0.f), cf(s, 1.f));
    return I(s, OpConvertFToU, tu(s), fadd(s, tf(s), fmul(s, tf(s), c, cf(s, scale)), cf(s, 0.5f)));
}

static u32 depth16(spv* s, u32 z, u32 is_float)
{
    u32 half = uand(s, tu(s), X(s, tu(s), GL_PackHalf2x16, I(s, OpCompositeConstruct, tv2(s), z, cf(s, 0.f))), cu(s, 0xFFFFu));
    return sel(s, tu(s), is_float, half, depth_unorm(s, z, 65535.f));
}

int rsx_spv_depth_pack(int d16, rsx_spv_blob* out)
{
    spv* s = spv_begin();
    if (!s) return -1;
    u32 mainid = newl(s);
    u32 dvar = sampler_var(s, 1, 0, 0);
    const u32 fimg = intern(s, OpTypeImage, 0, (const u32[]){ tf(s), 1, 0, 0, 0, 1, 0 }, 7, 0);
    const u32 uimg = intern(s, OpTypeImage, 0, (const u32[]){ tu(s), 1, 0, 0, 0, 1, 0 }, 7, 0);
    const u32 usi = intern(s, OpTypeSampledImage, 0, &uimg, 1, 0);
    u32 svar = 0;
    if (!d16) {
        svar = gvar(s, usi, SC_UniformConstant, 0);
        deco1(s, svar, DEC_DescriptorSet, 0);
        deco1(s, svar, DEC_Binding, 1);
    }
    u32 ra = intern(s, OpTypeRuntimeArray, 0, (const u32[]){ tu(s) }, 1, 1);
    deco1(s, ra, DEC_ArrayStride, 4);
    u32 sst = intern(s, OpTypeStruct, 0, &ra, 1, 1);
    mdeco(s, sst, 0, DEC_Offset, 0);
    deco(s, sst, DEC_Block);
    u32 buf = gvar(s, sst, SC_StorageBuffer, 0);
    deco1(s, buf, DEC_DescriptorSet, 0);
    deco1(s, buf, DEC_Binding, 2);
    u32 pst = intern(s, OpTypeStruct, 0, (const u32[]){ tu(s), tu(s), tu(s) }, 3, 1);
    for (u32 m = 0; m < 3; m++) mdeco(s, pst, m, DEC_Offset, m * 4u);
    deco(s, pst, DEC_Block);
    u32 push = gvar(s, pst, SC_PushConstant, 0);
    u32 tu3 = tvec(s, tu(s), 3);
    u32 gvarid = gvar(s, tu3, SC_Input, 0);
    deco1(s, gvarid, DEC_BuiltIn, BI_GlobalInvocationId);

    fn_begin(s, mainid);
    u32 gid = ld(s, tu3, gvarid);
    u32 x = ex(s, tu(s), gid, 0), y = ex(s, tu(s), gid, 1);
    u32 pp = tptr(s, SC_PushConstant, tu(s));
    u32 w = ld(s, tu(s), I(s, OpAccessChain, pp, push, ci(s, 0)));
    u32 h = ld(s, tu(s), I(s, OpAccessChain, pp, push, ci(s, 1)));
    u32 mode = ld(s, tu(s), I(s, OpAccessChain, pp, push, ci(s, 2)));
    u32 cols = d16 ? ushr(s, tu(s), I(s, OpIAdd, tu(s), w, cu(s, 1)), cu(s, 1)) : w;
    u32 inside = band(s, I(s, OpULessThan, tbool(s), x, cols), I(s, OpULessThan, tbool(s), y, h));
    u32 merge, then_l;
    sel_if(s, inside, &merge, &then_l);
    u32 dimg = I(s, OpImage, fimg, ld(s, sampler_type(s, 1, 0), dvar));
    u32 word;
    if (d16) {
        u32 x0 = ushl(s, tu(s), x, cu(s, 1));
        u32 x1 = X(s, tu(s), GL_UMin, I(s, OpIAdd, tu(s), x0, cu(s, 1)), I(s, OpISub, tu(s), w, cu(s, 1)));
        u32 is_float = I(s, OpIEqual, tbool(s), mode, cu(s, 1));
        u32 p0 = depth16(s, depth_fetch(s, dimg, x0, y), is_float);
        u32 p1 = depth16(s, depth_fetch(s, dimg, x1, y), is_float);
        p1 = sel(s, tu(s), I(s, OpULessThan, tbool(s), I(s, OpIAdd, tu(s), x0, cu(s, 1)), w), p1, cu(s, 0));
        word = uor(s, tu(s), p0, ushl(s, tu(s), p1, cu(s, 16)));
    } else {
        u32 z = depth_fetch(s, dimg, x, y);
        u32 f24 = uand(s, tu(s), ushr(s, tu(s), bc(s, tu(s), z), cu(s, 7)), cu(s, 0xFFFFFFu));
        u32 d24 = sel(s, tu(s), I(s, OpIEqual, tbool(s), mode, cu(s, 1)), f24, depth_unorm(s, z, 16777215.f));
        u32 simg = I(s, OpImage, uimg, ld(s, usi, svar));
        u32 coord = I(s, OpCompositeConstruct, tvec(s, ti(s), 2), bc(s, ti(s), x), bc(s, ti(s), y));
        u32 st8 = uand(s, tu(s), ex(s, tu(s), I(s, OpImageFetch, tu4(s), simg, coord, IMG_Lod, ci(s, 0)), 0), cu(s, 0xFFu));
        word = uor(s, tu(s), ushl(s, tu(s), d24, cu(s, 8)), st8);
    }
    u32 idx = I(s, OpIAdd, tu(s), I(s, OpIMul, tu(s), y, cols), x);
    st(s, I(s, OpAccessChain, tptr(s, SC_StorageBuffer, tu(s)), buf, ci(s, 0), idx), word);
    sel_end(s, merge);
    sb_ins(s, BODY(s), OpReturn, k_none, 0);
    fn_end(s);
    const u32 caps[1] = { 1 };
    return spv_finish(s, 5, caps, 1, mainid, NULL, 0, out);
}

static u32 box_first(spv* s, u32 a, u32 cell) { return I(s, OpUDiv, tu(s), a, cell); }

static u32 box_end(spv* s, u32 b, u32 cell) { return I(s, OpUDiv, tu(s), I(s, OpIAdd, tu(s), b, I(s, OpISub, tu(s), cell, cu(s, 1))), cell); }

static u32 box_cover(spv* s, u32 j, u32 cell, u32 a, u32 b)
{
    u32 lo = I(s, OpIMul, tu(s), j, cell);
    return I(s, OpISub, tu(s), X(s, tu(s), GL_UMin, I(s, OpIAdd, tu(s), lo, cell), b), X(s, tu(s), GL_UMax, lo, a));
}

int rsx_spv_box_down(rsx_spv_blob* out)
{
    spv* s = spv_begin();
    if (!s) return -1;
    u32 mainid = newl(s);
    u32 src = sampler_var(s, 1, 0, 0);
    const u32 fimg = intern(s, OpTypeImage, 0, (const u32[]){ tf(s), 1, 0, 0, 0, 1, 0 }, 7, 0);
    const u32 oimg = intern(s, OpTypeImage, 0, (const u32[]){ tf(s), 1, 0, 0, 0, 2, 1 }, 7, 0);
    u32 dst = gvar(s, oimg, SC_UniformConstant, 0);
    deco1(s, dst, DEC_DescriptorSet, 0);
    deco1(s, dst, DEC_Binding, 1);
    u32 pst = intern(s, OpTypeStruct, 0, (const u32[]){ tu(s), tu(s), tu(s), tu(s), tu(s), tu(s), tf(s) }, 7, 1);
    for (u32 m = 0; m < 7; m++) mdeco(s, pst, m, DEC_Offset, m * 4u);
    deco(s, pst, DEC_Block);
    u32 push = gvar(s, pst, SC_PushConstant, 0);
    u32 tu3 = tvec(s, tu(s), 3);
    u32 gvarid = gvar(s, tu3, SC_Input, 0);
    deco1(s, gvarid, DEC_BuiltIn, BI_GlobalInvocationId);

    fn_begin(s, mainid);
    u32 sum = lvar(s, tv4(s)), n = lvar(s, tu(s));
    u32 gid = ld(s, tu3, gvarid);
    u32 x = ex(s, tu(s), gid, 0), y = ex(s, tu(s), gid, 1);
    u32 pp = tptr(s, SC_PushConstant, tu(s));
    u32 pc[6];
    for (u32 m = 0; m < 6; m++) pc[m] = ld(s, tu(s), I(s, OpAccessChain, pp, push, ci(s, (s32)m)));
    u32 inv = ld(s, tf(s), I(s, OpAccessChain, tptr(s, SC_PushConstant, tf(s)), push, ci(s, 6)));
    u32 inside = band(s, I(s, OpULessThan, tbool(s), x, pc[0]), I(s, OpULessThan, tbool(s), y, pc[1]));
    u32 merge, then_l;
    sel_if(s, inside, &merge, &then_l);
    u32 img = I(s, OpImage, fimg, ld(s, sampler_type(s, 1, 0), src));
    u32 ti2 = tvec(s, ti(s), 2);
    u32 ax = I(s, OpIMul, tu(s), x, pc[2]), bx = I(s, OpIAdd, tu(s), ax, pc[2]);
    u32 ay = I(s, OpIMul, tu(s), y, pc[4]), by = I(s, OpIAdd, tu(s), ay, pc[4]);
    u32 x0 = box_first(s, ax, pc[3]), y0 = box_first(s, ay, pc[5]);
    u32 cols = I(s, OpISub, tu(s), box_end(s, bx, pc[3]), x0);
    u32 taps = I(s, OpIMul, tu(s), cols, I(s, OpISub, tu(s), box_end(s, by, pc[5]), y0));
    st(s, sum, cv4(s, 0.f, 0.f, 0.f, 0.f));
    st(s, n, cu(s, 0));
    u32 hdr = newl(s), chk = newl(s), body = newl(s), cont = newl(s), done = newl(s);
    V(s, OpBranch, hdr);
    label(s, hdr);
    V(s, OpLoopMerge, done, cont, 0);
    V(s, OpBranch, chk);
    label(s, chk);
    u32 i = ld(s, tu(s), n);
    V(s, OpBranchConditional, I(s, OpULessThan, tbool(s), i, taps), body, done);
    label(s, body);
    u32 jx = I(s, OpIAdd, tu(s), x0, I(s, OpUMod, tu(s), i, cols)), jy = I(s, OpIAdd, tu(s), y0, I(s, OpUDiv, tu(s), i, cols));
    u32 coord = I(s, OpCompositeConstruct, ti2, bc(s, ti(s), jx), bc(s, ti(s), jy));
    u32 weight = I(s, OpConvertUToF, tf(s), I(s, OpIMul, tu(s), box_cover(s, jx, pc[3], ax, bx), box_cover(s, jy, pc[5], ay, by)));
    u32 texel = I(s, OpVectorTimesScalar, tv4(s), I(s, OpImageFetch, tv4(s), img, coord, IMG_Lod, ci(s, 0)), weight);
    st(s, sum, fadd(s, tv4(s), ld(s, tv4(s), sum), texel));
    V(s, OpBranch, cont);
    label(s, cont);
    st(s, n, I(s, OpIAdd, tu(s), i, cu(s, 1)));
    V(s, OpBranch, hdr);
    label(s, done);
    u32 avg = I(s, OpVectorTimesScalar, tv4(s), ld(s, tv4(s), sum), inv);
    V(s, OpImageWrite, ld(s, oimg, dst), I(s, OpCompositeConstruct, ti2, bc(s, ti(s), x), bc(s, ti(s), y)), avg);
    sel_end(s, merge);
    sb_ins(s, BODY(s), OpReturn, k_none, 0);
    fn_end(s);
    const u32 caps[1] = { 1 };
    return spv_finish(s, 5, caps, 1, mainid, NULL, 0, out);
}

void rsx_spv_blob_free(rsx_spv_blob* blob)
{
    if (!blob) return;
    free(blob->words);
    blob->words = NULL;
    blob->count = 0;
    blob->capacity = 0;
}
