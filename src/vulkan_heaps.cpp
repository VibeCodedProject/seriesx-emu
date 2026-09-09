#include "vulkan_heaps.h"

#include <vulkan/vulkan.h>

bool vulkan_device_available() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return false;
  uint32_t ndev = 0;
  const bool has_device =
      vkEnumeratePhysicalDevices(inst, &ndev, nullptr) == VK_SUCCESS &&
      ndev > 0;
  vkDestroyInstance(inst, nullptr);
  return has_device;
}

bool query_vulkan_heaps(VulkanHeapInfo& out) {
  out = VulkanHeapInfo{};

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;

  VkInstance inst = VK_NULL_HANDLE;
  if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return false;

  uint32_t ndev = 0;
  bool ok = false;
  if (vkEnumeratePhysicalDevices(inst, &ndev, nullptr) == VK_SUCCESS && ndev > 0) {
    // Clamp to local array size; re-query with clamped count.
    uint32_t query_count = ndev > 8 ? 8 : ndev;
    VkPhysicalDevice devs[8];
    if (vkEnumeratePhysicalDevices(inst, &query_count, devs) == VK_SUCCESS) {
      const uint32_t max_devs = query_count;
      VkPhysicalDevice chosen = VK_NULL_HANDLE;
      VkPhysicalDeviceProperties chosen_props{};
      // Prefer discrete AMD GPU, else first discrete, else first device.
      for (uint32_t i = 0; i < max_devs; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          if (!chosen || (p.vendorID == 0x1002 &&
                          chosen_props.vendorID != 0x1002)) {
            chosen = devs[i];
            chosen_props = p;
          }
        }
      }
      if (!chosen) {
        chosen = devs[0];
        vkGetPhysicalDeviceProperties(chosen, &chosen_props);
      }

      VkPhysicalDeviceMemoryProperties mem{};
      vkGetPhysicalDeviceMemoryProperties(chosen, &mem);

      out.device_name = chosen_props.deviceName;
      for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        const uint64_t sz = mem.memoryHeaps[i].size;
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
          if (sz > out.vram_bytes) out.vram_bytes = sz;
        } else {
          if (sz > out.gtt_bytes) out.gtt_bytes = sz;
        }
      }
      // ReBAR visible iff a memory type is DEVICE_LOCAL + HOST_VISIBLE.
      for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        const auto f = mem.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
          out.rebar = true;
          break;
        }
      }
      out.found = true;
      ok = true;
    }
  }
  vkDestroyInstance(inst, nullptr);
  return ok;
}
