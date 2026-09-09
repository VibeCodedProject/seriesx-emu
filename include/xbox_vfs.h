#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Homebrew-only VFS (no XVD, no keys, no retail titles).
// Drives: S:\ payload (read-mostly), T:\ scratch, U:\ save.
// Title is always the synthetic homebrew title.
//
// Thread-safety: methods take an internal mutex, so the async IoQueue
// worker can read slices while the title thread creates/updates files.
struct XTitle {
  uint32_t id = 0xFFFF0001u;
  std::string name = "HOME-BREW";
};

class Vfs {
 public:
  static constexpr size_t kMaxFile = 64ULL << 20;  // 64MB per file cap

  const XTitle& title() const { return title_; }

  bool create_file(const std::string& path, const std::vector<uint8_t>& data);
  bool create_file(const std::string& path, const void* data, size_t size);
  bool write_file(const std::string& path, const std::vector<uint8_t>& data);
  bool read_file(const std::string& path, std::vector<uint8_t>& out) const;
  // Bounded in-file read of [offset, offset+size); fails if the range
  // runs past EOF (no clamping, matching DirectStorage request rules).
  bool read_slice(const std::string& path, uint64_t offset, size_t size,
                  std::vector<uint8_t>& out) const;
  bool exists(const std::string& path) const;
  bool file_size(const std::string& path, size_t& out) const;
  bool remove_file(const std::string& path);
  size_t file_count() const { return files_.size(); }

  // Path must be D:\rest with D in {S,T,U}, no "..", rest non-empty.
  static bool valid_path(const std::string& path);

 private:
  XTitle title_{};
  mutable std::mutex mu_;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files_;

  const std::vector<uint8_t>* find(const std::string& path) const;
  std::vector<uint8_t>* find(const std::string& path);
};
