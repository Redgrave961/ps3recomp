#pragma once

#include "vk_device.h"

#include <deque>

struct vk_slice {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* ptr;
};

class vk_ring {
public:
    bool create(vk_device& d, VkDeviceSize size, VkDeviceSize persistent, VkSemaphore timeline);
    void destroy();
    vk_slice alloc(VkDeviceSize size, VkDeviceSize align);
    bool hold(VkDeviceSize size, VmaVirtualAllocation& a, VkDeviceSize& offset);
    void release(VmaVirtualAllocation a) { vmaVirtualFree(m_heap, a); }
    void flush();
    void close(uint64_t value);
    void invalidate(VkDeviceSize offset, VkDeviceSize size);
    VkBuffer buffer() const { return m_buffer; }
    uint8_t* data() const { return m_ptr; }
    VkDeviceSize capacity() const { return m_size + m_persistent; }

private:
    void wait_for(VkDeviceSize need);

    struct fence {
        VkDeviceSize end;
        uint64_t value;
    };

    vk_device* m_d = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_mem = nullptr;
    uint8_t* m_ptr = nullptr;
    VkDeviceSize m_size = 0;
    VkDeviceSize m_persistent = 0;
    VmaVirtualBlock m_heap = VK_NULL_HANDLE;
    VkDeviceSize m_head = 0;
    VkDeviceSize m_tail = 0;
    VkDeviceSize m_used = 0;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    std::deque<fence> m_fences;
    bool m_coherent = true;
};
