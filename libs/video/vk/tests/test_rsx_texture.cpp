#include "../rsx_texture.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace rsx;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fails;                                                            \
            if (g_fails < 60) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)

enum kind {
    K_B8,
    K_S16,
    K_S16X2,
    K_S32,
    K_W16,
    K_W32,
    K_655,
    K_D24,
    K_D24F,
    K_D16F,
    K_DXT,
    K_BRGR,
    K_RBRG,
};

struct ref_fmt {
    uint32_t fmt;
    const char* name;
    uint32_t bpb;
    uint32_t bw;
    uint32_t bh;
    kind k;
    uint32_t out;
    VkFormat vk;
};

static const ref_fmt g_ref[] = {
    { 0x81, "B8", 1, 1, 1, K_B8, 1, VK_FORMAT_R8_UNORM },
    { 0x82, "A1R5G5B5", 2, 1, 1, K_S16, 2, VK_FORMAT_A1R5G5B5_UNORM_PACK16 },
    { 0x83, "A4R4G4B4", 2, 1, 1, K_S16, 2, VK_FORMAT_R4G4B4A4_UNORM_PACK16 },
    { 0x84, "R5G6B5", 2, 1, 1, K_S16, 2, VK_FORMAT_R5G6B5_UNORM_PACK16 },
    { 0x85, "A8R8G8B8", 4, 1, 1, K_S32, 4, VK_FORMAT_B8G8R8A8_UNORM },
    { 0x86, "DXT1", 8, 4, 4, K_DXT, 8, VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
    { 0x87, "DXT23", 16, 4, 4, K_DXT, 16, VK_FORMAT_BC2_UNORM_BLOCK },
    { 0x88, "DXT45", 16, 4, 4, K_DXT, 16, VK_FORMAT_BC3_UNORM_BLOCK },
    { 0x8B, "G8B8", 2, 1, 1, K_S16, 2, VK_FORMAT_R8G8_UNORM },
    { 0x8D, "B8R8_G8R8", 4, 2, 1, K_BRGR, 4, VK_FORMAT_B8G8R8A8_UNORM },
    { 0x8E, "R8B8_R8G8", 4, 2, 1, K_RBRG, 4, VK_FORMAT_B8G8R8A8_UNORM },
    { 0x8F, "R6G5B5", 2, 1, 1, K_655, 2, VK_FORMAT_R5G6B5_UNORM_PACK16 },
    { 0x90, "DEPTH24_D8", 4, 1, 1, K_D24, 4, VK_FORMAT_D32_SFLOAT },
    { 0x91, "DEPTH24_D8_FLOAT", 4, 1, 1, K_D24F, 4, VK_FORMAT_D32_SFLOAT },
    { 0x92, "DEPTH16", 2, 1, 1, K_S16, 2, VK_FORMAT_D16_UNORM },
    { 0x93, "DEPTH16_FLOAT", 2, 1, 1, K_D16F, 4, VK_FORMAT_D32_SFLOAT },
    { 0x94, "X16", 2, 1, 1, K_S16, 2, VK_FORMAT_R16_UNORM },
    { 0x95, "Y16_X16", 4, 1, 1, K_S16X2, 4, VK_FORMAT_R16G16_UNORM },
    { 0x97, "R5G5B5A1", 2, 1, 1, K_S16, 2, VK_FORMAT_R5G5B5A1_UNORM_PACK16 },
    { 0x98, "HILO8", 2, 1, 1, K_S16, 2, VK_FORMAT_R8G8_UNORM },
    { 0x99, "HILO_S8", 2, 1, 1, K_S16, 2, VK_FORMAT_R8G8_SNORM },
    { 0x9A, "W16Z16Y16X16_F", 8, 1, 1, K_W16, 8, VK_FORMAT_R16G16B16A16_SFLOAT },
    { 0x9B, "W32Z32Y32X32_F", 16, 1, 1, K_W32, 16, VK_FORMAT_R32G32B32A32_SFLOAT },
    { 0x9C, "X32_FLOAT", 4, 1, 1, K_S32, 4, VK_FORMAT_R32_SFLOAT },
    { 0x9D, "D1R5G5B5", 2, 1, 1, K_S16, 2, VK_FORMAT_A1R5G5B5_UNORM_PACK16 },
    { 0x9E, "D8R8G8B8", 4, 1, 1, K_S32, 4, VK_FORMAT_B8G8R8A8_UNORM },
    { 0x9F, "Y16_X16_FLOAT", 4, 1, 1, K_S16X2, 4, VK_FORMAT_R16G16_SFLOAT },
};

static uint32_t ref_log2_ceil(uint32_t v)
{
    uint32_t l = 0;
    while ((1u << l) < v) ++l;
    return l;
}

static uint32_t ref_pow2_ceil(uint32_t v)
{
    if (v <= 2) return v;
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static uint64_t ref_morton(uint32_t x, uint32_t y, uint32_t z, uint32_t lw, uint32_t lh, uint32_t ld)
{
    uint64_t r = 0;
    uint32_t s = 0;
    while (lw || lh || ld) {
        if (lw) {
            r |= uint64_t(x & 1) << s++;
            x >>= 1;
            --lw;
        }
        if (lh) {
            r |= uint64_t(y & 1) << s++;
            y >>= 1;
            --lh;
        }
        if (ld) {
            r |= uint64_t(z & 1) << s++;
            z >>= 1;
            --ld;
        }
    }
    return r;
}

static void ref_texel(kind k, const uint8_t* p, uint32_t sub, uint8_t* o)
{
    switch (k) {
    case K_B8: o[0] = p[0]; break;
    case K_S16: o[0] = p[1]; o[1] = p[0]; break;
    case K_S16X2: o[0] = p[1]; o[1] = p[0]; o[2] = p[3]; o[3] = p[2]; break;
    case K_S32: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;
    case K_W16:
        for (int i = 0; i < 4; ++i) {
            o[i * 2] = p[i * 2 + 1];
            o[i * 2 + 1] = p[i * 2];
        }
        break;
    case K_W32:
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) o[i * 4 + j] = p[i * 4 + 3 - j];
        break;
    case K_655: {
        const uint32_t v = (uint32_t(p[0]) << 8) | p[1];
        const uint32_t r6 = v >> 10, g5 = (v >> 5) & 31, b5 = v & 31;
        const uint32_t out = ((r6 >> 1) << 11) | (g5 << 6) | b5;
        o[0] = uint8_t(out);
        o[1] = uint8_t(out >> 8);
        break;
    }
    case K_D24: {
        const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        const float f = float(v >> 8) / 16777215.f;
        std::memcpy(o, &f, 4);
        break;
    }
    case K_D24F: {
        const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        const uint32_t b = (v >> 8) << 7;
        std::memcpy(o, &b, 4);
        break;
    }
    case K_D16F: {
        const uint32_t v = (uint32_t(p[0]) << 8) | p[1];
        const uint32_t e = v >> 12, m = v & 0xFFF;
        const double d = e ? std::ldexp(1.0 + m / 4096.0, int(e) - 16) : std::ldexp(double(m), -27);
        const float f = float(d);
        std::memcpy(o, &f, 4);
        break;
    }
    case K_BRGR:
        o[0] = p[0];
        o[1] = p[2];
        o[2] = sub ? p[3] : p[1];
        o[3] = 0xFF;
        break;
    case K_RBRG:
        o[0] = p[1];
        o[1] = p[3];
        o[2] = sub ? p[2] : p[0];
        o[3] = 0xFF;
        break;
    case K_DXT: break;
    }
}

struct ref_level {
    uint64_t off;
    uint32_t w, h, d, wb, hb, pb, fh, bs;
};

struct ref_tex {
    const ref_fmt* f;
    bool linear, border;
    uint32_t w, h, d, layers, levels, pitch;
};

static std::vector<ref_level> ref_layout(const ref_tex& t, uint64_t& total, uint64_t& stride)
{
    std::vector<ref_level> v;
    const uint32_t bs = t.border ? (t.linear ? 1 : 4) : 0;
    uint64_t off = 0;
    uint32_t w = t.w, h = t.h, d = t.d;
    for (uint32_t l = 0; l < t.levels; ++l) {
        ref_level L{};
        L.off = off;
        L.w = w;
        L.h = h;
        L.d = d;
        L.wb = (w + t.f->bw - 1) / t.f->bw;
        L.hb = (h + t.f->bh - 1) / t.f->bh;
        L.bs = bs;
        if (t.linear) {
            L.pb = t.pitch / t.f->bpb;
            L.fh = L.hb + 2 * bs;
        } else if (bs) {
            L.pb = ref_pow2_ceil(L.wb + 2 * bs);
            L.fh = ref_pow2_ceil(L.hb + 2 * bs);
        } else {
            L.pb = L.wb;
            L.fh = L.hb;
        }
        off += uint64_t(L.pb) * t.f->bpb * L.fh * d;
        v.push_back(L);
        w = w / 2 ? w / 2 : 1;
        h = h / 2 ? h / 2 : 1;
        d = d / 2 ? d / 2 : 1;
    }
    stride = t.linear ? off : (off + 127) / 128 * 128;
    total = stride * (t.layers - 1) + off;
    return v;
}

static std::vector<uint8_t> ref_level_out(const ref_tex& t, const ref_level& L, const uint8_t* guest, uint64_t base, uint64_t guest_size)
{
    std::vector<uint8_t> out;
    const ref_fmt& f = *t.f;
    if (f.k == K_DXT) {
        const bool vtc = L.d > 1 && std::has_single_bit(L.w) && std::has_single_bit(L.h);
        const uint64_t plane = uint64_t(L.wb) * L.hb;
        for (uint32_t z = 0; z < L.d; ++z) {
            for (uint32_t yb = 0; yb < L.hb; ++yb) {
                for (uint32_t xb = 0; xb < L.wb; ++xb) {
                    uint64_t src;
                    if (vtc) {
                        const uint32_t full_groups = L.d / 4;
                        const uint32_t g = z / 4;
                        const uint32_t tiles = g < full_groups ? 4 : L.d % 4;
                        const uint64_t i = uint64_t(yb) * L.wb + xb;
                        src = base + (g * plane * 4 + i * tiles + (z % 4)) * f.bpb;
                    } else {
                        src = base + ((uint64_t(z) * L.fh + yb) * L.pb + xb) * f.bpb;
                    }
                    out.insert(out.end(), guest + src, guest + src + f.bpb);
                }
            }
        }
        return out;
    }
    const bool swz = !t.linear && f.k != K_BRGR && f.k != K_RBRG;
    const uint32_t unit = f.bpb < 4 ? f.bpb : 4;
    const uint32_t k = f.bpb / unit;
    uint8_t block[16];
    uint8_t texel[16];
    for (uint32_t z = 0; z < L.d; ++z) {
        for (uint32_t y = 0; y < L.h; ++y) {
            for (uint32_t x = 0; x < L.w; ++x) {
                const uint32_t xb = x / f.bw;
                if (swz) {
                    for (uint32_t j = 0; j < k; ++j) {
                        const uint64_t idx = ref_morton((L.bs + xb) * k + j, L.bs + y, z, ref_log2_ceil(L.pb * k),
                                                        ref_log2_ceil(L.fh), ref_log2_ceil(L.d));
                        const uint64_t a = base + idx * unit;
                        for (uint32_t b = 0; b < unit; ++b) block[j * unit + b] = a + b < guest_size ? guest[a + b] : 0;
                    }
                } else {
                    const uint64_t a = base + ((uint64_t(z) * L.fh + L.bs + y) * L.pb + L.bs + xb) * f.bpb;
                    std::memcpy(block, guest + a, f.bpb);
                }
                ref_texel(f.k, block, x % f.bw, texel);
                out.insert(out.end(), texel, texel + f.out);
            }
        }
    }
    return out;
}

static texture_regs make_regs(uint32_t fmt, bool linear, uint32_t dim, bool cube, uint32_t w, uint32_t h, uint32_t d,
                              uint32_t mips, uint32_t pitch, bool border)
{
    texture_regs r{};
    r.offset = 0x80001000u;
    r.format = 1u | (cube ? 4u : 0u) | (border ? 0u : 8u) | (dim << 4) | ((fmt | (linear ? 0x20u : 0u)) << 8) | (mips << 16);
    r.address = 0x00030101u;
    r.control0 = 0x80000000u | (0u << 19) | (0xC00u << 7);
    r.control1 = 0xAAE4u;
    r.filter = 0x02052000u;
    r.image_rect = (w << 16) | h;
    r.border = 0;
    r.control2 = 0;
    r.control3 = (d << 20) | pitch;
    return r;
}

static std::mt19937 g_rng(12345);

static int run_conversion_case(const ref_fmt& f, bool linear, uint32_t dim, bool cube, uint32_t w, uint32_t h, uint32_t d,
                               uint32_t mips, uint32_t pitch, bool border, const char* label)
{
    const int before = g_fails;
    const texture_regs regs = make_regs(f.fmt, linear, dim, cube, w, h, d, mips, pitch, border);
    texture_desc desc;
    const bool ok = texture_describe(regs, false, desc);
    CHECK(ok);
    if (!ok) return 1;
    CHECK(desc.gcm_format == f.fmt);
    CHECK(desc.linear == linear);
    CHECK(desc.layers == (cube ? 6u : 1u));
    CHECK(desc.location == 0);
    CHECK(desc.offset == 0x1000u);

    ref_tex t;
    t.f = &f;
    t.linear = linear;
    t.border = desc.border;
    t.w = desc.width;
    t.h = desc.height;
    t.d = desc.depth_slices;
    t.layers = desc.layers;
    t.levels = desc.levels;
    t.pitch = desc.pitch;
    CHECK(desc.border == (border && f.k != K_DXT));
    uint64_t total = 0, stride = 0;
    const std::vector<ref_level> lv = ref_layout(t, total, stride);
    CHECK(desc.guest_size == total);

    std::vector<uint8_t> guest(desc.guest_size);
    for (auto& b : guest) b = uint8_t(g_rng());

    const uint64_t ssize = texture_staging_size(desc);
    std::vector<uint8_t> staging(ssize + 64, 0xCD);
    std::vector<texture_copy> copies;
    texture_convert(desc, guest.data(), staging.data(), copies);
    CHECK(copies.size() == size_t(desc.layers) * lv.size());
    for (size_t i = ssize; i < staging.size(); ++i) CHECK(staging[i] == 0xCD);

    uint64_t expect_end = 0;
    size_t ci = 0;
    for (uint32_t layer = 0; layer < desc.layers; ++layer) {
        for (uint32_t l = 0; l < lv.size(); ++l, ++ci) {
            if (ci >= copies.size()) break;
            const texture_copy& c = copies[ci];
            const ref_level& L = lv[l];
            CHECK(c.level == l);
            CHECK(c.layer == layer);
            CHECK(c.width == L.w);
            CHECK(c.height == L.h);
            CHECK(c.depth == L.d);
            CHECK(c.offset % 16 == 0);
            CHECK(c.offset >= expect_end);
            if (f.k == K_DXT) {
                CHECK(c.row_texels == L.wb * 4);
                CHECK(c.rows == L.hb * 4);
            } else {
                CHECK(c.row_texels == L.w);
                CHECK(c.rows == L.h);
            }
            const std::vector<uint8_t> want = ref_level_out(t, L, guest.data(), layer * stride + L.off, guest.size());
            const bool same = std::memcmp(staging.data() + c.offset, want.data(), want.size()) == 0;
            CHECK(same);
            if (!same && g_fails < 60) {
                for (size_t i = 0; i < want.size(); ++i) {
                    if (staging[c.offset + i] != want[i]) {
                        std::printf("  %s %s layer %u level %u first diff at byte %zu: got %02X want %02X\n", f.name, label, layer,
                                    l, i, staging[c.offset + i], want[i]);
                        break;
                    }
                }
            }
            expect_end = c.offset + want.size();
        }
    }
    CHECK(expect_end == ssize);
    return g_fails != before;
}

static void test_all_formats()
{
    int bad = 0, cases = 0;
    for (const ref_fmt& f : g_ref) {
        const bool dxt = f.k == K_DXT;
        auto lp = [&](uint32_t w, bool brd) {
            const uint32_t wb = (w + f.bw - 1) / f.bw + (brd && !dxt ? 2 : 0);
            return wb * f.bpb + 64;
        };
        bad += run_conversion_case(f, false, 2, false, 64, 32, 1, 15, 0, false, "2d-swz");
        bad += run_conversion_case(f, true, 2, false, 37, 13, 1, 4, lp(37, false), false, "2d-lin-npot");
        bad += run_conversion_case(f, true, 2, false, 64, 64, 1, 15, (64 + f.bw - 1) / f.bw * f.bpb, false, "2d-lin-packed");
        bad += run_conversion_case(f, false, 2, true, 16, 16, 1, 15, 0, false, "cube-swz");
        bad += run_conversion_case(f, true, 2, true, 8, 8, 1, 3, lp(8, false), false, "cube-lin");
        bad += run_conversion_case(f, false, 3, false, 16, 8, 4, 15, 0, false, "3d-swz");
        bad += run_conversion_case(f, true, 3, false, 9, 5, 3, 2, lp(9, false), false, "3d-lin-npot");
        bad += run_conversion_case(f, false, 3, false, 8, 8, 6, 15, 0, false, "3d-swz-d6");
        bad += run_conversion_case(f, false, 1, false, 32, 1, 1, 15, 0, false, "1d-swz");
        bad += run_conversion_case(f, false, 2, false, 8, 8, 1, 2, 0, true, "2d-swz-border");
        bad += run_conversion_case(f, true, 2, false, 10, 6, 1, 2, lp(10, true), true, "2d-lin-border");
        bad += run_conversion_case(f, false, 2, false, 2048, 2, 1, 15, 0, false, "2d-swz-wide");
        bad += run_conversion_case(f, false, 2, false, 4, 256, 1, 15, 0, false, "2d-swz-tall");
        cases += 13;
    }
    std::printf("format conversion: %d cases, %d with mismatches\n", cases, bad);
}

static void test_npot_swizzled_bounded()
{
    const ref_fmt& f = g_ref[4];
    const int before = g_fails;
    run_conversion_case(f, false, 2, false, 12, 10, 1, 4, 0, false, "2d-swz-npot");
    run_conversion_case(g_ref[0], false, 3, false, 6, 5, 3, 2, 0, false, "3d-swz-npot");
    std::printf("npot swizzled (bounded reads): %s\n", g_fails == before ? "ok" : "MISMATCH");
}

static std::vector<uint8_t> convert_one(uint32_t fmt, const std::vector<uint8_t>& guest, uint32_t w, uint32_t h, bool linear)
{
    const ref_fmt* f = nullptr;
    for (const ref_fmt& r : g_ref)
        if (r.fmt == fmt) f = &r;
    const uint32_t pitch = linear ? (w + f->bw - 1) / f->bw * f->bpb : 0;
    const texture_regs regs = make_regs(fmt, linear, 2, false, w, h, 1, 1, pitch, false);
    texture_desc desc;
    CHECK(texture_describe(regs, false, desc));
    CHECK(desc.guest_size == guest.size());
    std::vector<uint8_t> staging(texture_staging_size(desc));
    std::vector<texture_copy> copies;
    texture_convert(desc, guest.data(), staging.data(), copies);
    return staging;
}

static float as_float(const uint8_t* p)
{
    float f;
    std::memcpy(&f, p, 4);
    return f;
}

static uint32_t as_u32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

static void test_hand_values()
{
    const int before = g_fails;
    {
        const auto s = convert_one(0x85, { 0x11, 0x22, 0x33, 0x44 }, 1, 1, true);
        CHECK(s[0] == 0x44 && s[1] == 0x33 && s[2] == 0x22 && s[3] == 0x11);
    }
    {
        const auto s = convert_one(0x84, { 0xAB, 0xCD }, 1, 1, true);
        CHECK(s[0] == 0xCD && s[1] == 0xAB);
    }
    {
        const auto s = convert_one(0x95, { 0x01, 0x02, 0x03, 0x04 }, 1, 1, true);
        CHECK(s[0] == 0x02 && s[1] == 0x01 && s[2] == 0x04 && s[3] == 0x03);
    }
    {
        const auto s = convert_one(0x9A, { 0, 1, 2, 3, 4, 5, 6, 7 }, 1, 1, true);
        const uint8_t e[8] = { 1, 0, 3, 2, 5, 4, 7, 6 };
        CHECK(std::memcmp(s.data(), e, 8) == 0);
    }
    {
        const auto s = convert_one(0x9B, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, 1, 1, true);
        const uint8_t e[16] = { 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12 };
        CHECK(std::memcmp(s.data(), e, 16) == 0);
    }
    {
        const auto s = convert_one(0x81, { 0x5A }, 1, 1, true);
        CHECK(s[0] == 0x5A);
    }
    {
        const auto s = convert_one(0x86, { 1, 2, 3, 4, 5, 6, 7, 8 }, 4, 4, true);
        const uint8_t e[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        CHECK(std::memcmp(s.data(), e, 8) == 0);
    }
    {
        const auto s = convert_one(0x8D, { 0x10, 0x20, 0x30, 0x40 }, 2, 1, true);
        const uint8_t e[8] = { 0x10, 0x30, 0x20, 0xFF, 0x10, 0x30, 0x40, 0xFF };
        CHECK(std::memcmp(s.data(), e, 8) == 0);
    }
    {
        const auto s = convert_one(0x8E, { 0x11, 0x22, 0x33, 0x44 }, 2, 1, true);
        const uint8_t e[8] = { 0x22, 0x44, 0x11, 0xFF, 0x22, 0x44, 0x33, 0xFF };
        CHECK(std::memcmp(s.data(), e, 8) == 0);
    }
    {
        const auto s = convert_one(0x8D, { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 }, 3, 1, true);
        CHECK(s.size() == 12);
        const uint8_t e[12] = { 0x10, 0x30, 0x20, 0xFF, 0x10, 0x30, 0x40, 0xFF, 0x50, 0x70, 0x60, 0xFF };
        CHECK(std::memcmp(s.data(), e, 12) == 0);
    }
    {
        const auto s = convert_one(0x8F, { 0xAE, 0xAA }, 1, 1, true);
        CHECK(s[0] == 0x4A && s[1] == 0xAD);
    }
    {
        const auto s = convert_one(0x90, { 0x80, 0x00, 0x00, 0x5A }, 1, 1, true);
        CHECK(as_float(s.data()) == 8388608.f / 16777215.f);
        const auto s1 = convert_one(0x90, { 0xFF, 0xFF, 0xFF, 0x00 }, 1, 1, true);
        CHECK(as_float(s1.data()) == 1.f);
    }
    {
        const auto s = convert_one(0x91, { 0x3F, 0x80, 0x00, 0x12 }, 1, 1, true);
        CHECK(as_u32(s.data()) == 0x1FC00000u);
    }
    {
        const auto s = convert_one(0x93, { 0x80, 0x00 }, 1, 1, true);
        CHECK(as_float(s.data()) == 0.00390625f);
        const auto s1 = convert_one(0x93, { 0x08, 0x00 }, 1, 1, true);
        CHECK(as_float(s1.data()) == std::ldexp(1.f, -16));
        const auto s2 = convert_one(0x93, { 0xFF, 0xFF }, 1, 1, true);
        CHECK(as_u32(s2.data()) == ((126u << 23) | (0xFFFu << 11)));
        const auto s3 = convert_one(0x93, { 0x00, 0x00 }, 1, 1, true);
        CHECK(as_u32(s3.data()) == 0u);
    }
    {
        const auto s = convert_one(0x92, { 0x12, 0x34 }, 1, 1, true);
        CHECK(s[0] == 0x34 && s[1] == 0x12);
        const auto h = convert_one(0x98, { 0xAA, 0xBB }, 1, 1, true);
        CHECK(h[0] == 0xBB && h[1] == 0xAA);
        const auto x = convert_one(0x94, { 0x01, 0x02 }, 1, 1, true);
        CHECK(x[0] == 0x02 && x[1] == 0x01);
    }
    {
        std::vector<uint8_t> g(16);
        for (uint32_t i = 0; i < 16; ++i) g[i] = uint8_t(i);
        const auto s = convert_one(0x81, g, 4, 4, false);
        const uint8_t e[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };
        CHECK(std::memcmp(s.data(), e, 16) == 0);
    }
    {
        std::vector<uint8_t> g(8 * 2 * 4);
        for (uint32_t i = 0; i < g.size(); ++i) g[i] = uint8_t(i);
        const auto s = convert_one(0x85, g, 8, 2, false);
        const uint32_t order[16] = { 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15 };
        bool okk = true;
        for (uint32_t t = 0; t < 16; ++t) {
            const uint8_t* p = s.data() + t * 4;
            const uint32_t u = order[t];
            okk &= p[0] == uint8_t(u * 4 + 3) && p[3] == uint8_t(u * 4);
        }
        CHECK(okk);
    }
    std::printf("hand-computed conversions: %s\n", g_fails == before ? "ok" : "MISMATCH");
}

static bool map_eq(const VkComponentMapping& m, VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a)
{
    return m.r == r && m.g == g && m.b == b && m.a == a;
}

static texture_desc describe_with(uint32_t fmt, uint32_t control1, uint32_t address, uint32_t filter, bool vertex = false,
                                  uint32_t control0_extra = 0)
{
    texture_regs r = make_regs(fmt, false, 2, false, 16, 16, 1, 1, 0, false);
    r.control1 = control1;
    r.address = address;
    r.filter = filter;
    r.control0 |= control0_extra;
    texture_desc d;
    CHECK(texture_describe(r, vertex, d));
    return d;
}

static void test_remap_and_ops()
{
    const int before = g_fails;
    constexpr auto R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B, A = VK_COMPONENT_SWIZZLE_A;
    constexpr auto ONE = VK_COMPONENT_SWIZZLE_ONE, ZERO = VK_COMPONENT_SWIZZLE_ZERO;
    const uint32_t addr = 0x00030101u, filt = 0x02052000u;

    CHECK(map_eq(describe_with(0x85, 0xAAE4, addr, filt).swizzle, R, G, B, A));
    CHECK(map_eq(describe_with(0x85, 0xAA1B, addr, filt).swizzle, G, R, A, B));
    CHECK(map_eq(describe_with(0x85, 0x06E4, addr, filt).swizzle, ONE, ZERO, ZERO, A));
    CHECK(map_eq(describe_with(0x85, 0x5AE4, addr, filt).swizzle, R, ONE, ONE, A));
    CHECK(map_eq(describe_with(0x85, 0x00E4, addr, filt).swizzle, ZERO, ZERO, ZERO, ZERO));
    CHECK(map_eq(describe_with(0x81, 0xAAE4, addr, filt).swizzle, R, R, R, ONE));
    CHECK(map_eq(describe_with(0x8B, 0xAAE4, addr, filt).swizzle, R, G, R, G));
    CHECK(map_eq(describe_with(0x83, 0xAAE4, addr, filt).swizzle, G, B, A, R));
    CHECK(map_eq(describe_with(0x86, 0xAAE4, addr, filt).swizzle, R, G, B, A));
    CHECK(map_eq(describe_with(0x9E, 0xAAE4, addr, filt).swizzle, R, G, B, ONE));
    CHECK(map_eq(describe_with(0x9D, 0xAAE4, addr, filt).swizzle, R, G, B, ONE));
    CHECK(map_eq(describe_with(0x94, 0xAAE4, addr, filt).swizzle, ONE, R, ONE, R));
    CHECK(map_eq(describe_with(0x94, 0x0001AAE4, addr, filt).swizzle, ONE, ONE, ONE, R));
    CHECK(map_eq(describe_with(0x95, 0xAAE4, addr, filt).swizzle, R, G, R, G));
    CHECK(map_eq(describe_with(0x95, 0xAA4E, addr, filt).swizzle, G, R, G, R));
    CHECK(map_eq(describe_with(0x98, 0xAAE4, addr, filt).swizzle, G, R, G, R));
    CHECK(map_eq(describe_with(0x9F, 0xAAE4, addr, filt).swizzle, G, R, G, R));
    CHECK(map_eq(describe_with(0x9F, 0x0001AA1B, addr, filt).swizzle, G, G, G, R));
    CHECK(map_eq(describe_with(0x9A, 0xAA1B, addr, filt).swizzle, R, G, B, A));
    CHECK(map_eq(describe_with(0x9C, 0xAA1B, addr, filt).swizzle, R, R, R, R));
    CHECK(map_eq(describe_with(0x90, 0xAA1B, addr, filt).swizzle, R, R, R, R));
    CHECK(map_eq(describe_with(0x85, 0xAA1B, addr, filt, true).swizzle, R, G, B, A));

    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr, filt | 0xF0000000u);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_SNORM && d.ops.sign_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr, filt | (1u << 29));
        CHECK(d.format == VK_FORMAT_B8G8R8A8_UNORM && d.ops.sign_mask == 1);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAA1B, addr, filt | (1u << 29));
        CHECK(d.ops.sign_mask == 2);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr | (0x7u << 20), filt);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_SRGB && d.ops.gamma_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr | (0xFu << 20), filt);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_UNORM && d.ops.gamma_mask == 0xF);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAA1B, addr | (0x7u << 20), filt);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_UNORM && d.ops.gamma_mask == 0x7);
    }
    {
        const texture_desc d = describe_with(0x86, 0xAAE4, addr | (0x7u << 20), filt);
        CHECK(d.format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK && d.ops.gamma_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x84, 0xAAE4, addr | (0x7u << 20), filt);
        CHECK(d.format == VK_FORMAT_R5G6B5_UNORM_PACK16 && d.ops.gamma_mask == 0x7);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr | (1u << 12), filt);
        CHECK(d.ops.bx2_mask == 0xF && d.format == VK_FORMAT_B8G8R8A8_UNORM);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr | (1u << 12) | (0x7u << 20), filt);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_SRGB && d.ops.gamma_mask == 0 && d.ops.bx2_mask == 0x8);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr | (0x7u << 20), filt | (1u << 29));
        CHECK(d.ops.sign_mask == 1 && d.ops.gamma_mask == 0x6 && d.format == VK_FORMAT_B8G8R8A8_UNORM);
    }
    {
        const texture_desc d = describe_with(0x81, 0xAAE4, addr, filt | 0xE0000000u);
        CHECK(d.format == VK_FORMAT_R8_SNORM && d.ops.sign_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x81, 0xAAE4, addr, filt | 0xF0000000u);
        CHECK(d.format == VK_FORMAT_R8_UNORM && d.ops.sign_mask == 0xF);
    }
    {
        const texture_desc d = describe_with(0x99, 0xAAE4, addr | (1u << 12) | (0xFu << 20), filt | 0xF0000000u);
        CHECK(d.format == VK_FORMAT_R8G8_SNORM && d.ops.sign_mask == 0 && d.ops.gamma_mask == 0 && d.ops.bx2_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x95, 0xAAE4, addr, filt | 0xF0000000u);
        CHECK(d.format == VK_FORMAT_R16G16_SNORM && d.ops.sign_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x94, 0xAAE4, addr | (1u << 12), filt | 0xF0000000u);
        CHECK(d.ops.bx2_mask == 0xF && d.ops.sign_mask == 0 && d.format == VK_FORMAT_R16_UNORM);
    }
    {
        const texture_desc d = describe_with(0x9A, 0xAAE4, addr | (1u << 12) | (0xFu << 20), filt | 0xF0000000u);
        CHECK(d.ops.bx2_mask == 0 && d.ops.sign_mask == 0 && d.ops.gamma_mask == 0);
    }
    {
        const texture_desc d = describe_with(0x92, 0xAAE4, addr | (1u << 28), filt);
        CHECK(d.depth && d.ops.depth_compare == 1 && d.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && d.format == VK_FORMAT_D16_UNORM);
        const texture_desc k = describe_with(0x92, 0xAAE4, addr | (1u << 28), filt, false, 1u << 2);
        CHECK(k.ops.depth_compare == 0);
        const texture_desc n = describe_with(0x90, 0xAAE4, addr | (7u << 28), filt);
        CHECK(n.ops.depth_compare == 0);
        const texture_desc z = describe_with(0x90, 0xAAE4, addr | (6u << 28), filt);
        CHECK(z.ops.depth_compare == 6);
        const texture_desc c = describe_with(0x85, 0xAAE4, addr | (1u << 28), filt);
        CHECK(c.ops.depth_compare == 0 && !c.depth && c.aspect == VK_IMAGE_ASPECT_COLOR_BIT);
        const texture_desc v = describe_with(0x92, 0xAAE4, addr | (1u << 28), filt, true);
        CHECK(v.ops.depth_compare == 0);
        const texture_desc b = describe_with(0x90, 0xAAE4, addr | (1u << 12), filt);
        CHECK(b.ops.bx2_mask == 0xF);
    }
    {
        texture_regs r = make_regs(0x85 | 0x40, false, 2, false, 16, 16, 1, 1, 0, false);
        texture_desc d;
        CHECK(texture_describe(r, false, d) && d.ops.unnormalized == 1 && d.gcm_format == 0x85);
        r.address = 0x00030303u;
        CHECK(texture_describe(r, true, d) && d.ops.unnormalized == 0);
        r.address = 0x00030101u;
        CHECK(texture_describe(r, true, d) && d.ops.unnormalized == 1);
    }
    {
        const texture_desc d = describe_with(0x85, 0xAAE4, addr | (0x7u << 20), filt | 0xF0000000u, true);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_UNORM && d.ops.sign_mask == 0 && d.ops.gamma_mask == 0);
    }
    std::printf("remap/swizzle composition and shader ops: %s\n", g_fails == before ? "ok" : "MISMATCH");
}

static void test_describe_rules()
{
    const int before = g_fails;
    texture_desc d;
    texture_regs r = make_regs(0x85, false, 2, false, 256, 64, 1, 15, 0, false);
    CHECK(texture_describe(r, false, d) && d.levels == 9 && d.width == 256 && d.height == 64 && d.dimension == 2);
    r = make_regs(0x86, false, 2, false, 256, 64, 1, 15, 0, false);
    CHECK(texture_describe(r, false, d) && d.levels == 5 && d.compressed);
    CHECK(texture_describe(r, true, d) && d.levels == 9);
    r = make_regs(0x85, false, 2, false, 256, 64, 1, 3, 0, false);
    CHECK(texture_describe(r, false, d) && d.levels == 3);
    r = make_regs(0x85, true, 2, false, 64, 16, 1, 1, 128, false);
    CHECK(texture_describe(r, false, d) && d.width == 32 && d.pitch == 128);
    r = make_regs(0x85, true, 2, false, 64, 16, 1, 1, 0, false);
    CHECK(texture_describe(r, false, d) && d.height == 1 && d.pitch == 256 && d.guest_size == 256);
    r = make_regs(0x85, false, 2, false, 64, 16, 1, 1, 0, false);
    r.format = (r.format & ~3u) | 2u;
    CHECK(texture_describe(r, false, d) && d.location == 1);
    r = make_regs(0x85, false, 2, false, 16, 16, 1, 1, 0, false);
    r.control0 &= 0x7FFFFFFFu;
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 2, false, 16, 16, 1, 1, 0, false);
    r.format &= ~3u;
    CHECK(!texture_describe(r, false, d));
    r.format |= 3u;
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 0, false, 16, 16, 1, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 4, false, 16, 16, 1, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x89, false, 2, false, 16, 16, 1, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x96, false, 2, false, 16, 16, 1, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 2, false, 16, 16, 1, 0, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 2, false, 0, 16, 1, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 3, false, 16, 16, 0, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 2, true, 16, 8, 1, 1, 0, false);
    CHECK(!texture_describe(r, false, d));
    r = make_regs(0x85, false, 1, false, 16, 77, 1, 15, 0, false);
    CHECK(texture_describe(r, false, d) && d.height == 1 && d.dimension == 1 && d.levels == 5);
    r = make_regs(0x85, false, 2, true, 32, 32, 1, 1, 0, false);
    CHECK(texture_describe(r, false, d) && d.cube && d.layers == 6 && d.guest_size == 5 * 4096 + 4096);
    r = make_regs(0x81, false, 2, true, 4, 4, 1, 3, 0, false);
    CHECK(texture_describe(r, false, d) && d.guest_size == 5 * 128 + 21);
    r = make_regs(0x81, true, 2, true, 4, 4, 1, 3, 4, false);
    CHECK(texture_describe(r, false, d) && d.guest_size == 6 * (16 + 8 + 4));
    r = make_regs(0x85, false, 3, false, 16, 16, 8, 15, 0, false);
    CHECK(texture_describe(r, false, d) && d.depth_slices == 8 && d.levels == 5 &&
          d.guest_size == 4 * (16 * 16 * 8 + 8 * 8 * 4 + 4 * 4 * 2 + 2 * 2 * 1 + 1));
    r = make_regs(0x85, false, 2, false, 16, 16, 1, 1, 0, true);
    CHECK(texture_describe(r, false, d) && d.border && d.guest_size == 32 * 32 * 4);
    CHECK(texture_describe(r, true, d) && !d.border && d.guest_size == 16 * 16 * 4);
    std::printf("describe validation, levels, pitch and size rules: %s\n", g_fails == before ? "ok" : "MISMATCH");
}

static void sample(const texture_regs& r, bool vertex, float cap, VkSamplerCreateInfo& info, VkSamplerCustomBorderColorCreateInfoEXT& b,
                   texture_desc& d)
{
    CHECK(texture_describe(r, vertex, d));
    texture_sampler(r, vertex, d, cap, info, b);
}

static void test_sampler()
{
    const int before = g_fails;
    VkSamplerCreateInfo info;
    VkSamplerCustomBorderColorCreateInfoEXT bc;
    texture_desc d;
    const VkSamplerAddressMode wraps[16] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    for (uint32_t w = 0; w < 16; ++w) {
        texture_regs r = make_regs(0x85, false, 3, false, 16, 16, 4, 1, 0, false);
        r.address = w | (((w + 1) & 15) << 8) | (((w + 2) & 15) << 16);
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        CHECK(info.addressModeU == wraps[w]);
        CHECK(info.addressModeV == wraps[(w + 1) & 15]);
        CHECK(info.addressModeW == wraps[(w + 2) & 15]);
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.addressModeU == wraps[w]);
        CHECK(info.addressModeV == wraps[(w + 1) & 15]);
        CHECK(info.addressModeW == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }

    struct minexp {
        VkFilter f;
        VkSamplerMipmapMode m;
        bool mips;
    };
    const minexp mins[8] = {
        { VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, false }, { VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, false },
        { VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, false },   { VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, true },
        { VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, true },    { VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR, true },
        { VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, true },     { VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, false },
    };
    for (uint32_t mn = 0; mn < 8; ++mn) {
        for (uint32_t mg = 0; mg < 8; ++mg) {
            texture_regs r = make_regs(0x85, false, 2, false, 64, 64, 1, 7, 0, false);
            r.control0 = 0x80000000u | (0x180u << 19) | (0x500u << 7);
            r.filter = (mg << 24) | (mn << 16) | 0x100u;
            sample(r, false, 16.f, info, bc, d);
            CHECK(info.minFilter == mins[mn].f);
            CHECK(info.mipmapMode == mins[mn].m);
            CHECK(info.magFilter == ((mg == 2 || mg == 4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST));
            if (mins[mn].mips) {
                CHECK(info.minLod == 1.5f && info.maxLod == 5.f && info.mipLodBias == 1.f);
            } else {
                CHECK(info.minLod == 0.f && info.maxLod == 0.f && info.mipLodBias == 0.f);
            }
            sample(r, true, 16.f, info, bc, d);
            CHECK(info.minFilter == VK_FILTER_NEAREST && info.magFilter == VK_FILTER_NEAREST);
            CHECK(info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST && info.mipLodBias == 0.f && !info.anisotropyEnable);
            CHECK(info.minLod == 1.5f && info.maxLod == 5.f);
        }
    }
    {
        texture_regs r = make_regs(0x85, false, 2, false, 4, 4, 1, 3, 0, false);
        r.control0 = 0x80000000u | (0x180u << 19) | (0x500u << 7);
        r.filter = 0x02060000u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.minLod == 1.5f && info.maxLod == 2.f);
        r = make_regs(0x85, false, 2, false, 64, 64, 1, 1, 0, false);
        r.filter = 0x02060100u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.minLod == 0.f && info.maxLod == 0.f && info.mipLodBias == 0.f && info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
    }
    {
        const struct {
            uint32_t bits;
            float lin;
            float nearest;
        } biases[] = {
            { 0x0100, 1.f, 1.f },       { 0x1F00, -1.f, -1.f },          { 0x1000, -16.f, -16.f }, { 0x0FFF, 4095.f / 256.f, 16.f },
            { 0x004D, 77.f / 256.f, 0.5f }, { 0x1FB3, -77.f / 256.f, -0.5f }, { 0x0000, 0.f, 0.f },     { 0x0040, 0.25f, 0.5f },
        };
        for (const auto& b : biases) {
            texture_regs r = make_regs(0x85, false, 2, false, 64, 64, 1, 7, 0, false);
            r.control0 = 0x80000000u | (0u << 19) | (0xC00u << 7);
            r.filter = 0x02060000u | b.bits;
            sample(r, false, 16.f, info, bc, d);
            CHECK(info.mipLodBias == b.lin);
            r.filter = 0x02040000u | b.bits;
            sample(r, false, 16.f, info, bc, d);
            CHECK(info.mipLodBias == b.nearest);
        }
    }
    {
        const float table[8] = { 1, 2, 4, 6, 8, 10, 12, 16 };
        const float caps[5] = { 16.f, 8.f, 4.f, 1.f, 0.f };
        for (uint32_t a = 0; a < 8; ++a) {
            for (float cap : caps) {
                texture_regs r = make_regs(0x85, false, 2, false, 64, 64, 1, 7, 0, false);
                r.control0 |= a << 4;
                sample(r, false, cap, info, bc, d);
                const float want = std::min(table[a], std::max(cap, 1.f));
                if (want >= 2.f) {
                    CHECK(info.anisotropyEnable == VK_TRUE && info.maxAnisotropy == want);
                } else {
                    CHECK(info.anisotropyEnable == VK_FALSE && info.maxAnisotropy == 1.f);
                }
                sample(r, true, cap, info, bc, d);
                CHECK(info.anisotropyEnable == VK_FALSE);
            }
        }
    }
    {
        const VkCompareOp ops[8] = { VK_COMPARE_OP_NEVER,          VK_COMPARE_OP_GREATER, VK_COMPARE_OP_EQUAL,
                                     VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS,    VK_COMPARE_OP_NOT_EQUAL,
                                     VK_COMPARE_OP_LESS_OR_EQUAL,    VK_COMPARE_OP_ALWAYS };
        for (uint32_t z = 0; z < 8; ++z) {
            texture_regs r = make_regs(0x90, false, 2, false, 64, 64, 1, 1, 0, false);
            r.address = 0x00030101u | (z << 28);
            sample(r, false, 16.f, info, bc, d);
            const bool active = z > 0 && z < 7;
            CHECK(info.compareEnable == (active ? VK_TRUE : VK_FALSE));
            if (active) CHECK(info.compareOp == ops[z]);
            CHECK(info.magFilter == VK_FILTER_LINEAR);
        }
    }
    {
        texture_regs r = make_regs(0x85, false, 2, false, 64, 64, 1, 7, 0, false);
        r.filter = 0x02060000u | (1u << 29);
        sample(r, false, 16.f, info, bc, d);
        CHECK(d.ops.sign_mask == 1);
        CHECK(info.magFilter == VK_FILTER_NEAREST && info.minFilter == VK_FILTER_NEAREST && info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
        r.filter = 0x02060000u | 0xF0000000u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(d.ops.sign_mask == 0 && info.magFilter == VK_FILTER_LINEAR && info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
    }
    {
        texture_regs r = make_regs(0x85, false, 2, false, 64, 64, 1, 1, 0, false);
        r.address = 0x00030404u;
        r.border = 0;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        r.border = 0xFFFFFFFFu;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
        r.border = 0xFF000000u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
        r.border = 0x80FF4000u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT);
        CHECK(bc.sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT && bc.format == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(bc.customBorderColor.float32[0] == 1.f && bc.customBorderColor.float32[1] == 64.f / 255.f &&
              bc.customBorderColor.float32[2] == 0.f && bc.customBorderColor.float32[3] == 128.f / 255.f);
        r.control1 = 0xAA1Bu;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT);
        CHECK(bc.customBorderColor.float32[0] == 64.f / 255.f && bc.customBorderColor.float32[1] == 1.f &&
              bc.customBorderColor.float32[2] == 128.f / 255.f && bc.customBorderColor.float32[3] == 0.f);
        r.control1 = 0xAAE4u;
        r.filter = 0x02052000u | (1u << 29);
        sample(r, false, 16.f, info, bc, d);
        CHECK(d.ops.sign_mask == 1);
        CHECK(bc.customBorderColor.float32[0] == 127.f / 255.f && bc.customBorderColor.float32[1] == 64.f / 255.f);
        r.filter = 0x02052000u | 0xF0000000u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(d.format == VK_FORMAT_B8G8R8A8_SNORM);
        CHECK(std::fabs(bc.customBorderColor.float32[0] - (1.f - 128.f / 255.f) * (255.f / 127.f)) < 1e-6f);
        CHECK(std::fabs(bc.customBorderColor.float32[3] - 0.f) < 1e-6f);
        r.filter = 0x02052000u;
        r.address = 0x00030101u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK && bc.format == VK_FORMAT_UNDEFINED);
        r.address = 0x00040101u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
        r = make_regs(0x85, false, 3, false, 16, 16, 4, 1, 0, false);
        r.address = 0x00040101u;
        r.border = 0x80FF4000u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT);
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
        r.address = 0x00000104u;
        r.control1 = 0xAA1Bu;
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT && bc.customBorderColor.float32[0] == 1.f);
    }
    {
        texture_regs r = make_regs(0x9B | 0x40, false, 2, false, 64, 64, 1, 1, 0, false);
        r.address = 0x00000303u;
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.unnormalizedCoordinates == VK_TRUE && info.minLod == 0.f && info.maxLod == 0.f && d.ops.unnormalized == 0);
        r.address = 0x00000505u;
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.unnormalizedCoordinates == VK_TRUE);
        r.address = 0x00000401u;
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.unnormalizedCoordinates == VK_FALSE && d.ops.unnormalized == 1);
        r = make_regs(0x9B | 0x40, false, 2, false, 64, 64, 1, 3, 0, false);
        r.address = 0x00000303u;
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.unnormalizedCoordinates == VK_FALSE && d.ops.unnormalized == 1);
        r = make_regs(0x9B | 0x40, false, 1, false, 64, 1, 1, 1, 0, false);
        r.address = 0x00000103u;
        sample(r, true, 16.f, info, bc, d);
        CHECK(info.unnormalizedCoordinates == VK_TRUE && info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        r = make_regs(0x9B | 0x40, false, 2, false, 64, 64, 1, 1, 0, false);
        r.address = 0x00000303u;
        sample(r, false, 16.f, info, bc, d);
        CHECK(info.unnormalizedCoordinates == VK_FALSE && d.ops.unnormalized == 1);
    }
    std::printf("sampler mapping (wrap x16, min x8 * mag x8, lod/bias, aniso x8 * caps, compare x8, border, vertex): %s\n",
                g_fails == before ? "ok" : "MISMATCH");
}

static double bench_convert(const texture_regs& r, int iters, double& mbps_guest, double& mbps_staging)
{
    texture_desc d;
    texture_describe(r, false, d);
    std::vector<uint8_t> guest(d.guest_size);
    for (auto& b : guest) b = uint8_t(g_rng());
    std::vector<uint8_t> staging(texture_staging_size(d));
    std::vector<texture_copy> copies;
    copies.reserve(64);
    for (int i = 0; i < 3; ++i) {
        copies.clear();
        texture_convert(d, guest.data(), staging.data(), copies);
    }
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            copies.clear();
            texture_convert(d, guest.data(), staging.data(), copies);
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double>(t1 - t0).count() / iters);
    }
    mbps_guest = double(d.guest_size) / best / 1e6;
    mbps_staging = double(staging.size()) / best / 1e6;
    return best;
}

static void benchmarks()
{
    double g, s;
    {
        const texture_regs r = make_regs(0x85, false, 2, false, 1024, 1024, 1, 15, 0, false);
        const double t = bench_convert(r, 50, g, s);
        std::printf("bench A8R8G8B8 1024x1024 swizzled, 11 mips (deswizzle+bswap32): %.3f ms, %.0f MB/s\n", t * 1e3, g);
    }
    {
        const texture_regs r = make_regs(0x85, true, 2, false, 1024, 1024, 1, 15, 4096, false);
        const double t = bench_convert(r, 50, g, s);
        std::printf("bench A8R8G8B8 1024x1024 linear, 11 mips (bswap32 only):        %.3f ms, %.0f MB/s\n", t * 1e3, g);
    }
    {
        const texture_regs r = make_regs(0x88, false, 2, false, 2048, 2048, 1, 15, 0, false);
        const double t = bench_convert(r, 50, g, s);
        std::printf("bench DXT5 2048x2048, 10 mips (block copy):                     %.3f ms, %.0f MB/s\n", t * 1e3, g);
    }
    {
        const texture_regs r = make_regs(0x88, true, 2, false, 2048, 2048, 1, 1, 8192, false);
        const double t = bench_convert(r, 50, g, s);
        std::printf("bench DXT5 2048x2048 linear, 1 mip:                              %.3f ms, %.0f MB/s\n", t * 1e3, g);
    }
    {
        const texture_regs r = make_regs(0x83, false, 2, false, 1024, 1024, 1, 15, 0, false);
        const double t = bench_convert(r, 50, g, s);
        std::printf("bench A4R4G4B4 1024x1024 swizzled, 11 mips (deswizzle+bswap16): %.3f ms, %.0f MB/s\n", t * 1e3, g);
    }
    {
        const texture_regs r = make_regs(0x9A, false, 2, false, 1024, 1024, 1, 15, 0, false);
        const double t = bench_convert(r, 20, g, s);
        std::printf("bench W16Z16Y16X16 1024x1024 swizzled, 11 mips:                 %.3f ms, %.0f MB/s\n", t * 1e3, g);
    }
    {
        texture_regs r = make_regs(0x85, false, 2, false, 1024, 1024, 1, 15, 0, false);
        r.control1 = 0xAA1B;
        r.address |= (1u << 12) | (7u << 20);
        texture_desc d;
        const int n = 2000000;
        uint64_t acc = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            r.offset = uint32_t(i);
            texture_describe(r, false, d);
            acc += d.guest_size + d.offset;
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("bench texture_describe: %.1f ns/call (acc %llu)\n", std::chrono::duration<double>(t1 - t0).count() / n * 1e9,
                    static_cast<unsigned long long>(acc));
    }
}

int main()
{
    test_all_formats();
    test_npot_swizzled_bounded();
    test_hand_values();
    test_remap_and_ops();
    test_describe_rules();
    test_sampler();
    benchmarks();
    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
