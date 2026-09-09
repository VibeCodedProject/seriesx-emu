#include <cstdio>

#include "gpu_stub.h"
#include "mem_map.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  MemMap m;
  CHECK(m.reserve());
  // Fast pool (GPU-optimal) + slow pool (standard) pages.
  CHECK(m.commit_range(0x10000, 4096));
  CHECK(m.commit_range(MemMap::kSlowBase + 0x10000, 8192));

  GpuStub gpu(&m);

  // Fill fast pool: Xbox GPU-optimal heap.
  XboxPacket fill{};
  fill.type = XboxPktType::FillMemory;
  fill.dst = XboxGpuAddr{.gpa = 0x10000,
                         .heap = XboxHeapClass::GpuOptimal,
                         .size = 4096};
  fill.pattern = 0xC0DE1234;
  CHECK(gpu.submit_xbox(fill));
  uint32_t v = 0;
  CHECK(m.read32(0x10000, v) && v == 0xC0DE1234u);
  CHECK(m.read32(0x10FFC, v) && v == 0xC0DE1234u);
  CHECK(gpu.bytes_filled() == 4096);

  // Heap-class violation: standard heap memory claimed as GPU-optimal.
  XboxPacket bad_heap = fill;
  bad_heap.dst = XboxGpuAddr{.gpa = MemMap::kSlowBase + 0x10000,
                             .heap = XboxHeapClass::GpuOptimal,
                             .size = 4096};
  CHECK(!gpu.submit_xbox(bad_heap));
  // And the reverse: fast memory claimed as standard.
  bad_heap.dst = XboxGpuAddr{.gpa = 0x10000,
                             .heap = XboxHeapClass::Standard,
                             .size = 4096};
  CHECK(!gpu.submit_xbox(bad_heap));

  // Copy fast -> slow with correct heap classes.
  XboxPacket copy{};
  copy.type = XboxPktType::CopyMemory;
  copy.src = XboxGpuAddr{.gpa = 0x10000,
                         .heap = XboxHeapClass::GpuOptimal,
                         .size = 1024};
  copy.dst = XboxGpuAddr{.gpa = MemMap::kSlowBase + 0x10000,
                         .heap = XboxHeapClass::Standard,
                         .size = 1024};
  CHECK(gpu.submit_xbox(copy));
  CHECK(m.read32(MemMap::kSlowBase + 0x10000, v) && v == 0xC0DE1234u);
  CHECK(gpu.bytes_copied() == 1024);
  // Size mismatch + self-overlap rejected.
  XboxPacket bad_copy = copy;
  bad_copy.dst.size = 512;
  CHECK(!gpu.submit_xbox(bad_copy));
  bad_copy = copy;
  bad_copy.dst = copy.src;
  CHECK(!gpu.submit_xbox(bad_copy));

  // ClearRenderTarget: 32x32 RGBA8 = 4096 bytes in fast pool.
  XboxPacket clear{};
  clear.type = XboxPktType::ClearRenderTarget;
  clear.dst = XboxGpuAddr{.gpa = 0x10000,
                          .heap = XboxHeapClass::GpuOptimal,
                          .size = 32 * 32 * 4};
  clear.color[0] = 0xFF;
  clear.color[1] = 0x00;
  clear.color[2] = 0xFF;
  clear.color[3] = 0xFF;
  clear.rt_width = 32;
  clear.rt_height = 32;
  CHECK(gpu.submit_xbox(clear));
  CHECK(m.read32(0x10000, v) && v == 0xFFFF00FFu);
  CHECK(gpu.clears() == 1);

  // Fences are monotonic; regression rejected.
  XboxPacket fence{};
  fence.type = XboxPktType::FenceSignal;
  fence.fence_value = 10;
  CHECK(gpu.submit_xbox(fence));
  CHECK(gpu.fence_signalled() == 10 && gpu.fence_completed() == 10);
  gpu.gpu_complete(10);
  CHECK(gpu.fence_completed() == 10);
  fence.fence_value = 10;
  CHECK(!gpu.submit_xbox(fence));
  fence.fence_value = 11;
  CHECK(gpu.submit_xbox(fence));

  // Flip bumps frame id with a QPC stamp; timestamp logs QPC.
  CHECK(gpu.frame_id() == 0);
  XboxPacket flip{};
  flip.type = XboxPktType::Flip;
  CHECK(gpu.submit_xbox(flip));
  CHECK(gpu.frame_id() == 1 && gpu.last_flip_qpc() != 0);
  XboxPacket ts{};
  ts.type = XboxPktType::Timestamp;
  CHECK(gpu.submit_xbox(ts));
  CHECK(gpu.timestamps().size() == 1);

  // Alias elsewhere must not redirect unrelated addresses (regression:
  // resolve_alias underflow sent high GPAs into the alias source).
  CHECK(m.commit_range(0x40000, 4096));
  CHECK(m.commit_range(0x50000, 4096));
  CHECK(m.alias_map(0x50000, 0x40000, 4096));
  XboxPacket far_fill = fill;
  far_fill.dst.gpa = 0x10000;
  CHECK(gpu.submit_xbox(far_fill));
  CHECK(m.alias_unmap(0x50000));

  // Uncommitted + OOB fail safely and count errors.
  const uint64_t errs = gpu.errors();
  XboxPacket oob = fill;
  oob.dst = XboxGpuAddr{.gpa = MemMap::kTotalSize - 4,
                        .heap = XboxHeapClass::Standard,
                        .size = 16};
  CHECK(!gpu.submit_xbox(oob));
  CHECK(gpu.errors() == errs + 1);
  CHECK(gpu.submitted() == 14);

  std::puts("test_xbox_gpu passed");
  return 0;
}
