#pragma once
#include <cstdint>
#include <string>
#include <vector>

class MemMap;

// Original-Xbox display HLE (homebrew only).
//
// Real HW: NV2A scans out a guest-RAM framebuffer (typically 640x480x32)
// through the video encoder; D3D Present / Flip swaps front/back buffers
// on vsync. Here: the title registers its frontbuffer (GPA + dimensions,
// RGBA8 only for now) and every Flip snapshots those guest bytes into a
// host-side image. That snapshot is the contract the Vulkan presenter
// uploads to the swapchain -- and the thing headless tests checksum and
// dump to PPM without needing a window or GPU.
//
// No shaders/pipelines: ClearRenderTarget/FillMemory in GpuStub remain the
// software rasterizer; this class only owns "what is currently on screen".
class XboxDisplay {
 public:
  static constexpr uint32_t kMaxWidth = 1920;
  static constexpr uint32_t kMaxHeight = 1080;
  static constexpr uint32_t kBytesPerPixel = 4;  // R8G8B8A8 only

  XboxDisplay() = default;
  explicit XboxDisplay(MemMap* mem) : mem_(mem) {}
  void attach(MemMap* mem) { mem_ = mem; }

  // Register the frontbuffer. w/h must be non-zero and within max;
  // w*h*4 must fit size_t; the range must be in-range and readable.
  // Replaces any previous framebuffer; clears the last snapshot.
  bool set_framebuffer(uint64_t gpa, uint32_t w, uint32_t h);
  void clear_framebuffer();

  bool has_framebuffer() const { return has_fb_; }
  uint64_t fb_gpa() const { return fb_gpa_; }
  uint32_t fb_width() const { return fb_w_; }
  uint32_t fb_height() const { return fb_h_; }
  uint64_t fb_bytes() const;

  // Snapshot guest framebuffer into host memory. Returns false when no
  // framebuffer is set, no MemMap is attached, or the range became
  // unreadable (counts an error, keeps the previous snapshot).
  bool present();
  // Flip hook for the demo/loop: present() under the honest name.
  bool on_flip() { return present(); }

  const std::vector<uint8_t>& frame() const { return frame_; }
  bool has_frame() const { return !frame_.empty(); }
  uint64_t frames_presented() const { return frames_presented_; }
  uint64_t last_present_qpc() const { return last_present_qpc_; }
  uint64_t errors() const { return errors_; }

  // Read one RGBA8 pixel from the last snapshot. False when OOB/no frame.
  bool pixel(uint32_t x, uint32_t y, uint32_t& out) const;

  // FNV-1a 64 over the last snapshot (0 when empty). Deterministic.
  uint64_t checksum() const;

  // Write the last snapshot as binary P6 PPM (RGB, alpha stripped).
  // False when there is no snapshot or the file cannot be written.
  bool save_ppm(const std::string& path) const;

 private:
  MemMap* mem_ = nullptr;
  bool has_fb_ = false;
  uint64_t fb_gpa_ = 0;
  uint32_t fb_w_ = 0;
  uint32_t fb_h_ = 0;
  std::vector<uint8_t> frame_;
  uint64_t frames_presented_ = 0;
  uint64_t last_present_qpc_ = 0;
  uint64_t errors_ = 0;
};
