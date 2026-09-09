#include "vulkan_heaps.h"
#include <cstdio>

#include "gpu_batch.h"
#include "vulkan_alloc.h"
#include "vulkan_buffer.h"

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                   \
    }                                                             \
  } while (0)

int main() {
  // Honest environment gate: without a Vulkan physical device these
  // hardware assertions cannot run. Exit 77 = ctest "Skipped"; on a
  // GPU host the test runs for real.
  if (!vulkan_device_available()) {
    std::puts("SKIP: no Vulkan physical device on this host");
    return 77;
  }
  VulkanDev d;
  CHECK(vulkan_dev_init(d));

  // Root layout: 4 constants + 1 CBV + 2-descriptor table.
  GpuBatch batch;
  CHECK(batch.init(d, {{RootParamType::Constants32, 4},
                       {RootParamType::Cbv, 1},
                       {RootParamType::Table, 2}}));
  CHECK(batch.valid() && batch.push_size() == 4);

  // Bad layouts rejected.
  GpuBatch bad;
  CHECK(!bad.init(d, {}));
  CHECK(!bad.init(d, {{RootParamType::Constants32, 0}}));
  CHECK(!bad.init(d, {{RootParamType::Constants32, 64}}));

  VulkanBuffer a{}, b{}, c{};
  CHECK(vulkan_buffer_create(d, 1ULL << 16, false,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &a));
  CHECK(vulkan_buffer_create(d, 1ULL << 16, false,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &b));
  CHECK(vulkan_buffer_create(d, 1ULL << 16, false,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &c));
  CHECK(vulkan_buffer_fill_gpu(d, a, 0xA11CE000));

  // Push/bind validation (no cmd needed).
  const uint32_t kConsts[4] = {1, 2, 3, 4};
  CHECK(batch.push(0, kConsts, 4));
  CHECK(!batch.push(0, kConsts, 3));   // wrong count
  CHECK(!batch.push(1, kConsts, 1));   // not a constants root
  CHECK(!batch.push(9, kConsts, 4));   // out of range
  CHECK(batch.bind(1, a));
  CHECK(batch.bind(2, b));
  CHECK(!batch.bind(0, a));  // constants root takes push(), not bind()
  CHECK(!batch.bind(9, a));
  CHECK(!batch.copy(b, a));  // no open cmd yet

  // One batch, two copies: A->B and A->C in a single submit.
  GpuStub log;
  CHECK(batch.begin());
  CHECK(!batch.begin());  // already recording
  CHECK(batch.copy(b, a));
  CHECK(batch.copy(c, a));
  CHECK(batch.submit(&log));
  CHECK(batch.submits() == 1 && log.submitted() == 1);
  CHECK(!batch.submit(&log));  // nothing recording
  CHECK(vulkan_buffer_verify_mapped(d, b, 0xA11CE000));
  CHECK(vulkan_buffer_verify_mapped(d, c, 0xA11CE000));
  std::puts("batch 2-copy single-submit ok");

  vulkan_buffer_destroy(d, a);
  vulkan_buffer_destroy(d, b);
  vulkan_buffer_destroy(d, c);
  batch.shutdown();
  vulkan_dev_shutdown(d);
  std::puts("test_gpu_batch passed");
  return 0;
}
