#include "vulkan_heaps.h"
#include <cstdio>

#include "vulkan_alloc.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  // Honest environment gate: without a Vulkan physical device these
  // hardware assertions cannot run. Exit 77 = ctest "Skipped"; on a
  // GPU host the test runs for real.
  if (!vulkan_device_available()) {
    std::puts("SKIP: no Vulkan physical device on this host");
    return 77;
  }
  VulkanDev d;
  CHECK(vulkan_dev_init(d));
  CHECK(d.valid);
  std::printf("gpu=%s fast_type=%u mappable=%d slow_type=%u\n",
              d.device_name.c_str(), d.fast_type, d.fast_mappable,
              d.slow_type);

  // Proof allocs: 64MB VRAM + 16MB GTT. Full 10/6 fit via heap query.
  VkDeviceMemory fast_mem = VK_NULL_HANDLE;
  VkDeviceMemory slow_mem = VK_NULL_HANDLE;
  CHECK(vulkan_alloc(d, 64ULL << 20, true, &fast_mem));
  CHECK(fast_mem != VK_NULL_HANDLE);
  CHECK(vulkan_alloc(d, 16ULL << 20, false, &slow_mem));
  CHECK(slow_mem != VK_NULL_HANDLE);

  // Slow heap is host-visible: round-trip pattern.
  CHECK(vulkan_write_read(d, slow_mem, 16ULL << 20, d.slow_type, 0xA5A50000));
  // Fast heap only if ReBAR-mappable.
  if (d.fast_mappable) {
    CHECK(vulkan_write_read(d, fast_mem, 64ULL << 20, d.fast_type, 0xC0DE0000));
    std::puts("fast heap mappable (ReBAR) + verified");
  } else {
    std::puts("fast heap device-only (expected without full ReBAR map)");
  }

  vkFreeMemory(d.dev, fast_mem, nullptr);
  vkFreeMemory(d.dev, slow_mem, nullptr);
  vulkan_dev_shutdown(d);
  std::puts("test_vulkan_alloc passed");
  return 0;
}
