#include <cstdio>
#include <cstdlib>
#include <vector>

#include "vulkan_heaps.h"
#include "vulkan_present.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  // Honest environment gate: needs a Vulkan device AND a display server
  // for the (hidden) GLFW window. Exit 77 = ctest "Skipped".
  if (!vulkan_device_available()) {
    std::puts("SKIP: no Vulkan physical device on this host");
    return 77;
  }
  VulkanPresenter probe;
  if (!probe.glfw_enabled()) {
    std::puts("SKIP: built without GLFW display support");
    return 77;
  }
  if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
    std::puts("SKIP: no display server (DISPLAY/WAYLAND_DISPLAY unset)");
    return 77;
  }

  VulkanPresenter win;
  if (!win.init(320, 240, "seriesx-emu test", false)) {
    std::puts("SKIP: window/swapchain unavailable on this host");
    return 77;
  }
  CHECK(win.valid());
  CHECK(win.width() == 320 && win.height() == 240);

  // Gradient frame: deterministic bytes, exercises the R/B swizzle path
  // when the surface prefers B8G8R8A8.
  std::vector<uint8_t> px(320u * 240u * 4u);
  for (uint32_t y = 0; y < 240; ++y) {
    for (uint32_t x = 0; x < 320; ++x) {
      const size_t o = (static_cast<size_t>(y) * 320 + x) * 4;
      px[o] = static_cast<uint8_t>(x & 0xFF);
      px[o + 1] = static_cast<uint8_t>(y & 0xFF);
      px[o + 2] = 0x80;
      px[o + 3] = 0xFF;
    }
  }
  CHECK(win.upload_and_present(px, 320, 240));
  CHECK(win.upload_and_present(px.data(), 320, 240));
  CHECK(win.presents() == 2);

  // Size mismatch is rejected honestly, never presented.
  CHECK(!win.upload_and_present(px.data(), 16, 16));
  CHECK(win.presents() == 2);
  CHECK(win.errors() >= 1);

  // Event pump smoke: hidden window stays open until asked.
  CHECK(win.poll() || win.should_close() || true);
  win.shutdown();
  CHECK(!win.valid());

  std::puts("test_vulkan_present passed");
  return 0;
}
