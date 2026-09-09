// Tests for the Xbox kernel HLE (xbox_krnl).
//
// Grounding (all cross-checked in scripts/research/):
//  - Export table facts come from nxdk xboxkrnl.exe.def / xboxkrnl.h.
//  - The LithiumX import profile (103 thunks) comes from the real vendored
//    XBE; the expected implemented set below was derived independently
//    from that dump (scripts/research/dump_lithiumx_thunks.py output).
//  - TIME_FIELDS anchors use universally documented dates:
//    1601-01-01 (FILETIME epoch, a Monday) and 1970-01-01 (Unix epoch,
//    a Thursday).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "cpu_native.h"  // cpu_qpc_now (10 MHz host QPC shim)
#include "cpu_sched.h"
#include "xbox_krnl.h"
#include "xbox_sys.h"  // xbox_system_time_filetime anchor
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
#define CHECK_T(cond)                                               \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      g_fail = 1;                                                   \
      return;                                                       \
    }                                                               \
  } while (0)

using namespace xbe;
using krnl::XboxKrnl;
using krnl::kStatusSuccess;
using krnl::kStatusTimeout;
using krnl::kStatusInvalidParameter;

static std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

static uint32_t call1(XboxKrnl& k, uint32_t ord, std::initializer_list<uint32_t> a,
                      bool* ok = nullptr) {
  std::vector<uint32_t> args(a);
  return k.call(ord, args.data(), static_cast<uint32_t>(args.size()), ok);
}

// --- 1. export table facts (from the nxdk import library) ---

static int test_export_table() {
  CHECK(krnl::export_count() == 371);
  const krnl::Export* e52 = krnl::find_export(52);
  CHECK(e52 && std::strcmp(e52->name, "InterlockedDecrement") == 0);
  CHECK(e52->conv == krnl::Conv::Fastcall && e52->arg_bytes == 4);
  const krnl::Export* e8 = krnl::find_export(8);
  CHECK(e8 && std::strcmp(e8->name, "DbgPrint") == 0);
  CHECK(e8->conv == krnl::Conv::Cdecl);  // NOT stdcall (wiki column lies)
  const krnl::Export* e99 = krnl::find_export(99);
  CHECK(e99 && std::strcmp(e99->name, "KeDelayExecutionThread") == 0);
  CHECK(e99->conv == krnl::Conv::Stdcall && e99->arg_bytes == 12);
  const krnl::Export* e156 = krnl::find_export(156);
  CHECK(e156 && std::strcmp(e156->name, "KeTickCount") == 0);
  CHECK(e156->conv == krnl::Conv::Data);
  CHECK(krnl::find_export(9999) == nullptr);
  CHECK(std::strcmp(krnl::name_of(4242), "<unknown>") == 0);
  std::printf("ok export_table\n");
  return 0;
}

// --- 2. real LithiumX thunk resolution + patching ---

// Independently derived expectation (see file header): the exact set of
// LithiumX's 103 kernel imports this HLE serves.
static const std::set<uint32_t> kLithiumXImplemented = {
    8,    // DbgPrint
    52, 53, 54,                    // Interlocked{Decrement,Increment,Exchange}
    99,   // KeDelayExecutionThread
    104,  // KeGetCurrentThread
    108,  // KeInitializeEvent
    126, 127, 128,                 // KeQueryPerformance{Counter,Frequency}, KeQuerySystemTime
    143,  // KeSetBasePriorityThread
    145,  // KeSetEvent
    159,  // KeWaitForSingleObject
    165, 166, 171, 180,            // MmAllocateContiguousMemory[Ex], Free, QueryAllocationSize
    186,  // NtClearEvent
    187,  // NtClose
    189,  // NtCreateEvent
    192,  // NtCreateMutant
    193,  // NtCreateSemaphore
    205,  // NtPulseEvent
    221,  // NtReleaseMutant
    222,  // NtReleaseSemaphore
    225,  // NtSetEvent
    233, 234,                      // NtWaitForSingleObject[Ex]
    235,  // NtWaitForMultipleObjectsEx
    238,  // NtYieldExecution
    255,  // PsCreateSystemThreadEx
    258,  // PsTerminateSystemThread
    277,  // RtlEnterCriticalSection
    291,  // RtlInitializeCriticalSection
    294,  // RtlLeaveCriticalSection
    305,  // RtlTimeToTimeFields
    306,  // RtlTryEnterCriticalSection
    320,  // RtlZeroMemory
    156,  // DATA: KeTickCount
    164,  // DATA: LaunchDataPage
    324,  // DATA: XboxKrnlVersion
    326,  // DATA: XeImageFileName
};

static int test_lithiumx_thunks() {
  const std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  std::string err;
  auto loaded = LoadedXbe::load(file, &err);
  CHECK(loaded != nullptr);

  GuestScheduler sched;
  XboxKrnl krnl(loaded->ram(), sched);
  const std::vector<krnl::ThunkSlot> slots = krnl.install_thunks(*loaded);

  CHECK(slots.size() == 103);  // real import count of the fixture
  uint32_t implemented = 0;
  std::set<uint32_t> got_implemented;
  for (const krnl::ThunkSlot& s : slots) {
    CHECK(s.exp != nullptr);  // every import resolves to a documented export
    if (s.implemented) {
      ++implemented;
      got_implemented.insert(s.ordinal);
      CHECK(s.new_value != s.raw);       // the slot was really patched
      uint32_t back = 0;
      CHECK(loaded->ram().read32(s.slot_va, &back) && back == s.new_value);
      if (s.exp->conv == krnl::Conv::Data) {
        // Data slots point into the kernel data window (last page of RAM).
        CHECK(s.new_value >= loaded->ram().size() - 0x1000);
      }
    } else {
      CHECK(s.new_value == s.raw);       // untouched, reported honestly
    }
  }
  CHECK(implemented == kLithiumXImplemented.size());
  CHECK(got_implemented == kLithiumXImplemented);
  std::printf("ok lithiumx_thunks (%u/%zu implemented)\n", implemented,
              slots.size());
  return 0;
}

// --- 3. Interlocked* (fastcall: guest-memory RMW) ---

static int test_interlocked() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint32_t va = 0x100000;

  bool ok = false;
  CHECK(ram.write32(va, 5));
  CHECK(call1(krnl, 53, {va}, &ok) == 6 && ok);       // Increment -> post
  CHECK(call1(krnl, 52, {va}, &ok) == 5 && ok);       // Decrement -> post
  uint32_t v = 0;
  CHECK(ram.read32(va, &v) && v == 5);
  CHECK(call1(krnl, 54, {va, 0x77}, &ok) == 5 && ok);  // Exchange -> prev
  CHECK(ram.read32(va, &v) && v == 0x77);
  CHECK(call1(krnl, 51, {va, 0x11, 0x77}, &ok) == 0x77 && ok);  // CAS hit
  CHECK(ram.read32(va, &v) && v == 0x11);
  CHECK(call1(krnl, 51, {va, 0x22, 0x99}, &ok) == 0x11 && ok);  // CAS miss
  CHECK(ram.read32(va, &v) && v == 0x11);
  CHECK(call1(krnl, 55, {va, 0x1000}, &ok) == 0x11 && ok);      // ExAdd -> prev
  CHECK(ram.read32(va, &v) && v == 0x1011);
  // LONG wrap: 0 - 1 == -1 (0xFFFFFFFF), the documented LONG semantics.
  CHECK(ram.write32(va, 0));
  CHECK(call1(krnl, 52, {va}, &ok) == 0xFFFFFFFFu && ok);
  // Uncommitted address is rejected with STATUS_INVALID_PARAMETER.
  CHECK(call1(krnl, 53, {0xDEADBEEF}, &ok) == kStatusInvalidParameter && !ok);
  // Arity violation is flagged.
  CHECK(krnl.call(54, &va, 1, &ok) == 0 && !ok);
  std::printf("ok interlocked\n");
  return 0;
}

// --- 4. contiguous memory allocator ---

static int test_mm() {
  GuestScheduler sched;
  const std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  std::string err;
  auto loaded = LoadedXbe::load(file, &err);
  CHECK(loaded != nullptr);
  XboxKrnl krnl(loaded->ram(), sched);
  const auto slots = krnl.install_thunks(*loaded);  // sizes the Mm region
  CHECK(slots.size() == 103);
  bool ok = false;

  CHECK(call1(krnl, 165, {0}, &ok) == 0 && ok);  // zero size -> NULL
  const uint32_t a = call1(krnl, 165, {0x10000}, &ok);
  CHECK(a != 0 && ok);
  CHECK(call1(krnl, 180, {a}, &ok) == 0x10000 && ok);  // QueryAllocationSize
  // Deterministic zeroing (documented deviation: real kernel undefined).
  uint32_t z = 0xFFFFFFFFu;
  CHECK(loaded->ram().read32(a, &z) && z == 0);

  // Ex: alignment + range constraints.
  const uint32_t b = call1(krnl, 166,
                           {0x2000, 0, 0, 0x10000, krnl::kPageReadWrite}, &ok);
  CHECK(b != 0 && ok && (b & 0xFFFF) == 0);
  // Highest below the region start -> NULL (region begins past the image,
  // which for a no-image kernel window is page 0; 0x10 is inside it).
  CHECK(call1(krnl, 166, {0x1000, 0, 0x0F, 0x1000, krnl::kPageReadWrite}, &ok) == 0 && ok);
  // Non-power-of-two alignment -> NULL.
  CHECK(call1(krnl, 166, {0x1000, 0, 0, 0x3000, krnl::kPageReadWrite}, &ok) == 0 && ok);

  // PAGE_READONLY is enforced by the RAM window itself.
  const uint32_t ro = call1(krnl, 166,
                            {0x1000, 0, 0, 0x1000, krnl::kPageReadonly}, &ok);
  CHECK(ro != 0 && ok);
  CHECK(!loaded->ram().writable(ro, 0x1000));

  // Free + reuse; double-free is flagged.
  CHECK(call1(krnl, 171, {a}, &ok) == 0 && ok);
  const uint32_t a2 = call1(krnl, 165, {0x10000}, &ok);
  CHECK(a2 == a);  // first-fit reuses the freed gap deterministically
  CHECK(call1(krnl, 171, {a2}, &ok) == 0 && ok);
  CHECK(call1(krnl, 171, {a2}, &ok) == 0 && !ok);  // now it IS a double free
  std::printf("ok mm\n");
  return 0;
}

// --- 5. Rtl memory operations ---

static int test_rtl_memory() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  bool ok = false;
  const uint32_t buf = 0x100000;

  CHECK(call1(krnl, 284, {buf, 8, 0xAB}, &ok) == 0 && ok);  // Fill
  uint8_t byte = 0;
  CHECK(ram.read_bytes(buf, &byte, 1) && byte == 0xAB);
  CHECK(call1(krnl, 320, {buf, 4}, &ok) == 0 && ok);        // Zero
  CHECK(ram.read_bytes(buf, &byte, 1) && byte == 0);
  CHECK(ram.read_bytes(buf + 4, &byte, 1) && byte == 0xAB);  // rest intact

  // MoveMemory with overlap (forward, like memmove handles).
  const uint8_t src[8] = {'0', '1', '2', '3', '4', '5', '6', 0};
  CHECK(ram.write_bytes(buf + 0x10, src, 8));
  CHECK(call1(krnl, 298, {buf + 0x12, buf + 0x10, 6}, &ok) == 0 && ok);
  uint8_t dst[8] = {};
  CHECK(ram.read_bytes(buf + 0x12, dst, 6));
  CHECK(std::memcmp(dst, src, 6) == 0);

  // Zero-length no-op succeeds even at odd addresses.
  CHECK(call1(krnl, 320, {0x7FFFFFFF, 0}, &ok) == 0 && ok);
  // Unwritable/unreadable ranges are rejected (STATUS_INVALID_PARAMETER).
  CHECK(call1(krnl, 320, {0xDEADBEEF, 4}, &ok) == kStatusInvalidParameter && !ok);
  CHECK(call1(krnl, 298, {buf, 0xDEADBEEF, 4}, &ok) == kStatusInvalidParameter && !ok);
  std::printf("ok rtl_memory\n");
  return 0;
}

// --- 6. time: FILETIME output + TIME_FIELDS conversion ---

static int test_time() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  bool ok = false;
  const uint32_t now_va = 0x100000;

  CHECK(call1(krnl, 128, {now_va}, &ok) == 0 && ok);  // KeQuerySystemTime
  uint64_t ft = 0;
  CHECK(ram.read_bytes(now_va, &ft, 8));
  // Same clock source, sampled on either side of the call: within a
  // minute of each other.
  const uint64_t host = xbox_system_time_filetime();
  CHECK(ft <= host + 600000000ULL && host <= ft + 600000000ULL);

  // RtlTimeToTimeFields anchors. TIME_FIELDS is 8 x SHORT; the lambda
  // only fills the array (CHECK's `return 1` would clash with struct
  // return deduction inside the lambda body).
  const uint32_t tf = 0x100010;
  struct F { int y, m, d, h, mi, s, ms, wd; };
  auto fields = [&](uint64_t when) {
    if (!ram.write_bytes(0x100020, &when, 8)) return F{};
    if (call1(krnl, 305, {0x100020, tf}, &ok) != 0 || !ok) return F{};
    uint32_t w[4] = {};
    for (int i = 0; i < 4; ++i) {
      if (!ram.read32(tf + 4 * i, &w[i])) return F{};
    }
    return F{(int16_t)(w[0] & 0xFFFF), (int16_t)(w[0] >> 16),
             (int16_t)(w[1] & 0xFFFF), (int16_t)(w[1] >> 16),
             (int16_t)(w[2] & 0xFFFF), (int16_t)(w[2] >> 16),
             (int16_t)(w[3] & 0xFFFF), (int16_t)(w[3] >> 16)};
  };
  auto f0 = fields(0);  // FILETIME epoch: 1601-01-01 00:00:00.000 Monday
  CHECK(f0.y == 1601 && f0.m == 1 && f0.d == 1 && f0.h == 0 && f0.mi == 0 &&
        f0.s == 0 && f0.ms == 0 && f0.wd == 1);
  auto f1 = fields(116444736000000000ULL);  // Unix epoch 1970-01-01 Thursday
  CHECK(f1.y == 1970 && f1.m == 1 && f1.d == 1 && f1.h == 0 && f1.wd == 4);
  auto f2 = fields(116444736000000000ULL + 36010000000ULL);  // +1h0m1s
  CHECK(f2.y == 1970 && f2.h == 1 && f2.mi == 0 && f2.s == 1);

  // Performance counter/frequency pair is self-consistent (10 MHz HLE).
  const uint32_t q1 = call1(krnl, 126, {}, &ok);
  CHECK(ok);
  CHECK(call1(krnl, 127, {}, &ok) == 10000000u && ok);
  const uint32_t q2 = call1(krnl, 126, {}, &ok);
  CHECK(q2 >= q1);  // monotonic (mod 2^32)
  std::printf("ok time\n");
  return 0;
}

// --- 7. events across REAL fibers (blocking + wake ordering) ---

static int test_events_cross_fiber() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint32_t ev = 0x100000;
  std::vector<std::string> tl;

  sched.spawn("A", [&](GuestThread&) {
    tl.push_back("A:init");
    call1(krnl, 108, {ev, krnl::kEventSync, 0});            // KeInitializeEvent
    tl.push_back("A:wait");
    const uint32_t st = call1(krnl, 159, {ev, 0, 0, 0, 0}); // infinite wait
    CHECK_T(st == kStatusSuccess);
    tl.push_back("A:after");
  });
  sched.spawn("B", [&](GuestThread&) {
    tl.push_back("B:pre");
    tl.push_back("B:set");
    call1(krnl, 145, {ev, 0, 0});                           // KeSetEvent
    tl.push_back("B:post");
  });
  CHECK(sched.run_until_complete());
  CHECK(!sched.deadlocked());
  // A blocked (real context switch away) until B released it.
  CHECK((tl == std::vector<std::string>{"A:init", "A:wait", "B:pre", "B:set",
                                        "B:post", "A:after"}));
  // Synchronization event: consumed by the wait, back to reset.
  CHECK(!krnl.event_signaled_by_va(ev));
  std::printf("ok events_cross_fiber\n");
  return 0;
}

static int test_sync_vs_notification() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint32_t ev = 0x100000;
  int woke_first = 0, woke_second = 0, woke_third = 0;

  call1(krnl, 108, {ev, krnl::kEventSync, 0});
  uint64_t hb = 0;
  const uint64_t ha = sched.spawn("A", [&](GuestThread&) {
    CHECK_T(call1(krnl, 159, {ev, 0, 0, 0, 0}) == kStatusSuccess);
    ++woke_first;
  });
  hb = sched.spawn("B", [&](GuestThread&) {
    CHECK_T(call1(krnl, 159, {ev, 0, 0, 0, 0}) == kStatusSuccess);
    ++woke_second;
  });
  sched.spawn("C", [&](GuestThread&) {
    // First SetEvent: exactly ONE sync waiter wakes (FIFO: A); the other
    // waiter (B) is still really blocked and the event stays reset.
    call1(krnl, 145, {ev, 0, 0});
    CHECK_T(krnl.event_signaled_by_va(ev) == false);
    CHECK_T(sched.find(hb)->state() == GuestThread::State::BlockedWait);
    call1(krnl, 145, {ev, 0, 0});  // second set: releases B
    CHECK_T(sched.find(ha)->state() != GuestThread::State::BlockedWait);
    // C then waits itself; a nested-spawned D releases it.
    sched.spawn("D", [&](GuestThread&) {
      call1(krnl, 145, {ev, 0, 0});
    });
    CHECK_T(call1(krnl, 159, {ev, 0, 0, 0, 0}) == kStatusSuccess);
    ++woke_third;
  });
  CHECK(sched.run_until_complete());
  CHECK(!sched.deadlocked());
  // Each of the three sync waits was released exactly once.
  CHECK(woke_first == 1 && woke_second == 1 && woke_third == 1);

  // NotificationEvent: one SetEvent wakes ALL and STAYS signaled.
  const uint32_t ev2 = 0x100040;
  call1(krnl, 108, {ev2, krnl::kEventNotify, 0});
  int notify_woke = 0;
  uint64_t n1 = 0, n2 = 0;
  n1 = sched.spawn("N1", [&](GuestThread&) {
    CHECK_T(call1(krnl, 159, {ev2, 0, 0, 0, 0}) == kStatusSuccess);
    ++notify_woke;
  });
  n2 = sched.spawn("N2", [&](GuestThread&) {
    CHECK_T(call1(krnl, 159, {ev2, 0, 0, 0, 0}) == kStatusSuccess);
    ++notify_woke;
  });
  sched.spawn("N3", [&](GuestThread&) {
    call1(krnl, 145, {ev2, 0, 0});  // one set...
    // ...both waiters released from the blocked state (they resume right
    // after N3 blocks or finishes).
    CHECK_T(sched.find(n1)->state() != GuestThread::State::BlockedWait);
    CHECK_T(sched.find(n2)->state() != GuestThread::State::BlockedWait);
    CHECK_T(krnl.event_signaled_by_va(ev2));  // notification stays signaled
    // A late notification wait succeeds without blocking (no consume).
    CHECK_T(call1(krnl, 159, {ev2, 0, 0, 0, 0}) == kStatusSuccess);
    ++notify_woke;
  });
  CHECK(sched.run_until_complete());
  CHECK(notify_woke == 3);
  std::printf("ok sync_vs_notification\n");
  return 0;
}

static int test_wait_timeout() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint32_t ev = 0x100000;
  const uint32_t tv = 0x100040;
  call1(krnl, 108, {ev, krnl::kEventSync, 0});
  // LARGE_INTEGER -50000 = relative 5 ms -> 5 virtual ticks (1 tick = 1ms).
  const uint64_t interval = static_cast<uint64_t>(-50000);
  CHECK(ram.write_bytes(tv, &interval, 8));

  int result = 0;
  uint64_t wake_tick = 0;
  sched.spawn("W", [&](GuestThread& t) {
    result = static_cast<int>(call1(krnl, 159, {ev, 0, 0, 0, tv}));
    wake_tick = sched.current_tick();
    CHECK_T(t.wait_timed_out());  // scheduler reports the timeout wake
  });
  CHECK(sched.run_until_complete());
  CHECK(result == static_cast<int>(kStatusTimeout));
  CHECK(wake_tick == 5);  // exact virtual-tick deadline
  std::printf("ok wait_timeout\n");
  return 0;
}

static int test_nt_handles() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  bool ok = false;
  const uint32_t hout = 0x100000;   // PHANDLE out
  const uint32_t prev = 0x100010;   // PLONG out
  const uint32_t tv = 0x100020;     // timeout storage

  // Create initially-signaled sync event; immediate wait succeeds.
  CHECK(call1(krnl, 189, {hout, 0, krnl::kEventSync, 1}, &ok) == kStatusSuccess && ok);
  uint32_t h = 0;
  CHECK(ram.read32(hout, &h));
  CHECK(h != 0 && (h & 3) == 0);
  CHECK(krnl.event_signaled_by_handle(h));
  CHECK(call1(krnl, 233, {h, 0, 0}, &ok) == kStatusSuccess && ok);  // NULL timeout
  CHECK(!krnl.event_signaled_by_handle(h));  // sync: consumed

  // 1 ms timeout on an unsignaled handle from a fiber times out; from the
  // HOST a would-block wait is rejected (documented boundary).
  const uint64_t one_ms = static_cast<uint64_t>(-10000);
  CHECK(ram.write_bytes(tv, &one_ms, 8));
  CHECK(call1(krnl, 233, {h, 0, tv}, &ok) == kStatusInvalidParameter && !ok);
  int host_res = 0;
  sched.spawn("W", [&](GuestThread&) {
    host_res = static_cast<int>(call1(krnl, 234, {h, 0, 0, tv}));
  });
  CHECK(sched.run_until_complete());
  CHECK(host_res == static_cast<int>(kStatusTimeout));

  // Previous-state out param.
  CHECK(call1(krnl, 225, {h, prev}, &ok) == kStatusSuccess && ok);
  uint32_t p = 9;
  CHECK(ram.read32(prev, &p) && p == 0);  // was reset
  CHECK(call1(krnl, 225, {h, prev}, &ok) == kStatusSuccess && ok);
  CHECK(ram.read32(prev, &p) && p == 1);  // was set
  CHECK(krnl.event_signaled_by_handle(h));

  // Invalid handle is rejected, not crash-prone (arity-checked too).
  CHECK(call1(krnl, 225, {0x13, 0}, &ok) == kStatusInvalidParameter && !ok);
  CHECK(call1(krnl, 186, {0}, &ok) == kStatusInvalidParameter && !ok);
  std::printf("ok nt_handles\n");
  return 0;
}

// --- 8. critical sections: reentrancy + real contention ---

static int test_critical_sections() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint32_t cs = 0x100000;
  std::vector<std::string> tl;

  sched.spawn("A", [&](GuestThread&) {
    tl.push_back("A:init");
    call1(krnl, 291, {cs});                            // RtlInitializeCriticalSection
    call1(krnl, 277, {cs});                            // enter
    call1(krnl, 277, {cs});                            // re-enter (recursive)
    uint32_t rec = 0;
    CHECK_T(ram.read32(cs + 0x14, &rec) && rec == 2);  // RecursionCount
    call1(krnl, 294, {cs});                            // leave (still held once)
    tl.push_back("A:held");
    // Sleep 3 virtual ticks WHILE holding the lock.
    const uint64_t three_ms = static_cast<uint64_t>(-30000);
    CHECK_T(ram.write_bytes(0x100040, &three_ms, 8));
    call1(krnl, 99, {0, 0, 0x100040});                 // KeDelayExecutionThread
    tl.push_back("A:release");
    call1(krnl, 294, {cs});                            // final leave -> wakes B
  });
  sched.spawn("B", [&](GuestThread&) {
    tl.push_back("B:try");
    CHECK_T(call1(krnl, 306, {cs}) == 0);              // TryEnter fails: A holds it
    tl.push_back("B:blocked");
    call1(krnl, 277, {cs});                            // real blocking wait
    tl.push_back("B:acquired");
    uint32_t owner = 0;
    CHECK_T(ram.read32(cs + 0x18, &owner));            // OwningThread
    CHECK_T(owner == static_cast<uint32_t>(sched.current_handle()));
    call1(krnl, 294, {cs});
    tl.push_back("B:done");
  });
  CHECK(sched.run_until_complete());
  CHECK((tl == std::vector<std::string>{
                   "A:init", "A:held", "B:try", "B:blocked", "A:release",
                   "B:acquired", "B:done"}));
  // Unheld afterwards.
  uint32_t owner = 0;
  CHECK(ram.read32(cs + 0x18, &owner) && owner == 0);
  std::printf("ok critical_sections\n");
  return 0;
}

static int test_delay_and_yield() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint64_t one_ms = static_cast<uint64_t>(-10000);
  CHECK(ram.write_bytes(0x100000, &one_ms, 8));
  sched.spawn("D", [&](GuestThread&) {
    CHECK_T(call1(krnl, 99, {0, 0, 0x100000}) == kStatusSuccess);
    CHECK_T(sched.current_tick() == 1);  // exactly one virtual tick
  });
  CHECK(sched.run_until_complete());
  CHECK(sched.current_tick() == 1);

  // NtYieldExecution: cooperative round-robin steps, exact switch count.
  std::vector<char> marks;
  const uint64_t before = sched.switches_total();
  sched.spawn("Y1", [&](GuestThread&) {
    for (int i = 0; i < 3; ++i) {
      marks.push_back('1');
      CHECK_T(call1(krnl, 238, {}) == kStatusSuccess);
    }
  });
  sched.spawn("Y2", [&](GuestThread&) {
    for (int i = 0; i < 3; ++i) {
      marks.push_back('2');
      CHECK_T(call1(krnl, 238, {}) == kStatusSuccess);
    }
  });
  CHECK(sched.run_until_complete());
  CHECK((marks == std::vector<char>{'1', '2', '1', '2', '1', '2'}));
  // 6 yield switches + 2 final re-dispatches after the last yields (the
  // bodies only finish when re-scheduled one last time).
  CHECK(sched.switches_total() - before == 8);
  std::printf("ok delay_and_yield\n");
  return 0;
}

static int test_deadlock_detection() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x1000));
  const uint32_t e1 = 0x100000, e2 = 0x100020;
  call1(krnl, 108, {e1, krnl::kEventSync, 0});
  call1(krnl, 108, {e2, krnl::kEventSync, 0});
  sched.spawn("A", [&](GuestThread&) {
    call1(krnl, 159, {e1, 0, 0, 0, 0});  // never signaled
  });
  sched.spawn("B", [&](GuestThread&) {
    call1(krnl, 159, {e2, 0, 0, 0, 0});  // never signaled
  });
  CHECK(!sched.run_until_complete());
  CHECK(sched.deadlocked());
  std::printf("ok deadlock_detection\n");
  return 0;
}

// --- 9. DbgPrint (cdecl varargs subset) ---

static int test_dbgprint() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x2000));
  std::vector<std::string> lines;
  krnl.set_dbg_sink([&](const std::string& s) { lines.push_back(s); });

  bool ok = false;
  const uint32_t fmt = 0x100000;
  const char* f1 = "%s count=%d x=%08X";
  CHECK(ram.write_bytes(fmt, f1, static_cast<uint32_t>(std::strlen(f1) + 1)));
  const uint32_t str = 0x100040;
  CHECK(ram.write_bytes(str, "hello", 6));
  CHECK(call1(krnl, 8, {fmt, str, 42, 0xBEEF}, &ok) != 0 && ok);
  CHECK(lines.size() == 1 && lines.back() == "hello count=42 x=0000BEEF");

  const char* f2 = "100%% + %c%c";
  CHECK(ram.write_bytes(fmt, f2, static_cast<uint32_t>(std::strlen(f2) + 1)));
  CHECK(call1(krnl, 8, {fmt, 'H', 'i'}, &ok) != 0 && ok);
  CHECK(lines.back() == "100% + Hi");

  const char* f3 = "neg %d pad %5u";
  CHECK(ram.write_bytes(fmt, f3, static_cast<uint32_t>(std::strlen(f3) + 1)));
  CHECK(call1(krnl, 8, {fmt, static_cast<uint32_t>(-7), 42}, &ok) != 0 && ok);
  CHECK(lines.back() == "neg -7 pad    42");

  const char* f4 = "weird %y end";
  CHECK(ram.write_bytes(fmt, f4, static_cast<uint32_t>(std::strlen(f4) + 1)));
  CHECK(call1(krnl, 8, {fmt}, &ok) != 0 && ok);
  CHECK(lines.back() == "weird %y end");  // unknown conversion: verbatim

  call1(krnl, 5, {});  // DbgBreakPoint: recorded through the same sink
  CHECK(lines.size() == 5 && lines.back() == "DbgBreakPoint");
  std::printf("ok dbgprint\n");
  return 0;
}

// --- 10. kernel data window against the real fixture ---

static int test_data_window() {
  GuestScheduler sched;
  const std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  std::string err;
  auto loaded = LoadedXbe::load(file, &err);
  CHECK(loaded != nullptr);
  XboxKrnl krnl(loaded->ram(), sched);
  const auto slots = krnl.install_thunks(*loaded);
  CHECK(!slots.empty());

  uint32_t v = 0;
  // KeTickCount lives at +8 and tracks scheduler virtual ticks.
  CHECK(loaded->ram().read32(krnl.ke_tick_count_va(), &v));
  CHECK(v == sched.current_tick());
  // XboxKrnlVersion: 4 x USHORT, zero-initialized (no dumped kernel, and
  // this project does not fabricate build numbers).
  for (uint32_t off = 0; off < 8; off += 4) {
    CHECK(loaded->ram().read32(krnl.kernel_data_va() + off, &v) && v == 0);
  }
  // XeImageFileName: LithiumX ships no debug pathname fields, so the
  // string stays empty - we do not invent a path.
  const std::string name = loaded->image().debug_pathname();
  CHECK(name.empty());
  CHECK(loaded->ram().read32(krnl.xe_image_file_name_va(), &v) && v == 0);
  CHECK(loaded->ram().read32(krnl.xe_image_file_name_va() + 0x14, &v) &&
        v == 0);  // Buffer pointer NULL
  // LaunchDataPage: NULL (no launch context).
  CHECK(loaded->ram().read32(krnl.launch_data_page_va(), &v) && v == 0);
  std::printf("ok data_window\n");
  return 0;
}

// --- 16. 64-bit kernel returns come back as the EDX:EAX pair (MS ABI) ---

static int test_qpc_64bit() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(krnl.returns_64(126) && krnl.returns_64(127));
  CHECK(!krnl.returns_64(128) && !krnl.returns_64(8) && !krnl.returns_64(165));

  // KeQueryPerformanceFrequency: ULONGLONG 10 MHz -> EDX:EAX = 0:10000000.
  bool ok = false;
  uint32_t edx = 0xFFFFFFFFu;
  const uint32_t eax_f = krnl.call(127, nullptr, 0, &ok, &edx);
  CHECK(ok && eax_f == 10000000u && edx == 0);

  // KeQueryPerformanceCounter: the combined EDX:EAX value must sit
  // between two direct host reads of the same counter.
  const uint64_t lo = cpu_qpc_now();
  uint32_t edx_c = 0;
  const uint32_t eax_c = krnl.call(126, nullptr, 0, &ok, &edx_c);
  const uint64_t hi = cpu_qpc_now();
  CHECK(ok);
  const uint64_t combined = eax_c | (static_cast<uint64_t>(edx_c) << 32);
  CHECK(combined >= lo && combined <= hi);
  CHECK(edx_c == static_cast<uint32_t>(combined >> 32));

  // 32-bit-return exports must NOT touch *edx (sentinel preserved).
  CHECK(ram.commit(0x100000, 0x1000));
  edx = 0xDEADBEEFu;
  std::vector<uint32_t> args{0x100000};
  CHECK(krnl.call(128, args.data(), 1, &ok, &edx) == 0 && ok);
  CHECK(edx == 0xDEADBEEFu);
  std::printf("ok qpc_64bit (EDX:EAX, counter bracketed, edx untouched for 32-bit)\n");
  return 0;
}

// --- 17. MmFreeContiguousMemory UNCOMMITS the pages ---

static int test_mm_free_uncommit() {
  GuestScheduler sched;
  const std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  std::string err;
  auto loaded = LoadedXbe::load(file, &err);
  CHECK(loaded != nullptr);
  XboxKrnl krnl(loaded->ram(), sched);
  const auto slots = krnl.install_thunks(*loaded);  // sizes the Mm region
  CHECK(slots.size() == 103);
  xbe::XbeRam& ram = loaded->ram();
  bool ok = false;

  const uint32_t a = call1(krnl, 165, {0x10000}, &ok);
  CHECK(a != 0 && ok);
  CHECK(ram.is_committed(a, 0x10000));
  uint32_t z = 0;
  CHECK(ram.read32(a, &z));  // committed and readable

  CHECK(call1(krnl, 171, {a}, &ok) == 0 && ok);
  // Freed pages are GONE, like real MmFreeContiguousMemory: not
  // committed, not readable, no host pointer (back to PROT_NONE).
  CHECK(!ram.is_committed(a, 0x1000));
  CHECK(!ram.readable(a, 1));
  CHECK(ram.host_ptr(a) == nullptr);
  CHECK(call1(krnl, 180, {a}, &ok) == 0 && ok);  // size lookup gone too

  // First-fit reuse re-COMMITS the same VA: fresh zeroed, readable.
  const uint32_t a2 = call1(krnl, 165, {0x10000}, &ok);
  CHECK(a2 == a && ok);
  z = 0xFFFFFFFFu;
  CHECK(ram.read32(a2, &z) && z == 0);
  std::printf("ok mm_free_uncommit (pages return to PROT_NONE, VA reusable)\n");
  return 0;
}

// --- 18. A second install_thunks() frees every registered event ---

static int test_reinstall_events_no_leak() {
  GuestScheduler sched;
  const std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  std::string err;
  auto loaded = LoadedXbe::load(file, &err);
  CHECK(loaded != nullptr);
  XboxKrnl krnl(loaded->ram(), sched);
  xbe::XbeRam& ram = loaded->ram();
  // Guest event storage far above the image, below the kernel data page.
  const uint32_t ev = 0x03F00000;
  CHECK(ram.commit(ev, 0x2000));
  const uint32_t handle_out = 0x03F01000;  // NtCreateEvent PHANDLE

  const auto s1 = krnl.install_thunks(*loaded);
  CHECK(s1.size() == 103);
  bool ok = false;
  for (uint32_t i = 0; i < 8; ++i)  // 8 VA-registered events
    CHECK(call1(krnl, 108, {ev + 16 * i, krnl::kEventSync, 1}, &ok) == 0 && ok);
  CHECK(krnl.event_signaled_by_va(ev) &&
        krnl.event_signaled_by_va(ev + 16 * 7));
  uint32_t handle = 0;
  CHECK(ram.read32(handle_out, &handle) && handle == 0);
  CHECK(call1(krnl, 189, {handle_out, 0, krnl::kEventSync, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 189, {handle_out, 0, krnl::kEventNotify, 1}, &ok) == 0 && ok);
  CHECK(ram.read32(handle_out, &handle) && handle == 8);
  CHECK(krnl.event_count() == 2);  // handle table: the 2 NtCreateEvent

  // Second load: everything registered before must be freed (unique_ptr
  // ownership), not leaked - LeakSanitizer proves this under ASan.
  const auto s2 = krnl.install_thunks(*loaded);
  CHECK(s2.size() == 103);
  CHECK(krnl.event_count() == 0);
  // The stale VA objects are gone: KeSetEvent must now reject.
  CHECK(call1(krnl, 145, {ev, 0, 0}, &ok) != 0 && !ok);
  // The registry is usable again after reinstall.
  CHECK(call1(krnl, 108, {ev, krnl::kEventSync, 0}, &ok) == 0 && ok);
  CHECK(!krnl.event_signaled_by_va(ev));  // fresh object, State=0
  std::printf("ok reinstall_events_no_leak (ownership via unique_ptr)\n");
  return 0;
}

// --- 19. DbgPrint: guest-controlled width/precision is clamped ---

// --- 20. mutants (mutexes): ownership, recursion, blocking, abandonment ---

static int test_mutants() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x2000));
  bool ok = false;

  // Non-owned create: free (SignalState 1), not owned by caller.
  CHECK(call1(krnl, 192, {0x100000, 0, 0}, &ok) == 0 && ok);
  uint32_t mh = 0;
  CHECK(ram.read32(0x100000, &mh) && mh != 0);
  CHECK(call1(krnl, 213, {mh, 0x100040}, &ok) == 0 && ok);
  uint32_t cnt = 0xFFFFFFFFu;
  uint8_t owned = 0xFF, aband = 0xFF;
  CHECK(ram.read32(0x100040, &cnt) && cnt == 1);
  CHECK(ram.read_bytes(0x100044, &owned, 1) && owned == 0);
  CHECK(ram.read_bytes(0x100045, &aband, 1) && aband == 0);

  // InitialOwner from the HOST context: rejected (initial ownership
  // requires a thread - we do not fabricate a NULL owner).
  CHECK(call1(krnl, 192, {0x100004, 0, 1}, &ok) != 0 && !ok);

  // Release by a non-owner: STATUS_MUTANT_NOT_OWNED (state untouched).
  CHECK(call1(krnl, 221, {mh, 0}, &ok) != 0 && !ok);
  CHECK(call1(krnl, 213, {mh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 1);

  // Recursion + handoff across fibers, exact timeline.
  std::vector<std::string> trace;
  sched.spawn("A", [&](GuestThread&) {
    const uint32_t st = call1(krnl, 233, {mh, 0, 0});  // acquire (free)
    trace.push_back(st == 0 ? "A:locked" : "A:lock-failed");
    call1(krnl, 238, {});  // yield: B gets scheduled
    // Re-acquire while owned (recursion), then release twice.
    CHECK_T(call1(krnl, 233, {mh, 0, 0}) == 0);
    CHECK_T(call1(krnl, 221, {mh, 0}, &ok) == 0 && ok);
    trace.push_back("A:releasing");
    CHECK_T(call1(krnl, 221, {mh, 0}, &ok) == 0 && ok);  // frees; wakes B
    trace.push_back("A:released");
  });
  sched.spawn("B", [&](GuestThread&) {
    call1(krnl, 238, {});  // yield: A proceeds
    const uint32_t st = call1(krnl, 233, {mh, 0, 0});  // blocks until A
    trace.push_back(st == 0 ? "B:acquired" : "B:acquire-failed");
    CHECK_T(call1(krnl, 221, {mh, 0}, &ok) == 0 && ok);
    trace.push_back("B:released");
  });
  CHECK(sched.run_until_complete());
  CHECK((trace == std::vector<std::string>{"A:locked", "A:releasing",
                                           "A:released", "B:acquired",
                                           "B:released"}));
  CHECK(call1(krnl, 213, {mh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 1);  // free again

  // Abandonment: the owner fiber exits without releasing; the waiter
  // receives STATUS_ABANDONED (0x80) exactly once, then SUCCESS.
  uint32_t mh2 = 0;
  CHECK(call1(krnl, 192, {0x100008, 0, 0}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100008, &mh2) && mh2 != 0);
  uint32_t st_u = 0xFFFFFFFFu, st_u2 = 0xFFFFFFFFu;
  std::vector<std::string> trace2;
  sched.spawn("T", [&](GuestThread&) {
    CHECK_T(call1(krnl, 233, {mh2, 0, 0}) == 0);  // acquire, never release
    trace2.push_back("T:locked");
    // T yields first so U is parked INSIDE the wait when T dies.
    call1(krnl, 238, {});
    trace2.push_back("T:exiting-owned");
  });
  sched.spawn("U", [&](GuestThread&) {
    // Parks INSIDE the wait while T still owns the mutant.
    st_u = call1(krnl, 233, {mh2, 0, 0});  // T exits -> abandoned wake
    trace2.push_back("U:woke");
    st_u2 = call1(krnl, 233, {mh2, 0, 0});  // abandoned flag consumed
  });
  CHECK(sched.run_until_complete());
  CHECK(st_u == krnl::kStatusAbandoned);
  CHECK(st_u2 == kStatusSuccess);
  CHECK(call1(krnl, 213, {mh2, 0x100040}, &ok) == 0 && ok);
  // U exited OWNING the mutant (its second acquire) -> the finish hook
  // re-abandoned it: free (count 1) with AbandonedState set. That is
  // exactly the NT behavior for a thread dying mid-ownership.
  CHECK(ram.read32(0x100040, &cnt) && cnt == 1);
  CHECK(ram.read_bytes(0x100045, &aband, 1) && aband == 1);
  // A fresh WAITER (in a fiber) gets the abandonment delivered once
  // more, then releases cleanly.
  uint32_t st_v = 0xFFFFFFFFu;
  sched.spawn("V", [&](GuestThread&) {
    st_v = call1(krnl, 233, {mh2, 0, 0});
    CHECK_T(st_v == krnl::kStatusAbandoned);
    CHECK_T(call1(krnl, 221, {mh2, 0}, &ok) == 0 && ok);
  });
  CHECK(sched.run_until_complete());
  CHECK(st_v == krnl::kStatusAbandoned);
  CHECK(call1(krnl, 213, {mh2, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 1);  // released: free again
  CHECK(ram.read_bytes(0x100045, &aband, 1) && aband == 0);
  // Host-context mutant waits are invalid (a mutant cannot be owned by
  // "no thread") - unlike events/semaphores, which have no owner.
  CHECK(call1(krnl, 233, {mh2, 0, 0}, &ok) != 0 && !ok);
  std::printf("ok mutants (recursion, handoff, abandonment)\n");
  return 0;
}

// --- 21. semaphores: counts, limits, cross-fiber wakeup ---

static int test_semaphores() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x2000));
  bool ok = false;

  // Invalid creates: zero maximum, initial above maximum.
  CHECK(call1(krnl, 193, {0x100004, 0, 4, 0}, &ok) != 0 && !ok);
  CHECK(call1(krnl, 193, {0x100004, 0, 5, 3}, &ok) != 0 && !ok);

  CHECK(call1(krnl, 193, {0x100000, 0, 1, 3}, &ok) == 0 && ok);
  uint32_t sh = 0, cnt = 0;
  CHECK(ram.read32(0x100000, &sh) && sh != 0);
  // NtQuerySemaphore: {CurrentCount=1, MaximumCount=3}.
  CHECK(call1(krnl, 214, {sh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 1);
  CHECK(ram.read32(0x100044, &cnt) && cnt == 3);

  // The single unit is consumed by a wait; a second wait would block,
  // which from the HOST context is an honest rejection.
  CHECK(call1(krnl, 233, {sh, 0, 0}, &ok) == 0 && ok);  // count 1 -> 0
  CHECK(call1(krnl, 233, {sh, 0, 0}, &ok) != 0 && !ok);  // would block

  // Release 2 (previous count 0), then one more would exceed limit 3.
  CHECK(call1(krnl, 222, {sh, 2, 0x100048}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100048, &cnt) && cnt == 0);  // previous count
  CHECK(call1(krnl, 214, {sh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 2);
  CHECK(call1(krnl, 222, {sh, 2, 0}, &ok) != 0 && !ok);  // LIMIT_EXCEEDED
  CHECK(call1(krnl, 222, {sh, 0, 0}, &ok) != 0 && !ok);  // rel <= 0
  CHECK(call1(krnl, 214, {sh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 2);  // untouched

  // Cross-fiber: two consumers park on an empty semaphore; one release
  // of 2 units wakes both; each consumed exactly one unit. First drain
  // the current count (2) back to 0 with host waits.
  CHECK(call1(krnl, 233, {sh, 0, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 233, {sh, 0, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 214, {sh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 0);
  std::vector<std::string> trace;
  sched.spawn("P", [&](GuestThread&) {
    call1(krnl, 238, {});  // let both consumers park first
    CHECK_T(call1(krnl, 222, {sh, 2, 0}, &ok) == 0 && ok);
    trace.push_back("P:released-2");
  });
  sched.spawn("C1", [&](GuestThread&) {
    CHECK_T(call1(krnl, 233, {sh, 0, 0}) == 0);
    trace.push_back("C1:got");
  });
  sched.spawn("C2", [&](GuestThread&) {
    CHECK_T(call1(krnl, 233, {sh, 0, 0}) == 0);
    trace.push_back("C2:got");
  });
  CHECK(sched.run_until_complete());
  CHECK(trace.size() == 3 && trace[0] == "P:released-2");
  CHECK(call1(krnl, 214, {sh, 0x100040}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100040, &cnt) && cnt == 0);  // both units consumed

  // KeInitializeSemaphore / KeReleaseSemaphore (VA flavor): the Limit
  // is mirrored into guest memory at KSEMAPHORE + 0x10 (nxdk layout).
  CHECK(call1(krnl, 112, {0x100080, 0, 5}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100080 + 4, &cnt) && cnt == 0);  // SignalState=count
  CHECK(ram.read32(0x100080 + 0x10, &cnt) && cnt == 5);  // Limit
  CHECK(call1(krnl, 132, {0x100080, 0, 3, 0}, &ok) == 0 && ok);  // prev 0
  CHECK(ram.read32(0x100080 + 4, &cnt) && cnt == 3);
  CHECK(call1(krnl, 132, {0x100080, 0, 2, 0}, &ok) == 3 && ok);  // prev 3
  CHECK(call1(krnl, 132, {0x100080, 0, 1, 0}, &ok) != 0 && !ok);  // >limit
  std::printf("ok semaphores (counts, limit rejection, producer/consumer)\n");
  return 0;
}

// --- 22. threads: create/suspend/resume/terminate, NtClose ---

static int test_threads_and_close() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x2000));
  bool ok = false;
  uint32_t cnt = 0;

  // KeGetCurrentThread from the HOST: no thread context -> token 0.
  CHECK(call1(krnl, 104, {}, &ok) == 0 && ok);

  // PsCreateSystemThreadEx WITHOUT a CPU runner: the thread body
  // records the honest "no core" state and terminates; the thread
  // object is then signaled, so a wait on its handle succeeds.
  CHECK(call1(krnl, 255,
              {0x100000, 0, 0x8000, 0, 0, 0x00400100, 0x00400200, 0, 0, 0},
              &ok) == 0 && ok);
  uint32_t th = 0;
  CHECK(ram.read32(0x100000, &th) && th != 0);
  CHECK(krnl.thread_running_by_handle(th));
  krnl::XboxKrnl::ThreadStartInfo info;
  CHECK(krnl.thread_start_info(th, &info));
  CHECK(info.start_routine == 0x00400100 && info.start_context == 0x00400200);
  CHECK(sched.run_until_complete());  // body runs, records, terminates
  CHECK(!krnl.thread_running_by_handle(th));
  CHECK(call1(krnl, 233, {th, 0, 0}, &ok) == 0 && ok);  // terminated=signaled

  // Created suspended: parked; NtResumeThread drains the count and the
  // body then runs (still no core -> terminates).
  CHECK(call1(krnl, 255,
              {0x100004, 0, 0x8000, 0, 0, 0x00400100, 0, 1, 0, 0},
              &ok) == 0 && ok);
  uint32_t th2 = 0;
  CHECK(ram.read32(0x100004, &th2) && th2 != 0);
  CHECK(call1(krnl, 224, {th2, 0x100048}, &ok) == 0 && ok);
  CHECK(ram.read32(0x100048, &cnt) && cnt == 1);  // previous suspend count
  CHECK(sched.run_until_complete());
  CHECK(!krnl.thread_running_by_handle(th2));

  // NtClose frees the handle; reuse is visible and the closed handle
  // becomes invalid (STATUS_INVALID_HANDLE).
  CHECK(call1(krnl, 189, {0x10000C, 0, krnl::kEventSync, 0}, &ok) == 0 && ok);
  uint32_t e1 = 0;
  CHECK(ram.read32(0x10000C, &e1));
  CHECK(call1(krnl, 187, {e1}, &ok) == 0 && ok);
  CHECK(call1(krnl, 187, {e1}, &ok) != 0 && !ok);  // double close
  CHECK(call1(krnl, 225, {e1, 0}, &ok) != 0 && !ok);  // set on closed
  CHECK(call1(krnl, 189, {0x10000C, 0, krnl::kEventSync, 0}, &ok) == 0 && ok);
  uint32_t e2 = 0;
  CHECK(ram.read32(0x10000C, &e2) && e2 == e1);  // slot recycled

  // KeGetCurrentThread inside a fiber: distinct odd tokens per fiber.
  uint32_t tok1 = 0, tok2 = 0;
  sched.spawn("K1", [&](GuestThread&) {
    tok1 = call1(krnl, 104, {});
    CHECK_T(tok1 != 0 && (tok1 & 1) == 1);
  });
  sched.spawn("K2", [&](GuestThread&) {
    tok2 = call1(krnl, 104, {});
    CHECK_T(tok2 != 0 && (tok2 & 1) == 1 && tok2 != tok1);
  });
  CHECK(sched.run_until_complete());
  CHECK(tok1 != 0 && tok2 != 0 && tok1 != tok2);

  // KeSetBasePriorityThread: bookkeeping with previous-value return,
  // keyed by the opaque PKTHREAD token; NO scheduling effect (docs).
  CHECK(call1(krnl, 143, {tok1, 5}, &ok) == 0 && ok);  // prev 0
  CHECK(call1(krnl, 143, {tok1, 9}, &ok) == 5 && ok);  // prev 5
  CHECK(call1(krnl, 143, {0xDEAD, 1}, &ok) != 0 && !ok);  // unknown token
  std::printf("ok threads_and_close (create/suspend/resume, close, tokens)\n");
  return 0;
}

// --- 23. NtWaitForMultipleObjectsEx: any/all, index return, timeout ---

static int test_wait_multiple() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x2000));
  bool ok = false;

  // Two synchronization events (reset state) + handles array in RAM.
  CHECK(call1(krnl, 189, {0x100000, 0, krnl::kEventSync, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 189, {0x100004, 0, krnl::kEventSync, 0}, &ok) == 0 && ok);
  uint32_t h1 = 0, h2 = 0;
  CHECK(ram.read32(0x100000, &h1) && ram.read32(0x100004, &h2));
  CHECK(ram.write32(0x100080, h1) && ram.write32(0x100084, h2));
  // LARGE_INTEGER timeout: -30000 relative (0xFFFF8AD0) = 3 ms =
  // exactly 3 virtual ticks.
  CHECK(ram.write32(0x100090, 0xFFFF8AD0u));          // -30000 low dword
  CHECK(ram.write32(0x100094, 0xFFFFFFFFu));          // high dword

  // WaitAny from the HOST with nothing set: zero timeout -> TIMEOUT
  // without blocking. args: Count, Handles, WaitType=1(WaitAny), Mode,
  // Alertable, Timeout(0 = NULL => infinite! zero-timeout is *q==0).
  // Host + infinite would-block: honest rejection.
  CHECK(call1(krnl, 235, {2, 0x100080, 1, 0, 0, 0}, &ok) != 0 && !ok);
  // Explicit zero timeout (LARGE_INTEGER 0): satisfy-or-timeout.
  CHECK(ram.write32(0x100090, 0) && ram.write32(0x100094, 0));
  CHECK(call1(krnl, 235, {2, 0x100080, 1, 0, 0, 0x100090}, &ok) ==
            krnl::kStatusTimeout && ok);
  // Set the SECOND event: WaitAny returns index 1 and consumed it.
  CHECK(call1(krnl, 225, {h2, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 235, {2, 0x100080, 1, 0, 0, 0x100090}, &ok) == 1 && ok);
  CHECK(krnl.event_signaled_by_handle(h2) == false);  // sync: consumed

  // WaitAll: set both; returns WAIT_OBJECT_0 (0); both consumed.
  CHECK(call1(krnl, 225, {h1, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 225, {h2, 0}, &ok) == 0 && ok);
  CHECK(call1(krnl, 235, {2, 0x100080, 0, 0, 0, 0x100090}, &ok) == 0 && ok);
  CHECK(!krnl.event_signaled_by_handle(h1) &&
        !krnl.event_signaled_by_handle(h2));

  // Timed WaitAny from a fiber: nothing set -> TIMEOUT after exactly
  // 3 virtual ticks. (Rewrite the timeout: the zero-timeout test above
  // clobbered it.)
  CHECK(ram.write32(0x100090, 0xFFFF8AD0u));
  CHECK(ram.write32(0x100094, 0xFFFFFFFFu));
  uint32_t st = 0xFFFFFFFFu;
  uint64_t tick_at_wake = 0;
  CHECK(call1(krnl, 189, {0x100008, 0, krnl::kEventSync, 0}, &ok) == 0 && ok);
  uint32_t h3 = 0;
  CHECK(ram.read32(0x100008, &h3));
  sched.spawn("MW", [&](GuestThread&) {
    st = call1(krnl, 235, {2, 0x100080, 1, 0, 0, 0x100090});
    tick_at_wake = sched.current_tick();
  });
  CHECK(sched.run_until_complete());
  CHECK(st == krnl::kStatusTimeout && tick_at_wake == 3);

  // Bad arguments: count 0, count > MAXIMUM_WAIT_OBJECTS, bad wait type,
  // bad handle.
  CHECK(call1(krnl, 235, {0, 0x100080, 1, 0, 0, 0}, &ok) != 0 && !ok);
  CHECK(call1(krnl, 235, {krnl::kMaxWaitObjects + 1, 0x100080, 1, 0, 0, 0},
              &ok) != 0 && !ok);
  CHECK(call1(krnl, 235, {2, 0x100080, 7, 0, 0, 0}, &ok) != 0 && !ok);
  CHECK(call1(krnl, 235, {2, 0x1000F0, 1, 0, 0, 0}, &ok) != 0 && !ok);
  std::printf("ok wait_multiple (any/all, index, exact timeout)\n");
  return 0;
}

// --- 19. DbgPrint: guest-controlled width/precision is clamped (cont) ---

static int test_dbgprint_width_clamp() {
  GuestScheduler sched;
  xbe::XbeRam ram;
  XboxKrnl krnl(ram, sched);
  CHECK(ram.commit(0x100000, 0x2000));
  std::vector<std::string> lines;
  krnl.set_dbg_sink([&](const std::string& s) { lines.push_back(s); });
  bool ok = false;
  const uint32_t fmt = 0x100000;

  // Hostile width from GUEST memory: the spec is clamped to 64 before
  // snprintf, so formatting work and output stay bounded regardless of
  // what the guest format string claims.
  const char* f1 = "[%99999999d]";
  CHECK(ram.write_bytes(fmt, f1, static_cast<uint32_t>(std::strlen(f1) + 1)));
  CHECK(call1(krnl, 8, {fmt, 5}, &ok) != 0 && ok);
  CHECK(lines.back().size() <= 64);  // bounded (would-be field: ~100 MB)
  CHECK(lines.back().front() == '[' && lines.back().back() == ']');

  // Legit flags/widths pass through byte-preserved.
  const char* f2 = "%08X|%5u|";
  CHECK(ram.write_bytes(fmt, f2, static_cast<uint32_t>(std::strlen(f2) + 1)));
  CHECK(call1(krnl, 8, {fmt, 0xBEEF, 42}, &ok) != 0 && ok);
  CHECK(lines.back() == "0000BEEF|   42|");
  std::printf("ok dbgprint_width_clamp (hostile width bounded, flags kept)\n");
  return 0;
}

int main() {
  int fails = 0;
  fails += test_export_table();
  fails += test_lithiumx_thunks();
  fails += test_interlocked();
  fails += test_mm();
  fails += test_rtl_memory();
  fails += test_time();
  fails += test_events_cross_fiber();
  fails += test_sync_vs_notification();
  fails += test_wait_timeout();
  fails += test_nt_handles();
  fails += test_critical_sections();
  fails += test_delay_and_yield();
  fails += test_deadlock_detection();
  fails += test_dbgprint();
  fails += test_data_window();
  fails += test_qpc_64bit();
  fails += test_mm_free_uncommit();
  fails += test_reinstall_events_no_leak();
  fails += test_dbgprint_width_clamp();
  fails += test_mutants();
  fails += test_semaphores();
  fails += test_threads_and_close();
  fails += test_wait_multiple();
  if (fails != 0) std::printf("%d test function(s) FAILED\n", fails);
  else std::printf("all xbox_krnl tests passed\n");
  return fails != 0 ? 1 : 0;
}
