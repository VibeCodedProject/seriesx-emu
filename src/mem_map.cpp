#include "mem_map.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

MemMap::MemMap() = default;

MemMap::~MemMap() {
  if (base_) {
    munmap(base_, kTotalSize);
    base_ = nullptr;
  }
}

long MemMap::page_size() const {
  static const long kPage = sysconf(_SC_PAGESIZE);
  return kPage > 0 ? kPage : 4096;
}

bool MemMap::reserve() {
  if (base_) return true;
  void* p = mmap(nullptr, kTotalSize, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) return false;
  base_ = p;
  const uint64_t pages = kTotalSize / static_cast<uint64_t>(page_size());
  committed_.assign(static_cast<size_t>(pages), 0);
  prot_.assign(static_cast<size_t>(pages),
               static_cast<uint8_t>(MemProt::ReadWrite));
  committed_pages_ = 0;
  aliases_.clear();
  return true;
}

bool MemMap::commit_page(uint64_t guest_phys) {
  if (!base_ || !in_range(guest_phys)) return false;
  const long page = page_size();
  uint64_t aligned = guest_phys & ~static_cast<uint64_t>(page - 1);
  void* addr = static_cast<char*>(base_) + aligned;
  if (mprotect(addr, static_cast<size_t>(page), PROT_READ | PROT_WRITE) != 0)
    return false;
  const uint64_t idx = aligned / static_cast<uint64_t>(page);
  if (idx < committed_.size() && !committed_[static_cast<size_t>(idx)]) {
    committed_[static_cast<size_t>(idx)] = 1;
    ++committed_pages_;
  }
  if (idx < prot_.size())
    prot_[static_cast<size_t>(idx)] =
        static_cast<uint8_t>(MemProt::ReadWrite);
  return true;
}

bool MemMap::commit_range(uint64_t guest_phys, size_t size) {
  if (!base_ || size == 0 || !in_range(guest_phys, size)) return false;
  const long page = page_size();
  const uint64_t upage = static_cast<uint64_t>(page);
  uint64_t start = guest_phys & ~(upage - 1);
  uint64_t end = guest_phys + size;  // in_range guarantees no wrap
  for (uint64_t a = start; a < end; a += upage) {
    if (!commit_page(a)) return false;
  }
  return true;
}

bool MemMap::is_fast(uint64_t guest_phys) const {
  return guest_phys < kFastSize;
}

bool MemMap::in_range(uint64_t guest_phys, size_t size) const {
  if (guest_phys >= kTotalSize) return false;
  return size <= (kTotalSize - guest_phys);
}

bool MemMap::is_committed(uint64_t guest_phys, size_t size) const {
  if (!base_ || !in_range(guest_phys, size) || committed_.empty()) return false;
  const long page = page_size();
  const uint64_t upage = static_cast<uint64_t>(page);
  uint64_t start = guest_phys & ~(upage - 1);
  uint64_t end = guest_phys + size;
  for (uint64_t a = start; a < end; a += upage) {
    const uint64_t idx = a / upage;
    if (idx >= committed_.size() || !committed_[static_cast<size_t>(idx)])
      return false;
  }
  return true;
}

void* MemMap::translate(uint64_t guest_phys) {
  if (!is_readable(guest_phys, 1)) return nullptr;
  return static_cast<char*>(base_) + resolve_alias(guest_phys, 1);
}

bool MemMap::write32(uint64_t guest_phys, uint32_t value) {
  return write_bytes(guest_phys, &value, sizeof(value));
}

bool MemMap::read32(uint64_t guest_phys, uint32_t& out) {
  return read_bytes(guest_phys, &out, sizeof(out));
}

bool MemMap::write_bytes(uint64_t guest_phys, const void* src, size_t size) {
  if (!src || size == 0) return false;
  if (!is_writable(guest_phys, size)) return false;
  void* p = static_cast<char*>(base_) + resolve_alias(guest_phys, size);
  std::memcpy(p, src, size);
  return true;
}

bool MemMap::read_bytes(uint64_t guest_phys, void* dst, size_t size) const {
  if (!dst || size == 0) return false;
  if (!is_readable(guest_phys, size)) return false;
  const void* p =
      static_cast<const char*>(base_) + resolve_alias(guest_phys, size);
  std::memcpy(dst, p, size);
  return true;
}

bool MemMap::protect(uint64_t guest_phys, size_t size, MemProt prot) {
  if (!base_ || size == 0 || !is_committed(guest_phys, size)) return false;
  const long page = page_size();
  const uint64_t upage = static_cast<uint64_t>(page);
  uint64_t start = guest_phys & ~(upage - 1);
  uint64_t end = guest_phys + size;
  const int os_prot = prot == MemProt::ReadWrite
                          ? (PROT_READ | PROT_WRITE)
                          : (prot == MemProt::ReadOnly ? PROT_READ : PROT_NONE);
  for (uint64_t a = start; a < end; a += upage) {
    void* addr = static_cast<char*>(base_) + a;
    if (mprotect(addr, static_cast<size_t>(page), os_prot) != 0) return false;
    const uint64_t idx = a / upage;
    if (idx < prot_.size())
      prot_[static_cast<size_t>(idx)] = static_cast<uint8_t>(prot);
  }
  return true;
}

bool MemMap::is_readable(uint64_t guest_phys, size_t size) const {
  if (!base_ || size == 0 || !in_range(guest_phys, size)) return false;
  const uint64_t backing = resolve_alias(guest_phys, size);
  if (!in_range(backing, size)) return false;
  const long page = page_size();
  const uint64_t upage = static_cast<uint64_t>(page);
  uint64_t start = backing & ~(upage - 1);
  uint64_t end = backing + size;
  for (uint64_t a = start; a < end; a += upage) {
    const uint64_t idx = a / upage;
    if (idx >= committed_.size() || !committed_[static_cast<size_t>(idx)])
      return false;
    if (idx < prot_.size() &&
        prot_[static_cast<size_t>(idx)] ==
            static_cast<uint8_t>(MemProt::NoAccess))
      return false;
  }
  return true;
}

bool MemMap::is_writable(uint64_t guest_phys, size_t size) const {
  if (!base_ || size == 0 || !in_range(guest_phys, size)) return false;
  const uint64_t backing = resolve_alias(guest_phys, size);
  if (!in_range(backing, size)) return false;
  const long page = page_size();
  const uint64_t upage = static_cast<uint64_t>(page);
  uint64_t start = backing & ~(upage - 1);
  uint64_t end = backing + size;
  for (uint64_t a = start; a < end; a += upage) {
    const uint64_t idx = a / upage;
    if (idx >= committed_.size() || !committed_[static_cast<size_t>(idx)])
      return false;
    if (idx >= prot_.size() ||
        prot_[static_cast<size_t>(idx)] !=
            static_cast<uint8_t>(MemProt::ReadWrite))
      return false;
  }
  return true;
}

bool MemMap::alias_map(uint64_t dst, uint64_t src, size_t size) {
  if (size == 0 || !in_range(dst, size) || !in_range(src, size)) return false;
  // Reject self-overlap and dst-overlap with existing aliases.
  const uint64_t dst_end = dst + size;
  const uint64_t src_end = src + size;
  if (dst < src_end && src < dst_end) return false;
  for (const auto& a : aliases_) {
    const uint64_t a_end = a.dst + a.size;
    if (dst < a_end && a.dst < dst_end) return false;
  }
  aliases_.push_back(Alias{.dst = dst, .src = src, .size = size});
  return true;
}

bool MemMap::alias_unmap(uint64_t dst) {
  for (size_t i = 0; i < aliases_.size(); ++i) {
    if (aliases_[i].dst == dst) {
      aliases_.erase(aliases_.begin() + static_cast<ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

uint64_t MemMap::resolve_alias(uint64_t guest_phys, size_t size) const {
  for (const auto& a : aliases_) {
    // Upper-bound check first: without it (a.dst + a.size - guest_phys)
    // underflows for guests above the alias and wrongly matches.
    if (guest_phys >= a.dst && guest_phys < a.dst + a.size &&
        size <= (a.dst + a.size - guest_phys))
      return a.src + (guest_phys - a.dst);
  }
  return guest_phys;
}
