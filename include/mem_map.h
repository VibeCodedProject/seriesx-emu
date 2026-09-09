#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Xbox Series X memory model, functionally mapped to PC.
// Real HW: 10GB @560GB/s GPU-optimal + 6GB @336GB/s standard, unified.
// PC map (RX 9060 XT 16GB verified):
//   fast pool -> VRAM heap (DEVICE_LOCAL, ReBAR visible)
//   slow pool -> GTT heap (HOST_VISIBLE, coherent)
// MVP reserves virtual address space only; commits pages on demand.
// Phase 3: per-page protection + HLE alias windows (two guest addrs, one host).
enum class MemProt : uint8_t { NoAccess = 0, ReadOnly = 1, ReadWrite = 2 };
class MemMap {
 public:
  static constexpr uint64_t kFastSize = 10ULL << 30;
  static constexpr uint64_t kSlowSize = 6ULL << 30;
  static constexpr uint64_t kTotalSize = kFastSize + kSlowSize;
  static constexpr uint64_t kFastBase = 0x0;
  static constexpr uint64_t kSlowBase = kFastSize;

  MemMap();
  ~MemMap();

  MemMap(const MemMap&) = delete;
  MemMap& operator=(const MemMap&) = delete;

  // Reserve TOTAL virtual space (PROT_NONE). Returns true on success.
  bool reserve();

  // Commit one page at guest_phys so read/write works. Used by tests/demo.
  bool commit_page(uint64_t guest_phys);

  // Commit every page covering [guest_phys, guest_phys+size).
  bool commit_range(uint64_t guest_phys, size_t size);

  bool is_fast(uint64_t guest_phys) const;
  bool in_range(uint64_t guest_phys, size_t size = 1) const;
  bool is_committed(uint64_t guest_phys, size_t size = 1) const;
  uint64_t committed_pages() const { return committed_pages_; }

  // Raw host pointer for committed region, nullptr if uncommitted/out of range.
  void* translate(uint64_t guest_phys);

  bool write32(uint64_t guest_phys, uint32_t value);
  bool read32(uint64_t guest_phys, uint32_t& out);

  bool write_bytes(uint64_t guest_phys, const void* src, size_t size);
  bool read_bytes(uint64_t guest_phys, void* dst, size_t size) const;

  // Phase 3: protection + alias. protect() applies to committed pages only
  // and is enforced by write/read (RO write fails, NoAccess read fails).
  bool protect(uint64_t guest_phys, size_t size, MemProt prot);
  bool is_readable(uint64_t guest_phys, size_t size = 1) const;
  bool is_writable(uint64_t guest_phys, size_t size = 1) const;

  // HLE alias: dst range mirrors src range (same host bytes). Both must be
  // in range; reads/writes to dst redirect to src. No MMU remap.
  bool alias_map(uint64_t dst, uint64_t src, size_t size);
  bool alias_unmap(uint64_t dst);
  uint64_t alias_count() const { return aliases_.size(); }

 private:
  // Resolve HLE alias: returns backing guest addr (src) if guest is an
  // alias dst, else guest unchanged.
  uint64_t resolve_alias(uint64_t guest_phys, size_t size) const;
  long page_size() const;
  void* base_ = nullptr;
  std::vector<uint8_t> committed_;  // one byte per virtual page
  std::vector<uint8_t> prot_;       // MemProt per virtual page
  struct Alias {
    uint64_t dst = 0;
    uint64_t src = 0;
    uint64_t size = 0;
  };
  std::vector<Alias> aliases_;
  uint64_t committed_pages_ = 0;
};
