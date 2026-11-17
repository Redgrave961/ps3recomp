#pragma once

#include <cstdint>
#include <numeric>

namespace vk {

struct ratio {
    uint32_t num = 1;
    uint32_t den = 1;
    static ratio of(uint32_t n, uint32_t d)
    {
        const uint32_t g = std::gcd(n, d);
        return { n / g, d / g };
    }
    uint32_t edge(uint32_t x) const { return (uint32_t)(((uint64_t)x * num * 2u + den - 1u) / (2u * (uint64_t)den)); }
    float exact(uint32_t x) const { return (float)((double)x * num / den); }
    float value() const { return (float)num / (float)den; }
    bool unit() const { return num == den; }
    bool operator==(const ratio&) const = default;
    bool operator>(const ratio& o) const { return (uint64_t)num * o.den > (uint64_t)o.num * den; }
};

}
