#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbe_loader.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

using namespace xbe;

// ---- synthetic XBE builder (layout from xboxdevwiki/Cxbx/xemu, all
// ---- offsets verified against those sources and against pyxbe) ----

namespace {

struct Builder {
  std::vector<uint8_t> buf;

  Builder() { buf.assign(0x1000, 0); }
  void ensure(size_t need) {
    if (buf.size() < need) buf.resize(need);
  }
  template <typename T>
  void put(size_t off, T v) {
    ensure(off + sizeof(T));
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    std::copy(p, p + sizeof(T), buf.begin() + static_cast<long>(off));
  }
  void put_bytes(size_t off, const void* p, size_t n) {
    ensure(off + n);
    const auto* b = static_cast<const uint8_t*>(p);
    std::copy(b, b + n, buf.begin() + static_cast<long>(off));
  }
  // Append data, return its file offset (== VA - base for this layout).
  uint32_t append(const void* p, size_t n) {
    if (buf.size() + n > buf.capacity()) buf.reserve((buf.size() + n) * 2);
    const uint32_t off = static_cast<uint32_t>(buf.size());
    buf.insert(buf.end(), static_cast<const uint8_t*>(p),
               static_cast<const uint8_t*>(p) + n);
    return off;
  }
  uint32_t append_cstr(const std::string& s) {
    return append(s.c_str(), s.size() + 1);
  }
  void finalize(size_t base, uint32_t ep, uint32_t kthunk, uint32_t cert_off,
                uint32_t sec_hdr_off, uint32_t nsec, uint32_t tls_off,
                uint32_t lib_off, uint32_t nlibs, uint32_t img_size,
                uint32_t init_flags) {
    put(0x000, 0x48454258u);  // 'XBEH'
    // digital signature block (0x004..0x104) left zero: we never check it
    put(0x104, static_cast<uint32_t>(base));
    put(0x108, 0x1000u);  // sizeof_headers: first page
    put(0x10C, img_size);
    put(0x110, 0x178u);
    put(0x114, 0x5F5E1000u);  // timedate
    put(0x118, static_cast<uint32_t>(base) + cert_off);
    put(0x11C, nsec);
    put(0x120, static_cast<uint32_t>(base) + sec_hdr_off);
    put(0x124, init_flags);
    put(0x128, ep);      // already encoded by the caller
    put(0x12C, tls_off ? static_cast<uint32_t>(base) + tls_off : 0);
    put(0x130, 0x00010000u);  // pe stack commit
    put(0x134, 0x00100000u);  // pe heap reserve
    put(0x138, 0x00010000u);  // pe heap commit
    put(0x13C, static_cast<uint32_t>(base));
    put(0x140, img_size);
    put(0x144, 0xDEADBEEFu);  // pe checksum
    put(0x148, 0x5F5E1001u);
    put(0x14C, 0);  // debug path
    put(0x150, 0);  // debug file
    put(0x154, 0);
    put(0x158, kthunk);  // already encoded
    put(0x15C, 0);
    put(0x160, nlibs);
    put(0x164, static_cast<uint32_t>(base) + lib_off);
    put(0x168, 0);
    put(0x16C, 0);
    put(0x170, 0);
    put(0x174, 0);
  }
};

struct TlsSpec {
  uint32_t data_off = 0;  // file offset of template (VA = base + off)
  uint32_t raw_size = 0;
  uint32_t index_off = 0;
  uint32_t callbacks_off = 0;  // 0 = none
  uint32_t zero_fill = 0;
};

// Writes a TLS directory + callback table into the builder.
void write_tls(Builder& b, uint32_t dir_off, const TlsSpec& t) {
  b.put(dir_off + 0x00, t.data_off ? 0x10000u + t.data_off : 0);
  b.put(dir_off + 0x04, 0x10000u + t.data_off + t.raw_size);
  b.put(dir_off + 0x08, 0x10000u + t.index_off);
  b.put(dir_off + 0x0C,
        t.callbacks_off ? 0x10000u + t.callbacks_off : 0u);
  b.put(dir_off + 0x10, t.zero_fill);
  b.put(dir_off + 0x14, 0u);
}

// A section with raw data and a BSS tail.
void write_section(Builder& b, uint32_t hdr_off, uint32_t name_off,
                   uint32_t va, uint32_t vsize, uint32_t raw_off,
                   uint32_t raw_size, uint32_t flags) {
  b.put(hdr_off + 0x00, flags);
  b.put(hdr_off + 0x04, va);
  b.put(hdr_off + 0x08, vsize);
  b.put(hdr_off + 0x0C, raw_off);
  b.put(hdr_off + 0x10, raw_size);
  b.put(hdr_off + 0x14, 0x10000u + name_off);
  b.put(hdr_off + 0x18, 1u);  // ref count
  b.put(hdr_off + 0x1C, 0x10000u + name_off);      // head ref addr
  b.put(hdr_off + 0x20, 0x10000u + name_off + 4);  // tail ref addr
  b.put_bytes(hdr_off + 0x24, name_off ? "SECTCODE" : "SECTDATA", 8);
}

uint32_t encode_ep(uint32_t ep, XbeType t) {
  return ep ^ (t == XbeType::Debug ? kXorEpDebug : kXorEpRetail);
}
uint32_t encode_kt(uint32_t kt, XbeType t) {
  return kt ^ (t == XbeType::Debug ? kXorKtDebug : kXorKtRetail);
}

std::vector<uint8_t> make_xbe(XbeType type, bool with_tls, bool with_bss,
                              uint32_t* ep_out, uint32_t* kthunk_out,
                              TlsSpec* tls_out = nullptr) {
  Builder b;
  const uint32_t base = 0x10000;
  const uint32_t kthunk_va = base + 0x800;  // thunk table inside headers
  // kernel thunk table: two imports + terminator, entries ordinal|1<<31
  const uint32_t kthunk_entries[3] = {0x80000034u, 0x800000A1u, 0};
  b.put_bytes(0x800, kthunk_entries, sizeof(kthunk_entries));

  // TLS structures (inside headers area).
  TlsSpec t{};
  uint8_t tmpl[16];
  for (int i = 0; i < 16; ++i) tmpl[i] = static_cast<uint8_t>(0xA0 + i);
  uint32_t tls_dir_off = 0;
  if (with_tls) {
    t.data_off = 0x600;  // template in headers page (memory-resident)
    t.raw_size = 16;
    t.index_off = 0x640;
    t.callbacks_off = 0x680;
    b.put_bytes(0x600, tmpl, 16);
    b.put(0x640, 0xFFFFFFFFu);  // index var: garbage pre-load
    const uint32_t cbs[2] = {base + 0x200000u, 0};  // 1 callback + NUL
    b.put_bytes(0x680, cbs, sizeof(cbs));
    tls_dir_off = 0x700;
    write_tls(b, tls_dir_off, t);
  }

  // Certificate (min size 0x1D0).
  const uint32_t cert_off = 0x400;
  b.put(cert_off + 0x00, 0x1000u);   // cert size
  b.put(cert_off + 0x04, 0x11111111u);
  b.put(cert_off + 0x08, 0x4A4F0001u);  // title id 'JO' + 1
  const char16_t title[6] = {u'T', u'e', u's', u't', u'!', 0};
  b.put_bytes(cert_off + 0x0C, title, sizeof(title));
  b.put(cert_off + 0x9C, 0x00000001u);        // media: hard disk
  b.put(cert_off + 0xA0, kRegionNA | kRegionJapan);
  b.put(cert_off + 0xA4, 0u);   // ratings
  b.put(cert_off + 0xA8, 1u);   // disc number
  b.put(cert_off + 0xAC, 0u);   // version

  // Library versions (one).
  const uint32_t lib_off = 0x540;
  b.put_bytes(lib_off, "XAPILIB", 8);
  b.put(lib_off + 0x8, static_cast<uint16_t>(1));
  b.put(lib_off + 0xA, static_cast<uint16_t>(2));
  b.put(lib_off + 0xC, static_cast<uint16_t>(5849));
  b.put(lib_off + 0xE, static_cast<uint16_t>(0x8000));  // debug build flag

  // Section headers (2 sections) + section data.
  const uint32_t sec_hdr_off = 0x300;
  const uint32_t name_text_off = 0x520, name_data_off = 0x528;
  b.put_bytes(name_text_off, ".text\0\0\0", 8);
  b.put_bytes(name_data_off, ".data\0\0\0", 8);

  const uint32_t text_va = 0x11000, text_raw_off = 0x1000;  // abs VA
  uint8_t text_data[0x800];
  for (size_t i = 0; i < sizeof(text_data); ++i)
    text_data[i] = static_cast<uint8_t>(i & 0xFF);
  b.put_bytes(text_raw_off, text_data, sizeof(text_data));

  const uint32_t data_va = 0x13000;  // abs VA; vsize 0x2000 > raw 0x100 (BSS)
  uint8_t data_raw[0x100];
  for (size_t i = 0; i < sizeof(data_raw); ++i)
    data_raw[i] = static_cast<uint8_t>(0x80 + (i & 0x3F));
  const uint32_t data_raw_off = 0x2000;
  b.put_bytes(data_raw_off, data_raw, sizeof(data_raw));

  write_section(b, sec_hdr_off, name_text_off, text_va, 0x1000, text_raw_off,
                sizeof(text_data), kSectionExecutable | kSectionPreload);
  write_section(b, sec_hdr_off + kSectionHeaderSize, name_data_off, data_va,
                with_bss ? 0x2000 : 0x100, data_raw_off,
                with_bss ? 0x100 : sizeof(data_raw),
                kSectionWriteable | kSectionPreload);

  const uint32_t ep_va = text_va + 0x40;
  // sizeof_image deliberately SMALLER than the section extent (the
  // .data BSS tail reaches 0x15000) to exercise the derived-extent
  // logic real nxdk XBEs require.
  const uint32_t img_size = 0x14000;
  b.finalize(base, encode_ep(ep_va, type), encode_kt(kthunk_va, type),
             cert_off, sec_hdr_off, 2, tls_dir_off, lib_off, 1, img_size, 1);
  if (ep_out) *ep_out = ep_va;
  if (kthunk_out) *kthunk_out = kthunk_va;
  if (tls_out) *tls_out = t;
  return b.buf;
}

}  // namespace

// ---- tests ----

static int test_synthetic_parse() {
  uint32_t ep = 0, kthunk = 0;
  TlsSpec t;
  const auto file = make_xbe(XbeType::Retail, true, true, &ep, &kthunk, &t);

  std::string err;
  auto img = XbeImage::parse(file, &err);
  CHECK(img != nullptr);
  const Header& h = img->header();
  CHECK(h.magic == kMagic);
  CHECK(h.base_addr == 0x10000);
  CHECK(h.sizeof_headers == 0x1000);
  CHECK(h.num_sections == 2);
  CHECK(h.init_flags == kInitMountUtilityDrive);
  CHECK(img->type() == XbeType::Retail);
  CHECK(img->entry_va() == ep);
  CHECK(img->kernel_thunk_va() == kthunk);
  CHECK(img->certificate().title == u"Test!");
  CHECK(img->certificate().title_id == 0x4A4F0001u);
  CHECK(img->certificate().game_region == (kRegionNA | kRegionJapan));
  CHECK(img->certificate().allowed_media == 0x1u);
  CHECK(img->certificate().disc_number == 1);
  CHECK(img->sections().size() == 2);
  CHECK(img->sections()[0].name == ".text");
  CHECK(img->sections()[1].name == ".data");
  CHECK(img->sections()[0].flags == (kSectionExecutable | kSectionPreload));
  CHECK(img->sections()[1].virtual_size == 0x2000);
  CHECK(img->sections()[1].sizeof_raw == 0x100);
  CHECK(img->libraries().size() == 1);
  CHECK(std::string(img->libraries()[0].name) == "XAPILIB");
  CHECK(img->libraries()[0].build == 5849);
  CHECK(img->libraries()[0].flags == 0x8000);  // debug build bit
  CHECK(img->has_tls());
  CHECK(img->tls().raw_size() == t.raw_size);
  CHECK(img->tls().zero_fill == t.zero_fill);
  std::puts("  synthetic_parse ok");
  return 0;
}

static int test_debug_type_detection() {
  // Debug key has the high bit set on the encoded thunk address.
  const auto file = make_xbe(XbeType::Debug, false, false, nullptr, nullptr);
  auto img = XbeImage::parse(file, nullptr);
  CHECK(img != nullptr);
  CHECK(img->type() == XbeType::Debug);
  // Same real thunk VA decodes consistently for both keys.
  uint32_t ep_r = 0;
  const auto retail = make_xbe(XbeType::Retail, false, false, &ep_r, nullptr);
  auto img_r = XbeImage::parse(retail, nullptr);
  CHECK(img_r != nullptr && img_r->type() == XbeType::Retail);
  CHECK(img_r->entry_va() == ep_r);
  std::puts("  debug_type_detection ok");
  return 0;
}

static int test_malformed() {
  std::string err;
  // bad magic: file[0] is the low byte of 'XBEH' (0x48454258 LE) -> 'Z'
  auto bad = make_xbe(XbeType::Retail, false, false, nullptr, nullptr);
  bad[0] = 'Z';
  CHECK(XbeImage::parse(bad, &err) == nullptr);
  CHECK(err.find("magic") != std::string::npos);
  // truncated header
  std::vector<uint8_t> trunc(0x100, 0);
  CHECK(XbeImage::parse(trunc, &err) == nullptr);
  // section raw data past EOF
  auto oob = make_xbe(XbeType::Retail, false, false, nullptr, nullptr);
  oob.resize(0x1600);  // cut into the .text raw data region
  auto img = XbeImage::parse(oob, &err);
  CHECK(img == nullptr);
  // section headers pointer outside headers
  auto badsec = make_xbe(XbeType::Retail, false, false, nullptr, nullptr);
  badsec[0x120] = 0x00;  // section_headers_addr = base + 0x51000 (past hdrs)
  badsec[0x121] = 0x51;
  CHECK(XbeImage::parse(badsec, &err) == nullptr);
  CHECK(err.find("section headers") != std::string::npos);
  std::puts("  malformed ok");
  return 0;
}

static int test_map_and_protect() {
  uint32_t ep = 0;
  const auto file = make_xbe(XbeType::Retail, true, true, &ep, nullptr);
  auto lx = LoadedXbe::load(file, nullptr);
  CHECK(lx != nullptr);
  XbeRam& ram = lx->ram();

  // Headers mapped and byte-identical to the file header block.
  std::vector<uint8_t> hdrs(0x1000);
  CHECK(ram.read_bytes(0x10000, hdrs.data(), 0x1000));
  CHECK(std::memcmp(hdrs.data(), file.data(), 0x1000) == 0);
  // Certificate readable through its VA.
  uint32_t title_id = 0;
  CHECK(ram.read32(0x10000 + 0x408, &title_id));
  CHECK(title_id == 0x4A4F0001u);

  // .text raw data matches the file bytes; read-only (not writable).
  std::vector<uint8_t> text(0x800);
  CHECK(ram.read_bytes(0x11000, text.data(), 0x800));
  CHECK(std::memcmp(text.data(), file.data() + 0x1000, 0x800) == 0);
  CHECK(ram.readable(0x11000, 0x800));
  CHECK(!ram.writable(0x11000, 0x800));
  // Writes to the RO section are refused (software enforcement).
  CHECK(!ram.write32(0x11000, 0x41414141u));

  // .data writable; BSS tail zero-filled.
  const uint32_t data_va = 0x13000;
  CHECK(ram.writable(data_va, 0x2000));
  uint32_t sentinel = 0;
  CHECK(ram.read32(data_va + 0x100, &sentinel));
  CHECK(sentinel == 0);  // zero fill begins right after raw
  CHECK(ram.read32(data_va + 0x1FFC, &sentinel));
  CHECK(sentinel == 0);
  // Raw prefix intact.
  CHECK(ram.read_bytes(data_va, text.data(), 0x100));
  CHECK(std::memcmp(text.data(), file.data() + 0x2000, 0x100) == 0);
  // Writes to the writable section succeed.
  CHECK(ram.write32(data_va, 0x42424242u));
  uint32_t back = 0;
  CHECK(ram.read32(data_va, &back) && back == 0x42424242u);

  // Uncommitted memory unreadable.
  CHECK(!ram.readable(0x40000000, 4));
  CHECK(ram.host_ptr(0x40000000) == nullptr);

  // Entry point VA sits inside the executable section.
  CHECK(ep >= 0x11000 && ep < 0x12000);

  // Kernel thunk table read back from the mapped image.
  const auto ords = lx->kernel_thunk_ordinals();
  CHECK(ords.size() == 2);
  CHECK(ords[0] == 0x34 && ords[1] == 0xA1);
  std::puts("  map_and_protect ok");
  return 0;
}

static int test_tls_resolution() {
  uint32_t ep = 0, kthunk = 0;
  TlsSpec t;
  const auto file = make_xbe(XbeType::Retail, true, true, &ep, &kthunk, &t);
  auto lx = LoadedXbe::load(file, nullptr);
  CHECK(lx != nullptr);
  CHECK(lx->has_tls());

  // Index variable held garbage in the file; loader writes the index.
  CHECK(lx->write_tls_index(0));
  uint32_t idx = 0;
  CHECK(lx->ram().read32(0x10000 + t.index_off, &idx));
  CHECK(idx == 0);

  // Template copied from MAPPED memory: bytes equal the file bytes for
  // the resident part (here the template sits in the headers page).
  const auto tmpl = lx->tls_template();
  CHECK(tmpl.size() == t.raw_size + t.zero_fill);
  CHECK(std::memcmp(tmpl.data(), file.data() + t.data_off, t.raw_size) == 0);

  // Callback table resolved (one entry + terminator, as built above).
  const auto cbs = lx->tls_callbacks();
  CHECK(cbs.size() == 1);
  CHECK(cbs[0] == 0x10000u + 0x200000u);  // the VA the builder wrote
  std::puts("  tls_resolution ok");
  return 0;
}

static int test_no_tls() {
  const auto file = make_xbe(XbeType::Retail, false, false, nullptr, nullptr);
  auto lx = LoadedXbe::load(file, nullptr);
  CHECK(lx != nullptr);
  CHECK(!lx->has_tls());
  CHECK(lx->tls_template().empty());
  CHECK(lx->tls_callbacks().empty());
  CHECK(!lx->write_tls_index(0));  // nothing to write -> refused
  std::puts("  no_tls ok");
  return 0;
}

static int test_real_lithiumx() {
  // Real nxdk-built XBE (LithiumX v0.9.7). Expected values independently
  // dumped with pyxbe; provenance in tests/lithiumx.xbe.README.txt.
#ifdef XBE_FIXTURE
  FILE* f = std::fopen(XBE_FIXTURE, "rb");
#else
  FILE* f = std::fopen("tests/lithiumx.xbe", "rb");
#endif
  if (!f) {
    std::puts("SKIP lithiumx fixture missing");
    return 77;
  }
  std::vector<uint8_t> file;
  file.resize(3170304);
  const size_t got = std::fread(file.data(), 1, file.size(), f);
  std::fclose(f);
  CHECK(got == file.size());

  auto lx = LoadedXbe::load(file, nullptr);
  CHECK(lx != nullptr);
  const Header& h = lx->image().header();
  CHECK(h.base_addr == 0x10000);
  CHECK(h.sizeof_image == 0x8CE114u);
  CHECK(h.sizeof_headers == 0x5B9u);
  CHECK(h.sizeof_image_header == 0x178u);
  CHECK(h.num_sections == 4);
  CHECK(h.init_flags == 0x5);  // MountUtilityDrive | Limit64MB
  CHECK(lx->image().type() == XbeType::Retail);
  CHECK(lx->image().entry_va() == 0x206570u);
  CHECK(lx->image().certificate().title == u"LithiumX");
  CHECK(lx->image().certificate().title_id == 0xFFFF0002u);
  CHECK(lx->image().certificate().game_region == 0x80000007u);
  CHECK(lx->image().certificate().allowed_media == 0xC0000205u);

  // Sections match the pyxbe dump exactly.
  const auto& secs = lx->image().sections();
  CHECK(secs[0].name == ".text");
  CHECK(secs[0].virtual_addr == 0x11000);
  CHECK(secs[0].virtual_size == 0x1F6000);
  CHECK(secs[0].sizeof_raw == 0x1F5C00);
  CHECK(secs[0].flags == 0x6);  // executable + preload
  CHECK(secs[1].name == ".rdata");
  CHECK(secs[1].virtual_addr == 0x207000);
  CHECK(secs[1].flags == 0x2);
  CHECK(secs[2].name == ".data");
  CHECK(secs[2].virtual_addr == 0x311000);
  CHECK(secs[2].virtual_size == 0x5CD000);
  CHECK(secs[2].sizeof_raw == 0x35FC);
  CHECK(secs[2].flags == 0x3);  // writable + preload
  CHECK(secs[3].name == ".tls");
  CHECK(secs[3].virtual_addr == 0x8DE000u);
  CHECK(secs[3].virtual_size == 0x114);
  CHECK(secs[3].sizeof_raw == 0x4);

  // Library table: one entry, name CXBE0 (per pyxbe).
  CHECK(lx->image().libraries().size() == 1);
  CHECK(std::string(lx->image().libraries()[0].name).substr(0, 5) == "CXBE0");

  // Kernel thunk table: 103 imports, first ordinal 52
  // (InterlockedDecrement), zero-terminated (verified via pyxbe).
  const auto ords = lx->kernel_thunk_ordinals();
  CHECK(ords.size() == 103);
  CHECK(ords[0] == 52);
  CHECK(ords[1] == 54);
  CHECK(ords[2] == 53);
  CHECK(ords[4] == 250);

  // TLS: template lives in .tls, 0x114 bytes, mostly BSS (file holds
  // only 4 raw bytes) -> the mapped copy must be mostly zeros.
  CHECK(lx->has_tls());
  const auto& tls = lx->image().tls();
  CHECK(tls.data_start == 0x8DE000u);
  CHECK(tls.data_end == 0x8DE110u);
  CHECK(tls.index_addr == 0x8DB234u);
  CHECK(tls.callbacks_addr == 0);
  const auto tmpl = lx->tls_template();
  CHECK(tmpl.size() == 0x110);  // data_end - data_start = 0x110
  size_t nonzero = 0;
  for (uint8_t b : tmpl) nonzero += (b != 0);
  CHECK(nonzero == 0);  // real file has a 4-byte zero raw + BSS zeros
  // Index variable lives in .data BSS -> readable after mapping.
  CHECK(lx->ram().writable(tls.index_addr, 4));
  CHECK(lx->write_tls_index(0));
  uint32_t idx = 0xFFFFFFFFu;
  CHECK(lx->ram().read32(tls.index_addr, &idx) && idx == 0);

  // Mapped .text must be byte-identical to the file's .text raw data.
  std::vector<uint8_t> probe(0x1000);
  CHECK(lx->ram().read_bytes(0x11000, probe.data(), 0x1000));
  CHECK(std::memcmp(probe.data(), file.data() + 0x1000, 0x1000) == 0);
  CHECK(!lx->ram().writable(0x11000, 0x100));  // .text is not writable
  CHECK(lx->ram().writable(0x311000, 0x100));  // .data is writable

  std::puts("  real_lithiumx ok (real nxdk binary, pyxbe-verified fields)");
  return 0;
}

int main() {
  std::puts("test_xbe_loader:");
  if (test_synthetic_parse()) return 1;
  if (test_debug_type_detection()) return 1;
  if (test_malformed()) return 1;
  if (test_map_and_protect()) return 1;
  if (test_tls_resolution()) return 1;
  if (test_no_tls()) return 1;
  if (test_real_lithiumx()) return 1;
  std::puts("test_xbe_loader: ALL OK");
  return 0;
}
