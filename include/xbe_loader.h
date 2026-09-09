#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Real XBE (Xbox Executable) binary loader.
//
// The XBE is the executable format of the original Xbox. Every structure
// below was transcribed from and cross-verified against three independent
// primary sources, and validated end-to-end against a real homebrew XBE
// (LithiumX v0.9.7, built with nxdk, MIT licensed, included as a test
// fixture):
//   1. xboxdevwiki.net/XBE format specification,
//   2. Cxbx-Reloaded src/common/xbe/Xbe.h (+ Xbe.cpp GetXbeType logic),
//   3. xemu-project xemu-xbe.h.
// The parse of the fixture is cross-checked against pyxbe
// (github.com/mborgerson/pyxbe, MIT) as a reference implementation.
//
// What is REAL here:
//   - Full header/certificate/section/library/TLS parsing with bounds
//     checking on every field.
//   - Retail/Debug detection from the encoded kernel thunk address high
//     bit (same method as Cxbx GetXbeType), then entry point + thunk
//     directory decode with the matching XOR key.
//   - Image mapping into a guest RAM window at the XBE base address
//     (retail titles are fixed-base at 0x00010000; the XBE format has
//     NO relocation directory - verified by its absence in all three
//     sources above), with raw data copy, virtual-size zero fill and
//     per-page protection from the section flags.
//   - TLS directory resolution: template copied from the MAPPED image
//     (not the file - for real titles the template often covers BSS
//     that exists only in memory), TLS index written back into the
//     image, callback table resolved.
//   - Kernel thunk table enumeration (ordinal | 0x80000000 entries,
//     zero terminated).
//
// What is NOT here (do not pretend otherwise):
//   - No execution IN THE LOADER: guest code is 32-bit x86; parsing and
//     mapping never run guest instructions. Execution is provided by
//     the OPTIONAL Unicorn CPU core (xbox_cpu.h, ucore::GuestCpu);
//     without that dependency nothing from the image is ever executed.
//   - No BIOS, no keys, no decryption, no signature verification
//     bypass: the format is parsed exactly as the public documentation
//     defines it (this is what every open-source Xbox emulator does).
//     Intended for homebrew XBEs (nxdk / OpenXDK).

namespace xbe {

// --- format constants (verified against the sources above) ---

constexpr uint32_t kMagic = 0x48454258u;  // 'XBEH'
constexpr size_t kHeaderSize = 0x178;     // bytes, minimum (XDK >= 5028 adds more)
constexpr size_t kSectionHeaderSize = 0x38;
constexpr size_t kLibraryVersionSize = 0x10;
constexpr size_t kTlsDirectorySize = 0x18;
constexpr size_t kCertificateMinSize = 0x1D0;  // up to alternate signature keys

// Entry point / kernel thunk XOR keys (Cxbx Xbe.h, xboxdevwiki).
constexpr uint32_t kXorEpDebug = 0x94859D4Bu;
constexpr uint32_t kXorEpRetail = 0xA8FC57ABu;
constexpr uint32_t kXorKtDebug = 0xEFB1F152u;
constexpr uint32_t kXorKtRetail = 0x5B6D40B6u;

// Section flags (Cxbx XBEIMAGE_SECTION_*).
constexpr uint32_t kSectionWriteable = 0x00000001u;
constexpr uint32_t kSectionPreload = 0x00000002u;
constexpr uint32_t kSectionExecutable = 0x00000004u;
constexpr uint32_t kSectionInsertedFile = 0x00000008u;
constexpr uint32_t kSectionHeadPageReadonly = 0x00000010u;
constexpr uint32_t kSectionTailPageReadonly = 0x00000020u;

// Certificate game region flags.
constexpr uint32_t kRegionNA = 0x00000001u;
constexpr uint32_t kRegionJapan = 0x00000002u;
constexpr uint32_t kRegionRestOfWorld = 0x00000004u;
constexpr uint32_t kRegionManufacturing = 0x80000000u;

// Initialization flags (header 0x124).
constexpr uint32_t kInitMountUtilityDrive = 0x00000001u;
constexpr uint32_t kInitFormatUtilityDrive = 0x00000002u;
constexpr uint32_t kInitLimit64MB = 0x00000004u;
constexpr uint32_t kInitDontSetupHarddisk = 0x00000008u;

enum class XbeType { Retail, Debug };

// --- parsed structures (field names follow the format docs) ---

struct Header {
  uint32_t magic;                       // 0x0000 'XBEH'
  uint32_t base_addr;                   // 0x0104 (0x00010000 retail)
  uint32_t sizeof_headers;              // 0x0108
  uint32_t sizeof_image;                // 0x010C
  uint32_t sizeof_image_header;         // 0x0110 (>= 0x178)
  uint32_t timedate;                    // 0x0114
  uint32_t certificate_addr;            // 0x0118
  uint32_t num_sections;                // 0x011C
  uint32_t section_headers_addr;        // 0x0120
  uint32_t init_flags;                  // 0x0124
  uint32_t entry_addr;                  // 0x0128 (encoded, see decode)
  uint32_t tls_addr;                    // 0x012C (0 = no TLS)
  uint32_t pe_stack_commit;             // 0x0130
  uint32_t pe_heap_reserve;             // 0x0134
  uint32_t pe_heap_commit;              // 0x0138
  uint32_t pe_base_addr;                // 0x013C
  uint32_t pe_sizeof_image;             // 0x0140
  uint32_t pe_checksum;                 // 0x0144
  uint32_t pe_timedate;                 // 0x0148
  uint32_t debug_pathname_addr;         // 0x014C
  uint32_t debug_filename_addr;         // 0x0150
  uint32_t debug_unicode_filename_addr; // 0x0154
  uint32_t kernel_thunk_addr;           // 0x0158 (encoded)
  uint32_t nonkernel_import_dir_addr;   // 0x015C
  uint32_t num_library_versions;        // 0x0160
  uint32_t library_versions_addr;       // 0x0164
  uint32_t kernel_library_version_addr; // 0x0168
  uint32_t xapi_library_version_addr;   // 0x016C
  uint32_t logo_bitmap_addr;            // 0x0170 (0 = none)
  uint32_t logo_bitmap_size;            // 0x0174
};

struct Certificate {
  uint32_t size;          // 0x000 size of certificate (page rounded)
  uint32_t timedate;      // 0x004
  uint32_t title_id;      // 0x008
  std::u16string title;   // 0x00C 40 UTF-16LE chars, NUL trimmed
  std::vector<uint32_t> alternate_title_ids;  // 0x05C 16 entries
  uint32_t allowed_media; // 0x09C
  uint32_t game_region;   // 0x0A0
  uint32_t game_ratings;  // 0x0A4
  uint32_t disc_number;   // 0x0A8
  uint32_t version;       // 0x0AC
  // LAN/signature/alternate keys (0x0B0..0x1D0) are NOT parsed further:
  // this loader never uses key material of any kind.
};

struct SectionHeader {
  uint32_t flags;            // kSection* bits
  uint32_t virtual_addr;     // relative to base_addr
  uint32_t virtual_size;     // may exceed raw size (BSS zero fill)
  uint32_t raw_addr;         // file offset
  uint32_t sizeof_raw;       // file bytes
  std::string name;          // read via name_addr (NUL-terminated, 8 chars inline)
  uint32_t section_name_addr;
  uint32_t section_ref_count;
  uint32_t head_shared_ref_count_addr;
  uint32_t tail_shared_ref_count_addr;
};

struct LibraryVersion {
  char name[9];  // 8 bytes + NUL
  uint16_t major;
  uint16_t minor;
  uint16_t build;
  uint16_t flags;  // QFE:13 | Approved:2 | DebugBuild:1
};

// PE32-identical TLS directory (xboxdevwiki: "can be directly copied
// from there"). All addresses are absolute VAs.
struct TlsDirectory {
  uint32_t data_start;     // VA of template start
  uint32_t data_end;       // VA of template end
  uint32_t index_addr;     // VA of the TLS index variable
  uint32_t callbacks_addr; // VA of NUL-terminated callback VA array (0 = none)
  uint32_t zero_fill;
  uint32_t characteristics;
  bool valid() const { return data_end >= data_start; }
  uint32_t raw_size() const { return data_end - data_start; }
};

// Read-only parse of an XBE file image. Bounds-checks every structure.
class XbeImage {
 public:
  // Parses `file`. Returns nullptr with a human-readable reason in
  // *error on malformed input (bad magic, truncated structures,
  // out-of-range VAs...).
  static std::unique_ptr<XbeImage> parse(const std::vector<uint8_t>& file,
                                         std::string* error = nullptr);

  const Header& header() const { return header_; }
  const Certificate& certificate() const { return cert_; }
  const std::vector<SectionHeader>& sections() const { return sections_; }
  const std::vector<LibraryVersion>& libraries() const { return libs_; }

  bool has_tls() const { return tls_present_; }
  const TlsDirectory& tls() const { return tls_; }

  XbeType type() const { return type_; }
  // Decoded (XOR key removed) entry point / kernel thunk directory VA.
  uint32_t entry_va() const { return entry_va_; }
  uint32_t kernel_thunk_va() const { return kernel_thunk_va_; }

  // Image extent as an absolute end VA: max(base+sizeof_image, section
  // ends, TLS end). Real XBEs can place sections beyond sizeof_image,
  // so every range validation uses this value (see xbe_loader.cpp).
  uint32_t image_end_abs() const { return image_end_abs_; }

  // UTF-16LE debug filename (may be empty when the field is 0).
  std::string debug_pathname() const { return cstring(header_.debug_pathname_addr); }
  std::string debug_filename() const { return cstring(header_.debug_filename_addr); }

  // Section name lookup: reads the NUL-terminated string at name_addr.
  // (SectionHeader.name carries the same string for convenience.)
  std::string cstring(uint32_t va) const;

  // Raw file bytes access for tests/tools.
  const std::vector<uint8_t>& file() const { return file_; }

  // VA -> file offset for header-region structures (base-relative).
  // va_file_offset() additionally resolves VAs that live in SECTION
  // raw data (real XBEs put the TLS directory in .rdata); BSS VAs have
  // no file bytes and return false.
  bool header_offset(uint32_t va, uint32_t* file_off) const;
  bool va_file_offset(uint32_t va, uint32_t* file_off) const;

 private:
  XbeImage() = default;
  bool parse_impl(std::string* error);
  void fail(std::string* error, const std::string& what) const;

  std::vector<uint8_t> file_;
  Header header_{};
  Certificate cert_;
  std::vector<SectionHeader> sections_;
  std::vector<LibraryVersion> libs_;
  TlsDirectory tls_{};
  bool tls_present_ = false;
  XbeType type_ = XbeType::Retail;
  uint32_t entry_va_ = 0;
  uint32_t kernel_thunk_va_ = 0;
  uint32_t image_end_abs_ = 0;
};

// Guest RAM window for XBE images: classic Xbox user memory starts at
// 0x00010000 inside a 64 MiB RAM (128 MiB debug). The window is mmap'd
// PROT_NONE and committed per page, like MemMap, but with the linear
// layout the XBE format expects (base_addr is an offset into this
// window, not into the Series-X map).
class XbeRam {
 public:
  explicit XbeRam(size_t size_bytes = 64u << 20);
  ~XbeRam();
  XbeRam(const XbeRam&) = delete;
  XbeRam& operator=(const XbeRam&) = delete;

  size_t size() const { return size_; }

  // Commit [addr, addr+size) RW. False on range/rounding errors.
  bool commit(uint32_t addr, uint32_t size);
  // Uncommit [addr, addr+size): pages return to PROT_NONE guard state
  // (real mprotect, read-back enforced) and are marked uncommitted.
  // False on range errors or when any covering page is not committed.
  // Used by MmFreeContiguousMemory - the real kernel returns freed
  // contiguous pages to free physical memory.
  bool uncommit(uint32_t addr, uint32_t size);
  // Shrink protection to RO (backed by mprotect; read-back enforced).
  bool protect_ro(uint32_t addr, uint32_t size);
  // Restore RW protection on previously read-only pages (used by the
  // kernel HLE when protection changes, e.g. MmSetAddressProtect).
  bool protect_rw(uint32_t addr, uint32_t size);
  bool is_committed(uint32_t addr, uint32_t size = 1) const;
  bool readable(uint32_t addr, uint32_t size = 1) const;
  bool writable(uint32_t addr, uint32_t size = 1) const;

  // Host pointer for a committed address, nullptr otherwise.
  uint8_t* host_ptr(uint32_t addr);
  const uint8_t* host_ptr(uint32_t addr) const;

  bool write_bytes(uint32_t addr, const void* src, uint32_t size);
  bool read_bytes(uint32_t addr, void* dst, uint32_t size) const;
  bool write32(uint32_t addr, uint32_t value);
  bool read32(uint32_t addr, uint32_t* out) const;

 private:
  bool range_ok(uint32_t addr, uint32_t size) const {
    return size_ > 0 && size != 0 && size <= size_ && addr <= size_ - size;
  }
  bool pages_committed(uint32_t addr, uint32_t size) const;
  uint8_t* base_ = nullptr;
  size_t size_ = 0;
  std::vector<uint8_t> committed_;  // per page: 0 = PROT_NONE, 1 = live
  std::vector<uint8_t> ro_;         // per page: 1 = read-only
  size_t page_size_ = 4096;
};

// A mapped XBE: owns the RAM window and exposes everything the "kernel"
// side (fiber TLS, thunk dispatch) needs from the loaded image.
class LoadedXbe {
 public:
  // Parses and maps `file` (headers + all sections) into a fresh
  // 64 MiB RAM window. Returns nullptr with *error set on failure.
  static std::unique_ptr<LoadedXbe> load(const std::vector<uint8_t>& file,
                                         std::string* error = nullptr);

  const XbeImage& image() const { return *image_; }
  XbeRam& ram() { return ram_; }
  const XbeRam& ram() const { return ram_; }

  // TLS support for the fiber runtime:
  //   - template(): raw TLS template bytes copied out of the MAPPED
  //     image (covers BSS correctly), followed by zero_fill bytes that
  //     the TLS runtime must also zero per fiber.
  //   - write_tls_index(index): stores the runtime-assigned TLS index
  //     into the image at tls.index_addr (this is what guest code reads
  //     with an absolute load before touching the per-thread array).
  //   - tls_callbacks(): resolved callback VAs from the NUL-terminated
  //     table in the image (empty when callbacks_addr is 0).
  std::vector<uint8_t> tls_template() const;
  bool write_tls_index(uint32_t index);
  std::vector<uint32_t> tls_callbacks() const;
  bool has_tls() const { return image_->has_tls(); }

  // Kernel thunk table: decoded directory VA and the ordinal list
  // (entry & 0x7FFFFFFF of each nonzero, non-flagged entry), read from
  // the mapped image. Entries are `ordinal | 0x80000000`, the table is
  // NUL-terminated (verified against a real XBE + pyxbe).
  uint32_t kernel_thunk_va() const { return image_->kernel_thunk_va(); }
  std::vector<uint32_t> kernel_thunk_ordinals() const;

  // Read helpers over the mapped image.
  bool read32(uint32_t va, uint32_t* out) const { return ram_.read32(va, out); }
  uint8_t* host_ptr(uint32_t va) { return ram_.host_ptr(va); }

 private:
  LoadedXbe() = default;
  bool map(std::string* error);
  std::unique_ptr<XbeImage> image_;
  XbeRam ram_;
};

}  // namespace xbe
