#include "xbox_vfs.h"

bool Vfs::valid_path(const std::string& path) {
  if (path.size() < 4) return false;  // e.g. "S:\a"
  const char drive = path[0];
  if (drive != 'S' && drive != 'T' && drive != 'U') return false;
  if (path[1] != ':' || path[2] != '\\') return false;
  const std::string rest = path.substr(3);
  if (rest.empty()) return false;
  if (rest.find("..") != std::string::npos) return false;
  for (char c : rest) {
    if (c == '\0' || c == ':') return false;
  }
  return true;
}

const std::vector<uint8_t>* Vfs::find(const std::string& path) const {
  for (const auto& kv : files_) {
    if (kv.first == path) return &kv.second;
  }
  return nullptr;
}

std::vector<uint8_t>* Vfs::find(const std::string& path) {
  for (auto& kv : files_) {
    if (kv.first == path) return &kv.second;
  }
  return nullptr;
}

bool Vfs::create_file(const std::string& path,
                      const std::vector<uint8_t>& data) {
  if (!valid_path(path) || data.size() > kMaxFile) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (find(path)) return false;  // create fails if exists
  files_.emplace_back(path, data);
  return true;
}

bool Vfs::create_file(const std::string& path, const void* data, size_t size) {
  if (!data && size != 0) return false;
  if (size > kMaxFile) return false;
  const auto* bytes = static_cast<const uint8_t*>(data);
  return create_file(path, std::vector<uint8_t>(bytes, bytes + size));
}

bool Vfs::write_file(const std::string& path,
                     const std::vector<uint8_t>& data) {
  if (!valid_path(path) || data.size() > kMaxFile) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (auto* slot = find(path)) {
    *slot = data;
    return true;
  }
  files_.emplace_back(path, data);
  return true;
}

bool Vfs::read_file(const std::string& path, std::vector<uint8_t>& out) const {
  if (!valid_path(path)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const auto* slot = find(path);
  if (!slot) return false;
  out = *slot;
  return true;
}

bool Vfs::read_slice(const std::string& path, uint64_t offset, size_t size,
                     std::vector<uint8_t>& out) const {
  if (!valid_path(path) || size == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const auto* slot = find(path);
  if (!slot) return false;
  // Bounded in-file read: [offset, offset+size) must lie inside the
  // file (no EOF clamping -- DirectStorage requests are bounds-checked
  // the same way). Overflow-safe comparison.
  if (offset > slot->size() || size > slot->size() - offset) return false;
  out.assign(slot->data() + offset, slot->data() + offset + size);
  return true;
}

bool Vfs::exists(const std::string& path) const {
  if (!valid_path(path)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  return find(path) != nullptr;
}

bool Vfs::file_size(const std::string& path, size_t& out) const {
  if (!valid_path(path)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const auto* slot = find(path);
  if (!slot) return false;
  out = slot->size();
  return true;
}

bool Vfs::remove_file(const std::string& path) {
  if (!valid_path(path)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < files_.size(); ++i) {
    if (files_[i].first == path) {
      files_.erase(files_.begin() + static_cast<ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}
