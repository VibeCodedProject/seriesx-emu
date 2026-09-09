#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <unicorn/unicorn.h>  // optional dependency (see below)

#include "xbox_krnl.h"

// Optional Unicorn-based x86-32 CPU core for guest execution.
//
// This module ACTUALLY EXECUTES guest machine code from a mapped XBE
// (or from synthetic test images) with the Unicorn engine
// (github.com/unicorn-engine/unicorn, GPL-2.0, QEMU/TCG-derived - an
// OPTIONAL build-time dependency: without it the rest of the project
// still builds and every other test still runs; the tests here are
// simply not built). Everything below was verified empirically against
// Unicorn 2.1.4 in scripts/uc_smoke.py before a line was written:
//
//   1. FS_BASE register writes are a NO-OP in 32-bit mode -> the FS
//      segment (the Xbox TEB/TLS access path, fs:[0x4] per the
//      disassembled nxdk model) is provided through a REAL GDT built in
//      guest-visible memory, one descriptor per CPU with its base set
//      to that CPU's TEB. GDTR must be programmed BEFORE the selectors
//      (selector loads cache the descriptor). Only ring 0 selectors
//      load (UC CPL is 0); the real Xbox runs titles at ring 3, which
//      does not change any user-mode instruction semantics we execute.
//   2. EIP writes inside a code hook do NOT redirect execution ->
//      kernel thunks use a virtualized return instead: the code hook at
//      kHleMarker|ordinal only records the call and stops emulation;
//      the host then runs XboxKrnl::call() OUTSIDE emu_start (this
//      also makes uc_mem_map_ptr for fresh Mm allocations legal -
//      never call map APIs while the CPU runs), performs the return
//      VIRTUALLY (EIP = popped return address, ESP += 4 + callee-popped
//      arg bytes, per the reverse-engineered convention/arg-size table)
//      and resumes emulation at the caller. The marker page itself is
//      a safety net of 1-byte RETs that never actually execute. No EIP
//      or ESP write ever happens inside a hook.
//   3. Memory is shared ZERO-COPY: XbeRam's committed pages are mapped
//      into the emulated address space with uc_mem_map_ptr over the
//      SAME host pages the kernel HLE handlers write. No copies, no
//      coherency problem. Committed-and-readonly pages are mapped
//      READ|EXEC, writable ones READ|WRITE|EXEC (the Pentium III has
//      no NX bit - all guest RAM is executable on real hardware).
//   4. One uc instance per guest fiber. Fibers run cooperatively on
//      one host thread, so the instances are used non-concurrently;
//      blocking kernel calls (KeDelayExecutionThread, event waits,
//      critical-section contention) simply swapcontext away from
//      INSIDE XboxKrnl::call(), which is now outside emu_start - each
//      fiber's dormant run() frame keeps its own uc untouched.
//
// What is REAL here: every guest instruction is decoded and executed
// by Unicorn's TCG; guest faults (null deref, write to read-only,
// jump into an unimplemented import) are Unicorn events turned into
// honest fault records; kernel calls from guest code go through the
// SAME XboxKrnl handlers the HLE tests cover.
//
// Honest limitations (do not remove):
//   - ~30 of 371 kernel exports are implemented; a title calling an
//     unimplemented import stops with a fault naming the ordinal.
//     This is the designed failure mode, never silently skipped.
//   - Guest writes to CODE pages go through Unicorn's TB cache only
//     when the guest itself writes through the emulated CPU. The
//     kernel HLE handlers write guest memory directly through the
//     shared host pages, which BYPASSES TB invalidation; this is safe
//     for data (all our handlers) but self-modifying code patched by
//     a handler would run stale translations.
//   - TLS CALLBACKS are still recorded, never executed (they are guest
//     code; dispatching them is future work). The TEB itself (self
//     pointer at +0x18, TLS array pointer at +0x4, per-fiber template
//     copy) is real and lives in guest RAM, so guest TLS code runs.
//   - Ring 0 only (UC CPL); no interrupts, no FPU exception reporting
//     (x87/SSE instructions do execute via TCG).
//   - The virtual IP 0xF00F0000 region (kHleMarker stubs, GDT page,
//     kExitVa) exists only inside each GuestCpu; it is not guest RAM.

namespace ucore {

using krnl::kHleMarker;

class GuestCpu;  // fwd: UnicornRuntime tracks attached CPUs

// Sentinel: guest code jumps here to exit cleanly (emu_start's `until`).
constexpr uint32_t kExitVa = kHleMarker | 0x0FFFu;

// One recorded reason a guest CPU stopped. `kind` is None while/after a
// clean exit.
struct GuestFault {
  enum class Kind : uint8_t {
    None = 0,
    UnmappedRead,
    UnmappedWrite,
    UnmappedFetch,
    WriteProt,             // write to a read-only mapped page
    ReadProt,
    UnimplementedImport,   // call through an unpatched kernel thunk
    InstructionLimit,      // did not exit within the requested budget
    EmuError,              // any other Unicorn error
  };
  Kind kind = Kind::None;
  uint32_t va = 0;       // faulting guest address (EIP for fetch kinds)
  uint32_t eip = 0;
  uint32_t ordinal = 0;  // UnimplementedImport / import-shaped fetches
  std::string detail;    // human-readable, test-asserted
  const char* kind_name() const;
};

// Cross-CPU runtime state: the top-down allocator for guest stacks and
// per-fiber TEBs (below the kernel data page, above the Mm arena's
// realistic reach - see honest note in the .cpp), the TLS template from
// the loaded image, and the mirror-registry that maps committed guest
// ranges into EVERY attached CPU (kernel Mm allocations happen on one
// fiber but must be visible to all).
class UnicornRuntime {
 public:
  UnicornRuntime(xbe::XbeRam& ram, krnl::XboxKrnl& krnl);
  ~UnicornRuntime();

  UnicornRuntime(const UnicornRuntime&) = delete;
  UnicornRuntime& operator=(const UnicornRuntime&) = delete;

  // Capture the TLS template from a loaded image and write the runtime
  // TLS index (0) into it, exactly what the fiber runtime did before.
  // Optional: synthetic guests without TLS never call this.
  bool set_image(xbe::LoadedXbe& image, std::string* error = nullptr);
  bool has_tls_template() const { return !tls_raw_.empty() || tls_zerofill_ != 0; }

  // Top-down page allocator (page-rounded, committed RW, zeroed).
  // Returns base VA or 0 when the below-kernel region is exhausted.
  uint32_t alloc_pages(uint32_t bytes);

  // Guest stack: allocates `bytes` (page-rounded) top-down; returns the
  // TOP-aligned VA to start ESP at (0 on failure).
  uint32_t alloc_stack(uint32_t bytes, uint32_t* base_out = nullptr);

  // Per-fiber TEB, Xapi model, ONE allocation in guest RAM:
  //   teb+0x000 TEB header (Self at +0x18 = teb VA, TLS array ptr at
  //             +0x04 = teb+0x100 - the fs:[4] of the disassembled
  //             access sequence)
  //   teb+0x100 64-slot TLS array (slot 0 -> teb+0x200)
  //   teb+0x200 TLS template copy + zero fill
  // Returns the TEB VA (the FS base for a GuestCpu) or 0.
  uint32_t build_teb();

  void register_cpu(GuestCpu* cpu);
  void unregister_cpu(GuestCpu* cpu);

  // Map a freshly committed guest range into every attached CPU.
  void map_on_all(uint32_t va, uint32_t size, bool ro);

  // Unmap a freed guest range from every attached CPU (MmFree
  // ContiguousMemory): guest access to the range afterwards faults
  // honestly; a re-allocation re-maps through map_on_all.
  void unmap_on_all(uint32_t va, uint32_t size);

  xbe::XbeRam& ram;
  krnl::XboxKrnl& krnl;

 private:
  friend class GuestCpu;
  // Body of a PsCreateSystemThreadEx thread: boots a per-fiber
  // GuestCpu and runs the thread's start routine as real guest code
  // (NT bootstrap convention; see the .cpp). Installed as the kernel's
  // thread runner in the constructor.
  void run_system_thread(uint32_t thread_handle);

  std::vector<uint8_t> tls_raw_;
  uint32_t tls_zerofill_ = 0;
  uint32_t tls_index_written_ = 0;
  uint32_t top_ = 0;               // next top-down allocation cursor
  std::vector<GuestCpu*> cpus_;    // attached CPUs (single host thread)
};

// One emulated x86-32 CPU, owned by one guest fiber. Lifecycle:
//   GuestCpu cpu(runtime);
//   cpu.set_teb(runtime.build_teb());   // optional (FS base)
//   cpu.attach(&error);
//   cpu.run(entry_eip, esp, max_instructions, &error);
class GuestCpu {
 public:
  explicit GuestCpu(UnicornRuntime& rt);
  ~GuestCpu();

  GuestCpu(const GuestCpu&) = delete;
  GuestCpu& operator=(const GuestCpu&) = delete;

  // FS base (TEB VA) - must be set BEFORE attach(). 0 leaves the FS
  // descriptor with base 0 (guest touching fs: then faults honestly).
  void set_teb(uint32_t va) { teb_va_ = va; }

  // Create the uc instance, zero-copy map all committed XbeRam ranges
  // (mirroring read-only protection), map the HLE stub + GDT pages,
  // build the GDT, install `ret imm` stubs for every implemented
  // kernel export, register the hooks.
  bool attach(std::string* error = nullptr);
  bool attached() const { return uc_ != nullptr; }

  // Run guest code until it jumps to kExitVa, faults, calls an
  // unimplemented import, or exhausts `max_instructions`. Blocking
  // kernel calls swap the calling fiber away inside this call, exactly
  // like the HLE-only path did.
  bool run(uint32_t entry_eip, uint32_t esp, uint64_t max_instructions,
           std::string* error = nullptr);

  // --- observability (tests / demo) ---
  const GuestFault& fault() const { return fault_; }
  uint64_t instructions() const { return instructions_; }
  uint32_t reg(uint32_t unicorn_reg_id) const;  // e.g. UC_X86_REG_EAX
  // Kernel imports this CPU actually served (in call order).
  const std::vector<uint32_t>& served_ordinals() const { return served_; }

 private:
  friend class UnicornRuntime;
  static void s_code_hook(uc_engine* uc, uint64_t addr, uint32_t size,
                          void* user);
  static bool s_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                         int size, int64_t value, void* user);
  void code_hook(uint64_t addr);
  bool mem_hook(uc_mem_type type, uint64_t addr);
  void handle_hle_call(uint32_t ordinal);
  bool map_range(uint32_t va, uint32_t size, bool ro, std::string* error);
  bool unmap_range(uint32_t va, uint32_t size);
  void build_gdt();
  void record_fault(GuestFault::Kind kind, uint32_t va, const std::string& why);

  UnicornRuntime& rt_;
  krnl::XboxKrnl& krnl_;
  xbe::XbeRam& ram_;
  uc_engine* uc_ = nullptr;
  uint32_t teb_va_ = 0;

  std::vector<uint8_t> stub_buf_;   // kHleMarker page image (4 KiB)
  std::vector<uint8_t> gdt_buf_;    // GDT page image (4 KiB)
  struct Range { uint32_t va; uint32_t size; };
  std::vector<Range> mapped_;       // ranges mapped into this instance

  // HLE stop/restart state machine.
  uint32_t pending_ordinal_ = 0;
  uint32_t pending_addr_ = 0;
  uint32_t pending_ret_ = 0;   // popped return address
  uint32_t pending_esp_ = 0;   // ESP after the virtual return

  GuestFault fault_{};
  uint64_t instructions_ = 0;
  std::vector<uint32_t> served_;
};

}  // namespace ucore
