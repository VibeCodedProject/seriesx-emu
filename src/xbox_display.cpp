#include "xbox_display.h"

#include <cstdio>
#include <limits>

#include "cpu_native.h"
#include "mem_map.h"

uint64_t XboxDisplay::fb_bytes() const {
  return static_cast<uint64_t>(fb_w_) * fb_h_ * kBytesPerPixel;
}

bool XboxDisplay::set_framebuffer(uint64_t gpa, uint32_t w, uint32_t h) {
  if (w == 0 || h == 0 || w > kMaxWidth || h > kMaxHeight) return false;
  const uint64_t need =
      static_cast<uint64_t>(w) * h * kBytesPerPixel;
  if (need / kBytesPerPixel != static_cast<uint64_t>(w) * h) return false;
  if (need > std::numeric_limits<size_t>::max()) return false;
  if (!mem_ || !mem_->in_range(gpa, static_cast<size_t>(need))) return false;
  if (!mem_->is_readable(gpa, static_cast<size_t>(need))) return false;
  fb_gpa_ = gpa;
  fb_w_ = w;
  fb_h_ = h;
  has_fb_ = true;
  frame_.clear();
  return true;
}

void XboxDisplay::clear_framebuffer() {
  has_fb_ = false;
  fb_gpa_ = 0;
  fb_w_ = 0;
  fb_h_ = 0;
  frame_.clear();
}

bool XboxDisplay::present() {
  if (!has_fb_ || !mem_) {
    ++errors_;
    return false;
  }
  const uint64_t need = fb_bytes();
  if (need == 0 || !mem_->is_readable(fb_gpa_, static_cast<size_t>(need))) {
    ++errors_;
    return false;
  }
  std::vector<uint8_t> snap(static_cast<size_t>(need));
  if (!mem_->read_bytes(fb_gpa_, snap.data(), snap.size())) {
    ++errors_;
    return false;
  }
  frame_ = std::move(snap);
  ++frames_presented_;
  last_present_qpc_ = cpu_qpc_now();
  return true;
}

bool XboxDisplay::pixel(uint32_t x, uint32_t y, uint32_t& out) const {
  if (!has_fb_ || frame_.empty() || x >= fb_w_ || y >= fb_h_) return false;
  const size_t off = (static_cast<size_t>(y) * fb_w_ + x) * kBytesPerPixel;
  if (off + 4 > frame_.size()) return false;
  out = static_cast<uint32_t>(frame_[off]) |
        (static_cast<uint32_t>(frame_[off + 1]) << 8) |
        (static_cast<uint32_t>(frame_[off + 2]) << 16) |
        (static_cast<uint32_t>(frame_[off + 3]) << 24);
  return true;
}

uint64_t XboxDisplay::checksum() const {
  if (frame_.empty()) return 0;
  uint64_t h = 1469598103934665603ULL;  // FNV-1a 64 offset
  for (uint8_t b : frame_) {
    h ^= b;
    h *= 1099511628211ULL;
  }
  return h;
}

bool XboxDisplay::save_ppm(const std::string& path) const {
  if (frame_.empty() || !has_fb_) return false;
  const size_t need =
      static_cast<size_t>(fb_w_) * fb_h_ * kBytesPerPixel;
  if (frame_.size() < need) return false;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", fb_w_, fb_h_);
  for (uint32_t y = 0; y < fb_h_; ++y) {
    for (uint32_t x = 0; x < fb_w_; ++x) {
      const size_t off = (static_cast<size_t>(y) * fb_w_ + x) * kBytesPerPixel;
      const uint8_t rgb[3] = {frame_[off], frame_[off + 1], frame_[off + 2]};
      if (std::fwrite(rgb, 1, 3, f) != 3) {
        std::fclose(f);
        return false;
      }
    }
  }
  std::fclose(f);
  return true;
}
