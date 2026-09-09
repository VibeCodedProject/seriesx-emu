#include "cpu_native.h"

#include <chrono>

namespace {
CpuTopology kSmtOff{};
CpuTopology kSmtOn{};
bool kInit = false;

void ensure_init() {
  if (kInit) return;
  kSmtOff.smt_enabled = false;
  kSmtOff.logical = 8;
  kSmtOn.smt_enabled = true;
  kSmtOn.logical = 16;
  kInit = true;
}

uint64_t steady_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace

const CpuTopology& cpu_topology(bool smt_on) {
  ensure_init();
  return smt_on ? kSmtOn : kSmtOff;
}

uint32_t cpu_game_threads(const CpuTopology& t) {
  return t.smt_enabled ? t.game_cores * 2 : t.game_cores;
}

uint64_t cpu_qpc_freq() { return kQpcFreq; }

uint64_t cpu_qpc_now() {
  // 10 ticks per microsecond.
  return steady_ns() * kQpcFreq / 1000000000ULL;
}

uint64_t cpu_rdtsc_scaled(const CpuTopology& t) {
  const uint64_t freq = t.smt_enabled ? t.freq_smt_on : t.freq_smt_off;
  return steady_ns() * freq / 1000000000ULL;
}

// Runs directly on host CPU: no decode loop, same ISA (Zen2 guest -> host x86_64).
void guest_demo_entry(TrapHandler trap) {
  volatile uint64_t acc = 0;
  for (uint64_t i = 0; i < 1000; ++i) acc += i;
  if (trap) trap(static_cast<uint32_t>(SysId::Log), acc);
  if (trap) trap(static_cast<uint32_t>(SysId::GpuFlush), 0);
  if (trap) {
    const auto& topo = cpu_topology(false);
    trap(static_cast<uint32_t>(SysId::GetTopology), topo.game_cores);
    trap(static_cast<uint32_t>(SysId::GetQpcFreq), cpu_qpc_freq());
    trap(static_cast<uint32_t>(SysId::GetQpc), cpu_qpc_now());
    trap(static_cast<uint32_t>(SysId::GetRdtsc), cpu_rdtsc_scaled(topo));
  }
}

void cpu_run_native(TrapHandler trap) { guest_demo_entry(trap); }
