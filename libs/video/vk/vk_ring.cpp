#include "vk_ring.h"

bool vk_ring::create(vk_device& d, VkDeviceSize size, VkSemaphore timeline)
{
    m_d = &d;
    m_size = size;
    m_timeline = timeline;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
    ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocationInfo info;
    VK_TRY(vmaCreateBuffer(d.vma, &bi, &ai, &m_buffer, &m_mem, &info));
    m_ptr = static_cast<uint8_t*>(info.pMappedData);
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(d.vma, m_mem, &flags);
    m_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!m_ptr || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        std::fprintf(stderr, "[vk] upload ring is not host visible\n");
        return false;
    }
    return true;
}

void vk_ring::destroy()
{
    if (m_buffer) vmaDestroyBuffer(m_d->vma, m_buffer, m_mem);
    m_buffer = VK_NULL_HANDLE;
    m_mem = nullptr;
}

void vk_ring::wait_for(VkDeviceSize need)
{
    uint64_t reached = 0;
    vkGetSemaphoreCounterValue(m_d->dev, m_timeline, &reached);
    while (!m_fences.empty()) {
        const fence f = m_fences.front();
        if (f.value > reached) {
            if (m_used + need < m_size) return;
            VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            wi.semaphoreCount = 1;
            wi.pSemaphores = &m_timeline;
            wi.pValues = &f.value;
            vkWaitSemaphores(m_d->dev, &wi, UINT64_MAX);
            reached = f.value;
        }
        m_fences.pop_front();
        m_tail = f.end;
        m_used = (m_head + m_size - m_tail) % m_size;
    }
}

vk_slice vk_ring::alloc(VkDeviceSize size, VkDeviceSize align)
{
    if (size >= m_size) return { VK_NULL_HANDLE, 0, nullptr };
    VkDeviceSize off = (m_head + align - 1) & ~(align - 1);
    VkDeviceSize consumed = off + size > m_size ? (m_size - m_head) + size : (off - m_head) + size;
    if (off + size > m_size) off = 0;
    if (m_used + consumed >= m_size) wait_for(consumed);
    if (m_used + consumed >= m_size) return { VK_NULL_HANDLE, 0, nullptr };
    m_head = off + size == m_size ? 0 : off + size;
    m_used += consumed;
    return { m_buffer, off, m_ptr + off };
}

void vk_ring::close(uint64_t value)
{
    if (!m_coherent) vmaFlushAllocation(m_d->vma, m_mem, 0, VK_WHOLE_SIZE);
    if (!m_fences.empty() && m_fences.back().end == m_head) {
        m_fences.back().value = value;
        return;
    }
    m_fences.push_back({ m_head, value });
}
