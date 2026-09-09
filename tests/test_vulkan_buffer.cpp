#include "vulkan_heaps.h"
#include <cstdio>

#include "vulkan_alloc.h"
#include "vulkan_buffer.h"

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
  CHECK(d.queue != VK_NULL_HANDLE);
  CHECK(d.cmd_pool != VK_NULL_HANDLE);

  // 1MB slow (GTT, host-visible): GPU fill then host verify.
  VulkanBuffer slow{};
  CHECK(vulkan_buffer_create(d, 1ULL << 20, false,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             &slow));
  CHECK(slow.valid);
  constexpr uint32_t kPattern = 0xDEADBEEF;
  CHECK(vulkan_buffer_fill_gpu(d, slow, kPattern));
  CHECK(vulkan_buffer_verify_mapped(d, slow, kPattern));
  std::puts("slow fill+verify ok");

  // 1MB fast (VRAM): GPU fill, then GPU copy to slow for host verify.
  VulkanBuffer fast{};
  CHECK(vulkan_buffer_create(d, 1ULL << 20, true,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             &fast));
  CHECK(fast.valid);
  constexpr uint32_t kFastPattern = 0x12340000;
  CHECK(vulkan_buffer_fill_gpu(d, fast, kFastPattern));
  // Copy fast -> slow, then verify slow holds fast pattern.
  CHECK(vulkan_buffer_copy(d, slow, fast));
  CHECK(vulkan_buffer_verify_mapped(d, slow, kFastPattern));
  std::puts("fast fill + copy to slow + verify ok");

  vulkan_buffer_destroy(d, fast);
  vulkan_buffer_destroy(d, slow);
  vulkan_dev_shutdown(d);
  std::puts("test_vulkan_buffer passed");
  return 0;
}
