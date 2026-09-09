#include <chrono>
#include <cstdio>
#include <thread>

#include "cpu_native.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  const auto& off = cpu_topology(false);
  const auto& on = cpu_topology(true);
  CHECK(off.phys == 8 && off.logical == 8 && !off.smt_enabled);
  CHECK(on.phys == 8 && on.logical == 16 && on.smt_enabled);
  CHECK(cpu_game_threads(off) == 7);
  CHECK(cpu_game_threads(on) == 14);
  CHECK(off.l3_per_ccx == (4u << 20));
  CHECK(off.freq_smt_off == 3800000000ULL);

  CHECK(cpu_qpc_freq() == 10000000ULL);
  const uint64_t a = cpu_qpc_now();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const uint64_t b = cpu_qpc_now();
  CHECK(b > a);  // monotonic

  const uint64_t t0 = cpu_rdtsc_scaled(off);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const uint64_t t1 = cpu_rdtsc_scaled(off);
  const uint64_t dt = t1 - t0;
  // 2ms @3.8GHz ~= 7.6M cycles, allow wide tolerance for CI jitter.
  CHECK(dt > 3000000ULL && dt < 20000000ULL);

  // Trap table: demo must issue Log + GpuFlush + 4 new IDs.
  int seen_log = 0, seen_flush = 0, seen_topo = 0, seen_qpc = 0;
  cpu_run_native([&](uint32_t id, uint64_t arg) {
    const auto s = static_cast<SysId>(id);
    if (s == SysId::Log && arg == 499500) ++seen_log;
    if (s == SysId::GpuFlush) ++seen_flush;
    if (s == SysId::GetTopology && arg == 7) ++seen_topo;
    if (s == SysId::GetQpcFreq && arg == kQpcFreq) ++seen_qpc;
  });
  CHECK(seen_log == 1 && seen_flush == 1 && seen_topo == 1 && seen_qpc == 1);

  std::puts("test_cpu passed");
  return 0;
}
