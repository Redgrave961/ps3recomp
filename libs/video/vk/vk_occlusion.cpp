#include "vk_occlusion.h"

#include "../rsx_core.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

namespace vk {

namespace {

constexpr uint32_t k_queries = 8192;

void write_report(uint32_t ea, uint64_t count)
{
    const uint64_t value = (uint64_t)__builtin_bswap32(count > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)count);
    std::memcpy(vm_base + ea + 8u, &value, 8);
    std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(vm_base + ea)).store(__builtin_bswap64(rsx_core_timestamp()), std::memory_order_release);
}

}

bool occlusion::create(vk_device& d, VkSemaphore timeline)
{
    m_d = &d;
    m_timeline = timeline;
    VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = k_queries;
    VK_TRY(vkCreateQueryPool(d.dev, &qi, nullptr, &m_pool));
    vkResetQueryPool(d.dev, m_pool, 0, k_queries);
    m_control = d.features.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    m_free.reserve(k_queries);
    for (uint32_t i = k_queries; i-- > 0;) m_free.push_back(i);
    m_thread = std::thread(&occlusion::worker, this);
    return true;
}

void occlusion::destroy()
{
    if (!m_pool) return;
    {
        std::lock_guard<std::mutex> hold(m_lock);
        m_stop = true;
    }
    m_work.notify_one();
    if (m_thread.joinable()) m_thread.join();
    vkDestroyQueryPool(m_d->dev, m_pool, nullptr);
    m_pool = VK_NULL_HANDLE;
}

uint32_t occlusion::allocate()
{
    std::unique_lock<std::mutex> hold(m_lock);
    m_freed.wait(hold, [this] { return !m_free.empty(); });
    const uint32_t q = m_free.back();
    m_free.pop_back();
    return q;
}

void occlusion::begin(VkCommandBuffer cmd, uint32_t scale)
{
    const uint32_t q = allocate();
    vkCmdBeginQuery(cmd, m_pool, q, m_control);
    m_segments.push_back({ q, scale * scale });
    m_active = true;
}

void occlusion::end(VkCommandBuffer cmd)
{
    vkCmdEndQuery(cmd, m_pool, m_segments.back().query);
    m_active = false;
}

void occlusion::clear()
{
    if (m_segments.empty()) return;
    m_pending.push_back({ 0u, true, 0u, std::exchange(m_segments, {}) });
}

void occlusion::report(uint32_t ea)
{
    if (m_segments.empty()) {
        write_report(ea, 0);
        return;
    }
    m_pending.push_back({ ea, false, 0u, m_segments });
    m_issued++;
    m_lo = std::min(m_lo, ea);
    m_hi = std::max(m_hi, ea + 16u);
}

bool occlusion::outstanding(uint32_t ea, uint32_t size) const
{
    return m_lo < ea + size && ea < m_hi;
}

void occlusion::drain()
{
    std::unique_lock<std::mutex> hold(m_lock);
    m_drained.wait(hold, [this] { return m_written == m_issued; });
    m_lo = 0xFFFFFFFFu;
    m_hi = 0;
}

void occlusion::submitted(uint64_t value)
{
    if (m_pending.empty()) return;
    {
        std::lock_guard<std::mutex> hold(m_lock);
        for (job& j : m_pending) {
            j.value = value;
            m_jobs.push_back(std::move(j));
        }
    }
    m_pending.clear();
    m_work.notify_one();
}

void occlusion::worker()
{
    for (;;) {
        job j;
        {
            std::unique_lock<std::mutex> hold(m_lock);
            m_work.wait(hold, [this] { return m_stop || !m_jobs.empty(); });
            if (m_jobs.empty()) return;
            j = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &m_timeline;
        wi.pValues = &j.value;
        vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
        if (j.release) {
            for (const segment& g : j.queries) vkResetQueryPool(m_d->dev, m_pool, g.query, 1);
            {
                std::lock_guard<std::mutex> hold(m_lock);
                for (const segment& g : j.queries) m_free.push_back(g.query);
            }
            m_freed.notify_all();
            continue;
        }
        uint64_t sum = 0;
        for (const segment& g : j.queries) {
            uint64_t n = 0;
            vkGetQueryPoolResults(m_d->dev, m_pool, g.query, 1, sizeof n, &n, sizeof n, VK_QUERY_RESULT_64_BIT);
            sum += n ? std::max<uint64_t>(n / g.area, 1u) : 0u;
        }
        write_report(j.ea, sum);
        {
            std::lock_guard<std::mutex> hold(m_lock);
            m_written++;
        }
        m_drained.notify_all();

    }
}

}
