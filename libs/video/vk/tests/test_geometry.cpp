#include "../vk_geometry.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace vk;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fails;                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
        }                                                                         \
    } while (0)

static std::vector<uint8_t> be(const std::vector<uint32_t>& v, uint32_t size)
{
    std::vector<uint8_t> out;
    for (uint32_t x : v)
        for (uint32_t b = size; b-- > 0;) out.push_back(uint8_t(x >> (b * 8u)));
    return out;
}

static std::vector<uint32_t> run(uint32_t prim, const index_format& f, const std::vector<uint32_t>& in, index_result& res)
{
    const uint32_t count = (uint32_t)in.size();
    const std::vector<uint8_t> src = be(in, f.in_size());
    std::vector<uint8_t> dst((size_t)(index_capacity(prim, count) + 1u) * f.out_size());
    res = convert_indices(prim, f, src.data(), count, dst.data());
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < res.count; i++) {
        if (f.out_size() == 4u) {
            uint32_t v;
            std::memcpy(&v, dst.data() + (size_t)i * 4u, 4);
            out.push_back(v);
        } else {
            uint16_t v;
            std::memcpy(&v, dst.data() + (size_t)i * 2u, 2);
            out.push_back(v);
        }
    }
    return out;
}

static void test_formats()
{
    const index_format a = index_format_for(PRIM_TRIANGLE_STRIP, false, true, 0xFFFFu);
    CHECK(a.restart && a.sentinel && !a.promote && a.out_size() == 2u);
    const index_format b = index_format_for(PRIM_TRIANGLE_STRIP, false, true, 0x1234u);
    CHECK(b.restart && b.sentinel && b.promote && b.out_size() == 4u);
    const index_format c = index_format_for(PRIM_TRIANGLE_STRIP, false, true, 0x12345u);
    CHECK(!c.restart && !c.sentinel && !c.promote && c.out_size() == 2u);
    CHECK(index_format_for(PRIM_POLYGON, false, true, 0xFFFFu).sentinel);
    CHECK(index_format_for(PRIM_LINE_LOOP, true, true, 7u).sentinel);
    CHECK(index_format_for(PRIM_QUAD_STRIP, true, true, 7u).sentinel);
    CHECK(index_format_for(PRIM_TRIANGLE_FAN, false, true, 0xFFFFu).sentinel);
    CHECK(index_format_for(PRIM_LINE_STRIP, false, true, 0xFFFFu).sentinel);
    CHECK(!index_format_for(PRIM_TRIANGLES, false, true, 0xFFFFu).sentinel);
    CHECK(!index_format_for(PRIM_QUADS, true, true, 7u).sentinel);
    CHECK(!index_format_for(PRIM_POINTS, true, true, 7u).sentinel);
    CHECK(!index_format_for(PRIM_TRIANGLE_STRIP, true, false, 7u).restart);
}

static void test_conversions()
{
    index_result r;
    {
        const index_format f = index_format_for(PRIM_TRIANGLE_STRIP, false, true, 0x1234u);
        const auto out = run(PRIM_TRIANGLE_STRIP, f, { 0, 1, 0x1234, 2, 0xFFFF, 3 }, r);
        CHECK((out == std::vector<uint32_t>{ 0, 1, 0xFFFFFFFFu, 2, 0xFFFF, 3 }));
        CHECK(r.min == 0 && r.max == 0xFFFF);
    }
    {
        const index_format f = index_format_for(PRIM_LINE_LOOP, false, true, 0xFFFFu);
        const auto out = run(PRIM_LINE_LOOP, f, { 0, 1, 2, 0xFFFF, 3, 4 }, r);
        CHECK((out == std::vector<uint32_t>{ 0, 1, 2, 0, 0xFFFF, 3, 4, 3 }));
        CHECK(r.min == 0 && r.max == 4);
    }
    {
        const index_format f = index_format_for(PRIM_LINE_LOOP, true, true, 9u);
        const auto out = run(PRIM_LINE_LOOP, f, { 9, 5, 9, 9, 6, 7, 9 }, r);
        CHECK((out == std::vector<uint32_t>{ 5, 5, 0xFFFFFFFFu, 6, 7, 6 }));
        CHECK(r.count <= index_capacity(PRIM_LINE_LOOP, 7));
    }
    {
        const index_format f = index_format_for(PRIM_LINE_LOOP, false, false, 0xFFFFu);
        const auto out = run(PRIM_LINE_LOOP, f, { 4, 5, 6 }, r);
        CHECK((out == std::vector<uint32_t>{ 4, 5, 6, 4 }));
    }
    {
        const index_format f = index_format_for(PRIM_POLYGON, false, true, 0xFFFFu);
        const auto out = run(PRIM_POLYGON, f, { 0, 1, 2, 0xFFFF, 3, 4, 5 }, r);
        CHECK((out == std::vector<uint32_t>{ 0, 1, 2, 0xFFFF, 3, 4, 5 }));
        CHECK(r.min == 0 && r.max == 5);
    }
    {
        const index_format f = index_format_for(PRIM_TRIANGLES, false, true, 0xFFFFu);
        const auto out = run(PRIM_TRIANGLES, f, { 0, 1, 2, 0xFFFF, 3, 4, 5 }, r);
        CHECK((out == std::vector<uint32_t>{ 0, 1, 2, 3, 4, 5 }));
    }
    {
        const index_format f = index_format_for(PRIM_QUADS, false, true, 0xFFFFu);
        const auto out = run(PRIM_QUADS, f, { 0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7 }, r);
        CHECK((out == std::vector<uint32_t>{ 0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4 }));
    }
    {
        const index_format f = index_format_for(PRIM_TRIANGLE_STRIP, true, true, 5u);
        const auto out = run(PRIM_TRIANGLE_STRIP, f, { 0, 5, 0xFFFFFFFFu, 3 }, r);
        CHECK((out == std::vector<uint32_t>{ 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 3 }));
        CHECK(r.min == 0 && r.max == 3);
    }
    for (uint32_t wide = 0; wide < 2; wide++) {
        const uint32_t ri = wide ? 0xFFFFFFFFu : 0xFFFFu;
        std::vector<uint32_t> in;
        uint32_t hi = 0;
        for (uint32_t i = 0; i < 77; i++) {
            const uint32_t v = i % 9 == 4 ? ri : i * 3u + 1u;
            in.push_back(v);
            if (v != ri) hi = v;
        }
        const index_format f = index_format_for(PRIM_TRIANGLE_STRIP, wide != 0, true, ri);
        const auto out = run(PRIM_TRIANGLE_STRIP, f, in, r);
        CHECK(out == in);
        CHECK(r.min == 1 && r.max == hi);
    }
}

int main()
{
    test_formats();
    test_conversions();
    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
