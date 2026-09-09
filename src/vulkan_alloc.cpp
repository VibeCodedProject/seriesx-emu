#include "vulkan_alloc.h"

#include <cstring>

namespace {

// Pick first discrete GPU, prefer AMD vendor.
VkPhysicalDevice pick_gpu(VkInstance inst, VkPhysicalDeviceProperties* props) {
  uint32_t n = 0;
  if (vkEnumeratePhysicalDevices(inst, &n, nullptr) != VK_SUCCESS || n == 0)
    return VK_NULL_HANDLE;
  uint32_t query_count = n > 8 ? 8 : n;
  VkPhysicalDevice devs[8];
  if (vkEnumeratePhysicalDevices(inst, &query_count, devs) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  VkPhysicalDevice chosen = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties cp{};
  bool have = false;
  for (uint32_t i = 0; i < query_count; ++i) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(devs[i], &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      if (!have || (p.vendorID == 0x1002 && cp.vendorID != 0x1002)) {
        chosen = devs[i];
        cp = p;
        have = true;
      }
    }
  }
  if (!have) {
    chosen = devs[0];
    vkGetPhysicalDeviceProperties(chosen, &cp);
  }
  if (props) *props = cp;
  return chosen;
}

uint32_t find_queue_family(VkPhysicalDevice phys) {
  uint32_t n = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
  if (n == 0) return 0;
  VkQueueFamilyProperties props[16];
  uint32_t m = n > 16 ? 16 : n;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &m, props);
  for (uint32_t i = 0; i < m; ++i) {
    if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
  }
  return 0;
}

}  // namespace

bool vulkan_dev_init(VulkanDev& d) {
  d = VulkanDev{};

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &d.inst) != VK_SUCCESS) return false;

  VkPhysicalDeviceProperties props{};
  d.phys = pick_gpu(d.inst, &props);
  if (d.phys == VK_NULL_HANDLE) {
    vkDestroyInstance(d.inst, nullptr);
    d.inst = VK_NULL_HANDLE;
    return false;
  }
  d.device_name = props.deviceName;

  const uint32_t qfam = find_queue_family(d.phys);
  d.queue_family = qfam;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  if (vkCreateDevice(d.phys, &dci, nullptr, &d.dev) != VK_SUCCESS) {
    vkDestroyInstance(d.inst, nullptr);
    d.inst = VK_NULL_HANDLE;
    return false;
  }
  vkGetDeviceQueue(d.dev, qfam, 0, &d.queue);

  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = qfam;
  if (vkCreateCommandPool(d.dev, &pci, nullptr, &d.cmd_pool) != VK_SUCCESS) {
    vkDestroyDevice(d.dev, nullptr);
    vkDestroyInstance(d.inst, nullptr);
    d.dev = VK_NULL_HANDLE;
    d.inst = VK_NULL_HANDLE;
    return false;
  }

  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(d.phys, &mem);

  // fast: DEVICE_LOCAL-only preferred (true VRAM); else ReBAR mapped type.
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const auto f = mem.memoryTypes[i].propertyFlags;
    if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
        !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      d.fast_type = i;
      break;
    }
  }
  if (d.fast_type == UINT32_MAX) {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
      const auto f = mem.memoryTypes[i].propertyFlags;
      if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        d.fast_type = i;
        d.fast_mappable = true;
        break;
      }
    }
  }
  // slow: host-visible coherent (GTT).
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const auto f = mem.memoryTypes[i].propertyFlags;
    if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      // Prefer non-device-local for true GTT.
      if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        d.slow_type = i;
        break;
      }
      if (d.slow_type == UINT32_MAX) d.slow_type = i;
    }
  }
  if (d.fast_type == UINT32_MAX || d.slow_type == UINT32_MAX) {
    vkDestroyDevice(d.dev, nullptr);
    vkDestroyInstance(d.inst, nullptr);
    d.dev = VK_NULL_HANDLE;
    d.inst = VK_NULL_HANDLE;
    return false;
  }
  // Check mappability of chosen fast type.
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(d.phys, &mp);
  const auto ff = mp.memoryTypes[d.fast_type].propertyFlags;
  d.fast_mappable = (ff & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

  d.valid = true;
  return true;
}

void vulkan_dev_shutdown(VulkanDev& d) {
  if (d.dev && d.cmd_pool) vkDestroyCommandPool(d.dev, d.cmd_pool, nullptr);
  if (d.dev) vkDestroyDevice(d.dev, nullptr);
  if (d.inst) vkDestroyInstance(d.inst, nullptr);
  d = VulkanDev{};
}

bool vulkan_alloc(VulkanDev& d, VkDeviceSize size, bool fast,
                  VkDeviceMemory* out) {
  if (!d.valid || !out) return false;
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = size;
  ai.memoryTypeIndex = fast ? d.fast_type : d.slow_type;
  return vkAllocateMemory(d.dev, &ai, nullptr, out) == VK_SUCCESS;
}

bool vulkan_write_read(VulkanDev& d, VkDeviceMemory mem, VkDeviceSize size,
                       uint32_t type_index, uint32_t pattern) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(d.phys, &mp);
  if (type_index >= mp.memoryTypeCount) return false;
  const auto f = mp.memoryTypes[type_index].propertyFlags;
  if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return false;
  void* p = nullptr;
  if (vkMapMemory(d.dev, mem, 0, size, 0, &p) != VK_SUCCESS) return false;
  auto* u = static_cast<uint32_t*>(p);
  const size_t n = static_cast<size_t>(size / sizeof(uint32_t));
  for (size_t i = 0; i < n; ++i) u[i] = pattern ^ static_cast<uint32_t>(i);
  // Non-coherent heaps need flush before GPU/read-back, invalidate on read.
  if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = mem;
    range.offset = 0;
    range.size = size;
    vkFlushMappedMemoryRanges(d.dev, 1, &range);
    vkInvalidateMappedMemoryRanges(d.dev, 1, &range);
  }
  for (size_t i = 0; i < n; ++i) {
    if (u[i] != (pattern ^ static_cast<uint32_t>(i))) {
      vkUnmapMemory(d.dev, mem);
      return false;
    }
  }
  vkUnmapMemory(d.dev, mem);
  return true;
}
