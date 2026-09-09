#include "xbe_loader.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace xbe {

namespace {

uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;  // file is little-endian, host x86_64 is too
}

uint16_t rd16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

constexpr uint32_t align_up(uint32_t v, uint32_t a) {
  return (v + a - 1) & ~(a - 1);
}

}  // namespace

// --- XbeImage ---

void XbeImage::fail(std::string* error, const std::string& what) const {
  if (error) *error = what;
}

std::unique_ptr<XbeImage> XbeImage::parse(const std::vector<uint8_t>& file,
                                          std::string* error) {
  std::unique_ptr<XbeImage> img(new XbeImage());
  img->file_ = file;
  if (!img->parse_impl(error)) return nullptr;
  return img;
}

bool XbeImage::parse_impl(std::string* error) {
  if (file_.size() < kHeaderSize) {
    fail(error, "file smaller than XBE header (0x178)");
    return false;
  }
  const uint8_t* f = file_.data();

  Header& h = header_;
  h.magic = rd32(f + 0x000);
  if (h.magic != kMagic) {
    fail(error, "bad magic (expected 'XBEH')");
    return false;
  }
  h.base_addr = rd32(f + 0x104);
  h.sizeof_headers = rd32(f + 0x108);
  h.sizeof_image = rd32(f + 0x10C);
  h.sizeof_image_header = rd32(f + 0x110);
  h.timedate = rd32(f + 0x114);
  h.certificate_addr = rd32(f + 0x118);
  h.num_sections = rd32(f + 0x11C);
  h.section_headers_addr = rd32(f + 0x120);
  h.init_flags = rd32(f + 0x124);
  h.entry_addr = rd32(f + 0x128);
  h.tls_addr = rd32(f + 0x12C);
  h.pe_stack_commit = rd32(f + 0x130);
  h.pe_heap_reserve = rd32(f + 0x134);
  h.pe_heap_commit = rd32(f + 0x138);
  h.pe_base_addr = rd32(f + 0x13C);
  h.pe_sizeof_image = rd32(f + 0x140);
  h.pe_checksum = rd32(f + 0x144);
  h.pe_timedate = rd32(f + 0x148);
  h.debug_pathname_addr = rd32(f + 0x14C);
  h.debug_filename_addr = rd32(f + 0x150);
  h.debug_unicode_filename_addr = rd32(f + 0x154);
  h.kernel_thunk_addr = rd32(f + 0x158);
  h.nonkernel_import_dir_addr = rd32(f + 0x15C);
  h.num_library_versions = rd32(f + 0x160);
  h.library_versions_addr = rd32(f + 0x164);
  h.kernel_library_version_addr = rd32(f + 0x168);
  h.xapi_library_version_addr = rd32(f + 0x16C);
  h.logo_bitmap_addr = rd32(f + 0x170);
  h.logo_bitmap_size = rd32(f + 0x174);

  // Sanity: base + image must fit a 32-bit space, headers cannot exceed
  // the image. (sizeof_image covers headers + sections on real XBEs.)
  if (h.base_addr == 0 || h.sizeof_image == 0 ||
      h.base_addr > 0xFFFFFFFFu - h.sizeof_image) {
    fail(error, "unreasonable base_addr/sizeof_image");
    return false;
  }
  if (h.sizeof_headers > h.sizeof_image) {
    fail(error, "sizeof_headers exceeds sizeof_image");
    return false;
  }
  // Type detection: Cxbx GetXbeType uses the high bit of the ENCODED
  // kernel thunk address. Retail XOR key 0x5B6D40B6 clears the high bit
  // for real (small) thunk VAs, debug key 0xEFB1F152 sets it. The VA
  // range check happens after the sections are parsed (the image extent
  // is derived from them).
  type_ = (h.kernel_thunk_addr & 0x80000000u) ? XbeType::Debug : XbeType::Retail;
  const uint32_t ep_key = type_ == XbeType::Debug ? kXorEpDebug : kXorEpRetail;
  const uint32_t kt_key = type_ == XbeType::Debug ? kXorKtDebug : kXorKtRetail;
  entry_va_ = h.entry_addr ^ ep_key;
  kernel_thunk_va_ = h.kernel_thunk_addr ^ kt_key;

  // Certificate.
  if (h.certificate_addr == 0) {
    fail(error, "certificate address is zero");
    return false;
  }
  {
    uint32_t off = 0;
    if (!header_offset(h.certificate_addr, &off)) {
      fail(error, "certificate address outside headers");
      return false;
    }
    if (off + kCertificateMinSize > file_.size()) {
      fail(error, "certificate extends past end of file");
      return false;
    }
    Certificate& c = cert_;
    c.size = rd32(f + off + 0x00);
    c.timedate = rd32(f + off + 0x04);
    c.title_id = rd32(f + off + 0x08);
    for (int i = 0; i < 40; ++i) {
      char16_t ch = static_cast<char16_t>(rd16(f + off + 0x0C + 2 * i));
      if (ch == 0) break;
      c.title.push_back(ch);
    }
    for (int i = 0; i < 16; ++i)
      c.alternate_title_ids.push_back(rd32(f + off + 0x5C + 4 * i));
    c.allowed_media = rd32(f + off + 0x9C);
    c.game_region = rd32(f + off + 0xA0);
    c.game_ratings = rd32(f + off + 0xA4);
    c.disc_number = rd32(f + off + 0xA8);
    c.version = rd32(f + off + 0xAC);
  }

  // Section headers + names.
  if (h.num_sections > 0) {
    uint32_t off = 0;
    if (!header_offset(h.section_headers_addr, &off)) {
      fail(error, "section headers outside headers");
      return false;
    }
    const uint64_t need =
        static_cast<uint64_t>(off) + kSectionHeaderSize * h.num_sections;
    if (need > file_.size()) {
      fail(error, "section headers extend past end of file");
      return false;
    }
    sections_.resize(h.num_sections);
    for (uint32_t i = 0; i < h.num_sections; ++i) {
      const uint8_t* s = f + off + kSectionHeaderSize * i;
      SectionHeader& sh = sections_[i];
      sh.flags = rd32(s + 0x00);
      sh.virtual_addr = rd32(s + 0x04);
      sh.virtual_size = rd32(s + 0x08);
      sh.raw_addr = rd32(s + 0x0C);
      sh.sizeof_raw = rd32(s + 0x10);
      sh.section_name_addr = rd32(s + 0x14);
      sh.section_ref_count = rd32(s + 0x18);
      sh.head_shared_ref_count_addr = rd32(s + 0x1C);
      sh.tail_shared_ref_count_addr = rd32(s + 0x20);
      // Section name: read from the image via name_addr, bounded.
      sh.name = cstring(sh.section_name_addr);
      if (sh.name.empty()) {
        // Fall back to the inline 8-byte field (name_addr usually points
        // at exactly these 8 bytes inside the header region).
        char inline_name[9];
        std::memcpy(inline_name, s + 0x24, 8);
        inline_name[8] = 0;
        sh.name = inline_name;
      }
      // Bounds: raw data must exist in the file; virtual_size may
      // exceed sizeof_raw (BSS). The virtual range is validated after
      // the loop against the section-derived image extent.
      if (static_cast<uint64_t>(sh.raw_addr) + sh.sizeof_raw > file_.size()) {
        fail(error, "section raw data extends past end of file");
        return false;
      }
      if (sh.sizeof_raw > sh.virtual_size) {
        fail(error, "section raw size exceeds virtual size");
        return false;
      }
    }
  }

  // Image extent (absolute end VA): NOT simply base+sizeof_image.
  // Real nxdk-built XBEs (LithiumX) place the .tls section beyond
  // sizeof_image (0x8CE114 header value vs 0x8EE114 absolute section
  // end), and real loaders map sections by their own headers, so the
  // extent is derived from the sections themselves. Section
  // virtual_addr values are ABSOLUTE VAs (verified: Cxbx GetAddr and
  // pyxbe vaddr_to_file_offset both compare VAs directly against
  // virtual_addr without adding the base).
  {
    uint64_t extent =
        static_cast<uint64_t>(h.base_addr) + h.sizeof_image;
    for (const auto& sh : sections_) {
      const uint64_t end =
          static_cast<uint64_t>(sh.virtual_addr) + sh.virtual_size;
      if (sh.virtual_size > 0 && end > 0xFFFFFFFFull) {
        fail(error, "section virtual range overflow");
        return false;
      }
      if (sh.virtual_size > 0 && sh.virtual_addr < h.base_addr) {
        fail(error, "section virtual address below image base");
        return false;
      }
      if (end > extent) extent = end;
    }
    image_end_abs_ = static_cast<uint32_t>(extent);
    for (uint32_t va : {entry_va_, kernel_thunk_va_}) {
      if (va < h.base_addr || va >= image_end_abs_) {
        fail(error, "decoded entry/thunk VA outside image (wrong XOR key?)");
        return false;
      }
    }
  }

  // Library versions.
  if (h.num_library_versions > 0) {
    uint32_t off = 0;
    if (!header_offset(h.library_versions_addr, &off)) {
      fail(error, "library versions outside headers");
      return false;
    }
    const uint64_t need =
        static_cast<uint64_t>(off) + kLibraryVersionSize * h.num_library_versions;
    if (need > file_.size()) {
      fail(error, "library versions extend past end of file");
      return false;
    }
    libs_.resize(h.num_library_versions);
    for (uint32_t i = 0; i < h.num_library_versions; ++i) {
      const uint8_t* s = f + off + kLibraryVersionSize * i;
      LibraryVersion& lv = libs_[i];
      std::memcpy(lv.name, s, 8);
      lv.name[8] = 0;
      lv.major = rd16(s + 0x8);
      lv.minor = rd16(s + 0xA);
      lv.build = rd16(s + 0xC);
      lv.flags = rd16(s + 0xE);
    }
  }

  // TLS directory. Real XBEs store it wherever the linker put it: in
  // the header block (XDK titles) or inside a section such as .rdata
  // (nxdk titles like LithiumX: dir at VA 0x2F3738). Translate through
  // the section table when needed.
  if (h.tls_addr != 0) {
    uint32_t off = 0;
    if (!va_file_offset(h.tls_addr, &off)) {
      fail(error, "TLS directory not present in file (header or section)");
      return false;
    }
    if (static_cast<uint64_t>(off) + kTlsDirectorySize > file_.size()) {
      fail(error, "TLS directory extends past end of file");
      return false;
    }
    tls_.data_start = rd32(f + off + 0x00);
    tls_.data_end = rd32(f + off + 0x04);
    tls_.index_addr = rd32(f + off + 0x08);
    tls_.callbacks_addr = rd32(f + off + 0x0C);
    tls_.zero_fill = rd32(f + off + 0x10);
    tls_.characteristics = rd32(f + off + 0x14);
    // The template lives in section memory (often .tls / BSS), not in
    // the headers. The full range is checked against the image extent.
    if (tls_.data_start < h.base_addr ||
        tls_.data_start >= image_end_abs_ || !tls_.valid()) {
      fail(error, "TLS template range outside image");
      return false;
    }
    if (tls_.data_end > image_end_abs_) {
      fail(error, "TLS template range outside image");
      return false;
    }
    if (tls_.index_addr < h.base_addr || tls_.index_addr >= image_end_abs_) {
      fail(error, "TLS index address outside image");
      return false;
    }
    tls_present_ = true;
  }

  return true;
}

bool XbeImage::header_offset(uint32_t va, uint32_t* file_off) const {
  if (va < header_.base_addr) return false;
  const uint32_t rel = va - header_.base_addr;
  if (rel >= header_.sizeof_headers) return false;
  // Header structures live at the same offset in file and image.
  if (rel >= file_.size()) return false;
  *file_off = rel;
  return true;
}

bool XbeImage::va_file_offset(uint32_t va, uint32_t* file_off) const {
  uint32_t off = 0;
  if (header_offset(va, &off)) {
    *file_off = off;
    return true;
  }
  if (va < header_.base_addr) return false;
  // Sections: translate through the section table. Section
  // virtual_addr values are ABSOLUTE VAs (see the extent comment in
  // parse_impl). Bytes beyond sizeof_raw are BSS (zero fill) and do
  // not exist in the file.
  for (const auto& sh : sections_) {
    if (sh.virtual_size == 0 || va < sh.virtual_addr) continue;
    const uint64_t into = static_cast<uint64_t>(va) - sh.virtual_addr;
    if (into < sh.virtual_size && into < sh.sizeof_raw) {
      *file_off = sh.raw_addr + static_cast<uint32_t>(into);
      return true;
    }
  }
  return false;
}

std::string XbeImage::cstring(uint32_t va) const {
  uint32_t off = 0;
  if (!va_file_offset(va, &off)) return {};
  const uint8_t* f = file_.data();
  std::string out;
  while (off < file_.size() && f[off] != 0) {
    out.push_back(static_cast<char>(f[off]));
    // Section names are short; never walk into unrelated data.
    if (out.size() >= 64) break;
    ++off;
  }
  return out;
}

// --- XbeRam ---

XbeRam::XbeRam(size_t size_bytes) : size_(size_bytes) {
  page_size_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t pages = (size_ + page_size_ - 1) / page_size_;
  committed_.assign(pages, 0);
  ro_.assign(pages, 0);
  base_ = static_cast<uint8_t*>(
      mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (base_ == MAP_FAILED) {
    base_ = nullptr;
    size_ = 0;
    committed_.clear();
    ro_.clear();
  }
}

XbeRam::~XbeRam() {
  if (base_) munmap(base_, size_);
}

bool XbeRam::pages_committed(uint32_t addr, uint32_t size) const {
  const size_t first = addr / page_size_;
  const size_t last = (addr + size - 1) / page_size_;
  for (size_t p = first; p <= last; ++p)
    if (p >= committed_.size() || committed_[p] == 0) return false;
  return true;
}

bool XbeRam::commit(uint32_t addr, uint32_t size) {
  if (!base_ || !range_ok(addr, size)) return false;
  const size_t first = addr / page_size_;
  const size_t last = (addr + size - 1) / page_size_;
  for (size_t p = first; p <= last; ++p) {
    if (committed_[p]) continue;
    if (mprotect(base_ + p * page_size_, page_size_,
                 PROT_READ | PROT_WRITE) != 0)
      return false;
    committed_[p] = 1;
    ro_[p] = 0;
  }
  return true;
}

bool XbeRam::uncommit(uint32_t addr, uint32_t size) {
  if (!base_ || !range_ok(addr, size)) return false;
  if (!pages_committed(addr, size)) return false;
  const size_t first = addr / page_size_;
  const size_t last = (addr + size - 1) / page_size_;
  for (size_t p = first; p <= last; ++p) {
    if (mprotect(base_ + p * page_size_, page_size_, PROT_NONE) != 0)
      return false;  // committed_ stays set for the untouched pages
    committed_[p] = 0;
    ro_[p] = 0;
  }
  return true;
}

bool XbeRam::protect_ro(uint32_t addr, uint32_t size) {
  if (!is_committed(addr, size)) return false;
  const size_t first = addr / page_size_;
  const size_t last = (addr + size - 1) / page_size_;
  for (size_t p = first; p <= last; ++p) {
    if (ro_[p]) continue;
    if (mprotect(base_ + p * page_size_, page_size_, PROT_READ) != 0)
      return false;
    ro_[p] = 1;
  }
  return true;
}

bool XbeRam::protect_rw(uint32_t addr, uint32_t size) {
  if (!is_committed(addr, size)) return false;
  const size_t first = addr / page_size_;
  const size_t last = (addr + size - 1) / page_size_;
  for (size_t p = first; p <= last; ++p) {
    if (!ro_[p]) continue;
    if (mprotect(base_ + p * page_size_, page_size_,
                 PROT_READ | PROT_WRITE) != 0)
      return false;
    ro_[p] = 0;
  }
  return true;
}

bool XbeRam::is_committed(uint32_t addr, uint32_t size) const {
  return base_ && range_ok(addr, size) && pages_committed(addr, size);
}

bool XbeRam::readable(uint32_t addr, uint32_t size) const {
  return is_committed(addr, size);
}

bool XbeRam::writable(uint32_t addr, uint32_t size) const {
  if (!is_committed(addr, size)) return false;
  const size_t first = addr / page_size_;
  const size_t last = (addr + size - 1) / page_size_;
  for (size_t p = first; p <= last; ++p)
    if (ro_[p]) return false;
  return true;
}

uint8_t* XbeRam::host_ptr(uint32_t addr) {
  if (!is_committed(addr, 1)) return nullptr;
  return base_ + addr;
}

const uint8_t* XbeRam::host_ptr(uint32_t addr) const {
  if (!is_committed(addr, 1)) return nullptr;
  return base_ + addr;
}

bool XbeRam::write_bytes(uint32_t addr, const void* src, uint32_t size) {
  if (!writable(addr, size)) return false;
  std::memcpy(base_ + addr, src, size);
  return true;
}

bool XbeRam::read_bytes(uint32_t addr, void* dst, uint32_t size) const {
  if (!readable(addr, size)) return false;
  std::memcpy(dst, base_ + addr, size);
  return true;
}

bool XbeRam::write32(uint32_t addr, uint32_t value) {
  return write_bytes(addr, &value, 4);
}

bool XbeRam::read32(uint32_t addr, uint32_t* out) const {
  return read_bytes(addr, out, 4);
}

// --- LoadedXbe ---

namespace {
void set_error(std::string* error, const std::string& what) {
  if (error) *error = what;
}
}  // namespace

std::unique_ptr<LoadedXbe> LoadedXbe::load(const std::vector<uint8_t>& file,
                                           std::string* error) {
  std::unique_ptr<LoadedXbe> lx(new LoadedXbe());
  lx->image_ = XbeImage::parse(file, error);
  if (!lx->image_) return nullptr;
  if (!lx->map(error)) return nullptr;
  return lx;
}

bool LoadedXbe::map(std::string* error) {
  const Header& h = image_->header();
  if (!ram_.commit(h.base_addr, h.sizeof_headers)) {
    set_error(error, "cannot commit header pages");
    return false;
  }
  // Copy the whole header block (header + cert + section headers +
  // libraries + strings + logo), exactly like the Xbox loader does:
  // everything before sizeof_headers is the on-image header area.
  if (!ram_.write_bytes(h.base_addr, image_->file().data(), h.sizeof_headers)) {
    set_error(error, "cannot copy header block");
    return false;
  }

  // Sections: copy raw bytes, zero the BSS tail, apply protection.
  // virtual_addr is an ABSOLUTE VA (see parse_impl).
  uint64_t prev_end = 0;
  for (const SectionHeader& sh : image_->sections()) {
    if (sh.virtual_size == 0) continue;
    const uint32_t va = sh.virtual_addr;
    // Overlap guard: real XBEs keep sections separated; refuse
    // anything else instead of guessing.
    if (va < prev_end) {
      set_error(error, "overlapping sections");
      return false;
    }
    prev_end = static_cast<uint64_t>(va) + sh.virtual_size;
    if (!ram_.commit(va, sh.virtual_size)) {
      set_error(error, "cannot commit section pages");
      return false;
    }
    if (sh.sizeof_raw > 0 &&
        !ram_.write_bytes(va, image_->file().data() + sh.raw_addr,
                          sh.sizeof_raw)) {
      set_error(error, "cannot copy section raw data");
      return false;
    }
    if (sh.virtual_size > sh.sizeof_raw) {
      // BSS: zero [va+raw, va+virtual). host_ptr per byte is slow for
      // big fills; the region is committed, so translate once.
      uint8_t* z = ram_.host_ptr(va + sh.sizeof_raw);
      if (!z) {
        set_error(error, "section zero-fill range uncommitted");
        return false;
      }
      std::memset(z, 0, sh.virtual_size - sh.sizeof_raw);
    }
    // Protection from section flags. Executable bit is tracked but NOT
    // passed to mprotect: guest code is never executed by this host.
    if (!(sh.flags & kSectionWriteable))
      ram_.protect_ro(va, sh.virtual_size);
    if (sh.flags & kSectionHeadPageReadonly)
      ram_.protect_ro(va, 1);  // first byte's page
    if (sh.flags & kSectionTailPageReadonly)
      ram_.protect_ro(va + sh.virtual_size - 1, 1);
  }
  return true;
}

std::vector<uint8_t> LoadedXbe::tls_template() const {
  std::vector<uint8_t> out;
  if (!has_tls()) return out;
  const TlsDirectory& t = image_->tls();
  out.resize(t.raw_size() + t.zero_fill);
  if (t.raw_size() > 0) {
    // Copy from the MAPPED image: the template range usually includes
    // BSS bytes that only exist in memory (true for real titles).
    if (!ram_.read_bytes(t.data_start, out.data(), t.raw_size())) return {};
  }
  // zero_fill part is already zeroed by resize().
  return out;
}

bool LoadedXbe::write_tls_index(uint32_t index) {
  if (!has_tls()) return false;
  return ram_.write32(image_->tls().index_addr, index);
}

std::vector<uint32_t> LoadedXbe::tls_callbacks() const {
  std::vector<uint32_t> out;
  if (!has_tls()) return out;
  const TlsDirectory& t = image_->tls();
  if (t.callbacks_addr == 0) return out;
  // NUL-terminated VA array, bounded by the image.
  const uint32_t base = image_->header().base_addr;
  const uint32_t limit = image_->image_end_abs();
  uint32_t va = t.callbacks_addr;
  for (int i = 0; i < 1024; ++i) {  // hard bound against runaway tables
    if (va < base || va > limit - 4) return {};
    uint32_t cb = 0;
    if (!ram_.read32(va, &cb)) return {};
    if (cb == 0) break;
    out.push_back(cb);
    va += 4;
  }
  return out;
}

std::vector<uint32_t> LoadedXbe::kernel_thunk_ordinals() const {
  std::vector<uint32_t> out;
  const uint32_t base = image_->header().base_addr;
  const uint32_t limit = image_->image_end_abs();
  uint32_t va = kernel_thunk_va();
  for (int i = 0; i < 4096; ++i) {  // 366 imports known; hard bound anyway
    if (va < base || va > limit - 4) return {};
    uint32_t entry = 0;
    if (!ram_.read32(va, &entry)) return {};
    if (entry == 0) break;  // zero-terminated
    out.push_back(entry & 0x7FFFFFFFu);
    va += 4;
  }
  return out;
}

}  // namespace xbe
