#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cpu_sched.h"
#include "xbe_loader.h"

// Xbox kernel (xboxkrnl.exe) HLE, driven by reverse-engineered data.
//
// Everything in this module is grounded in public, verifiable sources:
//   - Ordinal/name/convention/arg-size table for ALL kernel exports:
//     generated into src/xbox_krnl_exports.inc from nxdk's
//     lib/xboxkrnl/xboxkrnl.exe.def (CC0-1.0, transcribed from the real
//     xboxkrnl.exe import library), cross-checked against
//     xboxdevwiki.net/Kernel. MS decoration grammar gives us the real
//     calling conventions (stdcall @N = stack bytes, @Name@N fastcall =
//     total arg bytes with ECX/EDX first, no suffix = cdecl, "DATA" =
//     exported data symbols). Notable real-world quirks this preserves:
//     Interlocked* are FASTCALL and DbgPrint is CDECL on Xbox.
//   - Signatures + struct layouts: nxdk xboxkrnl.h / xboxdef.h /
//     ntstatus.h (e.g. RTL_CRITICAL_SECTION embeds a dispatcher EVENT;
//     NtCreateEvent takes NO DesiredAccess; KeQueryPerformanceCounter
//     RETURNS ULONGLONG instead of filling an out-parameter).
//   - The import profile of the vendored LithiumX XBE (103 kernel
//     imports, all resolved against the table) drives what matters.
//
// What is REAL here:
//   - The full export table and per-export calling conventions.
//   - Thunk resolution against a LoadedXbe: every kernel thunk slot of
//     a real XBE is resolved to (ordinal, name, implemented?).
//   - 64-bit returns follow the MS x86 ABI: the value comes back in
//     EDX:EAX. call() returns EAX (low dword) and writes the high dword
//     through its `edx` out-parameter for the exports listed by
//     returns_64() (KeQueryPerformanceCounter/Frequency); all other
//     exports leave *edx untouched.
//   - Handlers implemented against the GUEST address space (XbeRam) and
//     the fiber scheduler, with real NT semantics where implemented:
//     Interlocked* (guest-memory RMW), Rtl{Zero,Fill,Move}Memory,
//     contiguous memory allocator (MmAllocateContiguousMemory[Ex] /
//     MmFreeContiguousMemory / MmQueryAllocationSize), the whole EVENT
//     model (KeInitializeEvent/KeSetEvent/KeResetEvent +
//     NtCreateEvent/NtSetEvent/NtClearEvent/NtPulseEvent +
//     KeWaitForSingleObject/NtWaitForSingleObject[Ex]) with REAL fiber
//     blocking (sync events release exactly one waiter, notification
//     events release all), critical sections (real reentrancy + real
//     blocking through the embedded-event model), KeDelayExecutionThread
//     (real fiber sleep), NtYieldExecution, KeQuerySystemTime,
//     RtlTimeToTimeFields, DbgPrint (cdecl varargs subset) and DATA
//     exports (KeTickCount, XboxKrnlVersion, XeImageFileName,
//     LaunchDataPage) living in a real guest-mapped kernel data window.
//
// Honest limitations (do not remove):
//   - Guest execution is provided by the optional Unicorn CPU core
//     (xbox_cpu.h): without it nothing EXECUTES guest machine code.
//     call() is the dispatch boundary the CPU core hits (args
//     positional, fastcall args 0/1 = ECX/EDX); installed thunk slots
//     carry HLE markers (kHleMarker|ordinal) for that core.
//   - ~30 of 371 exports are implemented; unknown/unimplemented thunks
//     are reported honestly per loaded image (see install_thunks).
//   - Alertable waits return as non-alertable (no APC support).
//   - XBOX_KRNL_VERSION is zero-initialized: real build numbers come
//     from dumped kernels, which this project deliberately never ships
//     or fabricates.
//   - Cooperative scheduler: critical section contention only manifests
//     when the owner voluntarily yields/blocks while holding the lock.
//   - XboxHDKey and the object-type data exports are NOT implemented
//     (per-console secrets / kernel pool objects).

namespace krnl {

// Calling conventions, decoded from the MS import-library decoration.
enum class Conv : uint8_t { Stdcall, Fastcall, Cdecl, Data };

struct Export {
  uint32_t ordinal;
  const char* name;
  Conv conv;
  // STDCALL: callee-popped stack bytes (arity = value / 4).
  // FASTCALL: TOTAL argument bytes (first 8 = ECX/EDX pair).
  // CDECL / DATA: 0 (caller-pops / address).
  uint32_t arg_bytes;
};

// Full kernel export table (generated; see file header).
size_t export_count();
const Export& export_at(size_t index);
// nullptr when the ordinal is not a documented kernel export.
const Export* find_export(uint32_t ordinal);
const char* name_of(uint32_t ordinal);  // "<unknown>" when not found

// STATUS_* values used by handlers (nxdk lib/xboxkrnl/ntstatus.h).
constexpr uint32_t kStatusSuccess = 0x00000000u;
constexpr uint32_t kStatusUserApc = 0x000000C0u;      // (documented; unused)
constexpr uint32_t kStatusAlerted = 0x00000101u;      // (documented; unused)
constexpr uint32_t kStatusAbandoned = 0x00000080u;    // STATUS_ABANDONED_WAIT_0
constexpr uint32_t kStatusTimeout = 0x00000102u;
constexpr uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr uint32_t kStatusMutantNotOwned = 0xC0000046u;
constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047u;

// MAXIMUM_WAIT_OBJECTS: standard NT WDM.h value (nxdk does not redefine
// it); bounds NtWaitForMultipleObjectsEx.
constexpr uint32_t kMaxWaitObjects = 64;

// Thunk slot marker for implemented FUNCTION exports: the CPU core
// (xbox_cpu.h, optional Unicorn build) dispatches on PC == kHleMarker |
// ordinal. The value sits in the 0xF0000000 region, far from real Xbox
// kernel (0x80010000+) and user (0x00010000+) addresses.
constexpr uint32_t kHleMarker = 0xF00F0000u;

// PAGE_* protection flags (nxdk xboxkrnl.h) - accepted by
// MmAllocateContiguousMemoryEx and recorded per allocation.
constexpr uint32_t kPageNoAccess = 0x01u;
constexpr uint32_t kPageReadonly = 0x02u;
constexpr uint32_t kPageReadWrite = 0x04u;
constexpr uint32_t kPageExecute = 0x10u;
constexpr uint32_t kPageExecuteRead = 0x20u;
constexpr uint32_t kPageExecuteReadWrite = 0x40u;
constexpr uint32_t kPageNocache = 0x200u;

// EVENT_TYPE (nxdk xboxkrnl.h).
constexpr uint32_t kEventNotify = 0u;  // NotificationEvent
constexpr uint32_t kEventSync = 1u;    // SynchronizationEvent

// One per-export result of resolving an XBE kernel thunk table.
struct ThunkSlot {
  uint32_t slot_va;    // guest VA of the IAT-style entry
  uint32_t raw;        // raw table entry
  uint32_t ordinal;    // raw & 0x7FFFFFFF (0 for non-import entries)
  const Export* exp;   // nullptr if not in the export table
  bool implemented;    // handler or implemented data export exists
  uint32_t new_value;  // value written into the slot (== raw if untouched)
};

class XboxKrnl {
 public:
  // ram: guest address space the handlers operate on.
  // sched: fiber scheduler used for real blocking waits. All fibers
  // must live on the thread that owns this object (no locking).
  XboxKrnl(xbe::XbeRam& ram, GuestScheduler& sched);
  ~XboxKrnl();

  XboxKrnl(const XboxKrnl&) = delete;
  XboxKrnl& operator=(const XboxKrnl&) = delete;

  // DbgPrint output. Without a sink, lines accumulate in dbg_lines().
  void set_dbg_sink(std::function<void(const std::string&)> sink);
  const std::vector<std::string>& dbg_lines() const { return dbg_lines_; }

  // Resolve every kernel thunk slot of `image` and patch the slots we
  // can serve: implemented function exports get kHleMarker|ordinal,
  // implemented data exports get the guest VA of their kernel data
  // area. Everything else keeps its raw entry (recorded honestly).
  // Also sizes the contiguous-memory region from the image extent.
  std::vector<ThunkSlot> install_thunks(xbe::LoadedXbe& image);

  // Dispatch one kernel call. `args` are positional (prototype order);
  // for FASTCALL exports args[0]/args[1] are ECX/EDX. `ok` (optional)
  // reports unknown/unimplemented exports and arity violations.
  // Returns the low dword of the result (EAX). Exports with a 64-bit
  // return type (returns_64() below) write the high dword through
  // `edx` when that pointer is non-null; other exports never touch it.
  uint32_t call(uint32_t ordinal, const uint32_t* args, uint32_t nargs,
                bool* ok = nullptr, uint32_t* edx = nullptr);

  // True for exports whose documented return type is 64-bit and hence
  // comes back in the EDX:EAX register pair on the MS x86 ABI:
  // KeQueryPerformanceCounter / KeQueryPerformanceFrequency (both
  // ULONGLONG per nxdk xboxkrnl.h).
  bool returns_64(uint32_t ordinal) const;

  // Push scheduler-driven DATA exports into the guest data window
  // (KeTickCount). Called automatically before every dispatch.
  void refresh_data();

  // Memory-visibility observer (used by the Unicorn CPU core): fired
  // whenever a kernel handler changes the guest mapping state.
  // freed=false: MmAllocateContiguousMemory* committed pages (ro=false
  // for writable allocations, ro=true for PAGE_READONLY ones) - the CPU
  // core maps the range into its emulated address space. freed=true:
  // MmFreeContiguousMemory uncommitted the range - the CPU core must
  // unmap it (guest access afterwards faults honestly, and a later
  // re-allocation of the VA re-maps it). Fired OUTSIDE guest execution
  // on the scheduler thread; must not call back into the kernel.
  void set_mem_observer(
      std::function<void(bool freed, uint32_t va, uint32_t size, bool ro)>
          obs) {
    mem_obs_ = std::move(obs);
  }

  // Guest-thread runner hook (PsCreateSystemThreadEx). The kernel
  // spawns one scheduler fiber per created thread; once the thread is
  // resumed (suspend count drained), that fiber's body calls this hook
  // with the thread handle. The optional CPU core installs a runner
  // that boots a per-fiber GuestCpu and executes the thread's start
  // routine as REAL guest code. Without a runner the thread records an
  // honest "no CPU core" state and terminates. Returning from the hook
  // terminates the thread (as if it had returned from its routine).
  void set_thread_runner(std::function<void(uint32_t thread_handle)> runner) {
    thread_runner_ = std::move(runner);
  }

  // Start parameters of a created thread (for the runner above).
  // PKSTART_ROUTINE/PKSYSTEM_ROUTINE are nxdk xboxkrnl.h types: the
  // NT bootstrap calls system_routine(start_routine, start_context)
  // when a system routine is given, else start_routine(start_context).
  struct ThreadStartInfo {
    uint32_t start_routine = 0;
    uint32_t start_context = 0;
    uint32_t system_routine = 0;  // 0 = none
    uint32_t kernel_stack_size = 0;  // RECORDED only (fibers use the
                                     // scheduler's standard stack)
    uint32_t tls_data_size = 0;      // reserved on the fiber stack top
  };
  bool thread_start_info(uint32_t handle, ThreadStartInfo* out) const;

  // --- object table introspection (tests) ---
  // Alive handle-table objects (events + mutants + semaphores + threads).
  size_t object_count() const;
  // Event-only views kept for the event tests.
  size_t event_count() const;
  bool event_signaled_by_va(uint32_t va) const;
  bool event_signaled_by_handle(uint32_t handle) const;
  bool thread_running_by_handle(uint32_t handle) const;

  // --- kernel data window (guest-mapped, real addresses) ---
  // [kernel_data_va(), kernel_data_va() + 0x1000) inside `ram`:
  //   +0x00 XBOX_KRNL_VERSION (4 x USHORT, zero-initialized)
  //   +0x08 KeTickCount (volatile DWORD)
  //   +0x10 XeImageFileName ANSI_STRING (Length/Max/Buffer*)
  //   +0x20 ... string buffer
  //   +0x120 LaunchDataPage (PLAUNCH_DATA_PAGE, NULL)
  uint32_t kernel_data_va() const { return kernel_data_va_; }
  uint32_t ke_tick_count_va() const { return kernel_data_va_ + 0x08; }
  uint32_t xe_image_file_name_va() const { return kernel_data_va_ + 0x10; }
  uint32_t launch_data_page_va() const { return kernel_data_va_ + 0x120; }

  // Contiguous memory region bounds used by MmAllocateContiguousMemory*:
  // [image_end page-rounded, kernel data window). Read-only for tests.
  uint32_t mm_region_lo() const { return mm_lo_; }
  uint32_t mm_region_hi() const { return mm_hi_; }

 private:
  // --- dispatcher objects (events / mutants / semaphores / threads) ---
  // One struct for every waitable kernel object, the way the NT object
  // manager treats all of them as dispatcher headers plus payload.
  // Layout notes (nxdk xboxkrnl.h): DISPATCHER_HEADER {UCHAR Type,
  // Absolute, Size, Inserted; LONG SignalState; LIST_ENTRY
  // WaitListHead}; KSEMAPHORE = header + LONG Limit; KMUTANT = header
  // + LIST_ENTRY MutantListEntry + PRKTHREAD OwnerThread + BOOLEAN
  // Abandoned.
  struct DispatcherObject {
    enum class Kind : uint8_t { Event, Mutant, Semaphore, Thread };
    Kind kind = Kind::Event;
    uint32_t guest_va = 0;  // 0 = kernel-pool object (NtCreate* flavor)
    uint64_t token = 0;     // scheduler wait channel (object identity)
    // Tokens of fibers waiting on this object as part of an
    // NtWaitForMultipleObjectsEx set (single waits wait on `token`).
    std::vector<uint64_t> mw_waiters;
    struct {
      uint32_t type = 0;       // kEventNotify / kEventSync
      bool signaled = false;
    } event;
    struct {
      int32_t signal_state = 1;  // 1 = free, 0 = owned once, <0 recursion
      uint32_t owner = 0;        // PKTHREAD token (thread id), 0 = unowned
      bool abandoned = false;
    } mutant;
    struct {
      int32_t count = 0;
      int32_t limit = 0;
    } semaphore;
    struct {
      uint32_t start_routine = 0;   // PKSTART_ROUTINE
      uint32_t start_context = 0;   // PVOID StartContext
      uint32_t system_routine = 0;  // PKSYSTEM_ROUTINE (0 = none)
      uint32_t kernel_stack_size = 0;
      uint32_t tls_data_size = 0;
      uint32_t sched_handle = 0;    // GuestScheduler fiber handle
      uint32_t object_handle = 0;   // this object's own handle-table slot
      uint32_t thread_id = 0;       // opaque PKTHREAD token (odd)
      uint32_t suspend_count = 0;
      int32_t base_priority = 0;    // recorded; no scheduling effect
      bool running = false;         // a TERMINATED thread is "signaled"
      bool runnable_missing_core = false;
    } thread;
  };

  // Handle table: alive slots hold unique_ptr, freed slots are null and
  // recycled through free_slots_ (handles stay 4*(slot+1) and are never
  // reused while the slot is alive; NtClose frees the slot).
  uint32_t insert_object(std::unique_ptr<DispatcherObject> o);
  DispatcherObject* find_object_by_handle(uint32_t handle);
  // VA registry: KeInitialize{Event,Mutant,Semaphore} objects.
  DispatcherObject* find_object_by_va(uint32_t va);
  // Per-fiber thread identity (KeGetCurrentThread): implicit thread
  // objects keyed by scheduler handle, created on first use. Mutable:
  // the identity cache fills lazily from const wait paths.
  DispatcherObject* ensure_thread_object(uint64_t sched_handle) const;
  DispatcherObject* find_thread_by_id(uint32_t thread_id) const;
  // System threads (PsCreateSystemThreadEx) by their scheduler fiber.
  DispatcherObject* find_thread_by_fiber(uint32_t sched_handle) const;
  // Opaque PKTHREAD of the CALLING fiber (0 outside any fiber).
  uint32_t current_thread_token() const;

  // Guest-visible dispatcher header mirroring ( SignalState, wait list,
  // and the kind-specific tail: Limit for semaphores, owner/list/
  // abandoned for mutants). Pool objects (guest_va == 0) are skipped.
  void write_dispatcher_header(const DispatcherObject& o);
  void write_signal_state(const DispatcherObject& o);
  // NT event set/reset shared by Ke* and Nt* flavors. Wake policy
  // follows the event type: sync releases exactly one waiter and leaves
  // the event reset; notification releases all and stays signaled.
  uint32_t ke_set_event(DispatcherObject* e);  // returns previous state
  uint32_t ke_reset_event(DispatcherObject* e);

  // Wait core, shared by Ke/Nt single waits and the multiple-wait path.
  // Satisfied checks have no side effects; acquire consumes (sync event
  // signal, mutant ownership, semaphore count). Acquire of an abandoned
  // mutant reports kStatusAbandoned exactly once and clears the flag.
  bool dispatcher_satisfied(const DispatcherObject* o) const;
  uint32_t dispatcher_acquire(DispatcherObject* o);
  uint32_t wait_dispatch(DispatcherObject* o, bool immediate,
                         uint64_t timeout_ticks);
  uint32_t wait_multiple(std::vector<DispatcherObject*>& objs, bool wait_all,
                         bool immediate, uint64_t timeout_ticks);
  // Reads an optional PLARGE_INTEGER timeout argument (0 = NULL = wait
  // forever) and converts it with interval_to_ticks.
  bool read_timeout(uint32_t ptr, bool* immediate, uint64_t* ticks) const;

  // Wake single waiters (one per release unit) and every registered
  // multi-wait waiter (they re-evaluate and re-register if still
  // unsatisfied); clears the mw list.
  void wake_object(DispatcherObject* o, uint32_t units);

  // Thread termination shared by PsTerminateSystemThread and the
  // natural end of the fiber body: marks the object not-running (its
  // "signaled" state), wakes waiters, abandons mutants owned by it.
  void finish_thread_object(DispatcherObject* o);

  // Contiguous allocator (first-fit over [mm_lo_, mm_hi_)).
  uint32_t mm_alloc(uint32_t size, uint32_t lowest, uint32_t highest,
                    uint32_t alignment, uint32_t protect);
  bool mm_free(uint32_t va);
  uint32_t mm_size(uint32_t va) const;

  // Critical sections: state lives IN the guest RTL_CRITICAL_SECTION
  // (28 bytes, xboxdef.h layout: embedded dispatcher event + LockCount,
  // RecursionCount, OwningThread) and is read/written through `ram_`,
  // exactly what a CPU core would see. LockCount models the CURRENT
  // waiter count (documented deviation from NT's -1-based encoding,
  // which is an internal optimization detail).
  uint32_t cs_init(uint32_t va);
  uint32_t cs_enter(uint32_t va);
  uint32_t cs_leave(uint32_t va);
  uint32_t cs_try_enter(uint32_t va);  // returns BOOLEAN (1/0)

  // Guest helpers.
  bool write64(uint32_t va, uint64_t value);
  bool read64(uint32_t va, uint64_t* out) const;
  std::string read_cstr(uint32_t va, uint32_t max_len = 4096) const;
  bool in_fiber() const { return sched_.current_handle() != 0; }

  struct MemAlloc {
    uint32_t va;
    uint32_t size;
    uint32_t protect;
  };

  xbe::XbeRam& ram_;
  GuestScheduler& sched_;

  std::function<void(const std::string&)> dbg_sink_;
  std::vector<std::string> dbg_lines_;
  std::function<void(bool, uint32_t, uint32_t, bool)> mem_obs_;
  std::function<void(uint32_t)> thread_runner_;

  std::vector<ThunkSlot> slots_;
  // Waitable-object registries (all unique_ptr-owned; a repeated
  // install_thunks() frees everything, no leaks).
  std::vector<std::unique_ptr<DispatcherObject>> objects_;   // handle table
  std::vector<size_t> free_slots_;                           // recycled slots
  std::vector<std::unique_ptr<DispatcherObject>> va_objects_;
  mutable std::map<uint64_t, std::unique_ptr<DispatcherObject>> fiber_threads_;
  mutable uint32_t next_thread_id_ = 1;  // odd ids: PKTHREAD tokens (0 = NULL)

  uint32_t kernel_data_va_ = 0;
  uint32_t mm_lo_ = 0;
  uint32_t mm_hi_ = 0;
  std::vector<MemAlloc> mem_allocs_;
  uint32_t mm_scan_ = 0;  // first-fit cursor

  // Wait-channel namespaces (token = (id << 1) | tag):
  //   events        -> (uintptr_t of Event) << 1 | 1
  //   critical secs -> (uint64_t cs guest VA) << 1 | 0
  //   KeDelayExecutionThread -> fixed reserved token, never signaled
  static constexpr uint64_t kDelayToken = 0x51AB000000000000ULL;
};

}  // namespace krnl
