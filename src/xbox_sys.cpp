#include "xbox_sys.h"

#include <chrono>

#include "cpu_native.h"

XboxAffinity xbox_game_affinity(bool smt_on) {
  const CpuTopology& t = cpu_topology(smt_on);
  XboxAffinity a{};
  // Game cores are the first `game_cores` phys cores; system owns the rest.
  for (uint32_t c = 0; c < t.game_cores && c < t.phys; ++c)
    a.game_mask |= 1ULL << c;
  for (uint32_t c = t.game_cores; c < t.phys; ++c) a.system_mask |= 1ULL << c;
  a.game_threads = cpu_game_threads(t);
  return a;
}

bool xbox_affinity_is_game_core(unsigned core, bool smt_on) {
  const CpuTopology& t = cpu_topology(smt_on);
  if (smt_on) core /= 2;  // two logical threads per phys core
  return core < t.game_cores;
}

XboxMemStatus xbox_mem_status(uint64_t committed_pages, uint64_t page_size) {
  XboxMemStatus s;
  s.committed_pages = committed_pages;
  s.committed_bytes = committed_pages * page_size;
  return s;
}

const std::string& xbox_title_name() {
  static const std::string kName = "HOME-BREW";
  return kName;
}

// FILETIME epoch delta: 1601-01-01 -> 1970-01-01 = 11644473600 seconds.
constexpr uint64_t kFiletimeEpochDelta100ns = 11644473600ULL * 10000000ULL;

uint64_t xbox_system_time_filetime() {
  const uint64_t unix_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  return unix_ns / 100ULL + kFiletimeEpochDelta100ns;
}

uint64_t xbox_filetime_to_unix100ns(uint64_t filetime) {
  return filetime >= kFiletimeEpochDelta100ns
             ? filetime - kFiletimeEpochDelta100ns
             : 0;
}
