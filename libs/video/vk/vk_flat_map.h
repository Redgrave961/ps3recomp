#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace vk {

inline uint64_t mix_key(uint64_t h)
{
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    return h ^ (h >> 32);
}

template <typename K>
uint64_t hash_key(const K& k)
{
    static_assert(std::is_trivially_copyable_v<K> && std::has_unique_object_representations_v<K> && sizeof(K) % 4u == 0u);
    const auto* p = reinterpret_cast<const unsigned char*>(&k);
    uint64_t h = sizeof(K);
    size_t i = 0;
    for (; i + 8u <= sizeof(K); i += 8u) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = mix_key(h ^ v);
    }
    if constexpr (sizeof(K) % 8u != 0u) {
        uint32_t v;
        std::memcpy(&v, p + i, 4);
        h = mix_key(h ^ v);
    }
    return h;
}

template <typename K, typename V>
class flat_map {
public:
    V& operator[](const K& key)
    {
        const uint64_t h = hash_key(key);
        if (slot* found = probe(h, key)) return found->value;
        slot& s = claim(h, (uint32_t)(h >> 32) | 1u);
        s.key = key;
        return s.value;
    }

    V* find(const K& key)
    {
        slot* found = probe(hash_key(key), key);
        return found ? &found->value : nullptr;
    }

    template <typename F>
    void erase_if(F&& erase)
    {
        if (!m_size) return;
        size_t i = 0;
        while (m_tags[i]) i++;
        i = (i + 1u) & m_mask;
        for (size_t left = m_mask; left;) {
            if (!m_tags[i] || !erase(m_slots[i].key, m_slots[i].value)) {
                i = (i + 1u) & m_mask;
                left--;
                continue;
            }
            size_t hole = i;
            for (size_t j = (i + 1u) & m_mask; m_tags[j]; j = (j + 1u) & m_mask)
                if (((j - (size_t)hash_key(m_slots[j].key)) & m_mask) >= ((j - hole) & m_mask)) {
                    m_tags[hole] = m_tags[j];
                    m_slots[hole] = std::move(m_slots[j]);
                    hole = j;
                }
            m_tags[hole] = 0;
            m_slots[hole] = slot{};
            m_size--;
        }
    }

    size_t size() const { return m_size; }
    size_t capacity() const { return m_slots.size(); }

private:
    struct slot {
        K key{};
        V value{};
    };

    slot* probe(uint64_t h, const K& key)
    {
        if (!m_size) return nullptr;
        const uint32_t tag = (uint32_t)(h >> 32) | 1u;
        for (size_t i = (size_t)h & m_mask; m_tags[i]; i = (i + 1u) & m_mask)
            if (m_tags[i] == tag && !std::memcmp(&m_slots[i].key, &key, sizeof(K))) return &m_slots[i];
        return nullptr;
    }

    slot& claim(uint64_t h, uint32_t tag)
    {
        if ((m_size + 1u) * 4u > m_slots.size() * 3u) grow();
        size_t i = (size_t)h & m_mask;
        while (m_tags[i]) i = (i + 1u) & m_mask;
        m_tags[i] = tag;
        m_size++;
        return m_slots[i];
    }

    void grow()
    {
        std::vector<uint32_t> tags(std::max<size_t>(m_slots.size() * 2u, 16u), 0u);
        std::vector<slot> slots(tags.size());
        std::swap(tags, m_tags);
        std::swap(slots, m_slots);
        m_mask = m_slots.size() - 1u;
        m_size = 0;
        for (size_t i = 0; i < slots.size(); i++)
            if (tags[i]) claim(hash_key(slots[i].key), tags[i]) = std::move(slots[i]);
    }

    std::vector<uint32_t> m_tags;
    std::vector<slot> m_slots;
    size_t m_mask = 0;
    size_t m_size = 0;
};

}
