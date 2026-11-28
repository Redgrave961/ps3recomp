#include "vk_ring.h"

bool vk_ring::create(vk_device& d, VkDeviceSize size, VkDeviceSize persistent, VkSemaphore timeline)
{
    m_d = &d;
    m_size = size;
    m_persistent = persistent;
    m_timeline = timeline;
    VmaVirtualBlockCreateInfo vi{};
    vi.size = persistent;
    VK_TRY(vmaCreateVirtualBlock(&vi, &m_heap));
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size + persistent;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocationInfo info{};
    VkResult r = vmaCreateBuffer(d.vma, &bi, &ai, &m_buffer, &m_mem, &info);
    if (r != VK_SUCCESS) {
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.preferredFlags = 0;
        r = vmaCreateBuffer(d.vma, &bi, &ai, &m_buffer, &m_mem, &info);
    }
    if (r != VK_SUCCESS) {
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        r = vmaCreateBuffer(d.vma, &bi, &ai, &m_buffer, &m_mem, &info);
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vmaCreateBuffer(upload ring, %llu bytes) failed: %d\n", (unsigned long long)bi.size, (int)r);
        destroy();
        return false;
    }
    m_ptr = static_cast<uint8_t*>(info.pMappedData);
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(d.vma, m_mem, &flags);
    m_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!m_ptr || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        std::fprintf(stderr, "[vk] upload ring is not host visible\n");
        destroy();
        return false;
    }
    return true;
}

void vk_ring::destroy()
{
    if (m_buffer) vmaDestroyBuffer(m_d->vma, m_buffer, m_mem);
    m_buffer = VK_NULL_HANDLE;
    m_mem = nullptr;
    if (m_heap) {
        vmaClearVirtualBlock(m_heap);
        vmaDestroyVirtualBlock(m_heap);
    }
    m_heap = VK_NULL_HANDLE;
}

bool vk_ring::hold(VkDeviceSize size, VmaVirtualAllocation& a, VkDeviceSize& offset)
{
    VmaVirtualAllocationCreateInfo ci{};
    ci.size = size;
    ci.alignment = 16;
    if (vmaVirtualAllocate(m_heap, &ci, &a, &offset) != VK_SUCCESS) return false;
    offset += m_size;
    return true;
}

void vk_ring::wait_for(VkDeviceSize need)
{
    uint64_t reached = vk_reached(*m_d, m_timeline);
    while (!m_fences.empty()) {
        const fence f = m_fences.front();
        if (f.value > reached) {
            if (m_used + need < m_size) return;
            vk_wait(*m_d, m_timeline, f.value);
            reached = f.value;
        }
        m_fences.pop_front();
        m_tail = f.end;
        m_used = (m_head + m_size - m_tail) % m_size;
    }
}

vk_slice vk_ring::alloc(VkDeviceSize size, VkDeviceSize align)
{
    if (!fits(size)) return { VK_NULL_HANDLE, 0, nullptr };
    if (m_used == 0) m_head = m_tail = 0;
    VkDeviceSize off = (m_head + align - 1) & ~(align - 1);
    VkDeviceSize consumed = off + size > m_size ? (m_size - m_head) + size : (off - m_head) + size;
    if (off + size > m_size) off = 0;
    if (m_used + consumed >= m_size) {
        wait_for(consumed);
        if (m_used == 0) {
            m_head = m_tail = 0;
            off = 0;
            consumed = size;
        }
    }
    if (m_used + consumed >= m_size) return { VK_NULL_HANDLE, 0, nullptr };
    m_head = off + size == m_size ? 0 : off + size;
    m_used += consumed;
    return { m_buffer, off, m_ptr + off };
}

void vk_ring::flush()
{
    if (!m_coherent && m_mem) vmaFlushAllocation(m_d->vma, m_mem, 0, VK_WHOLE_SIZE);
}

void vk_ring::close(uint64_t value)
{
    if (!m_fences.empty() && m_fences.back().end == m_head) {
        m_fences.back().value = value;
        return;
    }
    m_fences.push_back({ m_head, value });
}
