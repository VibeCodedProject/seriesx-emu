#include <cstdio>
#include <filesystem>
#include <vector>

#include "gpu_stub.h"
#include "mem_map.h"
#include "xbox_display.h"

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

  XboxDisplay disp(&m);
  CHECK(!disp.has_framebuffer());
  CHECK(!disp.present());  // no fb -> honest failure
  CHECK(disp.errors() == 1);

  // Reject bad modes before touching memory.
  CHECK(!disp.set_framebuffer(0x10000, 0, 32));
  CHECK(!disp.set_framebuffer(0x10000, 32, 0));
  CHECK(!disp.set_framebuffer(0x10000, 4096, 4096));
  // Uncommitted range rejected.
  CHECK(!disp.set_framebuffer(0x10000, 32, 32));

  // 32x32 RGBA8 = 4096 bytes in the fast pool.
  CHECK(m.commit_range(0x10000, 32 * 32 * 4));
  CHECK(disp.set_framebuffer(0x10000, 32, 32));
  CHECK(disp.has_framebuffer());
  CHECK(disp.fb_gpa() == 0x10000);
  CHECK(disp.fb_bytes() == 32u * 32u * 4u);
  CHECK(!disp.has_frame());

  // Paint through the real GPU path: ClearRenderTarget magenta.
  GpuStub gpu(&m);
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

  // Flip hook snapshots the guest frontbuffer.
  CHECK(disp.on_flip());
  CHECK(disp.frames_presented() == 1);
  CHECK(disp.last_present_qpc() != 0);
  CHECK(disp.has_frame());
  CHECK(disp.frame().size() == 32u * 32u * 4u);
  uint32_t px = 0;
  CHECK(disp.pixel(0, 0, px) && px == 0xFFFF00FFu);
  CHECK(disp.pixel(31, 31, px) && px == 0xFFFF00FFu);
  CHECK(!disp.pixel(32, 0, px));
  CHECK(disp.checksum() != 0);

  // Mutating guest memory without a Flip must not move the snapshot.
  const uint64_t before = disp.checksum();
  CHECK(m.write32(0x10000, 0x00000000u));
  CHECK(disp.checksum() == before);
  CHECK(disp.present());
  CHECK(disp.frames_presented() == 2);
  CHECK(disp.pixel(0, 0, px) && px == 0x00000000u);
  CHECK(disp.checksum() != before);

  // Headless screenshot: deterministic PPM dump to the system temp dir.
  const auto ppm =
      (std::filesystem::temp_directory_path() / "seriesx-emu_display_test.ppm")
          .string();
  CHECK(disp.save_ppm(ppm));
  std::FILE* f = std::fopen(ppm.c_str(), "rb");
  CHECK(f != nullptr);
  char magic[3] = {0};
  CHECK(std::fread(magic, 1, 2, f) == 2 && magic[0] == 'P' && magic[1] == '6');
  std::fclose(f);
  std::remove(ppm.c_str());

  // Slow-pool frontbuffer also works (any committed readable range).
  CHECK(m.commit_range(MemMap::kSlowBase + 0x20000, 16 * 16 * 4));
  CHECK(disp.set_framebuffer(MemMap::kSlowBase + 0x20000, 16, 16));
  CHECK(!disp.has_frame());  // set clears the snapshot
  XboxPacket fill{};
  fill.type = XboxPktType::FillMemory;
  fill.dst = XboxGpuAddr{.gpa = MemMap::kSlowBase + 0x20000,
                         .heap = XboxHeapClass::Standard,
                         .size = 16 * 16 * 4};
  fill.pattern = 0x11223344u;
  CHECK(gpu.submit_xbox(fill));
  CHECK(disp.present());
  CHECK(disp.pixel(0, 0, px) && px == 0x11223344u);

  // Unreadable (protected) range fails honestly, keeps old snapshot.
  CHECK(m.protect(MemMap::kSlowBase + 0x20000, 4096, MemProt::NoAccess));
  const uint64_t errs = disp.errors();
  const uint64_t frames = disp.frames_presented();
  CHECK(!disp.present());
  CHECK(disp.errors() == errs + 1);
  CHECK(disp.frames_presented() == frames);
  CHECK(m.protect(MemMap::kSlowBase + 0x20000, 4096, MemProt::ReadWrite));

  disp.clear_framebuffer();
  CHECK(!disp.has_framebuffer() && !disp.has_frame());
  CHECK(!disp.present());

  std::puts("test_xbox_display passed");
  return 0;
}
