#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cpu_sched.h"

// Fiber-aware guest TLS, implementing the Xbox TLS model on top of the
// ucontext fiber scheduler.
//
// On the original Xbox (an NT-kernel derivative), __declspec(thread)
// storage is reached through a per-thread indirection that we verified
// by DISASSEMBLING a real homebrew XBE (LithiumX, nxdk-built):
//
//     mov  eax, [IndexVA]        ; 1. absolute load of the TLS index,
//                                ;    written there by the loader/kernel
//     mov  ecx, fs:[0x4]         ; 2. per-THREAD pointer to the TLS
//                                ;    array of per-module data blocks
//     mov  eax, [ecx+eax*4]      ; 3. block = array[index]
//     ... [eax+offset]           ; 4. the variable itself
//
// Cxbx-Reloaded (EmuFS.cpp, mirroring Xapi's thread startup) shows the
// per-thread initialization: one allocation per thread, carved out of
// the thread stack, containing the array followed by a copy of the XBE
// TLS template plus its zero-fill:
//
//     [array ...][template copy (DataEnd-DataStart bytes)][zero fill]
//
// This module reproduces that model, with the FIBER taking the role of
// the thread (our guest threads ARE ucontext fibers on one host thread,
// which is exactly why C++ thread_local cannot provide this):
//   - every fiber gets its OWN TLS allocation, carved from the top of
//     its own stack (spawn_reserved), so it lives and dies with the
//     fiber and never collides with other fibers on the same host
//     thread;
//   - the allocation holds a 64-entry array (NT TLS_MINIMUM_AVAILABLE;
//     real titles only ever use index 0 because the XBE is the only
//     module that can own TLS) at the start, followed by the template
//     copy + zero fill. array[index] points at the data area;
//   - the TLS index is assigned by the runtime and written into the
//     mapped image at tls.index_addr (loaded xbe_loader::write_tls_index),
//     matching step 1 above;
//   - TLS callback events are RECORDED, never executed by this module:
//     callbacks are guest machine code. Dispatching them is the job of
//     the OPTIONAL Unicorn CPU core (xbox_cpu.h); without it the record
//     (fiber, callback VA, reason) is what remains - an honest,
//     inspectable trace. Reason codes follow the NT DLL
//     notification model that XAPI inherits: 1 = process attach (main
//     fiber), 2 = thread attach, 3 = thread detach.
//
// All fibers live on one host thread (cooperative scheduler), so no
// locking is needed and none is used -- do not share a FiberTls across
// host threads.

class FiberTls {
 public:
  // One recorded TLS notification. Callback VAs are GUEST addresses
  // inside the mapped image (0 when the XBE has no callback table --
  // most titles don't; the attach/detach notifications still happen).
  struct Event {
    uint64_t fiber = 0;   // scheduler handle
    uint32_t callback = 0;  // guest VA of the callback, 0 = none
    uint32_t reason = 0;    // 1 process attach, 2 thread attach, 3 detach
  };

  // The per-fiber initialization payload, built from the loaded XBE by
  // the caller (LoadedXbe::tls_template() + tls index + callbacks).
  struct Init {
    std::vector<uint8_t> raw;   // template copy (already includes BSS)
    uint32_t zero_fill = 0;     // extra zeroed bytes after raw
    uint32_t index = 0;         // TLS index assigned by the runtime
    std::vector<uint32_t> callbacks;  // guest VAs, in table order
    bool empty() const { return raw.empty() && zero_fill == 0; }
  };

  // Notification reason codes (NT model, used by XAPI on Xbox).
  static constexpr uint32_t kReasonProcessAttach = 1;
  static constexpr uint32_t kReasonThreadAttach = 2;
  static constexpr uint32_t kReasonThreadDetach = 3;

  // NT TLS_MINIMUM_AVAILABLE: slots per fiber. The XBE module owns
  // index 0; Xbox cannot load additional TLS-owning modules.
  static constexpr size_t kSlots = 64;

  explicit FiberTls(GuestScheduler& sched);

  // Provide the per-fiber initialization payload (from the loaded XBE).
  // Must be called before the first spawn_with_tls. When never called,
  // fibers have no guest TLS (accessors return nullptr). The TLS index
  // must be < kSlots (real titles only ever use 0); invalid inits are
  // rejected.
  void set_init(Init init);

  // Spawn a guest thread WITH guest TLS storage carved from its own
  // stack top. The first fiber spawned is the "main" fiber and receives
  // a process-attach record, later fibers a thread-attach record (the
  // main fiber is created by the Xbox loader itself, not by
  // CreateThread, and NT notifies it with PROCESS_ATTACH only).
  uint64_t spawn_with_tls(const std::string& name, GuestThreadFn fn);

  // Guest TLS data area of the CURRENT fiber (template copy + zero
  // fill; step 3+4 of the access sequence). nullptr outside fibers or
  // when no TLS init is present. `offset`/`size` bounds-check the read
  // against the data area size.
  void* current_data(size_t offset = 0, size_t size = 0);
  const void* current_data(size_t offset = 0, size_t size = 0) const;

  // Guest TLS data area of a specific fiber (host-side inspection).
  void* data_of(uint64_t fiber, size_t offset = 0, size_t size = 0);
  const void* data_of(uint64_t fiber, size_t offset = 0, size_t size = 0) const;

  // The per-fiber array pointer -- what guest code would obtain from
  // its thread pointer before array[index] (step 2 above).
  void* current_array();
  void* array_of(uint64_t fiber);

  // Data area size per fiber: raw template + zero fill.
  size_t data_size() const { return init_ ? init_->raw.size() + init_->zero_fill : 0; }

  // Recorded notifications, in firing order. Attach records are
  // appended at spawn (that is when the real kernel's thread startup
  // runs TLS init); detach records come from the scheduler finish hook
  // while the fiber is still current.
  const std::vector<Event>& events() const { return events_; }

  // true when `fiber` was spawned with TLS storage.
  bool has_tls(uint64_t fiber) const;

 private:
  struct Block {
    void* array = nullptr;   // start of the fiber's allocation
    void* data = nullptr;    // array + kSlots * sizeof(void*)
    size_t size = 0;         // data area size (raw + zero fill)
  };

  // Placement: the allocation is carved from the fiber stack top by
  // the scheduler; this writes the array + template copy into it.
  bool install(uint64_t fiber);
  void record(uint64_t fiber, uint32_t callback, uint32_t reason);

  GuestScheduler& sched_;
  std::unique_ptr<Init> init_;
  std::vector<Event> events_;
  std::vector<std::pair<uint64_t, Block>> blocks_;  // small, linear scan
  bool main_seen_ = false;
  bool hook_installed_ = false;
};
