#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Xbox Series X|S GPU front-end (HLE, homebrew only).
//
// Real HW: RDNA2 DX12U command processor fed by D3D12 command lists.
// Here: Xbox-semantic packets validated against the 10GB GPU-optimal
// (fast) / 6GB standard (slow) split, executed against MemMap so the
// command processor is testable without a GPU. The Vulkan backend
// (vulkan_buffer / vulkan_image / gpu_batch) is the PC remap target;
// this class is the Xbox side that feeds it.
//
// GPU VA == guest GPA in this model, with a heap-class check:
//   GpuOptimal (default) must land in the fast pool,
//   Standard must land in the slow pool.
// That is the Series X rule that generic Vulkan testers skip.
class MemMap;

enum class XboxHeapClass : uint8_t { GpuOptimal = 0, Standard = 1 };

struct XboxGpuAddr {
  uint64_t gpa = 0;                       // guest physical == GPU VA
  XboxHeapClass heap = XboxHeapClass::GpuOptimal;
  uint32_t size = 0;
};

enum class XboxPktType : uint32_t {
  Nop = 0,
  FillMemory = 1,      // fill [gpa, gpa+size) with u32 pattern
  CopyMemory = 2,      // copy src -> dst, sizes must match
  ClearRenderTarget = 3,  // clear [gpa, gpa+w*h*4) to RGBA8 color
  FenceSignal = 4,     // signal monotonic fence value
  Flip = 5,            // present: bump frame id, latch flip QPC
  Timestamp = 6,       // capture QPC into timestamps_ log
};

// Keep wire-compat with the old GpuStub packet log.
struct GpuPacket {
  uint32_t type = 0;
  uint64_t addr = 0;
  uint32_t size = 0;
};

struct XboxPacket {
  XboxPktType type = XboxPktType::Nop;
  XboxGpuAddr dst{};   // Fill/Clear dst, Copy dst
  XboxGpuAddr src{};   // Copy src
  uint32_t pattern = 0;       // Fill: u32 fill value
  uint8_t color[4] = {};      // ClearRenderTarget: RGBA8
  uint32_t rt_width = 0;      // ClearRenderTarget: pixels
  uint32_t rt_height = 0;
  uint64_t fence_value = 0;   // FenceSignal: must be > last signalled
};

class GpuStub {
 public:
  explicit GpuStub(MemMap* mem = nullptr) : mem_(mem) {}
  void attach(MemMap* mem) { mem_ = mem; }

  // Legacy log-only submit (kept for old tests/demo). Nop-equivalent.
  void submit(const GpuPacket& pkt);

  // Real path: validate + execute one Xbox packet. Returns false on
  // heap-class violation, uncommitted/OOB memory, fence regression,
  // or zero-size fill/copy/clear. Flip/Timestamp/Nop always succeed.
  bool submit_xbox(const XboxPacket& pkt);

  uint64_t submitted() const { return submitted_; }
  std::string last_log() const { return last_log_; }

  // Xbox processor state.
  uint64_t fence_signalled() const { return fence_signalled_; }
  uint64_t fence_completed() const { return fence_completed_; }
  uint64_t frame_id() const { return frame_id_; }
  uint64_t bytes_filled() const { return bytes_filled_; }
  uint64_t bytes_copied() const { return bytes_copied_; }
  uint64_t clears() const { return clears_; }
  uint64_t errors() const { return errors_; }
  uint64_t last_flip_qpc() const { return last_flip_qpc_; }
  const std::vector<uint64_t>& timestamps() const { return timestamps_; }

  // Test hook: GPU signals completion up to a fence value.
  void gpu_complete(uint64_t value);

 private:
  bool heap_ok(const XboxGpuAddr& a) const;
  bool commit_ok(const XboxGpuAddr& a) const;

  MemMap* mem_ = nullptr;
  uint64_t submitted_ = 0;
  std::string last_log_;
  uint64_t fence_signalled_ = 0;
  uint64_t fence_completed_ = 0;
  uint64_t frame_id_ = 0;
  uint64_t bytes_filled_ = 0;
  uint64_t bytes_copied_ = 0;
  uint64_t clears_ = 0;
  uint64_t errors_ = 0;
  uint64_t last_flip_qpc_ = 0;
  std::vector<uint64_t> timestamps_;
};
