#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

#include "vulkan_alloc.h"

// GPU buffer backed by fast (VRAM DEVICE_LOCAL) or slow (GTT HOST_VISIBLE)
// memory, with real queue submit (fill/copy) — next step after raw alloc proof.
struct VulkanBuffer {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  uint32_t mem_type = UINT32_MAX;
  bool fast = false;
  bool valid = false;
};

bool vulkan_buffer_create(VulkanDev& d, VkDeviceSize size, bool fast,
                          VkBufferUsageFlags usage, VulkanBuffer* out);
void vulkan_buffer_destroy(VulkanDev& d, VulkanBuffer& b);

// GPU-side fill via vkCmdFillBuffer + fence wait. Size must be % 4 == 0.
bool vulkan_buffer_fill_gpu(VulkanDev& d, VulkanBuffer& b, uint32_t pattern);

// Host check: maps buffer (host-visible only) and verifies fill pattern.
bool vulkan_buffer_verify_mapped(VulkanDev& d, VulkanBuffer& b,
                                 uint32_t pattern);

// GPU-side copy via vkCmdCopyBuffer + fence wait. Sizes must match.
bool vulkan_buffer_copy(VulkanDev& d, VulkanBuffer& dst, VulkanBuffer& src);
