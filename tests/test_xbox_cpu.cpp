// Guest execution tests for the Unicorn x86-32 CPU core (xbox_cpu.h).
//
// Two layers, both real:
//   1. Synthetic guest programs (hand-assembled x86-32) driving the REAL
//      kernel HLE through the REAL thunk protocol - exact instruction
//      counts, exact stack arithmetic, exact virtual-tick timelines.
//   2. The real LithiumX XBE: its entry point executes as real machine
//      code until an honest stop (unimplemented import / fault), with
//      its real TLS directory powering a real per-fiber TEB.
//
// Nothing here stubs the CPU: every guest instruction is decoded and
// executed by Unicorn TCG; kernel calls cross XboxKrnl::call(), the
// same dispatch the HLE tests cover.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cpu_native.h"  // cpu_qpc_now (10 MHz host QPC shim)
#include "cpu_sched.h"
#include "xbe_loader.h"
#include "xbox_cpu.h"
#include "xbox_krnl.h"

using namespace ucore;
using krnl::kHleMarker;

namespace {

int g_fails = 0;
const char* g_case = "";

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ++g_fails;                                                          \
      std::printf("FAIL [%s] %s:%d: %s\n", g_case, __FILE__, __LINE__,    \
                  #cond);                                                 \
    }                                                                     \
  } while (0)

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

// Ordinal lookup by name (the export table is generated from nxdk's
// xboxkrnl.exe.def; names are authoritative).
uint32_t ordinal_of(const char* name) {
  for (size_t i = 0; i < krnl::export_count(); ++i) {
    const krnl::Export& e = krnl::export_at(i);
    if (std::strcmp(e.name, name) == 0) return e.ordinal;
  }
  return 0;
}

constexpr uint32_t kPage = 0x1000;

// -- tiny 32-bit code builder (test-only; real XBEs need none of this) --
struct Asm {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
  }
  void mov_eax_imm(uint32_t v) { u8(0xB8); u32(v); }
  void mov_ecx_imm(uint32_t v) { u8(0xB9); u32(v); }
  void inc_eax() { u8(0xFF); u8(0xC0); }
  void push_imm(uint32_t v) { u8(0x68); u32(v); }
  void call_mem(uint32_t slot_va) {  // FF 15 <moffs32>
    u8(0xFF); u8(0x15); u32(slot_va);
  }
  void mov_mem_eax(uint32_t va) { u8(0xA3); u32(va); }
  void mov_mem_edx(uint32_t va) { u8(0x89); u8(0x15); u32(va); }  // mov [moffs], edx
  void mov_eax_mem(uint32_t va) { u8(0xA1); u32(va); }  // mov eax, [moffs]
  void push_eax() { u8(0x50); }
  void jmp(uint32_t from_va, uint32_t to_va) {  // E9 rel32
    u8(0xE9);
    u32(to_va - (from_va + 5));
  }
  void jmp_self() { u8(0xEB); u8(0xFE); }  // jmp $
};

struct Harness {
  GuestScheduler sched;
  krnl::XboxKrnl krnl;
  UnicornRuntime rt;
  // The kernel + CPU must operate on the SAME window the image is
  // mapped into (LoadedXbe owns its own XbeRam).
  explicit Harness(xbe::XbeRam& ram)
      : krnl(ram, sched), rt(ram, krnl) {}
  xbe::XbeRam& ram() { return rt.ram; }

  uint32_t commit_at(uint32_t va, uint32_t size) {
    if (!ram().commit(va, size)) return 0;
    return va;
  }
  uint32_t put_code(uint32_t va, const Asm& a) {
    if (!commit_at(va, kPage)) return 0;
    if (!ram().write_bytes(va, a.b.data(), static_cast<uint32_t>(a.b.size())))
      return 0;  // e.g. the page is part of a read-only mapped section
    return va;
  }
};

}  // namespace

// --------------------------------------------------------------------------
static int test_basic_execution() {
  g_case = "basic_execution";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  GuestCpu cpu(h.rt);

  const uint32_t kCode = 0x00100000;
  Asm a;
  a.mov_eax_imm(41);
  a.inc_eax();
  a.jmp(kCode + 7, kExitVa);  // after mov(5) + inc(2)
  h.put_code(kCode, a);
  CHECK(cpu.attach());  // attach AFTER commits: ranges are scanned here

  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  CHECK(stack_top != 0);
  const bool ran = cpu.run(kCode, stack_top, 1000);
  if (!ran)
    std::printf("debug basic: fault=%s va=0x%08X eip=0x%08X: %s\n",
                cpu.fault().kind_name(), cpu.fault().va, cpu.fault().eip,
                cpu.fault().detail.c_str());
  CHECK(ran);
  CHECK(cpu.fault().kind == GuestFault::Kind::None);
  CHECK(cpu.reg(UC_X86_REG_EAX) == 42);
  CHECK(cpu.instructions() == 3);  // exact: mov, inc, jmp
  std::printf("ok basic_execution (3 instructions, EAX=%u)\n",
              cpu.reg(UC_X86_REG_EAX));
  return 0;
}

// --------------------------------------------------------------------------
static int test_fastcall_interlocked() {
  g_case = "fastcall_interlocked";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  const uint32_t ord_dec = ordinal_of("InterlockedDecrement");
  CHECK(ord_dec != 0);
  const krnl::Export* exp = krnl::find_export(ord_dec);
  CHECK(exp && exp->conv == krnl::Conv::Fastcall && exp->arg_bytes == 4);

  GuestCpu cpu(h.rt);

  const uint32_t kCode = 0x00100000;
  const uint32_t kData = 0x00110000;
  const uint32_t kSlot = 0x00120000;
  h.commit_at(kData, kPage);
  h.commit_at(kSlot, kPage);
  h.ram().write32(kData + 0, 7);                              // counter
  h.ram().write32(kData + 4, 0);                              // result
  h.ram().write32(kSlot, kHleMarker | ord_dec);               // thunk slot

  Asm a;
  a.mov_ecx_imm(kData);          // fastcall arg0 = ECX = counter ptr
  a.call_mem(kSlot);             // call [slot] -> HLE InterlockedDecrement
  a.mov_mem_eax(kData + 4);      // store result
  a.jmp(kCode + 16, kExitVa);    // after mov(5) + call(6) + mov(5)
  h.put_code(kCode, a);
  CHECK(cpu.attach());

  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  const bool ran = cpu.run(kCode, stack_top, 1000);
  CHECK(ran);
  CHECK(cpu.fault().kind == GuestFault::Kind::None);
  CHECK(cpu.reg(UC_X86_REG_EAX) == 6);
  uint32_t res = 0;
  h.ram().read32(kData + 4, &res);
  CHECK(res == 6);
  uint32_t cnt = 0;
  h.ram().read32(kData, &cnt);
  CHECK(cnt == 6);
  CHECK(cpu.reg(UC_X86_REG_ESP) == stack_top);  // callee popped ret only
  CHECK(cpu.served_ordinals().size() == 1 &&
        cpu.served_ordinals()[0] == ord_dec);
  std::printf("ok fastcall_interlocked (7->6 through ordinal %u)\n", ord_dec);
  return 0;
}

// --------------------------------------------------------------------------
static int test_stdcall_system_time() {
  g_case = "stdcall_system_time";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  const uint32_t ord = ordinal_of("KeQuerySystemTime");
  CHECK(ord != 0);
  const krnl::Export* exp = krnl::find_export(ord);
  CHECK(exp && exp->conv == krnl::Conv::Stdcall && exp->arg_bytes == 4);

  GuestCpu cpu(h.rt);

  const uint32_t kCode = 0x00100000;
  const uint32_t kData = 0x00110000;
  const uint32_t kSlot = 0x00120000;
  h.commit_at(kData, kPage);
  h.commit_at(kSlot, kPage);
  h.ram().write32(kSlot, kHleMarker | ord);

  Asm a;
  a.push_imm(kData);             // out ULONGLONG*
  a.call_mem(kSlot);
  a.jmp(kCode + 6 + 5, kExitVa);
  h.put_code(kCode, a);
  CHECK(cpu.attach());

  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  // Wall-clock bracket: KeQuerySystemTime reads the HOST clock (xbox_sys),
  // so the value captured by the guest must lie between two direct HLE
  // reads around the guest call.
  bool ok = false;
  uint32_t direct_args[1] = {kData + 0x40};
  uint64_t t_before = 0, t_after = 0;
  h.krnl.call(ord, direct_args, 1, &ok);
  CHECK(ok);
  h.ram().read_bytes(kData + 0x40, &t_before, 8);

  const bool ran = cpu.run(kCode, stack_top, 1000);
  CHECK(ran);
  CHECK(cpu.fault().kind == GuestFault::Kind::None);

  uint64_t guest_value = 0;
  h.ram().read_bytes(kData, &guest_value, 8);
  h.krnl.call(ord, direct_args, 1, &ok);
  h.ram().read_bytes(kData + 0x40, &t_after, 8);
  CHECK(t_before <= guest_value && guest_value <= t_after);
  CHECK(guest_value != 0);
  CHECK(cpu.reg(UC_X86_REG_ESP) == stack_top);  // stdcall: 4 bytes cleaned
  std::printf("ok stdcall_system_time (FILETIME %llu via ordinal %u)\n",
              (unsigned long long)guest_value, ord);
  return 0;
}

// --------------------------------------------------------------------------
// 64-bit kernel returns through REAL guest code: KeQueryPerformance-
// Counter/Frequency are ULONGLONG per nxdk xboxkrnl.h, i.e. the EDX:EAX
// pair on the MS x86 ABI. The guest stores both halves to memory; the
// test brackets the run with host reads of the same counter.
static int test_qpc_edx_eax_from_guest() {
  g_case = "qpc_edx_eax_from_guest";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  const uint32_t ord_qpc = ordinal_of("KeQueryPerformanceCounter");
  const uint32_t ord_freq = ordinal_of("KeQueryPerformanceFrequency");
  CHECK(ord_qpc == 126 && ord_freq == 127);
  CHECK(h.krnl.returns_64(ord_qpc) && h.krnl.returns_64(ord_freq));

  GuestCpu cpu(h.rt);
  const uint32_t kCode = 0x00100000;
  const uint32_t kData = 0x00110000;
  const uint32_t kSlotA = 0x00120000;
  const uint32_t kSlotB = 0x00121000;
  h.commit_at(kData, kPage);
  h.commit_at(kSlotA, kPage);
  h.commit_at(kSlotB, kPage);
  h.ram().write32(kSlotA, kHleMarker | ord_qpc);
  h.ram().write32(kSlotB, kHleMarker | ord_freq);

  Asm a;
  a.call_mem(kSlotA);        // EDX:EAX <- performance counter
  a.mov_mem_eax(kData + 0);
  a.mov_mem_edx(kData + 4);
  a.call_mem(kSlotB);        // EDX:EAX <- frequency
  a.mov_mem_eax(kData + 8);
  a.mov_mem_edx(kData + 12);
  a.jmp(kCode + 34, kExitVa);  // after 6+5+6+6+5+6 = 34 bytes
  h.put_code(kCode, a);
  CHECK(cpu.attach());

  const uint64_t lo = cpu_qpc_now();
  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  const bool ran = cpu.run(kCode, stack_top, 1000);
  const uint64_t hi = cpu_qpc_now();
  CHECK(ran);
  CHECK(cpu.fault().kind == GuestFault::Kind::None);

  uint32_t eax_c = 0, edx_c = 0, eax_f = 0, edx_f = 0;
  CHECK(h.ram().read32(kData + 0, &eax_c));
  CHECK(h.ram().read32(kData + 4, &edx_c));
  CHECK(h.ram().read32(kData + 8, &eax_f));
  CHECK(h.ram().read32(kData + 12, &edx_f));
  // Frequency: the full 64-bit 10 MHz in EDX:EAX.
  CHECK(eax_f == 10000000u && edx_f == 0);
  // Counter: the combined 64-bit value sits between the host reads.
  const uint64_t got = eax_c | (static_cast<uint64_t>(edx_c) << 32);
  CHECK(got >= lo && got <= hi);
  CHECK(cpu.served_ordinals().size() == 2);
  CHECK(cpu.reg(UC_X86_REG_ESP) == stack_top);  // arg_bytes 0: ESP restored
  std::printf("ok qpc_edx_eax_from_guest (64-bit counter bracketed, freq 10MHz)\n");
  return 0;
}

// --------------------------------------------------------------------------
static int test_dbprint_and_delay_two_fibers() {
  g_case = "dbprint_delay_two_fibers";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  const uint32_t ord_delay = ordinal_of("KeDelayExecutionThread");
  const uint32_t ord_dbg = ordinal_of("DbgPrint");
  CHECK(ord_delay != 0 && ord_dbg != 0);

  const uint32_t kCodeA = 0x00100000;
  const uint32_t kCodeB = 0x00102000;
  const uint32_t kData = 0x00110000;
  const uint32_t kSlotA = 0x00120000;  // A: delay + dbg slots
  const uint32_t kSlotB = 0x00130000;  // B: dbg slot
  h.commit_at(kData, kPage);
  h.commit_at(kSlotA, kPage);
  h.commit_at(kSlotB, kPage);
  const char msg_a[] = "A-woke";  // NUL included: DbgPrint reads a C string
  const char msg_b[] = "B-ran";
  const uint64_t kInterval = static_cast<uint64_t>(-30000);  // 3 ms relative
  h.ram().write_bytes(kData + 0x80, &kInterval, 8);
  h.ram().write_bytes(kData + 0xC0, msg_a, sizeof(msg_a));
  h.ram().write_bytes(kData + 0xE0, msg_b, sizeof(msg_b));
  h.ram().write32(kSlotA + 0, kHleMarker | ord_delay);
  h.ram().write32(kSlotA + 4, kHleMarker | ord_dbg);
  h.ram().write32(kSlotB + 0, kHleMarker | ord_dbg);

  Asm a;
  a.push_imm(kData + 0x80);      // Interval (args pushed in reverse)
  a.push_imm(0);                 // Alertable
  a.push_imm(0);                 // WaitMode
  a.call_mem(kSlotA + 0);        // KeDelayExecutionThread(0,0,&3ms) = 12b
  a.push_imm(kData + 0xC0);      // fmt
  a.call_mem(kSlotA + 4);        // DbgPrint("A-woke")
  a.jmp(kCodeA + 32, kExitVa);   // push*3(15) + call(6) + push(5) + call(6)
  h.put_code(kCodeA, a);

  Asm b;
  b.push_imm(kData + 0xE0);      // fmt
  b.call_mem(kSlotB + 0);        // DbgPrint("B-ran")
  b.jmp(kCodeB + 6 + 5, kExitVa);
  h.put_code(kCodeB, b);

  GuestCpu cpu_a(h.rt), cpu_b(h.rt);
  CHECK(cpu_a.attach());
  CHECK(cpu_b.attach());
  const uint32_t teb_a = h.rt.build_teb();  // real TEBs in guest RAM
  const uint32_t teb_b = h.rt.build_teb();
  CHECK(teb_a != 0 && teb_b != 0 && teb_a != teb_b);
  cpu_a.set_teb(teb_a);
  cpu_b.set_teb(teb_b);
  const uint32_t stack_a = h.rt.alloc_stack(0x10000);
  const uint32_t stack_b = h.rt.alloc_stack(0x10000);
  CHECK(stack_a != 0 && stack_b != 0);

  // No dbg sink: DbgPrint output accumulates in krnl.dbg_lines().

  h.sched.spawn("A", [&](class GuestThread&) {
    CHECK(cpu_a.run(kCodeA, stack_a, 100000));
  });
  h.sched.spawn("B", [&](class GuestThread&) {
    CHECK(cpu_b.run(kCodeB, stack_b, 100000));
  });
  CHECK(h.sched.run_until_complete());

  // B (no delay) must print BEFORE A (really slept 3 virtual ticks).
  const std::vector<std::string>& lines = h.krnl.dbg_lines();
  CHECK(lines.size() == 2);
  if (lines.size() == 2) {
    CHECK(lines[0] == "B-ran");
    CHECK(lines[1] == "A-woke");
  }
  CHECK(h.sched.current_tick() == 3);
  CHECK(cpu_a.reg(UC_X86_REG_EAX) == 6);  // DbgPrint chars printed
  CHECK(cpu_a.fault().kind == GuestFault::Kind::None);
  CHECK(cpu_b.fault().kind == GuestFault::Kind::None);
  CHECK(cpu_a.served_ordinals().size() == 2);
  CHECK(cpu_b.served_ordinals().size() == 1);
  std::printf("ok dbprint_delay_two_fibers (B-ran, A-woke, tick=%llu)\n",
              (unsigned long long)h.sched.current_tick());
  return 0;
}

// --------------------------------------------------------------------------
static int test_mm_alloc_from_guest() {
  g_case = "mm_alloc_from_guest";
  std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  CHECK(!file.empty());
  if (file.empty()) return 0;
  std::string lerr;
  auto image = xbe::LoadedXbe::load(file, &lerr);
  CHECK(image != nullptr);
  if (!image) return 0;
  xbe::XbeRam& ram = image->ram();
  Harness h{ram};
  CHECK(h.krnl.install_thunks(*image).size() > 0);
  const uint32_t ord = ordinal_of("MmAllocateContiguousMemoryEx");
  CHECK(ord != 0);
  const krnl::Export* exp = krnl::find_export(ord);
  CHECK(exp && exp->conv == krnl::Conv::Stdcall && exp->arg_bytes == 20);

  GuestCpu cpu(h.rt);

  const uint32_t kCode = 0x00A00000;   // above the LithiumX image extent
  const uint32_t kData = 0x00A0F000;
  const uint32_t kSlot = 0x00A10000;
  h.commit_at(kData, kPage);
  h.commit_at(kSlot, kPage);
  h.ram().write32(kSlot, kHleMarker | ord);

  Asm a;
  a.push_imm(0x04);              // Protect = PAGE_READWRITE (reverse order)
  a.push_imm(0);                 // Alignment
  a.push_imm(0);                 // HighestAcceptable
  a.push_imm(0x00B00000);        // LowestAcceptable (above code/slots)
  a.push_imm(0x2000);            // Length (2 pages)
  a.call_mem(kSlot);
  a.mov_ecx_imm(kData + 0x10);   // remember VA
  // mov [ecx], eax: 89 01
  a.u8(0x89); a.u8(0x01);
  // mov dword [eax], 0x1234A5A5 (write into the fresh allocation):
  // C7 00 A5 A5 34 12
  a.u8(0xC7); a.u8(0x00);
  a.u8(0xA5); a.u8(0xA5); a.u8(0x34); a.u8(0x12);
  a.jmp(kCode + 44, kExitVa);  // push*5(25) + call(6) + mov_ecx(5) + 2 + 6
  h.put_code(kCode, a);
  CHECK(cpu.attach());  // after commits: the scan sees code/data/slots

  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  CHECK(cpu.run(kCode, stack_top, 1000));
  CHECK(cpu.fault().kind == GuestFault::Kind::None);
  CHECK(cpu.reg(UC_X86_REG_EAX) != 0);

  uint32_t alloc_va = 0;
  h.ram().read32(kData + 0x10, &alloc_va);
  CHECK(alloc_va >= 0x00B00000);
  CHECK(h.krnl.mm_region_lo() <= alloc_va);  // inside the Mm arena
  uint32_t cookie = 0;
  h.ram().read32(alloc_va, &cookie);
  CHECK(cookie == 0x1234A5A5);  // guest wrote its fresh allocation
  std::printf("ok mm_alloc_from_guest (VA 0x%08X mapped mid-run)\n", alloc_va);
  return 0;
}

// --------------------------------------------------------------------------
static int test_honest_faults() {
  g_case = "honest_faults";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};

  {  // unmapped read (null page is never committed)
    GuestCpu cpu(h.rt);
    const uint32_t kCode = 0x00100000;
    Asm a;
    // mov eax, [0]: A1 00 00 00 00
    a.u8(0xA1); a.u32(0);
    a.jmp(kCode + 5, kExitVa);
    h.put_code(kCode, a);
    CHECK(cpu.attach());
    const uint32_t stack_top = h.rt.alloc_stack(0x10000);
    CHECK(!cpu.run(kCode, stack_top, 1000));
    CHECK(cpu.fault().kind == GuestFault::Kind::UnmappedRead);
    CHECK(cpu.fault().va == 0);
    std::printf("ok honest_faults/unmapped-read (va=0, eip=0x%08X)\n",
                cpu.fault().eip);
  }
  {  // write to read-only memory (loader-style RO page)
    GuestCpu cpu(h.rt);
    const uint32_t kCode = 0x00140000;
    Asm a;
    // mov dword [kCode], 1
    a.u8(0xC7); a.u8(0x05); a.u32(kCode); a.u32(1);
    a.jmp(kCode + 10, kExitVa);
    h.put_code(kCode, a);        // host write while page is RW
    h.ram().protect_ro(kCode, kPage);  // then RO - attach() mirrors it
    CHECK(cpu.attach());
    const uint32_t stack_top = h.rt.alloc_stack(0x10000);
    CHECK(!cpu.run(kCode, stack_top, 1000));
    CHECK(cpu.fault().kind == GuestFault::Kind::WriteProt);
    CHECK(cpu.fault().va == kCode);
    std::printf("ok honest_faults/write-readonly (va=0x%08X)\n",
                cpu.fault().va);
  }
  {  // call through an unimplemented kernel import (raw slot value)
    GuestCpu cpu(h.rt);
    const uint32_t ord_unimpl = ordinal_of("AvGetSavedDataAddress");
    CHECK(ord_unimpl != 0);
    const uint32_t kCode = 0x00100000;
    const uint32_t kSlot = 0x00120000;
    h.commit_at(kSlot, kPage);
    h.ram().write32(kSlot, ord_unimpl | 0x80000000u);  // RAW XBE entry
    Asm a;
    a.call_mem(kSlot);
    a.jmp(kCode + 5, kExitVa);
    h.put_code(kCode, a);
    CHECK(cpu.attach());
    const uint32_t stack_top = h.rt.alloc_stack(0x10000);
    CHECK(!cpu.run(kCode, stack_top, 1000));
    CHECK(cpu.fault().kind == GuestFault::Kind::UnimplementedImport);
    CHECK(cpu.fault().ordinal == ord_unimpl);
    CHECK(cpu.fault().detail.find("AvGetSavedDataAddress") !=
          std::string::npos);
    std::printf("ok honest_faults/unimplemented-import (%s)\n",
                cpu.fault().detail.c_str());
  }
  {  // instruction limit on an infinite loop, exact count
    GuestCpu cpu(h.rt);
    const uint32_t kCode = 0x00100000;
    Asm a;
    a.jmp_self();
    h.put_code(kCode, a);
    CHECK(cpu.attach());
    const uint32_t stack_top = h.rt.alloc_stack(0x10000);
    CHECK(!cpu.run(kCode, stack_top, 1000));
    CHECK(cpu.fault().kind == GuestFault::Kind::InstructionLimit);
    CHECK(cpu.instructions() == 1000);
    std::printf("ok honest_faults/instruction-limit (exactly 1000)\n");
  }
  return 0;
}

// --------------------------------------------------------------------------
static int test_tls_teb_real_path() {
  g_case = "tls_teb_real_path";
  std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  CHECK(!file.empty());
  if (file.empty()) return 0;
  std::string err;
  auto image = xbe::LoadedXbe::load(file, &err);
  CHECK(image != nullptr);
  if (!image) return 0;

  Harness h{image->ram()};
  CHECK(h.krnl.install_thunks(*image).size() > 0);
  CHECK(h.rt.set_image(*image, &err));
  const uint32_t teb = h.rt.build_teb();
  CHECK(teb != 0);
  uint32_t v = 0;
  h.ram().read32(teb + 0x18, &v);
  CHECK(v == teb);  // TEB.Self
  h.ram().read32(teb + 0x04, &v);
  CHECK(v == teb + 0x100);  // array ptr (fs:[4])
  h.ram().read32(teb + 0x100, &v);
  CHECK(v == teb + 0x200);  // array[0]

  // The TLS index was written into the real image (nxdk step 1).
  uint32_t index = 0xFFFF;
  CHECK(image->read32(image->image().tls().index_addr, &index));
  CHECK(index == 0);

  // Template copy at the data area.
  const std::vector<uint8_t> tmpl = image->tls_template();
  CHECK(tmpl.size() >= 4);
  uint32_t t0 = 0;
  h.ram().read_bytes(teb + 0x200, &t0, 4);
  uint32_t want = 0;
  std::memcpy(&want, tmpl.data(), 4);
  CHECK(t0 == want);

  // Guest executes the disassembled nxdk access sequence on real
  // instructions: idx=[IndexVA]; ecx=fs:[4]; eax=[ecx+idx*4]; v=[eax].
  GuestCpu cpu(h.rt);
  cpu.set_teb(teb);
  // Above the LithiumX image extent (0x8EF114): this page is free Mm
  // arena, not part of any mapped section.
  const uint32_t kCode = 0x00900000;
  const uint32_t kIdx = image->image().tls().index_addr;
  const uint32_t kJmpAt = kCode + 17;  // mov5 + mov7 + mov3 + mov2
  Asm a;
  // mov eax, [IndexVA]: A1 <moffs>
  a.u8(0xA1); a.u32(kIdx);
  // mov ecx, fs:[4]: 64 8B 0D 04 00 00 00
  a.u8(0x64); a.u8(0x8B); a.u8(0x0D); a.u32(4);
  // mov eax, [ecx+eax*4]: 8B 04 81
  a.u8(0x8B); a.u8(0x04); a.u8(0x81);
  // mov eax, [eax]: 8B 00
  a.u8(0x8B); a.u8(0x00);
  // jmp exit
  a.jmp(kJmpAt, kExitVa);
  h.put_code(kCode, a);
  CHECK(cpu.attach());  // after commit: the scan sees the code page
  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  const bool ran = cpu.run(kCode, stack_top, 1000);
  CHECK(ran);
  CHECK(cpu.fault().kind == GuestFault::Kind::None);
  CHECK(cpu.reg(UC_X86_REG_EAX) == want);
  // fs:[0x18] self-pointer, the classic TEB probe:
  // mov eax, fs:[0x18] (A1 under FS prefix)
  const uint32_t kCode2 = kCode + kPage - 32;  // inside the same page
  Asm b;
  b.u8(0x64); b.u8(0xA1); b.u32(0x18);   // mov eax, fs:[0x18] = 6 bytes
  b.jmp(kCode2 + 6, kExitVa);
  h.ram().write_bytes(kCode2, b.b.data(), (uint32_t)b.b.size());
  const bool ran2 = cpu.run(kCode2, stack_top, 1000);
  CHECK(ran2);
  CHECK(cpu.reg(UC_X86_REG_EAX) == teb);
  std::printf("ok tls_teb_real_path (TEB 0x%08X, template dword 0x%08X)\n",
              teb, want);
  return 0;
}

// --------------------------------------------------------------------------
static int test_lithiumx_real_entry() {
  g_case = "lithiumx_real_entry";
  std::vector<uint8_t> file = read_file(XBE_FIXTURE);
  CHECK(!file.empty());
  if (file.empty()) return 0;
  std::string err;
  auto image = xbe::LoadedXbe::load(file, &err);
  CHECK(image != nullptr);
  if (!image) return 0;

  Harness h{image->ram()};
  const auto slots = h.krnl.install_thunks(*image);
  CHECK(slots.size() == 103);
  CHECK(h.rt.set_image(*image, &err));
  const uint32_t teb = h.rt.build_teb();
  CHECK(teb != 0);

  GuestCpu cpu(h.rt);
  cpu.set_teb(teb);
  CHECK(cpu.attach());
  const uint32_t stack = h.rt.alloc_stack(0x40000);
  CHECK(stack != 0);

  const uint32_t entry = image->image().entry_va();
  const bool ok = cpu.run(entry, stack, 2000000, &err);
  std::printf("lithiumx: entry=0x%08X executed=%llu instrs -> %s\n", entry,
              (unsigned long long)cpu.instructions(),
              ok ? "CLEAN EXIT" : cpu.fault().detail.c_str());
  std::printf("lithiumx: served %zu kernel calls:",
              cpu.served_ordinals().size());
  for (uint32_t o : cpu.served_ordinals())
    std::printf(" %s", krnl::name_of(o));
  std::printf("\n");
  for (const auto& l : h.krnl.dbg_lines())
    std::printf("lithiumx dbg: %s\n", l.c_str());

  // Honest assertions only: REAL code ran, and it stopped for a RECORDED
  // reason (never silently, never fabricated). The entry's first kernel
  // call may be an unimplemented import - that is the designed,
  // honestly-reported failure mode of an HLE that implements ~30 of 371
  // kernel exports.
  CHECK(cpu.instructions() > 10);
  if (!ok) {
    CHECK(cpu.fault().kind != GuestFault::Kind::None);
    if (cpu.fault().kind == GuestFault::Kind::UnimplementedImport) {
      CHECK(cpu.fault().ordinal != 0);
      CHECK(std::strcmp(krnl::name_of(cpu.fault().ordinal), "<unknown>") != 0);
    }
  }
  return 0;
}

// --------------------------------------------------------------------------
// Guest code drives the FULL mutant (mutex) lifecycle through real thunk
// slots: create -> acquire -> release -> close, all statuses stored.
static int test_guest_mutant_roundtrip() {
  g_case = "guest_mutant_roundtrip";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  const uint32_t kCode = 0x00100000;
  const uint32_t kData = 0x00110000;
  const uint32_t kSlotCreate = 0x00120000;
  const uint32_t kSlotWait = 0x00121000;
  const uint32_t kSlotRelease = 0x00122000;
  const uint32_t kSlotClose = 0x00123000;
  h.commit_at(kData, kPage);
  for (uint32_t s : {kSlotCreate, kSlotWait, kSlotRelease, kSlotClose})
    h.commit_at(s, kPage);
  h.ram().write32(kSlotCreate, kHleMarker | 192);   // NtCreateMutant
  h.ram().write32(kSlotWait, kHleMarker | 233);     // NtWaitForSingleObject
  h.ram().write32(kSlotRelease, kHleMarker | 221);  // NtReleaseMutant
  h.ram().write32(kSlotClose, kHleMarker | 187);    // NtClose

  Asm a;
  // NtCreateMutant(&h, NULL, FALSE) - stdcall 12.
  a.push_imm(0);
  a.push_imm(0);
  a.push_imm(kData + 0x40);
  a.call_mem(kSlotCreate);
  a.mov_mem_eax(kData + 0);
  // NtWaitForSingleObject(h, Alertable=FALSE, Timeout=NULL) - stdcall
  // 12 (acquire: the mutant is free, so this succeeds immediately).
  a.mov_eax_mem(kData + 0x40);
  a.push_imm(0);  // Timeout = NULL
  a.push_imm(0);  // Alertable
  a.push_eax();   // Handle
  a.call_mem(kSlotWait);
  a.mov_mem_eax(kData + 4);
  // NtReleaseMutant(h, NULL) - stdcall 8.
  a.mov_eax_mem(kData + 0x40);
  a.push_imm(0);
  a.push_eax();
  a.call_mem(kSlotRelease);
  a.mov_mem_eax(kData + 8);
  // NtClose(h) - stdcall 4.
  a.mov_eax_mem(kData + 0x40);
  a.push_eax();
  a.call_mem(kSlotClose);
  a.mov_mem_eax(kData + 0xC);
  a.jmp(kCode + static_cast<uint32_t>(a.b.size()), kExitVa);  // after last store
  h.put_code(kCode, a);
  GuestCpu cpu(h.rt);
  CHECK(cpu.attach());
  const uint32_t stack_top = h.rt.alloc_stack(0x10000);
  // Guest code always runs inside a scheduler fiber (a mutant cannot
  // be owned by "no thread" - that rule is tested here too).
  std::string fault_txt;
  bool ran = false;
  h.sched.spawn("mt", [&](GuestThread&) {
    ran = cpu.run(kCode, stack_top, 1000);
    if (!ran)
      fault_txt = cpu.fault().detail;
  });
  CHECK(h.sched.run_until_complete());
  CHECK(ran);
  CHECK(cpu.fault().kind == GuestFault::Kind::None);
  CHECK(cpu.reg(UC_X86_REG_ESP) == stack_top);  // all stdcall frames balanced
  for (const uint32_t off : {0u, 4u, 8u, 0xCu}) {
    uint32_t st = 0xFFFFFFFFu;
    CHECK(h.ram().read32(kData + off, &st) && st == 0);  // STATUS_SUCCESS x4
  }
  CHECK(cpu.instructions() == 21);  // 20 real instructions + jmp
  CHECK(cpu.served_ordinals().size() == 4);
  CHECK(h.krnl.object_count() == 0);  // created AND closed
  CHECK(fault_txt.empty());
  std::printf("ok guest_mutant_roundtrip (create/wait/release/close, 4 real calls)\n");
  return 0;
}

// --------------------------------------------------------------------------
// The CRT-startup shape on real code: the guest's main thread calls
// PsCreateSystemThreadEx, the kernel spawns a fiber, the UnicornRuntime
// runner boots a SECOND GuestCpu and runs the thread routine (which
// stores a magic value and calls PsTerminateSystemThread), and the main
// thread's NtWaitForSingleObject on the thread handle wakes on
// termination. Every step is real guest instructions + real kernel HLE.
static int test_guest_system_thread() {
  g_case = "guest_system_thread";
  xbe::XbeRam ram{64u << 20};
  Harness h{ram};
  constexpr uint32_t kMagic = 0x600DF00Du;
  const uint32_t kCode = 0x00100000;      // main guest thread
  const uint32_t kCode2 = 0x00140000;     // system thread routine
  const uint32_t kData = 0x00110000;
  const uint32_t kSlotCreate = 0x00120000;
  const uint32_t kSlotWait = 0x00121000;
  const uint32_t kSlotTerm = 0x00122000;
  h.commit_at(kData, kPage);
  for (uint32_t s : {kSlotCreate, kSlotWait, kSlotTerm}) h.commit_at(s, kPage);
  h.ram().write32(kSlotCreate, kHleMarker | 255);  // PsCreateSystemThreadEx
  h.ram().write32(kSlotWait, kHleMarker | 233);    // NtWaitForSingleObject
  h.ram().write32(kSlotTerm, kHleMarker | 258);    // PsTerminateSystemThread

  // Thread routine: *(uint32_t*)(kData+0x10) = MAGIC; PsTerminate(42).
  Asm tf;
  tf.mov_eax_imm(kMagic);
  tf.mov_mem_eax(kData + 0x10);
  tf.push_imm(42);
  tf.call_mem(kSlotTerm);  // never returns
  h.put_code(kCode2, tf);

  Asm a;
  // PsCreateSystemThreadEx(&h, 0, 0x8000, 0, NULL, kCode2, ctx, FALSE,
  //                        FALSE, NULL) - stdcall 40.
  a.push_imm(0);           // SystemRoutine = NULL
  a.push_imm(0);           // DebuggerThread
  a.push_imm(0);           // CreateSuspended
  a.push_imm(kData + 0x80);  // StartContext
  a.push_imm(kCode2);        // StartRoutine
  a.push_imm(0);             // ThreadId = NULL
  a.push_imm(0);             // TlsDataSize
  a.push_imm(0x8000);        // KernelStackSize
  a.push_imm(0);             // ThreadExtensionSize
  a.push_imm(kData + 0x40);  // ThreadHandle out
  a.call_mem(kSlotCreate);
  a.mov_mem_eax(kData + 0);
  // NtWaitForSingleObject(handle, FALSE, NULL) - stdcall 12; blocks
  // until the system thread terminates.
  a.mov_eax_mem(kData + 0x40);
  a.push_imm(0);  // Timeout = NULL
  a.push_imm(0);  // Alertable
  a.push_eax();   // Handle
  a.call_mem(kSlotWait);
  a.mov_mem_eax(kData + 4);
  a.jmp(kCode + static_cast<uint32_t>(a.b.size()), kExitVa);
  h.put_code(kCode, a);

  std::vector<std::string> trace;
  h.sched.spawn("main", [&](GuestThread&) {
    GuestCpu cpu(h.rt);  // per-fiber CPU (the runner makes one for the
    cpu.set_teb(h.rt.build_teb());  // system thread itself)
    CHECK(cpu.attach());
    const uint32_t stack = h.rt.alloc_stack(0x20000);
    CHECK(cpu.run(kCode, stack, 10000, nullptr));
    trace.push_back(cpu.fault().kind == GuestFault::Kind::None
                        ? "main:clean"
                        : "main:fault");
  });
  CHECK(h.sched.run_until_complete());
  CHECK(trace.size() == 1 && trace[0] == "main:clean");

  uint32_t create_st = 0, wait_st = 0, magic = 0, handle = 0;
  CHECK(h.ram().read32(kData + 0, &create_st) && create_st == 0);
  CHECK(h.ram().read32(kData + 4, &wait_st) && wait_st == 0);
  CHECK(h.ram().read32(kData + 0x10, &magic) && magic == kMagic);
  CHECK(h.ram().read32(kData + 0x40, &handle) && handle != 0);
  CHECK(!h.krnl.thread_running_by_handle(handle));  // terminated
  std::printf(
      "ok guest_system_thread (created by guest, ran real code, terminated)\n");
  return 0;
}

// --------------------------------------------------------------------------
int main() {
  test_basic_execution();
  test_fastcall_interlocked();
  test_stdcall_system_time();
  test_qpc_edx_eax_from_guest();
  test_dbprint_and_delay_two_fibers();
  test_mm_alloc_from_guest();
  test_guest_mutant_roundtrip();
  test_guest_system_thread();
  test_honest_faults();
  test_tls_teb_real_path();
  test_lithiumx_real_entry();
  if (g_fails) {
    std::printf("\n%d CHECK(s) failed\n", g_fails);
    return 1;
  }
  std::printf("all xbox_cpu tests passed\n");
  return 0;
}
