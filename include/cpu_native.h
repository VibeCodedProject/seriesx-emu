#pragma once
#include <cstdint>
#include <functional>

// Native exec model: x86_64 guest runs directly (no interpreter).
// Privileged / OS calls trap to host via callback (HLE).
// Xbox Series X CPU: 8C Zen2 (Renoir-like, 4MB L3/CCX), 3.8GHz SMT-off /
// 3.6GHz SMT-on, 1 core reserved -> 7 game cores. Homebrew only.
using TrapHandler = std::function<void(uint32_t syscall_id, uint64_t arg)>;

// HLE syscall table (homebrew only, no GameOS binary).
enum class SysId : uint32_t {
  Log = 1,
  GpuFlush = 2,
  GetTopology = 10,
  GetQpcFreq = 11,
  GetQpc = 12,
  GetRdtsc = 13,
  Yield = 14,
};

// Fixed Xbox-like CPU topology (SMT mode chosen at boot per-title).
struct CpuTopology {
  uint32_t phys = 8;
  uint32_t logical = 8;  // 8 (SMT-off) or 16 (SMT-on)
  uint32_t game_cores = 7;
  uint32_t l1i = 32u << 10;
  uint32_t l1d = 32u << 10;
  uint32_t l2_per_core = 512u << 10;
  uint32_t l3_per_ccx = 4u << 20;
  uint64_t freq_smt_off = 3800000000ULL;  // Series X
  uint64_t freq_smt_on = 3600000000ULL;
  bool smt_enabled = false;
};

const CpuTopology& cpu_topology(bool smt_on);
uint32_t cpu_game_threads(const CpuTopology& t);  // 7 or 14

// QPC shim: fixed 10MHz (Hyper-V style), steady_clock backed.
constexpr uint64_t kQpcFreq = 10000000ULL;
uint64_t cpu_qpc_freq();
uint64_t cpu_qpc_now();  // ticks @ kQpcFreq
uint64_t cpu_rdtsc_scaled(const CpuTopology& t);  // ns * freq, no RDTSC trap

// Demo guest payload: runs natively, issues fake syscalls.
void guest_demo_entry(TrapHandler trap);

// Host runner: invokes guest code in-process (sandboxed by convention only).
void cpu_run_native(TrapHandler trap);
