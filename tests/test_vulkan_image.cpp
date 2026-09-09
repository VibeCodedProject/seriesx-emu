#include "vulkan_heaps.h"
#include <cstdio>

#include "vulkan_alloc.h"
#include "vulkan_buffer.h"
#include "vulkan_image.h"

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

  // 512x512 R8G8B8A8 = exactly 1MB staging.
  VulkanImage im{};
  CHECK(vulkan_image_create_2d(d, 512, 512, VK_FORMAT_R8G8B8A8_UNORM, true,
                               &im));
  CHECK(im.valid);
  CHECK(im.layout == VK_IMAGE_LAYOUT_UNDEFINED);

  VulkanBuffer staging{};
  CHECK(vulkan_buffer_create(d, 1ULL << 20, false,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging));

  // Clear to magenta (1,0,1,1) -> bytes FF 00 FF FF -> u32 0xFFFF00FF.
  VkClearColorValue color{};
  color.float32[0] = 1.0f;
  color.float32[1] = 0.0f;
  color.float32[2] = 1.0f;
  color.float32[3] = 1.0f;
  CHECK(vulkan_image_clear_color(d, im, color));
  CHECK(im.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  CHECK(vulkan_image_copy_to_buffer(d, im, staging));
  CHECK(vulkan_buffer_verify_mapped(d, staging, 0xFFFF00FFu));
  std::puts("image clear magenta + copy verify ok");

  // Second cycle proves layout chain works repeatedly (green -> 0xFF00FF00).
  color.float32[0] = 0.0f;
  color.float32[1] = 1.0f;
  color.float32[2] = 0.0f;
  CHECK(vulkan_image_clear_color(d, im, color));
  CHECK(vulkan_image_copy_to_buffer(d, im, staging));
  CHECK(vulkan_buffer_verify_mapped(d, staging, 0xFF00FF00u));
  std::puts("image re-clear green + copy verify ok");

  vulkan_buffer_destroy(d, staging);
  vulkan_image_destroy(d, im);
  vulkan_dev_shutdown(d);
  std::puts("test_vulkan_image passed");
  return 0;
}
