#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Host window + Vulkan swapchain presenter for the Xbox framebuffer.
//
// Title flow: XboxDisplay::present() snapshots guest RGBA8 bytes ->
// upload_and_present() stages them to a swapchain image and presents
// with FIFO vsync. The swapchain extent is fixed at init; uploads must
// match it exactly (the Xbox mode is fixed at set_framebuffer time).
//
// Optional dependency (like Unicorn): when GLFW is absent at configure
// time the implementation is a stub whose init() returns false, so the
// rest of the emulator builds unchanged. Tests skip honestly (exit 77)
// when there is no Vulkan device or no display server.
class VulkanPresenter {
 public:
  struct Impl;
  VulkanPresenter() = default;
  ~VulkanPresenter() { shutdown(); }
  VulkanPresenter(const VulkanPresenter&) = delete;
  VulkanPresenter& operator=(const VulkanPresenter&) = delete;

  // Create a window + swapchain of w*h. visible=false keeps the window
  // hidden (tests, headless screenshots). Returns false when GLFW,
  // Vulkan, or a present-capable device is unavailable.
  bool init(uint32_t w, uint32_t h, const std::string& title = "seriesx-emu",
            bool visible = false);
  void shutdown();

  bool valid() const { return valid_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint64_t presents() const { return presents_; }
  uint64_t errors() const { return errors_; }
  bool glfw_enabled() const;

  // Pump window events. Returns false when the window was closed.
  bool poll();
  bool should_close() const;

  // Upload RGBA8 pixels (w*h*4 bytes, must equal the swapchain extent)
  // and present one vsync frame. Returns false on size mismatch or
  // Vulkan failure (counts an error).
  bool upload_and_present(const uint8_t* pixels, uint32_t w, uint32_t h);
  bool upload_and_present(const std::vector<uint8_t>& pixels, uint32_t w,
                          uint32_t h) {
    return upload_and_present(pixels.data(), w, h);
  }

 private:
  bool valid_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint64_t presents_ = 0;
  uint64_t errors_ = 0;
  // Opaque PIMPL so the header needs no GLFW/Vulkan types.
  Impl* impl_ = nullptr;
};
