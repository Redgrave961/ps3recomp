#include "../vk_flat_map.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <tuple>

namespace {

struct wide_key {
    uint32_t a, b, c, d, e, f;
    bool operator<(const wide_key& o) const { return std::tie(a, b, c, d, e, f) < std::tie(o.a, o.b, o.c, o.d, o.e, o.f); }
};

struct odd_key {
    uint32_t a, b, c, d, e;
    bool operator<(const odd_key& o) const { return std::tie(a, b, c, d, e) < std::tie(o.a, o.b, o.c, o.d, o.e); }
};

struct value {
    uint64_t v = 0;
    uint32_t hits = 0;
};

template <typename K, typename Make>
void check(const char* name, Make make)
{
    std::mt19937_64 rng(0x5EEDull);
    vk::flat_map<K, value> flat;
    std::map<K, value> ref;
    for (uint32_t round = 0; round < 40; round++) {
        for (uint32_t i = 0; i < 4000; i++) {
            const K k = make((uint32_t)(rng() % 3000u));
            value& f = flat[k];
            value& r = ref[k];
            assert(f.v == r.v && f.hits == r.hits);
            f.hits++;
            r.hits++;
            f.v = r.v = rng();
            assert(flat[k].v == r.v);
        }
        const uint64_t cut = rng();
        flat.erase_if([&](const K&, const value& v) { return v.v < cut; });
        std::erase_if(ref, [&](const auto& e) { return e.second.v < cut; });
        assert(flat.size() == ref.size());
        for (const auto& [k, v] : ref) {
            const value& f = flat[k];
            assert(f.v == v.v && f.hits == v.hits);
        }
        assert(flat.size() == ref.size());
    }
    std::printf("%s ok, %zu entries\n", name, ref.size());
}

}

int main()
{
    check<uint64_t>("u64", [](uint32_t n) { return ((uint64_t)n << 32) | (n * 16u); });
    check<wide_key>("wide", [](uint32_t n) { return wide_key{ n * 64u, n % 7u, n % 5u, 4u, 0xFFFFu, n & 3u }; });
    check<odd_key>("odd", [](uint32_t n) { return odd_key{ 0x10000000u + n * 0x100u, n % 3u, 0u, n % 11u, 0xFu }; });
    vk::flat_map<uint64_t, value> empty;
    empty.erase_if([](const uint64_t&, const value&) { return true; });
    assert(empty.size() == 0 && empty[7].v == 0 && empty.size() == 1);
    return 0;
}
