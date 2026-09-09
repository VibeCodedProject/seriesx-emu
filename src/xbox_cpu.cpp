#include "xbox_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "cpu_sched.h"
#include "xbe_loader.h"

namespace ucore {

namespace {

constexpr uint32_t kPage = 0x1000u;
constexpr uint32_t kGdtVa = kHleMarker + kPage;  // per-CPU GDT page image

// GDT selectors (ring 0; see module header for the CPL caveat).
constexpr uint32_t kSelCs = 0x08;
constexpr uint32_t kSelDs = 0x10;
constexpr uint32_t kSelFs = 0x18;

uint32_t page_round(uint32_t v) { return (v + kPage - 1) & ~(kPage - 1u); }

// x86 GDT entry (base/limit/access/flags - the canonical encoding).
void put_desc(uint8_t* dst, uint32_t base, uint32_t limit, uint8_t access,
              uint8_t flags) {
  dst[0] = limit & 0xFF;
  dst[1] = (limit >> 8) & 0xFF;
  dst[2] = base & 0xFF;
  dst[3] = (base >> 8) & 0xFF;
  dst[4] = (base >> 16) & 0xFF;
  dst[5] = access;
  dst[6] = ((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F);
  dst[7] = (base >> 24) & 0xFF;
}

}  // namespace

const char* GuestFault::kind_name() const {
  switch (kind) {
    case Kind::None: return "none";
    case Kind::UnmappedRead: return "unmapped-read";
    case Kind::UnmappedWrite: return "unmapped-write";
    case Kind::UnmappedFetch: return "unmapped-fetch";
    case Kind::WriteProt: return "write-to-readonly";
    case Kind::ReadProt: return "read-protected";
    case Kind::UnimplementedImport: return "unimplemented-import";
    case Kind::InstructionLimit: return "instruction-limit";
    case Kind::EmuError: return "emu-error";
  }
  return "?";
}

// ---------------------------------------------------------------- runtime --

UnicornRuntime::UnicornRuntime(xbe::XbeRam& ram_in, krnl::XboxKrnl& krnl_in)
    : ram(ram_in), krnl(krnl_in) {
  // Top-down allocations start right below the kernel data page. A
  // kernel that never had thunks installed (synthetic tests) reports
  // VA 0 - fall back to the last RAM page, which is where install_thunks
  // would have placed it.
  const uint32_t kd = krnl.kernel_data_va();
  top_ = kd != 0 ? kd : static_cast<uint32_t>(ram.size()) - kPage;
  // Mirror every kernel mapping change into all attached CPUs. Fired
  // outside guest execution (see xbox_krnl.h), so uc_mem_map_ptr /
  // uc_mem_unmap here are always legal: a commit maps the range, a
  // free unmaps it (guest access afterwards faults honestly; a later
  // re-allocation of the same VA re-maps).
  krnl.set_mem_observer([this](bool freed, uint32_t va, uint32_t size,
                               bool ro) {
    if (freed)
      unmap_on_all(va, size);
    else
      map_on_all(va, size, ro);
  });
  // Boot guest threads created by PsCreateSystemThreadEx on REAL guest
  // code: the kernel spawns the fiber, this runner boots a per-fiber
  // GuestCpu and calls the thread's start routine through the NT
  // bootstrap convention (system_routine(start, ctx) when given, else
  // start(ctx)), with kExitVa as the return-address sentinel.
  krnl.set_thread_runner([this](uint32_t thread_handle) {
    run_system_thread(thread_handle);
  });
}

// Runs a PsCreateSystemThreadEx thread body as real guest code, inside
// the fiber the kernel spawned. PsTerminateSystemThread (from guest)
// never returns here: it ends the fiber mid-run; the runtime destructor
// closes this fiber's uc engine because the GuestCpu destructor cannot
// run through swapcontext.
void UnicornRuntime::run_system_thread(uint32_t thread_handle) {
  krnl::XboxKrnl::ThreadStartInfo info;
  if (!krnl.thread_start_info(thread_handle, &info)) return;
  if (info.start_routine == 0) return;  // nothing to run (recorded)
  GuestCpu cpu(*this);
  cpu.set_teb(build_teb());
  if (!cpu.attach()) return;
  // Honor the requested kernel stack size when plausible; otherwise a
  // 64 KiB default (the scheduler enforces its own guard pages).
  const uint32_t stack_bytes =
      info.kernel_stack_size >= 0x2000 ? info.kernel_stack_size : 0x10000;
  uint32_t esp = alloc_stack(stack_bytes);
  if (esp == 0) return;
  // Push arguments per the nxdk PKSTART_ROUTINE / PKSYSTEM_ROUTINE
  // prototypes (stdcall): [esp] = return address (the kExitVa sentinel
  // - a clean guest exit when the routine returns), then right-to-left
  // arguments: system(StartRoutine, StartContext) or start(StartContext).
  const auto push = [&esp, this](uint32_t v) {
    esp -= 4;
    ram.write32(esp, v);
  };
  const uint32_t entry =
      info.system_routine != 0 ? info.system_routine : info.start_routine;
  push(kExitVa);
  if (info.system_routine != 0) {
    push(info.start_context);
    push(info.start_routine);
  } else {
    push(info.start_context);
  }
  std::string err;
  // Large budget: a system thread runs until it terminates itself or
  // faults; the ctest 60s timeout is the real watchdog.
  cpu.run(entry, esp, 100000000ull, &err);
}

UnicornRuntime::~UnicornRuntime() {
  krnl.set_mem_observer({});
  krnl.set_thread_runner({});
  // Fibers ended by PsTerminateSystemThread die mid-run, so their
  // GuestCpu destructors never execute; close the engines here (the
  // scheduler frees the fiber stacks only at ITS destruction, which
  // must come after this runtime's - see the member order in tests).
  for (GuestCpu* c : cpus_) {
    if (c->uc_) {
      uc_close(c->uc_);
      c->uc_ = nullptr;
    }
  }
  cpus_.clear();
}

bool UnicornRuntime::set_image(xbe::LoadedXbe& image, std::string* error) {
  if (!image.has_tls()) {
    if (error) *error = "image has no TLS directory";
    return false;
  }
  tls_raw_ = image.tls_template();  // includes zero-fill (already zeroed)
  tls_zerofill_ = image.image().tls().zero_fill;
  // Runtime-assigned TLS index; the XBE is the only TLS-owning module.
  tls_index_written_ = 0;
  image.write_tls_index(tls_index_written_);
  return true;
}

uint32_t UnicornRuntime::alloc_pages(uint32_t bytes) {
  if (bytes == 0) return 0;
  const uint32_t size = page_round(bytes);
  // Keep an 8 MiB safety margin above the Mm arena's low-water first-fit
  // region so a huge title allocation can never silently collide with
  // TEB/stack pages (documented limitation; mm_alloc only knows its own
  // allocations).
  const uint32_t floor = top_ - 0x800000u;
  if (size > top_ || top_ - size < floor) return 0;
  const uint32_t base = top_ - size;
  if (!ram.commit(base, size)) return 0;
  if (uint8_t* p = ram.host_ptr(base)) std::memset(p, 0, size);
  top_ = base;
  map_on_all(base, size, false);
  return base;
}

uint32_t UnicornRuntime::alloc_stack(uint32_t bytes, uint32_t* base_out) {
  const uint32_t base = alloc_pages(bytes);
  if (base_out) *base_out = base;
  if (base == 0) return 0;
  return base + page_round(bytes);  // top-aligned for ESP
}

uint32_t UnicornRuntime::build_teb() {
  const uint32_t data_size = static_cast<uint32_t>(tls_raw_.size());
  const uint32_t total = 0x200 + data_size;  // header + array + data area
  const uint32_t base = alloc_pages(total);
  if (base == 0) return 0;
  uint8_t* p = ram.host_ptr(base);
  if (!p) return 0;
  auto w32 = [&](uint32_t off, uint32_t v) {
    std::memcpy(p + off, &v, 4);
  };
  w32(0x18, base);        // TEB.Self (fs:[0x18])
  w32(0x04, base + 0x100);  // TLS array pointer (fs:[0x4])
  w32(0x100, base + 0x200);  // array[0] -> data area (index 0)
  // slots 1..63 stay NULL; template copy + zero fill already zeroed.
  if (data_size > 0) std::memcpy(p + 0x200, tls_raw_.data(), data_size);
  return base;
}

void UnicornRuntime::register_cpu(GuestCpu* cpu) {
  cpus_.push_back(cpu);
}

void UnicornRuntime::unregister_cpu(GuestCpu* cpu) {
  for (size_t i = 0; i < cpus_.size(); ++i) {
    if (cpus_[i] == cpu) {
      cpus_.erase(cpus_.begin() + i);
      return;
    }
  }
}

void UnicornRuntime::map_on_all(uint32_t va, uint32_t size, bool ro) {
  for (GuestCpu* cpu : cpus_) cpu->map_range(va, size, ro, nullptr);
}

void UnicornRuntime::unmap_on_all(uint32_t va, uint32_t size) {
  for (GuestCpu* cpu : cpus_) cpu->unmap_range(va, size);
}

// -------------------------------------------------------------------- cpu --

GuestCpu::GuestCpu(UnicornRuntime& rt)
    : rt_(rt), krnl_(rt.krnl), ram_(rt.ram) {}

GuestCpu::~GuestCpu() {
  if (uc_) {
    rt_.unregister_cpu(this);
    uc_close(uc_);
  }
}

void GuestCpu::record_fault(GuestFault::Kind kind, uint32_t va,
                            const std::string& why) {
  if (fault_.kind == GuestFault::Kind::None) {
    fault_.kind = kind;
    fault_.va = va;
    fault_.eip = reg(UC_X86_REG_EIP);
    fault_.detail = why;
  }
}

uint32_t GuestCpu::reg(uint32_t unicorn_reg_id) const {
  uint32_t v = 0;
  if (uc_) uc_reg_read(uc_, static_cast<int>(unicorn_reg_id), &v);
  return v;
}

bool GuestCpu::map_range(uint32_t va, uint32_t size, bool ro,
                         std::string* error) {
  if (!uc_) return false;
  const uint32_t lo = va & ~(kPage - 1u);
  const uint32_t hi = page_round(va + size);
  const uint32_t len = hi - lo;
  // Idempotent: skip if already (fully) mapped.
  for (const Range& r : mapped_) {
    if (lo >= r.va && hi <= r.va + r.size) return true;
  }
  const uint8_t* host = ram_.host_ptr(lo);
  if (!host) {
    if (error) *error = "map_range: pages not committed";
    return false;
  }
  const uint32_t perms = ro ? (UC_PROT_READ | UC_PROT_EXEC)
                            : (UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC);
  if (uc_mem_map_ptr(uc_, lo, len, perms, const_cast<uint8_t*>(host)) !=
      UC_ERR_OK) {
    if (error) *error = "uc_mem_map_ptr failed";
    return false;
  }
  mapped_.push_back({lo, len});
  return true;
}

bool GuestCpu::unmap_range(uint32_t va, uint32_t size) {
  if (!uc_) return false;
  const uint32_t lo = va & ~(kPage - 1u);
  const uint32_t hi = page_round(va + size);
  const uint32_t len = hi - lo;
  // Same rounding as map_range, so a free of an Mm allocation unmaps
  // exactly what that allocation mapped. Unicorn 2 splits regions on
  // sub-range unmaps (needed when attach() merged this range into a
  // bigger contiguous mapping).
  if (uc_mem_unmap(uc_, lo, len) != UC_ERR_OK) return false;
  // Bookkeeping: drop covered entries, keep trimmed remainders (their
  // pages stay mapped in the uc instance).
  std::vector<Range> kept;
  kept.reserve(mapped_.size() + 2);
  for (const Range& r : mapped_) {
    const uint32_t r_end = r.va + r.size;
    if (r_end <= lo || r.va >= hi) {
      kept.push_back(r);
      continue;
    }
    if (r.va < lo) kept.push_back({r.va, lo - r.va});
    if (r_end > hi) kept.push_back({hi, r_end - hi});
  }
  mapped_ = std::move(kept);
  return true;
}

bool GuestCpu::attach(std::string* error) {
  if (uc_) return true;
  auto fail = [&](const char* what) {
    if (error) *error = what;
    return false;
  };
  if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc_) != UC_ERR_OK)
    return fail("uc_open(x86, 32) failed");

  // 1) Zero-copy map every committed XbeRam range, mirroring the
  //    read-only protection the loader/kernel recorded.
  const uint32_t ram_size = static_cast<uint32_t>(ram_.size());
  for (uint32_t addr = 0; addr < ram_size;) {
    if (!ram_.is_committed(addr)) {
      addr += kPage;
      continue;
    }
    const bool ro = !ram_.writable(addr);
    uint32_t end = addr + kPage;
    while (end < ram_size && ram_.is_committed(end) &&
           ram_.writable(end) == ro)  // same protection class
      end += kPage;
    if (!map_range(addr, end - addr, ro, error)) {
      uc_close(uc_);
      uc_ = nullptr;
      return false;
    }
    addr = end;
  }

  // 2) HLE marker page (safety-net RETs; execution never lands here -
  //    the code hook stops emulation first) and the per-CPU GDT page.
  stub_buf_.assign(kPage, 0xC3);
  gdt_buf_.assign(kPage, 0);
  if (uc_mem_map_ptr(uc_, kHleMarker, kPage, UC_PROT_READ | UC_PROT_EXEC,
                     stub_buf_.data()) != UC_ERR_OK ||
      uc_mem_map_ptr(uc_, kGdtVa, kPage, UC_PROT_READ, gdt_buf_.data()) !=
          UC_ERR_OK) {
    uc_close(uc_);
    uc_ = nullptr;
    return fail("mapping HLE/GDT pages failed");
  }
  mapped_.push_back({kHleMarker, kPage});
  mapped_.push_back({kGdtVa, kPage});

  // 3) GDT + selectors (GDTR FIRST - selector loads cache descriptors).
  build_gdt();
  const uint32_t cs = kSelCs, ds = kSelDs, fs = kSelFs;
  uc_reg_write(uc_, UC_X86_REG_CS, &cs);
  uc_reg_write(uc_, UC_X86_REG_DS, &ds);
  uc_reg_write(uc_, UC_X86_REG_ES, &ds);
  uc_reg_write(uc_, UC_X86_REG_SS, &ds);
  uc_reg_write(uc_, UC_X86_REG_FS, &fs);

  // 4) Hooks: global instruction counter + HLE marker interceptor;
  //    invalid/protected memory -> honest fault records.
  uc_hook h;
  if (uc_hook_add(uc_, &h, UC_HOOK_CODE, reinterpret_cast<void*>(s_code_hook),
                  this, 1, 0) != UC_ERR_OK ||
      uc_hook_add(uc_, &h, UC_HOOK_MEM_INVALID,
                  reinterpret_cast<void*>(s_mem_hook), this, 1, 0) != UC_ERR_OK) {
    uc_close(uc_);
    uc_ = nullptr;
    return fail("uc_hook_add failed");
  }

  rt_.register_cpu(this);
  return true;
}

void GuestCpu::build_gdt() {
  // null | CS flat 4G RX 32-bit | DS flat 4G RW 32-bit | FS = TEB
  put_desc(gdt_buf_.data() + 0x00, 0, 0, 0, 0);
  put_desc(gdt_buf_.data() + 0x08, 0, 0xFFFFF, 0x9A, 0xC);
  put_desc(gdt_buf_.data() + 0x10, 0, 0xFFFFF, 0x92, 0xC);
  put_desc(gdt_buf_.data() + 0x18, teb_va_, 0xFFF, 0x92, 0x4);
  uc_x86_mmr gdtr{};
  gdtr.base = kGdtVa;
  gdtr.limit = 4 * 8 - 1;
  uc_reg_write(uc_, UC_X86_REG_GDTR, &gdtr);
}

void GuestCpu::s_code_hook(uc_engine* uc, uint64_t addr, uint32_t size,
                           void* user) {
  (void)uc;
  (void)size;
  static_cast<GuestCpu*>(user)->code_hook(addr);
}

void GuestCpu::code_hook(uint64_t addr) {
  const uint32_t a = static_cast<uint32_t>(addr);
  if ((a & ~(kPage * 16 - 1u)) == kHleMarker) {  // low 64 KiB = marker region
    // HLE dispatch event, NOT a guest instruction: the CALL jumped to
    // the thunk marker; count it separately from instructions_.
    pending_ordinal_ = a & 0xFFFF;
    pending_addr_ = a;
    uc_emu_stop(uc_);
    return;
  }
  ++instructions_;
  static const bool dbg = std::getenv("XBOX_CPU_TRACE") != nullptr;
  if (dbg) std::fprintf(stderr, "[trace] %llu 0x%08llx\n",
                        (unsigned long long)instructions_, (unsigned long long)addr);
}

bool GuestCpu::s_mem_hook(uc_engine* uc, uc_mem_type type, uint64_t addr,
                          int size, int64_t value, void* user) {
  (void)uc;
  (void)size;
  (void)value;
  return static_cast<GuestCpu*>(user)->mem_hook(type, addr);
}

bool GuestCpu::mem_hook(uc_mem_type type, uint64_t addr) {
  const uint32_t va = static_cast<uint32_t>(addr);
  switch (type) {
    case UC_MEM_READ_UNMAPPED:
      record_fault(GuestFault::Kind::UnmappedRead, va, "guest read of unmapped memory");
      break;
    case UC_MEM_WRITE_UNMAPPED:
      record_fault(GuestFault::Kind::UnmappedWrite, va, "guest write to unmapped memory");
      break;
    case UC_MEM_FETCH_UNMAPPED: {
      // Call/jump through an unpatched kernel thunk: the raw XBE entry
      // is ordinal | 0x80000000, so the fetch lands above 2 GiB.
      if (addr >= 0x80000000ull) {
        const uint32_t ord = va & 0x7FFFFFFF;
        fault_.kind = GuestFault::Kind::UnimplementedImport;
        fault_.va = va;
        fault_.ordinal = ord;
        fault_.detail = std::string("call through unimplemented kernel import: ") +
                        krnl::name_of(ord);
      } else {
        record_fault(GuestFault::Kind::UnmappedFetch, va, "guest fetch from unmapped memory");
      }
      break;
    }
    case UC_MEM_WRITE_PROT:
      record_fault(GuestFault::Kind::WriteProt, va, "guest write to read-only memory");
      break;
    case UC_MEM_READ_PROT:
      record_fault(GuestFault::Kind::ReadProt, va, "guest read of protected memory");
      break;
    default:
      record_fault(GuestFault::Kind::EmuError, va, "guest memory fault");
      break;
  }
  return false;  // stop emulation; run() reports the recorded fault
}

void GuestCpu::handle_hle_call(uint32_t ordinal) {
  const krnl::Export* exp = krnl::find_export(ordinal);
  if (!exp || exp->conv == krnl::Conv::Data) {
    fault_.kind = GuestFault::Kind::UnimplementedImport;
    fault_.va = pending_addr_;
    fault_.eip = reg(UC_X86_REG_EIP);
    fault_.ordinal = ordinal;
    fault_.detail = std::string("kernel import not implemented: ") +
                    krnl::name_of(ordinal);
    return;
  }

  uint32_t args[14] = {};
  uint32_t nargs = 0;
  const uint32_t esp = reg(UC_X86_REG_ESP);
  uint32_t stack_bytes = 0;
  auto stack_arg = [&](uint32_t i) {
    uint32_t v = 0;
    ram_.read32(esp + 4 + 4 * i, &v);  // [esp] = return address
    return v;
  };

  switch (exp->conv) {
    case krnl::Conv::Stdcall: {
      stack_bytes = exp->arg_bytes;
      nargs = stack_bytes / 4;
      for (uint32_t i = 0; i < nargs && i < 14; ++i) args[i] = stack_arg(i);
      break;
    }
    case krnl::Conv::Fastcall: {
      // args[0]/args[1] = ECX/EDX (MS fastcall); the rest on the stack.
      const uint32_t total = exp->arg_bytes;
      args[nargs++] = reg(UC_X86_REG_ECX);
      if (total > 4) args[nargs++] = reg(UC_X86_REG_EDX);
      stack_bytes = total > 8 ? total - 8 : 0;
      for (uint32_t i = 0; i * 4 < stack_bytes && nargs < 14; ++i)
        args[nargs++] = stack_arg(i);
      break;
    }
    case krnl::Conv::Cdecl: {
      // Caller pops; args[0] is the first stack argument (e.g. the
      // DbgPrint format pointer). Walk a bounded window; the handler
      // consumes exactly what its format string needs.
      nargs = 13;
      for (uint32_t i = 0; i < nargs; ++i) args[i] = stack_arg(i);
      stack_bytes = 0;
      break;
    }
    case krnl::Conv::Data:
      return;  // handled above
  }

  bool ok = false;
  if (getenv("XBOX_CPU_DEBUG"))
    std::fprintf(stderr, "[hle] ord=%u esp=%#x nargs=%u args0=%#x\n",
                 ordinal, esp, nargs, nargs ? args[0] : 0);
  uint32_t edx = 0;  // high dword of 64-bit returns (EDX:EAX, MS ABI)
  const uint32_t eax = krnl_.call(ordinal, args, nargs, &ok, &edx);
  if (!ok) {
    fault_.kind = GuestFault::Kind::UnimplementedImport;
    fault_.va = pending_addr_;
    fault_.eip = reg(UC_X86_REG_EIP);
    fault_.ordinal = ordinal;
    fault_.detail =
        std::string("kernel import not implemented: ") + krnl::name_of(ordinal);
    return;
  }
  uc_reg_write(uc_, UC_X86_REG_EAX, &eax);
  // Only exports with a documented 64-bit return type write EDX;
  // everything else leaves the caller's EDX untouched (it is a
  // volatile register, but we are precise about what we clobber).
  if (krnl_.returns_64(ordinal))
    uc_reg_write(uc_, UC_X86_REG_EDX, &edx);

  // Virtual return: pop the return address and the callee-cleaned stack
  // bytes (stdcall / fastcall-tail). Never touches EIP inside a hook -
  // emulation is already stopped here.
  pending_ret_ = 0;
  ram_.read32(esp, &pending_ret_);
  pending_esp_ = esp + 4 + stack_bytes;
  uc_reg_write(uc_, UC_X86_REG_ESP, &pending_esp_);
  uc_reg_write(uc_, UC_X86_REG_EIP, &pending_ret_);
}

bool GuestCpu::run(uint32_t entry_eip, uint32_t esp, uint64_t max_instructions,
                   std::string* error) {
  if (!uc_) {
    if (error) *error = "not attached";
    return false;
  }
  fault_ = GuestFault{};
  served_.clear();
  instructions_ = 0;
  pending_ordinal_ = 0;
  uc_reg_write(uc_, UC_X86_REG_ESP, &esp);

  uint64_t eip = entry_eip;
  for (;;) {
    if (instructions_ >= max_instructions) {
      record_fault(GuestFault::Kind::InstructionLimit,
                   static_cast<uint32_t>(eip),
                   "did not exit within the instruction budget");
      return false;
    }
    const uint64_t before = instructions_;
    const uc_err e = uc_emu_start(uc_, eip, kExitVa, 0,
                                 static_cast<size_t>(max_instructions - instructions_));
    if (fault_.kind != GuestFault::Kind::None) return false;

    if (e != UC_ERR_OK) {
      if (fault_.kind == GuestFault::Kind::None) {
        fault_.kind = GuestFault::Kind::EmuError;
        fault_.va = reg(UC_X86_REG_EIP);
        fault_.eip = fault_.va;
        fault_.detail = uc_strerror(e);
      }
      return false;
    }

    const uint32_t now = reg(UC_X86_REG_EIP);
    if (now == kExitVa) return true;  // clean guest exit

    if (pending_ordinal_ != 0) {
      const uint32_t ordinal = pending_ordinal_;
      pending_ordinal_ = 0;
      handle_hle_call(ordinal);
      served_.push_back(ordinal);
      if (fault_.kind != GuestFault::Kind::None) return false;
      eip = pending_ret_;  // resume at the virtualized return address
      continue;
    }

    // UC_OK, not at the exit sentinel, no pending HLE call: the
    // instruction budget of this emu_start round was exhausted.
    (void)before;
    record_fault(GuestFault::Kind::InstructionLimit, now,
                 "did not exit within the instruction budget");
    return false;
  }
}

}  // namespace ucore
