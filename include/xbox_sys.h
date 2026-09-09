#pragma once
#include <cstdint>
#include <string>

// Xbox Series X|S system services HLE (homebrew only).
//
// Covers what titles actually query at boot: processor topology with the
// reserved system core, memory split status, title identity, QPC/system
// time. Pure logic over cpu_native.h + mem_map.h; no GameOS calls.

struct XboxMemStatus {
  uint64_t total_bytes = 16ULL << 30;
  uint64_t gpu_optimal_bytes = 10ULL << 30;
  uint64_t standard_bytes = 6ULL << 30;
  uint64_t committed_pages = 0;
  uint64_t committed_bytes = 0;
};

// Affinity masks: bit i == core i. Core 7 is the system core, never in
// the game mask. SMT-on exposes 16 logical threads (2 per core).
struct XboxAffinity {
  uint64_t game_mask = 0;    // phys cores available to the title
  uint64_t system_mask = 0;  // reserved core(s)
  uint32_t game_threads = 0;
};

XboxAffinity xbox_game_affinity(bool smt_on);
bool xbox_affinity_is_game_core(unsigned core, bool smt_on);
XboxMemStatus xbox_mem_status(uint64_t committed_pages, uint64_t page_size);

// Title identity: always the synthetic homebrew title (no retail titles,
// no package loading).
constexpr uint32_t kXboxHomebrewTitleId = 0xFFFF0001u;
const std::string& xbox_title_name();

// System time as Windows FILETIME (100ns ticks since 1601-01-01), derived
// from the host clock. QPC lives in cpu_native.h (10MHz Hyper-V style).
uint64_t xbox_system_time_filetime();
uint64_t xbox_filetime_to_unix100ns(uint64_t filetime);
