#pragma once

#include "vk_device.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace vk {

class occlusion {
public:
    bool create(vk_device& d, VkSemaphore timeline);
    void destroy();

    bool enabled() const { return m_enabled; }
    bool active() const { return m_active; }
    void enable(bool on) { m_enabled = on; }
    void begin(VkCommandBuffer cmd);
    void end(VkCommandBuffer cmd);
    void clear();
    void report(uint32_t ea);
    void submitted(uint64_t value);
    bool unsubmitted() const { return !m_pending.empty(); }
    bool outstanding(uint32_t ea, uint32_t size) const;
    void drain();

private:
    struct job {
        uint32_t ea;
        bool release;
        uint64_t value;
        std::vector<uint32_t> queries;
    };

    void worker();
    uint32_t allocate();

    vk_device* m_d = nullptr;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    VkQueryControlFlags m_control = 0;
    bool m_enabled = false;
    bool m_active = false;
    std::vector<uint32_t> m_segments;
    std::vector<job> m_pending;

    std::mutex m_lock;
    std::condition_variable m_work;
    std::condition_variable m_freed;
    std::condition_variable m_drained;
    uint64_t m_issued = 0;
    uint64_t m_written = 0;
    uint32_t m_lo = 0xFFFFFFFFu;
    uint32_t m_hi = 0;
    std::deque<job> m_jobs;
    std::vector<uint32_t> m_free;
    bool m_stop = false;
    std::thread m_thread;
};

}
