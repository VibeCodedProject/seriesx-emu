#include <cstdint>
#include <cstdio>

#include "mem_map.h"
#include "gpu_stub.h"

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      return 1;                                                     \
    }                                                               \
  } while (0)

int main() {
  MemMap m;
  CHECK(m.reserve());
  CHECK(m.in_range(0));
  CHECK(m.in_range(MemMap::kTotalSize - 4, 4));
  CHECK(!m.in_range(MemMap::kTotalSize - 3, 4));
  CHECK(m.is_fast(0));
  CHECK(m.is_fast(MemMap::kFastSize - 1));
  CHECK(!m.is_fast(MemMap::kSlowBase));

  CHECK(m.commit_page(0x2000));
  CHECK(m.commit_page(MemMap::kSlowBase + 0x2000));
  CHECK(m.write32(0x2000, 12345));
  uint32_t v = 0;
  CHECK(m.read32(0x2000, v) && v == 12345);
  CHECK(!m.write32(MemMap::kTotalSize, 1));  // out of range

  // Uncommitted access must fail safely (no PROT_NONE fault).
  uint32_t unused = 0;
  CHECK(!m.is_committed(0x4000));
  CHECK(m.translate(0x4000) == nullptr);
  CHECK(!m.read32(0x4000, unused));
  CHECK(!m.write32(0x4000, 1));

  // Range commit + bytes round-trip across a page boundary.
  CHECK(m.commit_range(0x5000, 8192));
  CHECK(m.is_committed(0x5000, 8192));
  CHECK(m.committed_pages() >= 4);
  const char msg[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  CHECK(m.write_bytes(0x5FFC, msg, sizeof(msg)));  // straddles pages
  char back[8] = {};
  CHECK(m.read_bytes(0x5FFC, back, sizeof(back)));
  for (int i = 0; i < 8; ++i) CHECK(back[i] == msg[i]);

  // Overflow-safe range check.
  CHECK(!m.in_range(UINT64_MAX, 4));
  CHECK(!m.in_range(MemMap::kTotalSize - 3, 4));

  GpuStub g;
  g.submit({.type = 7, .addr = 0x2000, .size = 16});
  CHECK(g.submitted() == 1);

  std::puts("test_mem passed");
  return 0;
}
