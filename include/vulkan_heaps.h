#pragma once
#include <cstdint>
#include <string>

// Real Vulkan heap sizes for sizing the 10GB fast / 6GB slow split.
// fast -> DEVICE_LOCAL VRAM, slow -> HOST_VISIBLE GTT.
struct VulkanHeapInfo {
  std::string device_name;
  uint64_t vram_bytes = 0;  // largest DEVICE_LOCAL heap
  uint64_t gtt_bytes = 0;   // largest non-DEVICE_LOCAL heap
  bool rebar = false;       // DEVICE_LOCAL + HOST_VISIBLE type exists
  bool found = false;
};

// Queries first discrete GPU (prefers AMD 0x1002). Returns false if no Vulkan.
bool query_vulkan_heaps(VulkanHeapInfo& out);

// Honest environment probe: true only if a Vulkan loader is present AND
// at least one physical device exists. GPU-dependent tests use this to
// skip (exit code 77, ctest "Skipped") on GPU-less hosts instead of
// failing -- they still run for real wherever a GPU is present.
bool vulkan_device_available();

// True if Xbox split fits: 10GB in VRAM, 6GB in GTT.
inline bool xbox_split_fits(const VulkanHeapInfo& h) {
  return h.found && h.vram_bytes >= (10ULL << 30) && h.gtt_bytes >= (6ULL << 30);
}
