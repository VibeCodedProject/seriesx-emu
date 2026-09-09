#include <cstdio>

#include "vulkan_heaps.h"

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
  VulkanHeapInfo h;
  CHECK(query_vulkan_heaps(h));
  CHECK(h.found);
  CHECK(!h.device_name.empty());
  CHECK(h.vram_bytes > 0);
  CHECK(h.gtt_bytes > 0);
  std::printf("gpu=%s vram=%llu GiB gtt=%llu GiB rebar=%d fits10_6=%d\n",
              h.device_name.c_str(), (unsigned long long)(h.vram_bytes >> 30),
              (unsigned long long)(h.gtt_bytes >> 30), h.rebar,
              xbox_split_fits(h));
  // Hard requirement for this box: 10GB fast + 6GB slow.
  CHECK(xbox_split_fits(h));
  CHECK(h.rebar);
  std::puts("test_vulkan passed");
  return 0;
}
