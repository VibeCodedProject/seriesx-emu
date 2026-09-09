#include "xbox_krnl.h"

#include <string.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "cpu_native.h"  // cpu_qpc_now (10 MHz HLE performance counter)
#include "xbox_sys.h"    // xbox_system_time_filetime (FILETIME helper)

// The full kernel export table, generated from nxdk's xboxkrnl.exe.def
// (see src/xbox_krnl_exports.inc header for provenance).
namespace krnl {

namespace {
#define CONV_STDCALL Conv::Stdcall
#define CONV_FASTCALL Conv::Fastcall
#define CONV_CDECL Conv::Cdecl
#define CONV_DATA Conv::Data
#define KRNL_EXPORT(o, n, c, b, d) {o, #n, CONV_##c, b},
const Export kExports[] = {
#include "xbox_krnl_exports.inc"
};
#undef KRNL_EXPORT
#undef CONV_STDCALL
#undef CONV_FASTCALL
#undef CONV_CDECL
#undef CONV_DATA
}  // namespace

size_t export_count() { return sizeof(kExports) / sizeof(kExports[0]); }
const Export& export_at(size_t i) { return kExports[i]; }

const Export* find_export(uint32_t ordinal) {
  for (const Export& e : kExports)
    if (e.ordinal == ordinal) return &e;
  return nullptr;
}

const char* name_of(uint32_t ordinal) {
  const Export* e = find_export(ordinal);
  return e ? e->name : "<unknown>";
}

namespace {

// Wait-channel token namespaces (see xbox_krnl.h): events tag bit 1,
// critical sections tag bit 0, so the two can never collide.
inline uint64_t event_token(const void* e) {
  return (reinterpret_cast<uintptr_t>(e) << 1) | 1u;
}
inline uint64_t cs_token_of(uint32_t cs_va) {
  return static_cast<uint64_t>(cs_va) << 1;
}
// NtWaitForMultipleObjectsEx per-fiber wait channel (never collides with
// the object/CS namespaces above).
constexpr uint64_t kMwTokenBase = 0x51AC000000000000ULL;
// Forward decl: defined in the dispatch section's anonymous namespace.
void interval_to_ticks(int64_t q, bool* immediate, uint64_t* ticks);
inline uint32_t ceil_ms(uint64_t units_100ns) {
  return static_cast<uint32_t>((units_100ns + 9999) / 10000);
}

// RTL_CRITICAL_SECTION field offsets (nxdk xboxdef.h):
// +0x00 embedded dispatcher event (16 bytes), +0x10 LockCount,
// +0x14 RecursionCount, +0x18 OwningThread; total size 0x1C.
constexpr uint32_t kCsLockCount = 0x10;
constexpr uint32_t kCsRecursion = 0x14;
constexpr uint32_t kCsOwningThread = 0x18;
constexpr uint32_t kCsSize = 0x1C;

// Howard Hinnant's civil_from_days (public domain algorithms collection),
// used for the FILETIME -> TIME_FIELDS conversion below.
void civil_from_days(int64_t z, int64_t* y, uint32_t* m, uint32_t* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = yy + (*m <= 2);
}

}  // namespace

// --- implemented export sets (HLE coverage; everything else is reported
// --- as known-but-unimplemented by install_thunks) ---

bool implemented_function(uint32_t ordinal) {
  switch (ordinal) {
    case 5:    // DbgBreakPoint (recorded, no debugger to enter)
    case 8:    // DbgPrint
    case 51:   // InterlockedCompareExchange
    case 52:   // InterlockedDecrement
    case 53:   // InterlockedIncrement
    case 54:   // InterlockedExchange
    case 55:   // InterlockedExchangeAdd
    case 99:   // KeDelayExecutionThread
    case 108:  // KeInitializeEvent
    case 104:  // KeGetCurrentThread
    case 110:  // KeInitializeMutant
    case 112:  // KeInitializeSemaphore
    case 126:  // KeQueryPerformanceCounter
    case 127:  // KeQueryPerformanceFrequency
    case 128:  // KeQuerySystemTime
    case 131:  // KeReleaseMutant
    case 132:  // KeReleaseSemaphore
    case 138:  // KeResetEvent
    case 140:  // KeResumeThread
    case 143:  // KeSetBasePriorityThread
    case 145:  // KeSetEvent
    case 152:  // KeSuspendThread
    case 159:  // KeWaitForSingleObject
    case 165:  // MmAllocateContiguousMemory
    case 166:  // MmAllocateContiguousMemoryEx
    case 171:  // MmFreeContiguousMemory
    case 180:  // MmQueryAllocationSize
    case 186:  // NtClearEvent
    case 187:  // NtClose
    case 189:  // NtCreateEvent
    case 192:  // NtCreateMutant
    case 193:  // NtCreateSemaphore
    case 205:  // NtPulseEvent
    case 213:  // NtQueryMutant
    case 214:  // NtQuerySemaphore
    case 221:  // NtReleaseMutant
    case 222:  // NtReleaseSemaphore
    case 224:  // NtResumeThread
    case 225:  // NtSetEvent
    case 231:  // NtSuspendThread
    case 233:  // NtWaitForSingleObject
    case 234:  // NtWaitForSingleObjectEx
    case 235:  // NtWaitForMultipleObjectsEx
    case 238:  // NtYieldExecution
    case 255:  // PsCreateSystemThreadEx
    case 258:  // PsTerminateSystemThread
    case 277:  // RtlEnterCriticalSection
    case 284:  // RtlFillMemory
    case 291:  // RtlInitializeCriticalSection
    case 294:  // RtlLeaveCriticalSection
    case 298:  // RtlMoveMemory
    case 305:  // RtlTimeToTimeFields
    case 306:  // RtlTryEnterCriticalSection
    case 320:  // RtlZeroMemory
      return true;
    default:
      return false;
  }
}

// DATA exports served from the kernel data window.
bool implemented_data(uint32_t ordinal) {
  switch (ordinal) {
    case 156:  // KeTickCount (volatile DWORD, refreshed from virtual ticks)
    case 164:  // LaunchDataPage (pointer, NULL: no launch data)
    case 324:  // XboxKrnlVersion (zero-initialized: no dumped kernel)
    case 326:  // XeImageFileName (ANSI_STRING from the XBE debug path)
      return true;
    default:
      return false;
  }
}

// --- construction / debug output ---

XboxKrnl::XboxKrnl(xbe::XbeRam& ram, GuestScheduler& sched)
    : ram_(ram), sched_(sched) {
  // Every fiber exit must mirror thread termination: mutants owned by
  // the exiting fiber become abandoned, and the fiber's thread identity
  // (if it ever materialized) becomes "signaled". Runs while the dying
  // fiber is still switched in (see GuestScheduler::thread_finish).
  sched_.add_finish_hook([this](GuestThread& t) {
    if (DispatcherObject* th =
            find_thread_by_fiber(static_cast<uint32_t>(t.handle())))
      finish_thread_object(th);
  });
}

// events_ / va_events_ own their Event objects via unique_ptr; nothing
// to free manually (a repeated install_thunks() also frees safely).
XboxKrnl::~XboxKrnl() = default;

void XboxKrnl::set_dbg_sink(std::function<void(const std::string&)> sink) {
  dbg_sink_ = std::move(sink);
}

// --- guest memory helpers ---

bool XboxKrnl::write64(uint32_t va, uint64_t value) {
  if (!ram_.write32(va, static_cast<uint32_t>(value & 0xFFFFFFFFu)))
    return false;  // LowPart first (little endian)
  return ram_.write32(va + 4, static_cast<uint32_t>(value >> 32));
}

bool XboxKrnl::read64(uint32_t va, uint64_t* out) const {
  uint32_t lo = 0, hi = 0;
  if (!ram_.read32(va, &lo) || !ram_.read32(va + 4, &hi)) return false;
  *out = lo | (static_cast<uint64_t>(hi) << 32);
  return true;
}

std::string XboxKrnl::read_cstr(uint32_t va, uint32_t max_len) const {
  std::string s;
  for (uint32_t i = 0; i < max_len; ++i) {
    uint8_t b = 0;
    if (!ram_.read_bytes(va + i, &b, 1)) break;  // unmapped: stop
    if (b == 0) break;
    s += static_cast<char>(b);
  }
  return s;
}

// --- dispatcher object registry ---

XboxKrnl::DispatcherObject* XboxKrnl::find_object_by_handle(uint32_t handle) {
  // Kernel handles are 4-aligned, 4*(slot+1), never 0.
  if (handle == 0 || (handle & 3) != 0) return nullptr;
  const uint64_t idx = handle / 4 - 1;
  if (idx >= objects_.size() || !objects_[static_cast<size_t>(idx)])
    return nullptr;
  return objects_[static_cast<size_t>(idx)].get();
}

XboxKrnl::DispatcherObject* XboxKrnl::find_object_by_va(uint32_t va) {
  for (std::unique_ptr<DispatcherObject>& o : va_objects_)
    if (o->guest_va == va) return o.get();
  return nullptr;
}

uint32_t XboxKrnl::insert_object(std::unique_ptr<DispatcherObject> o) {
  size_t slot = 0;
  if (!free_slots_.empty()) {
    slot = free_slots_.back();
    free_slots_.pop_back();
    objects_[slot] = std::move(o);
  } else {
    slot = objects_.size();
    objects_.push_back(std::move(o));
  }
  return static_cast<uint32_t>(4 * (slot + 1));
}

XboxKrnl::DispatcherObject* XboxKrnl::ensure_thread_object(
    uint64_t sched_handle) const {
  auto it = fiber_threads_.find(sched_handle);
  if (it != fiber_threads_.end()) return it->second.get();
  auto o = std::make_unique<DispatcherObject>();
  o->kind = DispatcherObject::Kind::Thread;
  o->guest_va = 0;
  o->thread.sched_handle = static_cast<uint32_t>(sched_handle);
  o->thread.thread_id = next_thread_id_;
  next_thread_id_ += 2;  // odd ids stay odd (uint32 wrap is theoretical)
  o->thread.running = true;
  o->token = event_token(o.get());
  DispatcherObject* raw = o.get();
  fiber_threads_.emplace(sched_handle, std::move(o));
  return raw;
}

XboxKrnl::DispatcherObject* XboxKrnl::find_thread_by_id(
    uint32_t thread_id) const {
  if (thread_id == 0) return nullptr;
  for (const auto& [handle, o] : fiber_threads_)
    if (o->thread.thread_id == thread_id) return o.get();
  for (const std::unique_ptr<DispatcherObject>& o : objects_)
    if (o && o->kind == DispatcherObject::Kind::Thread &&
        o->thread.thread_id == thread_id)
      return o.get();
  for (const std::unique_ptr<DispatcherObject>& o : va_objects_)
    if (o->kind == DispatcherObject::Kind::Thread &&
        o->thread.thread_id == thread_id)
      return o.get();
  return nullptr;
}

XboxKrnl::DispatcherObject* XboxKrnl::find_thread_by_fiber(
    uint32_t sched_handle) const {
  if (sched_handle == 0) return nullptr;
  auto it = fiber_threads_.find(sched_handle);
  if (it != fiber_threads_.end() &&
      it->second->thread.sched_handle == sched_handle)
    return it->second.get();
  for (const std::unique_ptr<DispatcherObject>& o : objects_)
    if (o && o->kind == DispatcherObject::Kind::Thread &&
        o->thread.sched_handle == sched_handle)
      return o.get();
  return nullptr;
}

uint32_t XboxKrnl::current_thread_token() const {
  const uint64_t h = sched_.current_handle();
  if (h == 0) return 0;  // host context: no PKTHREAD
  // Implicit per-fiber identity (created on first use). KeGetCurrent-
  // Thread and mutant ownership use its id as an opaque PKTHREAD token;
  // it is NOT a real kernel address (no KTHREAD is mirrored).
  return ensure_thread_object(h)->thread.thread_id;
}

// --- guest-visible dispatcher headers ---
// Grounded in nxdk xboxkrnl.h layouts (DISPATCHER_HEADER, KSEMAPHORE,
// KMUTANT). The Type/Size bytes are only verified for events (0 / 2
// quadwords); mutants and semaphores get Size = ceil(object bytes / 8)
// derived by the same arithmetic (KMUTANT 28 bytes -> 4, KSEMAPHORE 20
// bytes -> 3) with Type = 0. Not fabricated beyond that: guest code
// reading .Type directly would see the documented values here.
void XboxKrnl::write_dispatcher_header(const DispatcherObject& o) {
  if (o.guest_va == 0) return;  // pool objects have no guest mapping
  const uint32_t va = o.guest_va;
  uint8_t type = 0, size = 2;
  int32_t signal_state = 0;
  switch (o.kind) {
    case DispatcherObject::Kind::Event:
      signal_state = o.event.signaled ? 1 : 0;
      break;
    case DispatcherObject::Kind::Mutant:
      signal_state = o.mutant.signal_state;
      size = 4;  // KMUTANT = 28 bytes
      break;
    case DispatcherObject::Kind::Semaphore:
      signal_state = o.semaphore.count;
      size = 3;  // KSEMAPHORE = 20 bytes
      break;
    case DispatcherObject::Kind::Thread:
      return;  // KTHREAD is not mirrored (handles/tokens only)
  }
  ram_.write32(va + 0, static_cast<uint32_t>(type) |
                           (static_cast<uint32_t>(size) << 16));
  ram_.write32(va + 4, static_cast<uint32_t>(signal_state));
  ram_.write32(va + 8, va + 8);   // WaitListHead.Flink -> self (empty)
  ram_.write32(va + 12, va + 8);  // WaitListHead.Blink -> self
  if (o.kind == DispatcherObject::Kind::Mutant) {
    ram_.write32(va + 0x10, va + 0x10);  // MutantListEntry.Flink -> self
    ram_.write32(va + 0x14, va + 0x10);  // MutantListEntry.Blink -> self
    ram_.write32(va + 0x18, o.mutant.owner);  // OwnerThread (PKTHREAD)
    const uint8_t abandoned = o.mutant.abandoned ? 1 : 0;
    ram_.write_bytes(va + 0x1C, &abandoned, 1);  // Abandoned (BOOLEAN)
  } else if (o.kind == DispatcherObject::Kind::Semaphore) {
    ram_.write32(va + 0x10, static_cast<uint32_t>(o.semaphore.limit));
  }
}

void XboxKrnl::write_signal_state(const DispatcherObject& o) {
  if (o.guest_va == 0) return;
  int32_t state = 0;
  switch (o.kind) {
    case DispatcherObject::Kind::Event:
      state = o.event.signaled ? 1 : 0;
      break;
    case DispatcherObject::Kind::Mutant:
      state = o.mutant.signal_state;
      break;
    case DispatcherObject::Kind::Semaphore:
      state = o.semaphore.count;
      break;
    default:
      return;
  }
  ram_.write32(o.guest_va + 4, static_cast<uint32_t>(state));
}

// --- wait core ---

bool XboxKrnl::dispatcher_satisfied(const DispatcherObject* o) const {
  switch (o->kind) {
    case DispatcherObject::Kind::Event:
      return o->event.signaled;
    case DispatcherObject::Kind::Mutant:
      // Free (SignalState > 0) or already owned by the caller (NT
      // mutant waits on an owned mutant just recurse). owner == 0 means
      // unowned, never "owned by the NULL thread".
      return o->mutant.signal_state > 0 ||
             (o->mutant.owner != 0 &&
              o->mutant.owner == current_thread_token());
    case DispatcherObject::Kind::Semaphore:
      return o->semaphore.count > 0;
    case DispatcherObject::Kind::Thread:
      return !o->thread.running;  // termination signals a thread object
  }
  return false;
}

uint32_t XboxKrnl::dispatcher_acquire(DispatcherObject* o) {
  switch (o->kind) {
    case DispatcherObject::Kind::Event:
      if (o->event.type == kEventSync) {  // synchronization: consumed
        o->event.signaled = false;
        write_signal_state(*o);
      }
      return kStatusSuccess;
    case DispatcherObject::Kind::Mutant: {
      // A mutant cannot be owned by "no thread": acquisition requires
      // a fiber (host-context waits on mutants are invalid, unlike
      // events/semaphores which have no ownership).
      const uint32_t tok = current_thread_token();
      if (tok == 0) return kStatusInvalidParameter;
      o->mutant.signal_state--;  // 1 -> 0 owned; recursion goes < 0
      o->mutant.owner = tok;
      if (o->mutant.abandoned) {
        o->mutant.abandoned = false;  // delivered exactly once (NT)
        write_dispatcher_header(*o);  // state + owner + Abandoned byte
        return kStatusAbandoned;
      }
      write_signal_state(*o);
      return kStatusSuccess;
    }
    case DispatcherObject::Kind::Semaphore:
      o->semaphore.count--;
      write_signal_state(*o);
      return kStatusSuccess;
    case DispatcherObject::Kind::Thread:
      return kStatusSuccess;  // observation, nothing to consume
  }
  return kStatusSuccess;
}

uint32_t XboxKrnl::wait_dispatch(DispatcherObject* o, bool immediate,
                                 uint64_t timeout_ticks) {
  for (;;) {
    if (dispatcher_satisfied(o)) return dispatcher_acquire(o);
    // Zero timeout: satisfy-or-timeout without ever blocking (the nxdk
    // NtWaitForMultipleObjectsEx doc states this for explicit zero).
    if (immediate) return kStatusTimeout;
    // Would block: only a fiber can block (the scheduler switches away
    // from the CALLING fiber; there is no host context to switch from).
    if (!in_fiber()) return kStatusInvalidParameter;
    const bool woken = sched_.block_current(o->token, timeout_ticks);
    if (!woken) return kStatusTimeout;
    if (o->kind == DispatcherObject::Kind::Event) {
      // Wake-implies-consumed for events: the releasing fiber updated
      // the state before waking exactly the right waiters.
      return kStatusSuccess;
    }
    // Mutant/semaphore: another fiber may have taken the unit first;
    // re-evaluate (correct NT behavior - no acquisition guarantee).
  }
}

uint32_t XboxKrnl::wait_multiple(std::vector<DispatcherObject*>& objs,
                                 bool wait_all, bool immediate,
                                 uint64_t timeout_ticks) {
  // This fiber's multi-wait channel (unique: one wait per fiber).
  const uint64_t mw_token =
      kMwTokenBase | sched_.current_handle();
  for (;;) {
    if (wait_all) {
      bool all = true;
      for (const DispatcherObject* o : objs)
        if (!dispatcher_satisfied(o)) { all = false; break; }
      if (all) {
        uint32_t worst = kStatusSuccess;
        for (size_t i = 0; i < objs.size(); ++i) {
          const uint32_t st = dispatcher_acquire(objs[i]);
          if (st != kStatusSuccess && worst == kStatusSuccess)
            worst = kStatusAbandoned + static_cast<uint32_t>(i);
        }
        return worst;
      }
    } else {
      for (size_t i = 0; i < objs.size(); ++i) {
        if (dispatcher_satisfied(objs[i])) {
          const uint32_t st = dispatcher_acquire(objs[i]);
          if (st == kStatusAbandoned)
            return kStatusAbandoned + static_cast<uint32_t>(i);
          return static_cast<uint32_t>(i);  // STATUS_WAIT_0 == 0
        }
      }
    }
    if (immediate) return kStatusTimeout;
    if (!in_fiber()) return kStatusInvalidParameter;
    // Register on every unsatisfied object, block, re-evaluate.
    for (DispatcherObject* o : objs)
      if (!dispatcher_satisfied(o)) o->mw_waiters.push_back(mw_token);
    const bool woken = sched_.block_current(mw_token, timeout_ticks);
    for (DispatcherObject* o : objs) {
      auto& v = o->mw_waiters;
      v.erase(std::remove(v.begin(), v.end(), mw_token), v.end());
    }
    if (!woken) return kStatusTimeout;
    // loop: re-evaluate (any waker may have satisfied any subset)
  }
}

void XboxKrnl::wake_object(DispatcherObject* o, uint32_t units) {
  for (uint32_t i = 0; i < units; ++i)
    if (sched_.signal(o->token, false) == 0) break;  // no (more) waiters
  // Multi-wait waiters re-evaluate their whole set on any change.
  if (!o->mw_waiters.empty()) {
    std::vector<uint64_t> waiters;
    waiters.swap(o->mw_waiters);
    for (uint64_t t : waiters) sched_.signal(t, false);
  }
}

bool XboxKrnl::read_timeout(uint32_t ptr, bool* immediate,
                            uint64_t* ticks) const {
  *immediate = false;
  *ticks = GuestScheduler::kNoTimeout;
  if (ptr == 0) return true;  // NULL timeout = wait forever
  int64_t q = 0;
  if (!read64(ptr, reinterpret_cast<uint64_t*>(&q))) return false;
  interval_to_ticks(q, immediate, ticks);
  return true;
}

// --- thread termination + introspection ---

void XboxKrnl::finish_thread_object(DispatcherObject* t) {
  t->thread.running = false;  // thread objects are signaled on exit
  // Abandon every mutant this thread still owns (real NT behavior; the
  // next acquirer receives STATUS_ABANDONED exactly once).
  const uint32_t token = t->thread.thread_id;
  auto abandon = [token, this](DispatcherObject* m) {
    if (m->kind == DispatcherObject::Kind::Mutant &&
        m->mutant.owner == token) {
      m->mutant.abandoned = true;
      m->mutant.signal_state = 1;
      m->mutant.owner = 0;
      write_signal_state(*m);
      wake_object(m, 1);
    }
  };
  for (std::unique_ptr<DispatcherObject>& o : objects_)
    if (o) abandon(o.get());
  for (std::unique_ptr<DispatcherObject>& o : va_objects_) abandon(o.get());
  wake_object(t, 1);  // release single waiters + multi-wait waiters
}

size_t XboxKrnl::object_count() const {
  size_t n = 0;
  for (const std::unique_ptr<DispatcherObject>& o : objects_)
    if (o) ++n;
  return n;
}

size_t XboxKrnl::event_count() const {
  size_t n = 0;
  for (const std::unique_ptr<DispatcherObject>& o : objects_)
    if (o && o->kind == DispatcherObject::Kind::Event) ++n;
  return n;
}

bool XboxKrnl::event_signaled_by_va(uint32_t va) const {
  const DispatcherObject* e =
      const_cast<XboxKrnl*>(this)->find_object_by_va(va);
  return (e && e->kind == DispatcherObject::Kind::Event) ? e->event.signaled
                                                         : false;
}

bool XboxKrnl::event_signaled_by_handle(uint32_t handle) const {
  const DispatcherObject* e =
      const_cast<XboxKrnl*>(this)->find_object_by_handle(handle);
  return (e && e->kind == DispatcherObject::Kind::Event) ? e->event.signaled
                                                         : false;
}

bool XboxKrnl::thread_running_by_handle(uint32_t handle) const {
  const DispatcherObject* t =
      const_cast<XboxKrnl*>(this)->find_object_by_handle(handle);
  return (t && t->kind == DispatcherObject::Kind::Thread) ? t->thread.running
                                                          : false;
}

bool XboxKrnl::thread_start_info(uint32_t handle,
                                 ThreadStartInfo* out) const {
  const DispatcherObject* t =
      const_cast<XboxKrnl*>(this)->find_object_by_handle(handle);
  if (!t || t->kind != DispatcherObject::Kind::Thread) return false;
  out->start_routine = t->thread.start_routine;
  out->start_context = t->thread.start_context;
  out->system_routine = t->thread.system_routine;
  out->kernel_stack_size = t->thread.kernel_stack_size;
  out->tls_data_size = t->thread.tls_data_size;
  return true;
}

// --- event set/reset (Ke core) ---

uint32_t XboxKrnl::ke_set_event(DispatcherObject* e) {
  const uint32_t prev = e->event.signaled ? 1u : 0u;
  // Wake policy by event type. A SynchronizationEvent with waiters goes
  // straight back to the reset state (NT semantics), otherwise it stays
  // set until a wait consumes it. NotificationEvents stay set until
  // KeResetEvent.
  if (e->event.type == kEventNotify) {
    e->event.signaled = true;
    write_signal_state(*e);
    sched_.signal(e->token, true);  // release ALL waiters
  } else {
    const uint64_t woken = sched_.signal(e->token, false);
    e->event.signaled = (woken == 0);  // consumed when a waiter woke
    write_signal_state(*e);
  }
  wake_object(e, 0);  // multi-wait waiters re-evaluate (units already woken)
  return prev;
}

uint32_t XboxKrnl::ke_reset_event(DispatcherObject* e) {
  const uint32_t prev = e->event.signaled ? 1u : 0u;
  e->event.signaled = false;
  write_signal_state(*e);
  return prev;
}

// --- contiguous memory allocator (Mm) ---

uint32_t XboxKrnl::mm_alloc(uint32_t size, uint32_t lowest, uint32_t highest,
                            uint32_t alignment, uint32_t protect) {
  if (size == 0 || mm_hi_ <= mm_lo_) return 0;
  uint32_t align = alignment ? alignment : 4096u;
  if ((align & (align - 1)) != 0) return 0;  // must be a power of two
  if (align < 4096u) align = 4096u;          // page granularity

  // [lowest, highest] are INCLUSIVE address constraints (as documented
  // for MmAllocateContiguousMemoryEx), intersected with our region.
  const uint32_t lo = std::max(mm_lo_, lowest);
  const uint32_t hi = std::min(highest == 0 ? mm_hi_ - 1 : highest, mm_hi_ - 1);
  if (lo > hi || size > hi - lo + 1) return 0;

  // Deterministic first-fit scan over the free gaps.
  uint32_t cand = (lo + align - 1) & ~(align - 1);
  for (;;) {
    if (cand > hi || size > hi - cand + 1) return 0;
    bool collided = false;
    uint32_t max_end = cand;
    for (const MemAlloc& a : mem_allocs_) {
      if (a.va < cand + size && cand < a.va + a.size) {
        collided = true;
        max_end = std::max(max_end, a.va + a.size);
      }
    }
    if (!collided) break;
    cand = (max_end + align - 1) & ~(align - 1);
  }

  if (!ram_.commit(cand, size)) return 0;
  // Freshly allocated memory is zeroed. The real kernel leaves the
  // content undefined; we choose deterministic zeroing and say so.
  if (uint8_t* p = ram_.host_ptr(cand)) std::memset(p, 0, size);
  const uint32_t base = protect & ~kPageNocache;
  bool ro = false;
  if (base == kPageReadonly) {
    ram_.protect_ro(cand, size);
    ro = true;
  }
  // NOTE: execute bits are recorded; the guest RAM window is never
  // mapped PROT_EXEC on the HOST (ucore::GuestCpu grants execute in the
  // emulated address space instead - TCG never calls guest bytes).
  mem_allocs_.push_back({cand, size, protect});
  if (mem_obs_) mem_obs_(false, cand, size, ro);
  return cand;
}

bool XboxKrnl::mm_free(uint32_t va) {
  for (size_t i = 0; i < mem_allocs_.size(); ++i) {
    if (mem_allocs_[i].va == va) {
      const uint32_t size = mem_allocs_[i].size;
      const bool ro = mem_allocs_[i].protect == kPageReadonly;
      mem_allocs_.erase(mem_allocs_.begin() + i);
      // Real MmFreeContiguousMemory returns the pages to free physical
      // memory: uncommit them in the guest RAM (access now faults) and
      // tell the CPU core to unmap the range (a later allocation of the
      // same VA re-commits and re-maps through the same observer).
      if (ram_.uncommit(va, size) && mem_obs_)
        mem_obs_(true, va, size, ro);
      return true;
    }
  }
  return false;
}

uint32_t XboxKrnl::mm_size(uint32_t va) const {
  for (const MemAlloc& a : mem_allocs_)
    if (a.va == va) return a.size;
  return 0;
}

// --- critical sections (state lives in guest memory) ---

uint32_t XboxKrnl::cs_init(uint32_t va) {
  if (!ram_.writable(va, kCsSize)) return kStatusInvalidParameter;
  DispatcherObject proto;  // embedded dispatcher event, initially reset
  proto.kind = DispatcherObject::Kind::Event;
  proto.guest_va = va;
  proto.event.signaled = false;
  write_dispatcher_header(proto);
  ram_.write32(va + kCsLockCount, static_cast<uint32_t>(-1));
  ram_.write32(va + kCsRecursion, 0);
  ram_.write32(va + kCsOwningThread, 0);
  return kStatusSuccess;
}

uint32_t XboxKrnl::cs_enter(uint32_t va) {
  const uint64_t cur = sched_.current_handle();
  for (;;) {
    int32_t lock = 0, rec = 0;
    uint32_t owner = 0;
    if (!ram_.read32(va + kCsLockCount,
                     reinterpret_cast<uint32_t*>(&lock)) ||
        !ram_.read32(va + kCsRecursion,
                     reinterpret_cast<uint32_t*>(&rec)) ||
        !ram_.read32(va + kCsOwningThread, &owner))
      return kStatusInvalidParameter;
    if (owner == cur && cur != 0) {  // re-enter (holds on reentrancy)
      ram_.write32(va + kCsRecursion, static_cast<uint32_t>(rec + 1));
      return kStatusSuccess;
    }
    if (owner == 0) {  // free: take it
      ram_.write32(va + kCsOwningThread, static_cast<uint32_t>(cur));
      ram_.write32(va + kCsRecursion, 1);
      return kStatusSuccess;
    }
    // Contended. Register as waiter and block for real; RtlLeave
    // decrements LockCount and releases exactly one waiter (FIFO).
    if (!in_fiber()) return kStatusInvalidParameter;
    ram_.write32(va + kCsLockCount, static_cast<uint32_t>(lock + 1));
    sched_.block_current(cs_token_of(va), GuestScheduler::kNoTimeout);
  }
}

uint32_t XboxKrnl::cs_leave(uint32_t va) {
  int32_t lock = 0, rec = 0;
  uint32_t owner = 0;
  if (!ram_.read32(va + kCsLockCount, reinterpret_cast<uint32_t*>(&lock)) ||
      !ram_.read32(va + kCsRecursion, reinterpret_cast<uint32_t*>(&rec)) ||
      !ram_.read32(va + kCsOwningThread, &owner))
    return kStatusInvalidParameter;
  const uint64_t cur = sched_.current_handle();
  if (owner != cur) return kStatusInvalidParameter;  // not the owner
  if (rec > 1) {
    ram_.write32(va + kCsRecursion, static_cast<uint32_t>(rec - 1));
    return kStatusSuccess;
  }
  ram_.write32(va + kCsOwningThread, 0);
  ram_.write32(va + kCsRecursion, 0);
  if (lock >= 0) {  // NT-style LockCount: -1 + waiters; release one
    ram_.write32(va + kCsLockCount, static_cast<uint32_t>(lock - 1));
    sched_.signal(cs_token_of(va), false);
  }
  return kStatusSuccess;
}

uint32_t XboxKrnl::cs_try_enter(uint32_t va) {
  int32_t rec = 0;
  uint32_t owner = 0;
  if (!ram_.read32(va + kCsRecursion, reinterpret_cast<uint32_t*>(&rec)) ||
      !ram_.read32(va + kCsOwningThread, &owner))
    return 0;
  const uint64_t cur = sched_.current_handle();
  if (owner == cur && cur != 0) {
    ram_.write32(va + kCsRecursion, static_cast<uint32_t>(rec + 1));
    return 1;
  }
  if (owner != 0) return 0;
  ram_.write32(va + kCsOwningThread, static_cast<uint32_t>(cur));
  ram_.write32(va + kCsRecursion, 1);
  return 1;
}

// --- kernel data window + thunk installation ---

void XboxKrnl::refresh_data() {
  if (kernel_data_va_ != 0)
    ram_.write32(kernel_data_va_ + 0x08,
                 static_cast<uint32_t>(sched_.current_tick()));
}

std::vector<ThunkSlot> XboxKrnl::install_thunks(xbe::LoadedXbe& image) {
  slots_.clear();
  objects_.clear();
  free_slots_.clear();
  va_objects_.clear();  // unique_ptr: every registered object freed
  fiber_threads_.clear();
  mem_allocs_.clear();
  dbg_lines_.clear();

  // Region layout inside the 64 MiB guest window:
  //   [image_end .. kernel_data) = contiguous memory pool
  //   last 4 KiB page            = kernel data window
  constexpr uint32_t kPage = 4096;
  mm_lo_ = (image.image().image_end_abs() + kPage - 1) & ~(kPage - 1);
  kernel_data_va_ = static_cast<uint32_t>(ram_.size()) - 0x1000;
  mm_hi_ = kernel_data_va_;
  if (mm_lo_ >= mm_hi_) mm_lo_ = mm_hi_;  // degenerate: empty pool

  ram_.commit(kernel_data_va_, 0x1000);
  if (uint8_t* p = ram_.host_ptr(kernel_data_va_)) std::memset(p, 0, 0x1000);

  // XeImageFileName: ANSI_STRING over the XBE's own debug pathname (a
  // real field of the loaded image). When the image provides none (e.g.
  // LithiumX), the string stays empty - we do not invent a path.
  std::string img_name = image.image().debug_pathname();
  if (img_name.empty()) img_name = image.image().debug_filename();
  if (!img_name.empty() && img_name.size() < 0xE0) {
    const uint32_t base = kernel_data_va_;
    // ANSI_STRING (xboxdef.h): +0x10 Length(USHORT), +0x12
    // MaximumLength(USHORT), +0x14 Buffer(PCHAR), string at +0x20.
    ram_.write32(base + 0x10, static_cast<uint32_t>(img_name.size()) |
                                   ((img_name.size() + 1) << 16));
    ram_.write32(base + 0x14, base + 0x20);
    ram_.write_bytes(base + 0x20, img_name.data(),
                     static_cast<uint32_t>(img_name.size() + 1));
  }
  refresh_data();

  // Walk the kernel thunk directory in place.
  const uint32_t kt = image.kernel_thunk_va();
  for (uint32_t i = 0; i < 100000; ++i) {  // sane upper bound
    uint32_t raw = 0;
    if (!ram_.read32(kt + 4 * i, &raw) || raw == 0) break;
    ThunkSlot s;
    s.slot_va = kt + 4 * i;
    s.raw = raw;
    s.ordinal = raw & 0x7FFFFFFFu;
    s.exp = (raw & 0x80000000u) ? find_export(s.ordinal) : nullptr;
    s.implemented = false;
    s.new_value = raw;
    if (s.exp) {
      const bool is_data = s.exp->conv == Conv::Data;
      if (!is_data && implemented_function(s.ordinal)) {
        s.new_value = kHleMarker | s.ordinal;
        s.implemented = true;
      } else if (is_data && implemented_data(s.ordinal)) {
        switch (s.ordinal) {
          case 156: s.new_value = kernel_data_va_ + 0x08; break;   // KeTickCount
          case 164: s.new_value = kernel_data_va_ + 0x120; break;  // LaunchDataPage
          case 324: s.new_value = kernel_data_va_ + 0x00; break;   // XboxKrnlVersion
          case 326: s.new_value = kernel_data_va_ + 0x10; break;   // XeImageFileName
        }
        s.implemented = true;
      }
      if (s.implemented) {
        // Thunk tables usually live in a read-only section (.rdata); the
        // real kernel's loader patches them with kernel addresses, so we
        // temporarily lift RO protection and restore it afterwards.
        const bool was_ro = !ram_.writable(s.slot_va, 4);
        if (was_ro) ram_.protect_rw(s.slot_va, 4);
        ram_.write32(s.slot_va, s.new_value);
        if (was_ro) ram_.protect_ro(s.slot_va, 4);
      }
    }
    slots_.push_back(s);
  }
  return slots_;
}

// --- dispatch ---

namespace {

// Clamp guest-controlled width/precision in a printf spec: any numeric
// run whose value exceeds `cap` is rewritten at the cap, so a format
// string like "%99999999d" read from GUEST memory cannot ask snprintf
// for an unbounded logical field (snprintf already bounds the write;
// this bounds the field too, keeping output deterministic). Flags are
// byte-preserved ("%08x" keeps its zero pad; it is not clamped since
// 8 <= cap).
std::string clamp_printf_spec(const std::string& raw, int cap = 64) {
  std::string out;
  size_t i = 0;
  while (i < raw.size()) {
    if (raw[i] >= '0' && raw[i] <= '9') {
      size_t j = i;
      while (j < raw.size() && raw[j] >= '0' && raw[j] <= '9') ++j;
      const long v = std::strtol(raw.substr(i, j - i).c_str(), nullptr, 10);
      out += (v > cap) ? std::to_string(cap) : raw.substr(i, j - i);
      i = j;
      continue;
    }
    out += raw[i++];
  }
  return out;
}

// NT interval (LARGE_INTEGER QuadPart, 100 ns units) -> virtual ticks.
// Negative = relative, positive = absolute FILETIME, 0 = immediate.
// 1 tick == 1 ms (documented HLE mapping). Sets *immediate when the
// deadline is already reached or sub-tick; when *immediate is false,
// *ticks is always >= 1. NT has no "infinite" interval encoding for
// these calls: waits WITHOUT a timeout are the Timeout == NULL pointer
// case, which the Nt*/Ke* handlers resolve to kNoTimeout before ever
// reaching this function.
void interval_to_ticks(int64_t q, bool* immediate, uint64_t* ticks) {
  *immediate = false;
  *ticks = 0;
  if (q == 0) {
    *immediate = true;
    return;
  }
  if (q > 0) {  // absolute FILETIME deadline
    const uint64_t now = xbox_system_time_filetime();
    if (static_cast<uint64_t>(q) <= now) {
      *immediate = true;
      return;
    }
    *ticks = ceil_ms(static_cast<uint64_t>(q) - now);
    if (*ticks == 0) *immediate = true;
    return;
  }
  *ticks = ceil_ms(static_cast<uint64_t>(-q));
  if (*ticks == 0) *immediate = true;
}

}  // namespace

bool XboxKrnl::returns_64(uint32_t ordinal) const {
  switch (ordinal) {
    case 126:  // KeQueryPerformanceCounter -> ULONGLONG
    case 127:  // KeQueryPerformanceFrequency -> ULONGLONG
      return true;
    default:
      return false;
  }
}

uint32_t XboxKrnl::call(uint32_t ordinal, const uint32_t* args, uint32_t nargs,
                        bool* ok, uint32_t* edx) {
  if (ok) *ok = true;
  // *edx is written ONLY by exports with a documented 64-bit return
  // (see returns_64); everything else leaves the caller's value alone.
  refresh_data();
  const Export* e = find_export(ordinal);
  if (!e || e->conv == Conv::Data) {
    if (ok) *ok = false;
    return 0;
  }
#define NEED(n)                         \
  do {                                  \
    if (nargs < (n)) {                  \
      if (ok) *ok = false;              \
      return 0;                         \
    }                                   \
  } while (0)
#define REJECT()                        \
  do {                                  \
    if (ok) *ok = false;                \
    return kStatusInvalidParameter;     \
  } while (0)
#define VOID_FAIL()                     \
  do {                                  \
    if (ok) *ok = false;                \
    return 0;                           \
  } while (0)

  switch (ordinal) {
    case 5: {  // DbgBreakPoint: a debugger would trap here; we record it.
      const std::string note = "DbgBreakPoint";
      if (dbg_sink_)
        dbg_sink_(note);
      else
        dbg_lines_.push_back(note);
      return 0;
    }

    case 8: {  // DbgPrint(PCSTR fmt, ...) - CDECL, caller pops. Varargs
      // arrive positionally in args[1..]. Supported conversions:
      // %s %c %d %i %u %x %X %o %p %% with flags/width/precision
      // passthrough for numeric conversions; l/h length modifiers are
      // accepted and ignored (32-bit guest).
      NEED(1);
      const std::string fmt = read_cstr(args[0]);
      std::string out;
      uint32_t argi = 1;
      size_t i = 0;
      while (i < fmt.size()) {
        if (fmt[i] != '%') {
          out += fmt[i++];
          continue;
        }
        size_t j = i + 1;
        if (j < fmt.size() && fmt[j] == '%') {
          out += '%';
          i = j + 1;
          continue;
        }
        const size_t spec_start = j;
        while (j < fmt.size() &&
               (fmt[j] == '-' || fmt[j] == '+' || fmt[j] == ' ' ||
                fmt[j] == '#' || fmt[j] == '0' || fmt[j] == '.' ||
                (fmt[j] >= '0' && fmt[j] <= '9')))
          ++j;
        while (j < fmt.size() && (fmt[j] == 'l' || fmt[j] == 'h')) ++j;
        if (j >= fmt.size()) {
          out += fmt.substr(i);
          break;
        }
        const char conv = fmt[j];
        const std::string spec =
            clamp_printf_spec("%" + fmt.substr(spec_start, j - spec_start));
        switch (conv) {
          case 's': case 'S': {
            const uint32_t p = (argi < nargs) ? args[argi++] : 0;
            out += p ? read_cstr(p) : "(null)";
            break;
          }
          case 'c': {
            const uint32_t v = (argi < nargs) ? args[argi++] : 0;
            out += static_cast<char>(v);
            break;
          }
          case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            const uint32_t v = (argi < nargs) ? args[argi++] : 0;
            char buf[48];
            if (conv == 'd' || conv == 'i')
              snprintf(buf, sizeof(buf), (spec + conv).c_str(),
                       static_cast<int32_t>(v));
            else
              snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
            out += buf;
            break;
          }
          case 'p': {
            const uint32_t v = (argi < nargs) ? args[argi++] : 0;
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%08x", v);
            out += buf;
            break;
          }
          default:  // unknown conversion: emit verbatim
            out += fmt.substr(i, j - i + 1);
            break;
        }
        i = j + 1;
      }
      if (dbg_sink_)
        dbg_sink_(out);
      else
        dbg_lines_.push_back(out);
      return static_cast<uint32_t>(out.size());
    }

    // --- Interlocked* (FASTCALL: arg0 = ECX, arg1 = EDX) ---
    // Guest-memory read-modify-write on LONG (32-bit, little endian).
    case 53: {  // InterlockedIncrement(Addend) -> post-increment value
      NEED(1);
      uint32_t v = 0;
      if (!ram_.read32(args[0], &v) || !ram_.writable(args[0], 4)) REJECT();
      ++v;
      ram_.write32(args[0], v);
      return v;
    }
    case 52: {  // InterlockedDecrement(Addend) -> post-decrement value
      NEED(1);
      uint32_t v = 0;
      if (!ram_.read32(args[0], &v) || !ram_.writable(args[0], 4)) REJECT();
      --v;
      ram_.write32(args[0], v);
      return v;
    }
    case 54: {  // InterlockedExchange(Target, Value) -> previous
      NEED(2);
      uint32_t old = 0;
      if (!ram_.read32(args[0], &old) || !ram_.writable(args[0], 4)) REJECT();
      ram_.write32(args[0], args[1]);
      return old;
    }
    case 51: {  // InterlockedCompareExchange(Dest, Exchange, Comparand)
      NEED(3);
      uint32_t old = 0;
      if (!ram_.read32(args[0], &old) || !ram_.writable(args[0], 4)) REJECT();
      if (old == args[2]) ram_.write32(args[0], args[1]);
      return old;
    }
    case 55: {  // InterlockedExchangeAdd(Addend, Increment) -> previous
      NEED(2);
      uint32_t old = 0;
      if (!ram_.read32(args[0], &old) || !ram_.writable(args[0], 4)) REJECT();
      ram_.write32(args[0], old + args[1]);
      return old;
    }

    // --- time ---
    // 64-bit returns are the EDX:EAX pair on the MS x86 ABI (nxdk
    // xboxkrnl.h: both ULONGLONG). EAX returns the low dword; EDX gets
    // the high dword through the out-param. 10 MHz in 64 bits wraps in
    // ~58,000 years; truncating to EAX alone wrapped in ~7 minutes.
    case 126: {  // KeQueryPerformanceCounter(void) -> ULONGLONG
      const uint64_t now = cpu_qpc_now();  // ONE read for both halves
      if (edx) *edx = static_cast<uint32_t>(now >> 32);
      return static_cast<uint32_t>(now);
    }
    case 127:  // KeQueryPerformanceFrequency(void) -> matches the counter
      if (edx) *edx = static_cast<uint32_t>(kQpcFreq >> 32);  // 10 MHz: 0
      return static_cast<uint32_t>(kQpcFreq);
    case 128: {  // KeQuerySystemTime(PLARGE_INTEGER)
      NEED(1);
      if (!write64(args[0], xbox_system_time_filetime())) REJECT();
      return 0;
    }
    case 99: {  // KeDelayExecutionThread(WaitMode, Alertable, Interval)
      NEED(3);
      // Interval == NULL: treated as a zero delay (sub-NT: a NULL
      // interval is caller error; we fail soft, documented).
      int64_t q = 0;
      if (args[2] != 0 && !read64(args[2], reinterpret_cast<uint64_t*>(&q)))
        REJECT();
      bool immediate = false;
      uint64_t ticks = 0;
      interval_to_ticks(q, &immediate, &ticks);
      if (immediate) return kStatusSuccess;  // deadline passed / zero
      if (!in_fiber()) REJECT();
      // interval_to_ticks guarantees ticks >= 1 here; block_current
      // treats kNoTimeout (0) as "no deadline", which a DELAY must
      // never become.
      sched_.block_current(kDelayToken, ticks);  // wakes by timeout
      return kStatusSuccess;
    }
    case 305: {  // RtlTimeToTimeFields(PLARGE_INTEGER, PTIME_FIELDS)
      NEED(2);
      uint64_t ft = 0;
      if (!read64(args[0], &ft)) REJECT();
      if (!ram_.writable(args[1], 16)) REJECT();
      // TIME_FIELDS: 8 x SHORT (Year..Weekday), 16 bytes.
      int64_t days = static_cast<int64_t>(ft) / 864000000000LL;
      int64_t rem = static_cast<int64_t>(ft) % 864000000000LL;
      if (rem < 0) {  // floor division for pre-1601 times
        rem += 864000000000LL;
        days -= 1;
      }
      int64_t y = 0;
      uint32_t m = 0, d = 0;
      // civil_from_days() takes days since 1970-01-01 (Hinnant's epoch);
      // FILETIME days are since 1601-01-01 = 134774 days earlier.
      civil_from_days(days - 134774, &y, &m, &d);
      const uint64_t total_ms = static_cast<uint64_t>(rem) / 10000u;
      const uint32_t ms = static_cast<uint32_t>(total_ms % 1000u);
      const uint32_t s = static_cast<uint32_t>((total_ms / 1000u) % 60u);
      const uint32_t mi = static_cast<uint32_t>((total_ms / 60000u) % 60u);
      const uint32_t h = static_cast<uint32_t>(total_ms / 3600000u);
      // Weekday: day 0 is 1601-01-01, a Monday; NT encodes Sunday = 0.
      const uint32_t wd =
          static_cast<uint32_t>(((days % 7) + 7 + 1) % 7);
      ram_.write32(args[1] + 0x0, static_cast<uint32_t>(y & 0xFFFF) | (m << 16));
      ram_.write32(args[1] + 0x4, d | (h << 16));
      ram_.write32(args[1] + 0x8, mi | (s << 16));
      ram_.write32(args[1] + 0xC, ms | (wd << 16));
      return 0;
    }

    // --- events (Ke* flavor: direct object pointers) ---
    case 108: {  // KeInitializeEvent(Event, Type, State)
      NEED(3);
      const uint32_t va = args[0];
      const uint32_t type = args[1];
      if (type > kEventSync) REJECT();
      if (!ram_.writable(va, 16)) REJECT();
      DispatcherObject* ev = find_object_by_va(va);
      if (ev && ev->kind != DispatcherObject::Kind::Event) REJECT();
      if (!ev) {
        auto o = std::make_unique<DispatcherObject>();
        o->kind = DispatcherObject::Kind::Event;
        o->guest_va = va;
        o->token = event_token(o.get());
        va_objects_.push_back(std::move(o));
        ev = va_objects_.back().get();
      }
      ev->event.type = type;
      ev->event.signaled = (args[2] & 1u) != 0;
      write_dispatcher_header(*ev);
      return 0;
    }
    case 145: {  // KeSetEvent(Event, Increment, Wait) -> previous state
      NEED(3);
      DispatcherObject* ev = find_object_by_va(args[0]);
      if (!ev || ev->kind != DispatcherObject::Kind::Event) REJECT();
      return ke_set_event(ev);  // Increment/Wait: no priority system
    }
    case 138: {  // KeResetEvent(Event) -> previous state
      NEED(1);
      DispatcherObject* ev = find_object_by_va(args[0]);
      if (!ev || ev->kind != DispatcherObject::Kind::Event) REJECT();
      return ke_reset_event(ev);
    }
    case 159: {  // KeWaitForSingleObject(Object, Reason, Mode, Alertable,
                 //                Timeout)
      NEED(5);
      // Any dispatcher object registered at `Object` (events, mutants,
      // semaphores - exactly the NT semantics of a generic wait).
      DispatcherObject* o = find_object_by_va(args[0]);
      if (!o) REJECT();
      // WaitReason/WaitMode: bookkeeping only. Alertable: no APC support
      // (documented: waits behave as non-alertable).
      bool immediate = false;
      uint64_t ticks = GuestScheduler::kNoTimeout;
      if (!read_timeout(args[4], &immediate, &ticks)) REJECT();
      const uint32_t st = wait_dispatch(o, immediate, ticks);
      if (st == kStatusInvalidParameter && ok) *ok = false;
      return st;
    }

    // --- events (Nt* flavor: handle table) ---
    case 189: {  // NtCreateEvent(PHANDLE, ObjectAttributes, Type, State)
      NEED(4);
      const uint32_t type = args[2];
      if (type > kEventSync) REJECT();
      if (!ram_.writable(args[0], 4)) REJECT();
      auto ev = std::make_unique<DispatcherObject>();
      ev->kind = DispatcherObject::Kind::Event;
      ev->guest_va = 0;  // kernel pool object: no guest mapping
      ev->event.type = type;
      ev->event.signaled = (args[3] & 1u) != 0;
      ev->token = event_token(ev.get());
      const uint32_t handle = insert_object(std::move(ev));
      ram_.write32(args[0], handle);
      return kStatusSuccess;
    }
    case 225: {  // NtSetEvent(Handle, PLONG PreviousState)
      NEED(2);
      DispatcherObject* ev = find_object_by_handle(args[0]);
      if (!ev || ev->kind != DispatcherObject::Kind::Event) REJECT();
      const uint32_t prev = ke_set_event(ev);
      if (args[1] != 0 && ram_.writable(args[1], 4))
        ram_.write32(args[1], prev);
      return kStatusSuccess;
    }
    case 186: {  // NtClearEvent(Handle)
      NEED(1);
      DispatcherObject* ev = find_object_by_handle(args[0]);
      if (!ev || ev->kind != DispatcherObject::Kind::Event) REJECT();
      ev->event.signaled = false;
      write_signal_state(*ev);
      return kStatusSuccess;
    }
    case 205: {  // NtPulseEvent(Handle, PLONG PreviousState)
      NEED(2);
      DispatcherObject* ev = find_object_by_handle(args[0]);
      if (!ev || ev->kind != DispatcherObject::Kind::Event) REJECT();
      const uint32_t prev = ev->event.signaled ? 1u : 0u;
      sched_.signal(ev->token, ev->event.type == kEventNotify);
      ev->event.signaled = false;  // pulse: release waiters, leave reset
      write_signal_state(*ev);
      wake_object(ev, 0);  // notify multi-wait waiters of the change
      if (args[1] != 0 && ram_.writable(args[1], 4))
        ram_.write32(args[1], prev);
      return kStatusSuccess;
    }
    case 233: {  // NtWaitForSingleObject(Handle, Alertable, Timeout)
      NEED(3);
      DispatcherObject* o = find_object_by_handle(args[0]);
      if (!o) REJECT();
      bool immediate = false;
      uint64_t ticks = GuestScheduler::kNoTimeout;
      if (!read_timeout(args[2], &immediate, &ticks)) REJECT();
      const uint32_t st = wait_dispatch(o, immediate, ticks);
      if (st == kStatusInvalidParameter && ok) *ok = false;
      return st;
    }
    case 234: {  // NtWaitForSingleObjectEx(Handle, Mode, Alertable, Timeout)
      NEED(4);
      DispatcherObject* o = find_object_by_handle(args[0]);
      if (!o) REJECT();
      bool immediate = false;
      uint64_t ticks = GuestScheduler::kNoTimeout;
      if (!read_timeout(args[3], &immediate, &ticks)) REJECT();
      const uint32_t st = wait_dispatch(o, immediate, ticks);
      if (st == kStatusInvalidParameter && ok) *ok = false;
      return st;
    }
    case 235: {  // NtWaitForMultipleObjectsEx(Count, Handles[], WaitType,
                 //                WaitMode, Alertable, Timeout)
      NEED(6);
      const uint32_t count = args[0];
      if (count == 0 || count > kMaxWaitObjects) REJECT();
      if (!ram_.readable(args[1], 4 * count)) REJECT();
      if (args[2] > 1) REJECT();  // WAIT_TYPE: 0 = WaitAll, 1 = WaitAny
      const bool wait_all = args[2] == 0;
      bool immediate = false;
      uint64_t ticks = GuestScheduler::kNoTimeout;
      if (!read_timeout(args[5], &immediate, &ticks)) REJECT();
      std::vector<DispatcherObject*> objs;
      objs.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t h = 0;
        if (!ram_.read32(args[1] + 4 * i, &h)) REJECT();
        DispatcherObject* o = find_object_by_handle(h);
        if (!o) REJECT();  // includes closed / non-dispatcher handles
        objs.push_back(o);
      }
      const uint32_t st =
          wait_multiple(objs, wait_all, immediate, ticks);
      if ((st == kStatusInvalidParameter || st == kStatusTimeout) && ok)
        *ok = (st != kStatusInvalidParameter);
      return st;
    }

    // --- mutants (mutexes): nxdk KMUTANT semantics ---
    // SignalState: 1 = free (signaled), 0 = owned once, <0 = owned with
    // recursion. Ownership is an opaque PKTHREAD token (the fiber's
    // kernel thread id), never a fabricated kernel address.
    case 192: {  // NtCreateMutant(PHANDLE, ObjectAttributes, InitialOwner)
      NEED(3);
      if (!ram_.writable(args[0], 4)) REJECT();
      auto m = std::make_unique<DispatcherObject>();
      m->kind = DispatcherObject::Kind::Mutant;
      m->guest_va = 0;  // pool object
      if (args[2] & 1u) {
        if (!in_fiber()) REJECT();  // initial ownership needs a thread
        m->mutant.signal_state = 0;
        m->mutant.owner = current_thread_token();
      } else {
        m->mutant.signal_state = 1;
      }
      m->token = event_token(m.get());
      const uint32_t handle = insert_object(std::move(m));
      ram_.write32(args[0], handle);
      return kStatusSuccess;
    }
    case 221: {  // NtReleaseMutant(Handle, PLONG PreviousCount)
      NEED(2);
      DispatcherObject* m = find_object_by_handle(args[0]);
      if (!m || m->kind != DispatcherObject::Kind::Mutant) REJECT();
      const uint32_t self = current_thread_token();
      // Must be owned AND owned by the caller (owner == 0 is unowned;
      // from the host both would otherwise compare equal as 0).
      if (m->mutant.owner == 0 || m->mutant.owner != self)
        REJECT();  // STATUS_MUTANT_NOT_OWNED
      const int32_t prev = m->mutant.signal_state;
      m->mutant.signal_state++;
      if (m->mutant.signal_state > 0) {
        m->mutant.owner = 0;  // fully released -> free
        wake_object(m, 1);    // exactly one waiter may now acquire
      }
      write_signal_state(*m);
      if (args[1] != 0 && ram_.writable(args[1], 4))
        ram_.write32(args[1], static_cast<uint32_t>(prev));
      return kStatusSuccess;
    }
    case 213: {  // NtQueryMutant(Handle, PMUTANT_BASIC_INFORMATION)
      NEED(2);
      DispatcherObject* m = find_object_by_handle(args[0]);
      if (!m || m->kind != DispatcherObject::Kind::Mutant) REJECT();
      if (!ram_.writable(args[1], 8)) REJECT();
      // MUTANT_BASIC_INFORMATION {LONG CurrentCount; UCHAR
      // OwnedByCaller; UCHAR AbandonedState;} (nxdk xboxkrnl.h).
      ram_.write32(args[1] + 0,
                   static_cast<uint32_t>(m->mutant.signal_state));
      const uint8_t owned =
          (m->mutant.owner != 0 && m->mutant.owner == current_thread_token())
              ? 1
              : 0;
      const uint8_t abandoned = m->mutant.abandoned ? 1 : 0;
      ram_.write_bytes(args[1] + 4, &owned, 1);
      ram_.write_bytes(args[1] + 5, &abandoned, 1);
      return kStatusSuccess;
    }
    case 110: {  // KeInitializeMutant(PRKMUTANT, InitialOwner)
      NEED(2);
      const uint32_t va = args[0];
      if (!ram_.writable(va, 28)) REJECT();  // sizeof(KMUTANT)
      auto m = std::make_unique<DispatcherObject>();
      m->kind = DispatcherObject::Kind::Mutant;
      m->guest_va = va;
      if (args[1] & 1u) {
        if (!in_fiber()) REJECT();  // initial ownership needs a thread
        m->mutant.signal_state = 0;
        m->mutant.owner = current_thread_token();
      } else {
        m->mutant.signal_state = 1;
      }
      m->token = event_token(m.get());
      va_objects_.push_back(std::move(m));
      write_dispatcher_header(*va_objects_.back());
      return 0;
    }
    case 131: {  // KeReleaseMutant(PRKMUTANT, Increment, Abandoned, Wait)
      NEED(4);
      DispatcherObject* m = find_object_by_va(args[0]);
      if (!m || m->kind != DispatcherObject::Kind::Mutant) REJECT();
      // Kernel-internal flavor: no ownership check (the caller is
      // trusted kernel code), per the NT KeReleaseMutant contract. The
      // Abandoned/Increment/Wait parameters are bookkeeping here.
      const int32_t prev = m->mutant.signal_state;
      m->mutant.signal_state++;
      if (m->mutant.signal_state > 0) {
        m->mutant.owner = 0;
        wake_object(m, 1);
      }
      write_signal_state(*m);
      return static_cast<uint32_t>(prev);
    }

    // --- semaphores: nxdk KSEMAPHORE semantics (count / limit) ---
    case 193: {  // NtCreateSemaphore(PHANDLE, ObjectAttributes, Initial,
                 //                 Maximum)
      NEED(4);
      if (!ram_.writable(args[0], 4)) REJECT();
      const int32_t initial = static_cast<int32_t>(args[2]);
      const int32_t maximum = static_cast<int32_t>(args[3]);
      if (maximum <= 0 || initial < 0 || initial > maximum) REJECT();
      auto s = std::make_unique<DispatcherObject>();
      s->kind = DispatcherObject::Kind::Semaphore;
      s->guest_va = 0;  // pool object
      s->semaphore.count = initial;
      s->semaphore.limit = maximum;
      s->token = event_token(s.get());
      const uint32_t handle = insert_object(std::move(s));
      ram_.write32(args[0], handle);
      return kStatusSuccess;
    }
    case 222: {  // NtReleaseSemaphore(Handle, ReleaseCount, PreviousCount)
      NEED(3);
      DispatcherObject* s = find_object_by_handle(args[0]);
      if (!s || s->kind != DispatcherObject::Kind::Semaphore) REJECT();
      const int32_t rel = static_cast<int32_t>(args[1]);
      if (rel <= 0) REJECT();
      const int32_t prev = s->semaphore.count;
      if (prev > s->semaphore.limit - rel) REJECT();  // LIMIT_EXCEEDED
      s->semaphore.count = prev + rel;
      // Wake up to `rel` waiters; each consumes one unit on wake.
      const uint32_t units =
          static_cast<uint32_t>(std::min<int64_t>(rel, s->semaphore.count));
      wake_object(s, units);
      write_signal_state(*s);
      if (args[2] != 0 && ram_.writable(args[2], 4))
        ram_.write32(args[2], static_cast<uint32_t>(prev));
      return kStatusSuccess;
    }
    case 214: {  // NtQuerySemaphore(Handle, PSEMAPHORE_BASIC_INFORMATION)
      NEED(2);
      DispatcherObject* s = find_object_by_handle(args[0]);
      if (!s || s->kind != DispatcherObject::Kind::Semaphore) REJECT();
      if (!ram_.writable(args[1], 8)) REJECT();
      // SEMAPHORE_BASIC_INFORMATION {LONG CurrentCount; LONG
      // MaximumCount;} (nxdk xboxkrnl.h).
      ram_.write32(args[1] + 0, static_cast<uint32_t>(s->semaphore.count));
      ram_.write32(args[1] + 4, static_cast<uint32_t>(s->semaphore.limit));
      return kStatusSuccess;
    }
    case 112: {  // KeInitializeSemaphore(PRKSEMAPHORE, Count, Limit)
      NEED(3);
      const uint32_t va = args[0];
      if (!ram_.writable(va, 20)) REJECT();  // sizeof(KSEMAPHORE)
      // VOID return: no validation channel (documented: garbage params
      // create a degenerate object, exactly like the real VOID init).
      auto s = std::make_unique<DispatcherObject>();
      s->kind = DispatcherObject::Kind::Semaphore;
      s->guest_va = va;
      s->semaphore.count = static_cast<int32_t>(args[1]);
      s->semaphore.limit = static_cast<int32_t>(args[2]);
      s->token = event_token(s.get());
      va_objects_.push_back(std::move(s));
      write_dispatcher_header(*va_objects_.back());
      return 0;
    }
    case 132: {  // KeReleaseSemaphore(PRKSEMAPHORE, Increment, Adjustment,
                 //                 Wait) -> previous count
      NEED(4);
      DispatcherObject* s = find_object_by_va(args[0]);
      if (!s || s->kind != DispatcherObject::Kind::Semaphore) REJECT();
      const int32_t adj = static_cast<int32_t>(args[2]);
      if (adj <= 0) REJECT();
      const int32_t prev = s->semaphore.count;
      if (prev > s->semaphore.limit - adj) REJECT();  // LIMIT_EXCEEDED
      s->semaphore.count = prev + adj;
      const uint32_t units =
          static_cast<uint32_t>(std::min<int64_t>(adj, s->semaphore.count));
      wake_object(s, units);
      write_signal_state(*s);
      return static_cast<uint32_t>(prev);
    }

    // --- threads ---
    // PKTHREAD is an opaque per-fiber token (the kernel thread id), NOT
    // a real KTHREAD address: no KTHREAD structure is mirrored into
    // guest memory. Thread objects are waitable: a terminated thread is
    // "signaled" (NT semantics), which makes handle-joins real.
    case 104:  // KeGetCurrentThread(void) -> PKTHREAD (opaque token)
      return current_thread_token();
    case 143: {  // KeSetBasePriorityThread(PKTHREAD, Increment) -> prev
      NEED(2);
      DispatcherObject* t = find_thread_by_id(args[0]);
      if (!t) REJECT();
      const int32_t prev = t->thread.base_priority;
      t->thread.base_priority = static_cast<int32_t>(args[1]);
      return static_cast<uint32_t>(prev);  // recorded; NO scheduling effect
    }
    case 255: {  // PsCreateSystemThreadEx(PHANDLE, ExtensionSize,
                 //     StackSize, TlsDataSize, PThreadId OPTIONAL,
                 //     StartRoutine, StartContext, CreateSuspended,
                 //     DebuggerThread, SystemRoutine OPTIONAL)
      NEED(10);
      if (!ram_.writable(args[0], 4)) REJECT();
      auto t = std::make_unique<DispatcherObject>();
      t->kind = DispatcherObject::Kind::Thread;
      t->guest_va = 0;
      t->thread.start_routine = args[5];
      t->thread.start_context = args[6];
      t->thread.system_routine = args[9];
      t->thread.kernel_stack_size = args[2];  // recorded, not honored
      t->thread.tls_data_size = args[3];      // reserved on the fiber stack
      t->thread.suspend_count = (args[7] & 1u) ? 1u : 0u;
      t->thread.thread_id = next_thread_id_;
      next_thread_id_ += 2;
      t->thread.running = true;
      t->token = event_token(t.get());
      const uint32_t tls = args[3];
      // Spawn the scheduler fiber first (spawn can fail). Its body
      // finds its own thread object by fiber handle - valid because the
      // body cannot run before this call returns (cooperative scheduler).
      const uint64_t fiber = sched_.spawn_reserved(
          "systhread",
          [this](GuestThread&) {
            const uint32_t cur =
                static_cast<uint32_t>(sched_.current_handle());
            DispatcherObject* self = find_thread_by_fiber(cur);
            if (self && self->thread.suspend_count > 0) {
              // Created suspended: parked until NtResumeThread drains
              // the suspend count (a real block on the thread token).
              sched_.block_current(self->token, GuestScheduler::kNoTimeout);
            }
            if (thread_runner_) {
              // Pass the thread's OWN handle: the runner fetches the
              // start routine/context through thread_start_info().
              const uint32_t self_handle =
                  self ? self->thread.object_handle : 0;
              thread_runner_(self_handle);
              if (self) finish_thread_object(self);  // routine returned
            } else if (self) {
              // No CPU core installed: honest, recorded non-run.
              self->thread.runnable_missing_core = true;
              finish_thread_object(self);
            }
            // Returning ends the fiber (scheduler trampoline finishes it).
          },
          tls);
      if (fiber == 0) REJECT();
      t->thread.sched_handle = static_cast<uint32_t>(fiber);
      const uint32_t handle = insert_object(std::move(t));
      if (DispatcherObject* self = find_object_by_handle(handle))
        self->thread.object_handle = handle;
      ram_.write32(args[0], handle);
      if (args[4] != 0 && ram_.writable(args[4], 4))
        ram_.write32(args[4], handle);  // ThreadId: HLE-assigned, documented
      return kStatusSuccess;
    }
    case 258: {  // PsTerminateSystemThread(ExitStatus) - never returns
      NEED(1);
      const uint32_t cur = static_cast<uint32_t>(sched_.current_handle());
      DispatcherObject* t = find_thread_by_fiber(cur);
      if (!t) REJECT();  // called outside any thread context
      finish_thread_object(t);
      GuestThread* g = sched_.find(cur);
      if (g) g->exit(static_cast<int>(args[0]));  // noreturn
      REJECT();
    }
    case 187: {  // NtClose(Handle) - valid for every object kind.
      // Closing a thread handle never terminates the thread (NT); the
      // fiber keeps its identity for PsTerminateSystemThread, which
      // looks itself up by scheduler handle, not by handle.
      NEED(1);
      if (!find_object_by_handle(args[0])) REJECT();  // INVALID_HANDLE
      const size_t slot = args[0] / 4 - 1;
      objects_[slot].reset();
      free_slots_.push_back(slot);
      return kStatusSuccess;
    }
    case 224: {  // NtResumeThread(Handle, PULONG PreviousSuspendCount)
      NEED(2);
      DispatcherObject* t = find_object_by_handle(args[0]);
      if (!t || t->kind != DispatcherObject::Kind::Thread) REJECT();
      const uint32_t prev = t->thread.suspend_count;
      if (t->thread.suspend_count > 0 &&
          --t->thread.suspend_count == 0) {
        sched_.signal(t->token, false);  // unpark the fiber
      }
      if (args[1] != 0 && ram_.writable(args[1], 4))
        ram_.write32(args[1], prev);
      return kStatusSuccess;
    }
    case 140: {  // KeResumeThread(PKTHREAD) -> previous suspend count
      NEED(1);
      DispatcherObject* t = find_thread_by_id(args[0]);
      if (!t || t->kind != DispatcherObject::Kind::Thread) REJECT();
      const uint32_t prev = t->thread.suspend_count;
      if (t->thread.suspend_count > 0 &&
          --t->thread.suspend_count == 0)
        sched_.signal(t->token, false);
      return prev;
    }
    case 231: {  // NtSuspendThread(Handle, PULONG PreviousSuspendCount)
      NEED(2);
      DispatcherObject* t = find_object_by_handle(args[0]);
      if (!t || t->kind != DispatcherObject::Kind::Thread) REJECT();
      const uint32_t self = current_thread_token();
      const bool is_self = (t->thread.thread_id == self);
      if (!is_self && t->thread.running && t->thread.sched_handle != 0) {
        // Suspending a STARTED, OTHER fiber would need to preempt it;
        // the cooperative scheduler cannot. Honest rejection.
        REJECT();
      }
      const uint32_t prev = t->thread.suspend_count++;
      if (is_self) {
        // Self-suspend parks this fiber for real until a resume.
        sched_.block_current(t->token, GuestScheduler::kNoTimeout);
      }
      if (args[1] != 0 && ram_.writable(args[1], 4))
        ram_.write32(args[1], prev);
      return kStatusSuccess;
    }
    case 152: {  // KeSuspendThread(PKTHREAD) -> previous suspend count
      NEED(1);
      DispatcherObject* t = find_thread_by_id(args[0]);
      if (!t || t->kind != DispatcherObject::Kind::Thread) REJECT();
      const uint32_t self = current_thread_token();
      const bool is_self = (t->thread.thread_id == self);
      if (!is_self && t->thread.running && t->thread.sched_handle != 0)
        REJECT();  // same cooperative limit as NtSuspendThread
      const uint32_t prev = t->thread.suspend_count++;
      if (is_self)
        sched_.block_current(t->token, GuestScheduler::kNoTimeout);
      return prev;
    }
    case 238: {  // NtYieldExecution(void): one cooperative round-robin step
      GuestThread* cur = sched_.find(sched_.current_handle());
      if (cur) cur->yield();
      return kStatusSuccess;
    }

    // --- contiguous memory ---
    case 165:  // MmAllocateContiguousMemory(NumberOfBytes)
      NEED(1);
      return mm_alloc(args[0], 0, 0, 0, kPageReadWrite);
    case 166:  // MmAllocateContiguousMemoryEx(NumberOfBytes, Lowest,
               //   Highest, Alignment, Protect)
      NEED(5);
      return mm_alloc(args[0], args[1], args[2], args[3], args[4]);
    case 171: {  // MmFreeContiguousMemory(BaseAddress) - VOID
      NEED(1);
      if (!mm_free(args[0])) VOID_FAIL();
      return 0;
    }
    case 180:  // MmQueryAllocationSize(BaseAddress) -> SIZE_T (EAX)
      NEED(1);
      return mm_size(args[0]);

    // --- memory operations ---
    case 320: {  // RtlZeroMemory(Destination, Length)
      NEED(2);
      if (args[1] == 0) return 0;  // zero-length: no-op (range checks need size > 0)
      if (!ram_.writable(args[0], args[1])) REJECT();
      if (uint8_t* p = ram_.host_ptr(args[0])) std::memset(p, 0, args[1]);
      return 0;
    }
    case 284: {  // RtlFillMemory(Destination, Length, Fill)
      NEED(3);
      if (args[1] == 0) return 0;
      if (!ram_.writable(args[0], args[1])) REJECT();
      if (uint8_t* p = ram_.host_ptr(args[0]))
        std::memset(p, static_cast<int>(args[2] & 0xFFu), args[1]);
      return 0;
    }
    case 298: {  // RtlMoveMemory(Destination, Source, Length) - overlap-safe
      NEED(3);
      if (args[2] == 0) return 0;
      if (!ram_.writable(args[0], args[2]) || !ram_.readable(args[1], args[2]))
        REJECT();
      std::memmove(ram_.host_ptr(args[0]), ram_.host_ptr(args[1]), args[2]);
      return 0;
    }

    // --- critical sections ---
    case 291: {  // RtlInitializeCriticalSection
      NEED(1);
      if (cs_init(args[0]) != kStatusSuccess) VOID_FAIL();
      return 0;
    }
    case 277: {  // RtlEnterCriticalSection (may block for real)
      NEED(1);
      if (cs_enter(args[0]) != kStatusSuccess) VOID_FAIL();
      return 0;
    }
    case 294: {  // RtlLeaveCriticalSection
      NEED(1);
      if (cs_leave(args[0]) != kStatusSuccess) VOID_FAIL();
      return 0;
    }
    case 306:  // RtlTryEnterCriticalSection -> BOOLEAN
      NEED(1);
      return cs_try_enter(args[0]);

    default:
      if (ok) *ok = false;
      return 0;
  }
#undef NEED
#undef REJECT
#undef VOID_FAIL
}

}  // namespace krnl
