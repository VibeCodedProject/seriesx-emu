#include "vulkan_buffer.h"

namespace {

bool pick_mem_type(VulkanDev& d, uint32_t type_bits, bool fast,
                   uint32_t* out) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(d.phys, &mp);
  const uint32_t preferred = fast ? d.fast_type : d.slow_type;
  // Prefer the pool's canonical type if compatible with the buffer.
  if (preferred != UINT32_MAX && (type_bits & (1u << preferred))) {
    *out = preferred;
    return true;
  }
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (!(type_bits & (1u << i))) continue;
    const auto f = mp.memoryTypes[i].propertyFlags;
    if (fast) {
      if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        *out = i;
        return true;
      }
    } else {
      if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
          (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        *out = i;
        return true;
      }
    }
  }
  // Fallback: any compatible host-visible (slow) or anything (fast).
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (!(type_bits & (1u << i))) continue;
    const auto f = mp.memoryTypes[i].propertyFlags;
    if (!fast && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      *out = i;
      return true;
    }
  }
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (type_bits & (1u << i)) {
      *out = i;
      return true;
    }
  }
  return false;
}

bool submit_one_time(VulkanDev& d, VkCommandBuffer cb) {
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;

  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(d.dev, &fi, nullptr, &fence) != VK_SUCCESS) return false;
  bool ok = false;
  if (vkQueueSubmit(d.queue, 1, &si, fence) == VK_SUCCESS) {
    ok = vkWaitForFences(d.dev, 1, &fence, VK_TRUE, 5'000'000'000ull) ==
         VK_SUCCESS;
  }
  vkDestroyFence(d.dev, fence, nullptr);
  return ok;
}

bool alloc_cmd(VulkanDev& d, VkCommandBuffer* out) {
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = d.cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  return vkAllocateCommandBuffers(d.dev, &ai, out) == VK_SUCCESS;
}

}  // namespace

bool vulkan_buffer_create(VulkanDev& d, VkDeviceSize size, bool fast,
                          VkBufferUsageFlags usage, VulkanBuffer* out) {
  if (!d.valid || !out || size == 0) return false;
  VulkanBuffer b{};
  b.size = size;
  b.fast = fast;

  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = size;
  bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(d.dev, &bci, nullptr, &b.buf) != VK_SUCCESS) return false;

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(d.dev, b.buf, &req);
  if (!pick_mem_type(d, req.memoryTypeBits, fast, &b.mem_type)) {
    vkDestroyBuffer(d.dev, b.buf, nullptr);
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = b.mem_type;
  if (vkAllocateMemory(d.dev, &ai, nullptr, &b.mem) != VK_SUCCESS) {
    vkDestroyBuffer(d.dev, b.buf, nullptr);
    return false;
  }
  if (vkBindBufferMemory(d.dev, b.buf, b.mem, 0) != VK_SUCCESS) {
    vkFreeMemory(d.dev, b.mem, nullptr);
    vkDestroyBuffer(d.dev, b.buf, nullptr);
    return false;
  }
  b.valid = true;
  *out = b;
  return true;
}

void vulkan_buffer_destroy(VulkanDev& d, VulkanBuffer& b) {
  if (b.buf) vkDestroyBuffer(d.dev, b.buf, nullptr);
  if (b.mem) vkFreeMemory(d.dev, b.mem, nullptr);
  b = VulkanBuffer{};
}

bool vulkan_buffer_fill_gpu(VulkanDev& d, VulkanBuffer& b, uint32_t pattern) {
  if (!d.valid || !b.valid || b.size % 4 != 0) return false;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (!alloc_cmd(d, &cb)) return false;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = false;
  if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
    vkCmdFillBuffer(cb, b.buf, 0, b.size, pattern);
    if (vkEndCommandBuffer(cb) == VK_SUCCESS) ok = submit_one_time(d, cb);
  }
  vkFreeCommandBuffers(d.dev, d.cmd_pool, 1, &cb);
  return ok;
}

bool vulkan_buffer_verify_mapped(VulkanDev& d, VulkanBuffer& b,
                                 uint32_t pattern) {
  if (!d.valid || !b.valid) return false;
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(d.phys, &mp);
  if (b.mem_type >= mp.memoryTypeCount) return false;
  const auto f = mp.memoryTypes[b.mem_type].propertyFlags;
  if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return false;
  void* p = nullptr;
  if (vkMapMemory(d.dev, b.mem, 0, b.size, 0, &p) != VK_SUCCESS) return false;
  if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = b.mem;
    r.offset = 0;
    r.size = b.size;
    vkInvalidateMappedMemoryRanges(d.dev, 1, &r);
  }
  const auto* u = static_cast<const uint32_t*>(p);
  const size_t n = static_cast<size_t>(b.size / sizeof(uint32_t));
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    if (u[i] != pattern) {
      ok = false;
      break;
    }
  }
  vkUnmapMemory(d.dev, b.mem);
  return ok;
}

bool vulkan_buffer_copy(VulkanDev& d, VulkanBuffer& dst, VulkanBuffer& src) {
  if (!d.valid || !dst.valid || !src.valid) return false;
  if (dst.size != src.size) return false;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (!alloc_cmd(d, &cb)) return false;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = false;
  if (vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
    VkBufferCopy region{};
    region.size = src.size;
    vkCmdCopyBuffer(cb, src.buf, dst.buf, 1, &region);
    if (vkEndCommandBuffer(cb) == VK_SUCCESS) ok = submit_one_time(d, cb);
  }
  vkFreeCommandBuffers(d.dev, d.cmd_pool, 1, &cb);
  return ok;
}
