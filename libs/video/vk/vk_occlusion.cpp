#include "vk_occlusion.h"
#include "ps3emu/thread_name.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace vk {

namespace {

constexpr uint32_t k_queries = 8192;
constexpr uint64_t k_pages = 1ull << 20;

void write_report(uint32_t ea, uint64_t count)
{
    const uint64_t value = (uint64_t)__builtin_bswap32(count > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)count);
    std::memcpy(vm_base + ea + 8u, &value, 8);
    std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(vm_base + ea)).store(__builtin_bswap64(rsx_core_timestamp()), std::memory_order_release);
}

template <typename F>
void for_pages(uint32_t ea, uint32_t size, F&& f)
{
    if (!size) return;
    const uint64_t last = std::min<uint64_t>(((uint64_t)ea + size - 1u) >> 12, k_pages - 1u);
    for (uint64_t p = ea >> 12; p <= last; p++)
        if (f(p)) return;
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
    m_pages = std::make_unique<std::atomic<uint32_t>[]>(k_pages);
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

void occlusion::begin(VkCommandBuffer cmd, const ratio& scale)
{
    const uint32_t q = allocate();
    vkCmdBeginQuery(cmd, m_pool, q, m_control);
    m_segments.push_back({ q, scale });
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
    job j;
    j.release = true;
    j.queries = std::exchange(m_segments, {});
    enqueue(std::move(j));
}

bool occlusion::report(uint32_t ea, bool counted)
{
    if ((!counted || m_segments.empty()) && idle()) {
        write_report(ea, 0);
        return false;
    }
    job j;
    j.ea = ea;
    j.size = 16u;
    if (counted) j.queries = m_segments;
    enqueue(std::move(j));
    return true;
}

bool occlusion::ordered(const rsx_write& w) const
{
    return w.barrier ? !idle() : pending(w.ea, w.size) || pending(w.src, w.src_size);
}

void occlusion::defer(const rsx_write& w)
{
    job j;
    j.ea = w.ea;
    j.size = w.size;
    j.src = w.src;
    j.src_size = w.src_size;
    j.run = w.run;
    const auto* arg = static_cast<const uint8_t*>(w.arg);
    j.arg.assign(arg, arg + w.arg_size);
    enqueue(std::move(j));
}

void occlusion::enqueue(job&& j)
{
    if (!j.release) {
        j.exposed = !hle_window(j.ea, j.size) || !hle_window(j.src, j.src_size);
        if (j.exposed) m_exposed.fetch_add(1u, std::memory_order_relaxed);
        count(j, 1u);
        m_issued++;
    }
    if (j.release || !j.queries.empty() || !m_pending.empty()) {
        if (!j.release && !j.queries.empty()) m_pending_reports = true;
        m_pending.push_back(std::move(j));
        return;
    }
    {
        std::lock_guard<std::mutex> hold(m_lock);
        m_jobs.push_back(std::move(j));
    }
    m_work.notify_one();
}

void occlusion::count(const job& j, uint32_t delta)
{
    const auto add = [&](uint64_t p) {
        m_pages[p].fetch_add(delta, std::memory_order_acq_rel);
        return false;
    };
    for_pages(j.ea, j.size, add);
    for_pages(j.src, j.src_size, add);
}

static inline bool byte_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return an && bn && (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

bool occlusion::pending_locked(uint32_t ea, uint32_t size) const
{
    if (byte_overlap(m_flight_ea, m_flight_size, ea, size) || byte_overlap(m_flight_src, m_flight_src_size, ea, size)) return true;
    for (const job& j : m_pending)
        if (!j.release && (byte_overlap(j.ea, j.size, ea, size) || byte_overlap(j.src, j.src_size, ea, size))) return true;
    for (const job& j : m_jobs)
        if (!j.release && (byte_overlap(j.ea, j.size, ea, size) || byte_overlap(j.src, j.src_size, ea, size))) return true;
    return false;
}

bool occlusion::pending(uint32_t ea, uint32_t size) const
{
    const bool outside = (uint64_t)ea + size <= RSX_SEMAPHORE_EA || ea >= (uint64_t)RSX_SEMAPHORE_EA + VM_HLE_INJECT_SIZE;
    if (idle() || (outside && !m_exposed.load(std::memory_order_acquire))) return false;
    bool hit = false;
    for_pages(ea, size, [&](uint64_t p) { return hit = m_pages[p].load(std::memory_order_acquire) != 0; });
    if (!hit) return false;
    std::lock_guard<std::mutex> hold(m_lock);
    return pending_locked(ea, size);
}

void occlusion::settle(uint32_t ea, uint32_t size)
{
    std::unique_lock<std::mutex> hold(m_lock);
    m_drained.wait(hold, [&] { return idle() || !pending_locked(ea, size); });
}

void occlusion::drain()
{
    std::unique_lock<std::mutex> hold(m_lock);
    m_drained.wait(hold, [this] { return idle(); });
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
    m_pending_reports = false;
    m_work.notify_one();
}

void occlusion::worker()
{
    ps3_thread_name("vk occlusion");
    uint64_t done = 0;
    for (;;) {
        job j;
        {
            std::unique_lock<std::mutex> hold(m_lock);
            m_work.wait(hold, [this] { return m_stop || !m_jobs.empty(); });
            if (m_jobs.empty()) return;
            j = std::move(m_jobs.front());
            m_jobs.pop_front();
            if (!j.release) {
                m_flight_ea = j.ea;
                m_flight_size = j.size;
                m_flight_src = j.src;
                m_flight_src_size = j.src_size;
            }
        }
        if (j.value > done) {
            VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            wi.semaphoreCount = 1;
            wi.pSemaphores = &m_timeline;
            wi.pValues = &j.value;
            vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
            done = j.value;
        }
        if (j.release) {
            for (const segment& g : j.queries) vkResetQueryPool(m_d->dev, m_pool, g.query, 1);
            {
                std::lock_guard<std::mutex> hold(m_lock);
                for (const segment& g : j.queries) m_free.push_back(g.query);
            }
            m_freed.notify_all();
            continue;
        }
        if (j.run) {
            j.run(j.arg.data());
        } else {
            uint64_t sum = 0;
            for (const segment& g : j.queries) {
                uint64_t n = 0;
                vkGetQueryPoolResults(m_d->dev, m_pool, g.query, 1, sizeof n, &n, sizeof n, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                const uint64_t area = (uint64_t)g.scale.num * g.scale.num, unit = (uint64_t)g.scale.den * g.scale.den;
                sum += n ? std::max<uint64_t>((n * unit + area / 2u) / area, 1u) : 0u;
            }
            write_report(j.ea, sum);
        }
        count(j, ~0u);
        if (j.exposed) m_exposed.fetch_sub(1u, std::memory_order_release);
        {
            std::lock_guard<std::mutex> hold(m_lock);
            m_flight_ea = m_flight_size = m_flight_src = m_flight_src_size = 0;
            m_written.fetch_add(1u, std::memory_order_release);
        }
        m_drained.notify_all();
    }
}

}
