#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "cpu_sched.h"
#include "fiber_tls.h"
#include "xbe_loader.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

// CHECK variant for guest thread bodies.
static int g_fail = 0;
#define CHECK_T(cond)                                                 \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      g_fail = 1;                                                     \
      return;                                                         \
    }                                                                 \
  } while (0)

using namespace xbe;  // LoadedXbe used to build a realistic init payload

// --- helpers ---

static FiberTls::Init make_init(size_t raw_bytes, size_t zero_fill,
                                uint32_t index,
                                std::vector<uint32_t> cbs = {}) {
  FiberTls::Init init;
  init.raw.resize(raw_bytes);
  for (size_t i = 0; i < raw_bytes; ++i)
    init.raw[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  init.zero_fill = zero_fill;
  init.index = index;
  init.callbacks = std::move(cbs);
  return init;
}

// Two fibers ping-pong; each writes/reads ITS OWN TLS data slot across
// real context switches. On one host thread, C++ thread_local cannot do
// this; per-fiber storage can.
static int test_two_fibers_isolation() {
  GuestScheduler sched;
  FiberTls tls(sched);
  tls.set_init(make_init(64, 32, 0));

  std::vector<uint64_t> seq;
  const uint64_t fa = tls.spawn_with_tls("A", [&](GuestThread& t) {
    auto* da = static_cast<uint32_t*>(tls.current_data(0, 8));
    CHECK_T(da != nullptr);
    *da = 0xAAAA0001u;
    for (int i = 0; i < 3; ++i) {
      seq.push_back(1);
      t.yield();  // one real switch per yield
      // After bouncing through other fibers, our block is intact and
      // still distinguishable.
      CHECK_T(*da == 0xAAAA0001u);
      CHECK_T(tls.current_data() == da);
    }
  });
  const uint64_t fb = tls.spawn_with_tls("B", [&](GuestThread& t) {
    auto* db = static_cast<uint32_t*>(tls.current_data(0, 8));
    CHECK_T(db != nullptr);
    *db = 0xBBBB0002u;
    for (int i = 0; i < 3; ++i) {
      seq.push_back(2);
      t.yield();
      CHECK_T(*db == 0xBBBB0002u);
      CHECK_T(tls.current_data() == db);
      CHECK_T(tls.current_data() != nullptr);
    }
  });
  CHECK(fa != 0 && fb != 0);
  CHECK(fa != fb);
  CHECK(sched.run_until_complete());
  CHECK(seq.size() == 6);
  for (size_t i = 0; i < seq.size(); ++i)
    CHECK(seq[i] == (i % 2) + 1);  // strict alternation = real switches
  CHECK(sched.switches_total() == 8);  // 4 dispatches per thread

  // Distinct blocks, both alive after the run, contents preserved.
  const void* da = tls.data_of(fa);
  const void* db = tls.data_of(fb);
  CHECK(da && db && da != db);
  CHECK(*static_cast<const uint32_t*>(da) == 0xAAAA0001u);
  CHECK(*static_cast<const uint32_t*>(db) == 0xBBBB0002u);
  std::puts("  two_fibers_isolation ok");
  return 0;
}

static int test_block_layout_and_template() {
  GuestScheduler sched;
  FiberTls tls(sched);
  const uint32_t idx = 0;
  tls.set_init(make_init(48, 80, idx));

  uint64_t fh = 0;
  bool layout_ok = false;
  fh = tls.spawn_with_tls("layout", [&](GuestThread&) {
    const auto* arr = static_cast<void* const*>(tls.current_array());
    CHECK_T(arr != nullptr);
    // slot[index] points at the data area; other slots are null.
    for (size_t i = 0; i < FiberTls::kSlots; ++i) {
      if (i == idx) continue;
      CHECK_T(arr[i] == nullptr);
    }
    const void* data = tls.current_data();
    CHECK_T(arr[idx] == data);
    // Template bytes copied byte-for-byte.
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < 48; ++i)
      CHECK_T(bytes[i] == static_cast<uint8_t>((i * 7 + 3) & 0xFF));
    // Zero fill region all zero.
    for (size_t i = 48; i < 48 + 80; ++i) CHECK_T(bytes[i] == 0);
    layout_ok = true;
  });
  CHECK(fh != 0);
  CHECK(sched.run_until_complete());
  CHECK(layout_ok);

  // The block lives ON the fiber's stack: data pointer inside the
  // reserved stack-top region of that fiber.
  void* resv = sched.stack_top_reserve(fh);
  CHECK(resv != nullptr);
  CHECK(tls.array_of(fh) == resv);  // allocation starts at the reserve
  const uint8_t* arr = static_cast<const uint8_t*>(resv);
  const uint8_t* dat = static_cast<const uint8_t*>(tls.data_of(fh));
  CHECK(dat == arr + FiberTls::kSlots * sizeof(void*));
  std::puts("  block_layout_and_template ok");
  return 0;
}

// Deep recursion across a switch must not touch the TLS reserve at the
// stack top (the fiber's stack pointer starts below it).
static uint8_t g_ref[32];
static void recurse(GuestThread& t, int depth) {
  uint8_t buf[256];
  for (size_t i = 0; i < sizeof(buf); ++i)
    buf[i] = static_cast<uint8_t>((depth + i) & 0xFF);
  if (depth > 0) recurse(t, depth - 1);
  else t.yield();
  if (buf[0] != static_cast<uint8_t>((depth + 0) & 0xFF)) g_fail = 1;
}

static int test_deep_stack_does_not_clobber_tls() {
  GuestScheduler sched;
  FiberTls tls(sched);
  tls.set_init(make_init(32, 0, 0));

  uint64_t fh = 0;
  static bool intact = true;
  fh = tls.spawn_with_tls("deep", [&](GuestThread& t) {
    const auto* data = static_cast<const uint8_t*>(tls.current_data());
    std::memcpy(g_ref, data, 32);
    recurse(t, 200);  // ~200 frames x 256B live across a yield
    if (std::memcmp(g_ref, data, 32) != 0) intact = false;
  });
  CHECK(fh != 0);
  CHECK(sched.run_until_complete());
  CHECK(intact);
  CHECK(g_fail == 0);
  std::puts("  deep_stack_does_not_clobber_tls ok");
  return 0;
}

static int test_attach_detach_events() {
  GuestScheduler sched;
  FiberTls tls(sched);
  const std::vector<uint32_t> cbs = {0x210000u, 0x210008u};
  tls.set_init(make_init(16, 0, 0, cbs));

  const uint64_t main_f = tls.spawn_with_tls("main", [&](GuestThread& t) {
    t.yield();
  });
  const uint64_t second = tls.spawn_with_tls("worker", [&](GuestThread&) {});
  CHECK(main_f && second);
  CHECK(sched.run_until_complete());

  const auto& ev = tls.events();
  // main: 2 attach records (one per callback, PROCESS_ATTACH), then
  // worker: 2 attach records (THREAD_ATTACH); detaches: main, worker
  // (order = finish order).
  CHECK(ev.size() == 6);
  CHECK(ev[0].fiber == main_f && ev[0].reason == FiberTls::kReasonProcessAttach);
  CHECK(ev[0].callback == 0x210000u);
  CHECK(ev[1].fiber == main_f && ev[1].reason == FiberTls::kReasonProcessAttach);
  CHECK(ev[1].callback == 0x210008u);
  CHECK(ev[2].fiber == second && ev[2].reason == FiberTls::kReasonThreadAttach);
  CHECK(ev[2].callback == 0x210000u);
  CHECK(ev[3].fiber == second && ev[3].reason == FiberTls::kReasonThreadAttach);
  CHECK(ev[4].reason == FiberTls::kReasonThreadDetach && ev[4].callback == 0);
  CHECK(ev[5].reason == FiberTls::kReasonThreadDetach && ev[5].callback == 0);
  // Detach records fire while the fiber is still current (hook runs
  // inside thread_finish), so they carry the finishing fiber's handle.
  CHECK((ev[4].fiber == main_f && ev[5].fiber == second) ||
        (ev[4].fiber == second && ev[5].fiber == main_f));
  std::puts("  attach_detach_events ok");
  return 0;
}

static int test_no_tls_and_edge_cases() {
  GuestScheduler sched;
  FiberTls tls(sched);
  // No set_init: spawns refuse and accessors stay null.
  CHECK(tls.spawn_with_tls("x", [](GuestThread&) {}) == 0);
  CHECK(tls.current_data() == nullptr);
  CHECK(tls.current_array() == nullptr);

  // Invalid index (> kSlots) rejected at set_init.
  GuestScheduler sched2;
  FiberTls tls2(sched2);
  tls2.set_init(make_init(8, 0, FiberTls::kSlots));  // out of range
  CHECK(tls2.spawn_with_tls("y", [](GuestThread&) {}) == 0);

  // Plain spawn (no TLS) still works alongside TLS spawns, has no block.
  FiberTls::Init init = make_init(8, 0, 0);
  tls2.set_init(std::move(init));
  uint64_t plain = 0;
  plain = sched2.spawn("plain", [](GuestThread&) {});
  CHECK(plain != 0);
  CHECK(!tls2.has_tls(plain));
  CHECK(sched2.stack_top_reserve(plain) == nullptr);
  CHECK(sched2.run_until_complete());
  std::puts("  no_tls_and_edge_cases ok");
  return 0;
}

// The full pipeline: load a synthetic XBE, write the index into the
// image, feed the runtime, and verify the guest access sequence
// end-to-end: index read from image memory -> array[index] -> data.
static int test_end_to_end_with_loaded_xbe() {
  // Build a minimal valid retail XBE with TLS (same builder ideas as
  // test_xbe_loader; kept local to avoid cross-test coupling).
  std::vector<uint8_t> file(0x2000, 0);
  auto put = [&file](size_t off, uint32_t v) {
    std::memcpy(file.data() + off, &v, 4);
  };
  put(0x000, 0x48454258u);
  put(0x104, 0x10000u);              // base
  put(0x108, 0x1000u);               // headers
  put(0x10C, 0x4000u);               // image size
  put(0x110, 0x178u);
  put(0x114, 0x11111111u);
  put(0x118, 0x10400u);              // cert @ base+0x400
  put(0x11C, 1u);                    // 1 section
  put(0x120, 0x10300u);              // section headers @ base+0x300
  put(0x124, 1u);
  const uint32_t ep_va = 0x11400u;   // inside .text [0x11000, 0x12000)
  put(0x128, ep_va ^ kXorEpRetail);
  put(0x12C, 0x10700u);              // TLS dir @ base+0x700
  put(0x130, 0x10000u);
  put(0x134, 0x100000u);
  put(0x138, 0x10000u);
  put(0x13C, 0x10000u);
  put(0x140, 0x4000u);
  put(0x158, 0x10800u ^ kXorKtRetail);  // thunk @ base+0x800
  put(0x160, 0u);                    // no libs
  // cert
  put(0x400, 0x1000u);
  put(0x408, 0x4A4F0002u);
  // section: .text-like, ABS VA 0x11000, vsize 0x1000, raw @ file 0x1000
  const uint32_t sh = 0x300;
  put(sh + 0x00, kSectionExecutable | kSectionWriteable);
  put(sh + 0x04, 0x11000u);
  put(sh + 0x08, 0x1000u);
  put(sh + 0x0C, 0x1000u);
  put(sh + 0x10, 0x1000u);
  put(sh + 0x14, 0x10500u);  // name addr -> ".secs"
  std::memcpy(file.data() + 0x500, ".secs", 6);
  // thunk entries
  put(0x800, 0x80000001u);
  put(0x804, 0);
  // TLS dir: template @ base+0x600 (16B), index @ base+0x640, no cbs
  for (int i = 0; i < 16; ++i) file[0x600 + i] = static_cast<uint8_t>(0x40 + i);
  put(0x640, 0xEEEEEEEEu);  // garbage index pre-load
  put(0x700, 0x10600u);
  put(0x704, 0x10610u);
  put(0x708, 0x10640u);
  put(0x70C, 0);
  put(0x710, 0);
  put(0x714, 0);

  auto lx = LoadedXbe::load(file, nullptr);
  CHECK(lx != nullptr);
  CHECK(lx->has_tls());
  CHECK(lx->image().tls().index_addr == 0x10640u);
  CHECK(lx->kernel_thunk_ordinals().size() == 1);

  FiberTls::Init init;
  init.raw = lx->tls_template();
  init.zero_fill = lx->image().tls().zero_fill;
  init.index = 0;
  init.callbacks = lx->tls_callbacks();
  CHECK(init.raw.size() == 16);
  CHECK(lx->write_tls_index(init.index));

  GuestScheduler sched;
  FiberTls tls(sched);
  tls.set_init(std::move(init));

  int sequence_ok = false;
  uint64_t fmain = 0;
  fmain = tls.spawn_with_tls("game.main", [&](GuestThread& t) {
    // Mirror the real guest access sequence from the LithiumX
    // disassembly, using our runtime instead of fs:[4]:
    //   idx  = [IndexVA]         (read from the MAPPED image)
    //   arr  = thread's array    (current fiber)
    //   blk  = arr[idx]
    //   var  = blk[offset]
    uint32_t idx = 0;
    CHECK_T(lx->ram().read32(lx->image().tls().index_addr, &idx));
    CHECK_T(idx == 0);  // was written by the loader-side call
    const auto* arr = static_cast<void* const*>(tls.current_array());
    CHECK_T(arr != nullptr);
    const auto* blk = static_cast<const uint8_t*>(arr[idx]);
    CHECK_T(blk != nullptr);
    // Template byte at offset 7 must be 0x40+7 (per-fiber copy).
    CHECK_T(blk[7] == 0x47);
    // Per-fiber mutation survives a real context switch and does not
    // touch the image template other fibers would copy.
    auto* var = static_cast<uint32_t*>(tls.current_data(8, 4));
    CHECK_T(var != nullptr);
    *var = 0x12345678u;
    t.yield();
    CHECK_T(*static_cast<uint32_t*>(tls.current_data(8, 4)) == 0x12345678u);
    sequence_ok = true;
  });
  CHECK(fmain != 0);
  CHECK(sched.run_until_complete());
  CHECK(sequence_ok);

  // Image template unchanged by fiber-local writes.
  const auto tmpl = lx->tls_template();
  CHECK(tmpl[7] == 0x47);
  std::puts("  end_to_end_with_loaded_xbe ok");
  return 0;
}

// 25 fibers doing traps + TLS writes concurrently (cooperatively).
static int test_many_fibers() {
  GuestScheduler sched;
  FiberTls tls(sched);
  tls.set_init(make_init(24, 8, 0));
  std::vector<int> hits(25, 0);
  std::vector<uint64_t> handles(25);
  for (int i = 0; i < 25; ++i) {
    handles[i] = tls.spawn_with_tls("w" + std::to_string(i),
                                    [i, &tls, &hits](GuestThread& t) {
      auto* p = static_cast<uint32_t*>(tls.current_data(0, 4));
      CHECK_T(p != nullptr);
      *p = static_cast<uint32_t>(0xC0000000u + i);
      for (int r = 0; r < 4; ++r) {
        CHECK_T(*p == static_cast<uint32_t>(0xC0000000u + i));
        ++hits[static_cast<size_t>(i)];
        t.yield();
      }
    });
    CHECK(handles[i] != 0);
  }
  CHECK(sched.run_until_complete());
  for (int i = 0; i < 25; ++i) CHECK(hits[static_cast<size_t>(i)] == 4);
  CHECK(tls.events().size() == 50);  // 25 attach + 25 detach
  CHECK(sched.finished_count() == 25);
  std::puts("  many_fibers ok");
  return 0;
}

int main() {
  std::puts("test_fiber_tls:");
  if (test_two_fibers_isolation()) return 1;
  if (test_block_layout_and_template()) return 1;
  if (test_deep_stack_does_not_clobber_tls()) return 1;
  if (test_attach_detach_events()) return 1;
  if (test_no_tls_and_edge_cases()) return 1;
  if (test_end_to_end_with_loaded_xbe()) return 1;
  if (test_many_fibers()) return 1;
  std::puts("test_fiber_tls: ALL OK");
  return 0;
}
