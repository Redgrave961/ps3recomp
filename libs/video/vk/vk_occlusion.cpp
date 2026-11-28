#include "vk_occlusion.h"
#include "ps3emu/thread_name.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

namespace vk {

namespace {

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
    m_control = d.features.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    if (!grow()) return false;
    m_pages = std::make_unique<std::atomic<uint32_t>[]>(k_pages);
    std::thread(&occlusion::worker, this).detach();
    return true;
}

bool occlusion::grow()
{
    if (m_pool_count == k_pool_limit) return false;
    VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = k_pool_queries;
    VkQueryPool pool = VK_NULL_HANDLE;
    VK_TRY(vkCreateQueryPool(m_d->dev, &qi, nullptr, &pool));
    vkResetQueryPool(m_d->dev, pool, 0, k_pool_queries);
    const uint32_t base = m_pool_count * k_pool_queries;
    m_pools[m_pool_count++] = pool;
    m_free.reserve(m_free.size() + k_pool_queries);
    for (uint32_t i = k_pool_queries; i-- > 0;) m_free.push_back(base + i);
    return true;
}

uint32_t occlusion::allocate()
{
    std::lock_guard<std::mutex> hold(m_lock);
    if (m_free.empty() && !grow()) return k_no_query;
    const uint32_t q = m_free.back();
    m_free.pop_back();
    return q;
}

void occlusion::begin(VkCommandBuffer cmd, const scale2d& scale)
{
    const uint32_t q = allocate();
    if (q == k_no_query) {
        note_dropped();
        return;
    }
    vkCmdBeginQuery(cmd, pool_of(q), slot_of(q), m_control);
    m_segments.push_back({ q, scale });
    m_active = true;
}

void occlusion::end(VkCommandBuffer cmd)
{
    const uint32_t q = m_segments.back().query;
    vkCmdEndQuery(cmd, pool_of(q), slot_of(q));
    m_active = false;
}

void occlusion::clear()
{
    m_dropped = false;
    if (m_segments.empty()) return;
    job j;
    j.release = true;
    j.queries = std::exchange(m_segments, {});
    enqueue(std::move(j));
}

bool occlusion::report(uint32_t ea, bool counted)
{
    const bool dropped = counted && m_dropped;
    if ((!counted || m_segments.empty()) && idle()) {
        write_report(ea, dropped ? 1u : 0u);
        return false;
    }
    job j;
    j.ea = ea;
    j.size = 16u;
    j.dropped = dropped;
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
            m_work.wait(hold, [this] { return !m_jobs.empty(); });
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
            vk_wait(*m_d, m_timeline, j.value);
            done = j.value;
        }
        if (j.release) {
            for (const segment& g : j.queries) vkResetQueryPool(m_d->dev, pool_of(g.query), slot_of(g.query), 1);
            std::lock_guard<std::mutex> hold(m_lock);
            for (const segment& g : j.queries) m_free.push_back(g.query);
            continue;
        }
        if (j.run) {
            j.run(j.arg.data());
        } else {
            uint64_t sum = 0;
            for (const segment& g : j.queries) {
                uint64_t n = 0;
                vk_must(*m_d,
                        vkGetQueryPoolResults(m_d->dev, pool_of(g.query), slot_of(g.query), 1, sizeof n, &n, sizeof n,
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                        "vkGetQueryPoolResults");
                const uint64_t area = (uint64_t)g.scale.x.num * g.scale.y.num, unit = (uint64_t)g.scale.x.den * g.scale.y.den;
                sum += n ? std::max<uint64_t>((n * unit + area / 2u) / area, 1u) : 0u;
            }
            if (j.dropped && sum == 0u) sum = 1u;
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
