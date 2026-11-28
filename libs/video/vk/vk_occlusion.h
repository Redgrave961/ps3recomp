#pragma once

#include "vk_device.h"
#include "vk_scale.h"
#include "../rsx_core.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace vk {

inline bool hle_window(uint32_t ea, uint32_t size)
{
    return !size || (ea - RSX_SEMAPHORE_EA < VM_HLE_INJECT_SIZE && VM_HLE_INJECT_SIZE - (ea - RSX_SEMAPHORE_EA) >= size);
}

class occlusion {
public:
    bool create(vk_device& d, VkSemaphore timeline);

    bool enabled() const { return m_enabled; }
    bool active() const { return m_active; }
    void enable(bool on) { m_enabled = on; }
    void begin(VkCommandBuffer cmd, const scale2d& scale);
    void end(VkCommandBuffer cmd);
    void note_dropped() { if (m_enabled) m_dropped = true; }
    void clear();
    bool report(uint32_t ea, bool counted);
    bool ordered(const rsx_write& w) const;
    bool has_pending_reports() const { return m_pending_reports; }
    void defer(const rsx_write& w);
    void submitted(uint64_t value);
    bool idle() const { return m_written.load(std::memory_order_acquire) == m_issued; }
    bool pending(uint32_t ea, uint32_t size) const;
    void settle(uint32_t ea, uint32_t size);
    void drain();

private:
    struct segment {
        uint32_t query;
        scale2d scale;
    };
    struct job {
        uint64_t value = 0;
        bool release = false;
        uint32_t ea = 0;
        uint32_t size = 0;
        uint32_t src = 0;
        uint32_t src_size = 0;
        bool exposed = false;
        bool dropped = false;
        std::vector<segment> queries;
        rsx_deferred run = nullptr;
        std::vector<uint8_t> arg;
    };

    static constexpr uint32_t k_pool_queries = 8192;
    static constexpr uint32_t k_pool_limit = 8;
    static constexpr uint32_t k_no_query = ~0u;

    void worker();
    bool grow();
    uint32_t allocate();
    VkQueryPool pool_of(uint32_t query) const { return m_pools[query / k_pool_queries]; }
    static uint32_t slot_of(uint32_t query) { return query % k_pool_queries; }
    void enqueue(job&& j);
    void count(const job& j, uint32_t delta);
    bool pending_locked(uint32_t ea, uint32_t size) const;

    vk_device* m_d = nullptr;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    std::array<VkQueryPool, k_pool_limit> m_pools{};
    uint32_t m_pool_count = 0;
    VkQueryControlFlags m_control = 0;
    bool m_enabled = false;
    bool m_active = false;
    bool m_dropped = false;
    bool m_pending_reports = false;
    std::vector<segment> m_segments;
    std::vector<job> m_pending;
    uint64_t m_issued = 0;
    std::unique_ptr<std::atomic<uint32_t>[]> m_pages;

    mutable std::mutex m_lock;
    std::condition_variable m_work;
    std::condition_variable m_drained;
    std::atomic<uint64_t> m_written{ 0 };
    std::atomic<uint32_t> m_exposed{ 0 };
    std::deque<job> m_jobs;
    uint32_t m_flight_ea = 0, m_flight_size = 0, m_flight_src = 0, m_flight_src_size = 0;
    std::vector<uint32_t> m_free;
};

}
