#pragma once
#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

// Real device + memory allocation for the 10GB fast / 6GB slow split.
// fast -> DEVICE_LOCAL VRAM type, slow -> HOST_VISIBLE|COHERENT GTT type.
// Demo allocates MBs (proof), not full GBs; heap fit is checked via query.
struct VulkanDev {
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  uint32_t fast_type = UINT32_MAX;  // DEVICE_LOCAL
  uint32_t slow_type = UINT32_MAX;  // HOST_VISIBLE | COHERENT
  bool fast_mappable = false;
  std::string device_name;
  bool valid = false;
};

bool vulkan_dev_init(VulkanDev& d);
void vulkan_dev_shutdown(VulkanDev& d);

// Allocate `size` bytes from fast (true) or slow (false) type.
bool vulkan_alloc(VulkanDev& d, VkDeviceSize size, bool fast,
                  VkDeviceMemory* out);

// If the backing type is host-visible, write pattern + read back.
bool vulkan_write_read(VulkanDev& d, VkDeviceMemory mem, VkDeviceSize size,
                       uint32_t type_index, uint32_t pattern);
