#include <cstdio>
#include <vector>

#include "cpu_native.h"
#include "cpu_sched.h"
#include "fiber_tls.h"
#include "gpu_batch.h"
#include "gpu_stub.h"
#include "io_queue.h"
#include "mem_map.h"
#include "vulkan_alloc.h"
#include "vulkan_buffer.h"
#include "vulkan_heaps.h"
#include "vulkan_image.h"
#include "xbe_loader.h"
#include "xbox_audio.h"
#include "xbox_input.h"
#include "xbox_krnl.h"
#include "xbox_sys.h"
#include "xbox_vfs.h"

#ifdef UNICORN_ENABLED
#include "xbox_cpu.h"  // optional Unicorn x86-32 CPU core (GPL-2.0 dep)
#endif

int main() {
  VulkanHeapInfo h;
  if (query_vulkan_heaps(h)) {
    std::printf("vulkan gpu=%s\n  vram=%.2f GiB gtt=%.2f GiB rebar=%d fits10_6=%d\n",
                h.device_name.c_str(), h.vram_bytes / 1073741824.0,
                h.gtt_bytes / 1073741824.0, h.rebar, xbox_split_fits(h));
  } else {
    std::puts("vulkan query failed, using logical 10/6 split only");
  }
  // Real VRAM proof: 64MB fast + 16MB slow.
  {
    VulkanDev vd;
    if (vulkan_dev_init(vd)) {
      VkDeviceMemory fast_mem = VK_NULL_HANDLE, slow_mem = VK_NULL_HANDLE;
      const bool ok_fast = vulkan_alloc(vd, 64ULL << 20, true, &fast_mem);
      const bool ok_slow = vulkan_alloc(vd, 16ULL << 20, false, &slow_mem);
      std::printf("vulkan alloc fast64MB=%d slow16MB=%d fast_mappable=%d\n",
                  ok_fast, ok_slow, vd.fast_mappable);
      if (ok_slow)
        std::printf("  slow write/read=%d\n",
                    vulkan_write_read(vd, slow_mem, 16ULL << 20, vd.slow_type,
                                      0xA5A50000));
      if (ok_fast && vd.fast_mappable)
        std::printf("  fast write/read=%d\n",
                    vulkan_write_read(vd, fast_mem, 64ULL << 20, vd.fast_type,
                                      0xC0DE0000));
      if (ok_fast) vkFreeMemory(vd.dev, fast_mem, nullptr);
      if (ok_slow) vkFreeMemory(vd.dev, slow_mem, nullptr);
      // Buffer + queue submit proof: GPU fill on slow, copy fast->slow.
      VulkanBuffer slow_buf{}, fast_buf{};
      const bool ok_slow_buf = vulkan_buffer_create(
          vd, 1ULL << 20, false, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slow_buf);
      const bool ok_fast_buf = vulkan_buffer_create(
          vd, 1ULL << 20, true, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &fast_buf);
      std::printf("vulkan buffer slow1MB=%d fast1MB=%d queue=%p\n",
                  ok_slow_buf, ok_fast_buf, (void*)vd.queue);
      if (ok_slow_buf) {
        const bool filled = vulkan_buffer_fill_gpu(vd, slow_buf, 0xDEADBEEF);
        const bool verified =
            filled && vulkan_buffer_verify_mapped(vd, slow_buf, 0xDEADBEEF);
        std::printf("  slow gpu-fill+verify=%d\n", verified);
      }
      if (ok_slow_buf && ok_fast_buf) {
        bool ok = vulkan_buffer_fill_gpu(vd, fast_buf, 0x12340000);
        ok = ok && vulkan_buffer_copy(vd, slow_buf, fast_buf);
        ok = ok && vulkan_buffer_verify_mapped(vd, slow_buf, 0x12340000);
        std::printf("  fast fill + copy + verify=%d\n", ok);
      }
      if (ok_slow_buf) vulkan_buffer_destroy(vd, slow_buf);
      if (ok_fast_buf) vulkan_buffer_destroy(vd, fast_buf);
      // Image + layout transition proof: 512x512 clear -> copy -> verify.
      VulkanImage img{};
      VulkanBuffer img_stage{};
      if (vulkan_image_create_2d(vd, 512, 512, VK_FORMAT_R8G8B8A8_UNORM,
                                 true, &img) &&
          vulkan_buffer_create(vd, 1ULL << 20, false,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, &img_stage)) {
        VkClearColorValue c{};
        c.float32[0] = 1.0f;
        c.float32[2] = 1.0f;
        c.float32[3] = 1.0f;
        const bool ok =
            vulkan_image_clear_color(vd, img, c) &&
            vulkan_image_copy_to_buffer(vd, img, img_stage) &&
            vulkan_buffer_verify_mapped(vd, img_stage, 0xFFFF00FFu);
        std::printf("vulkan image 512x512 clear+copy+verify=%d layout=%d\n",
                    ok, (int)img.layout);
      }
      vulkan_buffer_destroy(vd, img_stage);
      vulkan_image_destroy(vd, img);
      // GpuBatch proof: Xbox GPU VAs + root layout + copy submit.
      // Buffers are registered at guest GPAs so binds/copies go through
      // the Xbox heap-class check, then execute as real Vulkan copies.
      VulkanBuffer ba{}, bb{};
      if (vulkan_buffer_create(vd, 1ULL << 16, false,
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ba) &&
          vulkan_buffer_create(vd, 1ULL << 16, false,
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &bb) &&
          vulkan_buffer_fill_gpu(vd, ba, 0xB47C4000)) {
        GpuStub batch_log;
        GpuBatch batch;
        const uint32_t pc[2] = {7, 8};
        const bool ok =
            batch.init(vd, {{RootParamType::Constants32, 2},
                            {RootParamType::Cbv, 1}}) &&
            batch.push(0, pc, 2) && batch.bind(1, ba) && batch.begin() &&
            batch.copy(bb, ba) && batch.submit(&batch_log) &&
            vulkan_buffer_verify_mapped(vd, bb, 0xB47C4000);
        std::printf("vulkan batch copy+verify=%d submits=%llu fence=%llu log=%s\n",
                    ok, (unsigned long long)batch.submits(),
                    (unsigned long long)batch.fence_signalled(),
                    batch_log.last_log().c_str());
      }
      vulkan_buffer_destroy(vd, ba);
      vulkan_buffer_destroy(vd, bb);
      vulkan_dev_shutdown(vd);
    } else {
      std::puts("vulkan device init failed");
    }
  }
  MemMap mem;
  if (!mem.reserve()) {
    std::puts("mem reserve failed");
    return 1;
  }
  // Demo: commit one fast page + one slow page, round-trip u32.
  mem.commit_page(0x1000);
  mem.commit_page(MemMap::kSlowBase + 0x1000);
  mem.write32(0x1000, 0xC0DECAFE);
  uint32_t v = 0;
  mem.read32(0x1000, v);
  std::printf("fast[0x1000] = 0x%x (is_fast=%d)\n", v,
              mem.is_fast(0x1000));
  std::printf("slow base 0x%llx is_fast=%d\n",
              (unsigned long long)MemMap::kSlowBase,
              mem.is_fast(MemMap::kSlowBase + 0x1000));

  // Phase 3 proof: homebrew VFS -> async IoQueue -> slow pool.
  {
    Vfs fs;
    const std::vector<uint8_t> payload = {9, 8, 7, 6, 5, 4, 3, 2};
    fs.create_file("S:\\hello.bin", payload);
    const uint64_t dst = MemMap::kSlowBase + 0x2000;
    mem.commit_page(dst);
    IoQueue io(&mem, &fs);
    const bool ok =
        io.enqueue("S:\\hello.bin", 0, payload.size(), dst, 42) &&
        io.submit();
    io.wait_all();
    std::vector<uint8_t> got(payload.size(), 0);
    const bool landed = ok && io.status(42) == IoQueue::Status::Done &&
                        mem.read_bytes(dst, got.data(), got.size()) &&
                        got == payload;
    // Protect + alias proof: RO blocks write, alias mirrors bytes.
    mem.commit_page(0x4000);
    mem.commit_page(0x5000);
    mem.write32(0x4000, 0xBEEFu);
    mem.protect(0x4000, 4096, MemProt::ReadOnly);
    const bool ro_blocked = !mem.write32(0x4000, 1u);
    mem.protect(0x4000, 4096, MemProt::ReadWrite);
    const bool aliased =
        mem.alias_map(0x5000, 0x4000, 4096) && mem.write32(0x5000, 0xCAFEu);
    uint32_t av = 0;
    const bool mirror = aliased && mem.read32(0x4000, av) && av == 0xCAFEu;
    std::printf("vfs/io landed=%d ro_blocked=%d alias_mirror=%d\n", landed,
                ro_blocked, mirror);
  }

  // Xbox GPU command processor: fill/copy/fence/flip against MemMap.
  {
    GpuStub xgpu(&mem);
    mem.commit_range(0x80000, 4096);
    mem.commit_range(MemMap::kSlowBase + 0x80000, 4096);
    XboxPacket fill{.type = XboxPktType::FillMemory,
                    .dst = {.gpa = 0x80000,
                            .heap = XboxHeapClass::GpuOptimal,
                            .size = 4096},
                    .pattern = 0x58B0C5u};
    XboxPacket copy{.type = XboxPktType::CopyMemory,
                    .dst = {.gpa = MemMap::kSlowBase + 0x80000,
                            .heap = XboxHeapClass::Standard,
                            .size = 1024},
                    .src = {.gpa = 0x80000,
                            .heap = XboxHeapClass::GpuOptimal,
                            .size = 1024}};
    XboxPacket fence{.type = XboxPktType::FenceSignal, .fence_value = 7};
    XboxPacket flip{.type = XboxPktType::Flip};
    const bool ok = xgpu.submit_xbox(fill) && xgpu.submit_xbox(copy) &&
                    xgpu.submit_xbox(fence) && xgpu.submit_xbox(flip);
    std::printf("xgpu fill/copy/fence/flip=%d frame=%llu fence=%llu log=%s\n",
                ok, (unsigned long long)xgpu.frame_id(),
                (unsigned long long)xgpu.fence_signalled(),
                xgpu.last_log().c_str());
  }

  // Real guest-thread scheduling: ucontext fibers with genuine
  // context switches (see cpu_sched.h + test_cpu_sched).
  {
    GuestScheduler sched;
    int ping = 0, pong = 0;
    uint64_t t1 = 0, t2 = 0;
    t1 = sched.spawn("ping", [&](GuestThread& t) {
      for (int i = 0; i < 4; ++i) {
        ++ping;
        if (i < 3) t.yield();  // one real swapcontext per yield
      }
    });
    t2 = sched.spawn("pong", [&](GuestThread& t) {
      for (int i = 0; i < 4; ++i) {
        ++pong;
        if (i < 3) t.yield();
      }
    });
    sched.run_until_complete();
    std::printf("sched ping=%d pong=%d switches=%llu (t1=%llu t2=%llu)\n",
                ping, pong, (unsigned long long)sched.switches_total(),
                (unsigned long long)sched.find(t1)->switches(),
                (unsigned long long)sched.find(t2)->switches());
  }

  // Xbox sys/input/audio HLE state.
  {
    const XboxAffinity aff = xbox_game_affinity(false);
    const XboxMemStatus mst = xbox_mem_status(mem.committed_pages(), 4096);
    XboxInput pad;
    XboxGamepad raw{};
    raw.buttons = kXboxBtnA;
    raw.thumb_rx = 20000;
    pad.set_state(0, raw);
    XboxGamepad cooked{};
    pad.get_state(0, cooked);
    XboxAudio au;
    float phase = 0.0f;
    au.submit_buffer(XboxAudio::render_sine(440.0f, 4800, phase));
    const size_t played = au.consume_frames(4800);
    std::printf("xsys game_mask=0x%llx threads=%u committed=%lluKB "
                "padA=%d stick=%d audio_played=%zu title=%s\n",
                (unsigned long long)aff.game_mask, aff.game_threads,
                (unsigned long long)(mst.committed_bytes >> 10),
                (cooked.buttons & kXboxBtnA) != 0, cooked.thumb_rx,
                played, xbox_title_name().c_str());
  }

  // XBE binary loader: parse + map a real homebrew Xbox executable
  // (LithiumX, nxdk-built, MIT licensed; see tests/lithiumx.xbe.*).
  // Then run two fibers through the Xbox TLS model with per-fiber
  // storage carved from their own stacks.
  {
    const char* paths[] = {"tests/lithiumx.xbe", "../tests/lithiumx.xbe",
                           "seriesx-emu/tests/lithiumx.xbe"};
    std::vector<uint8_t> file;
    for (const char* p : paths) {
      std::FILE* f = std::fopen(p, "rb");
      if (!f) continue;
      std::fseek(f, 0, SEEK_END);
      const long sz = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      file.resize(static_cast<size_t>(sz));
      const size_t got = std::fread(file.data(), 1, file.size(), f);
      std::fclose(f);
      if (got == file.size() && !file.empty()) break;
      file.clear();
    }
    if (file.empty()) {
      std::puts("xbe loader: fixture not found (skipping demo)");
    } else {
      std::string err;
      auto lx = xbe::LoadedXbe::load(file, &err);
      if (!lx) {
        std::printf("xbe loader FAILED: %s\n", err.c_str());
      } else {
        const auto& h = lx->image().header();
        const xbe::XbeType t = lx->image().type();
        // Title: UTF-16LE certificate string -> ASCII.
        std::string title;
        for (char16_t c : lx->image().certificate().title) title.push_back(static_cast<char>(c));
        std::printf(
            "xbe loaded: title=%s type=%s base=0x%X image=0x%X sections=%u\n",
            title.c_str(), t == xbe::XbeType::Retail ? "retail" : "debug",
            h.base_addr, h.sizeof_image, h.num_sections);
        std::printf(
            "  entry=0x%X (xor-decoded) tls=%s thunks=%zu stack_commit=0x%X\n",
            lx->image().entry_va(), lx->has_tls() ? "yes" : "no",
            lx->kernel_thunk_ordinals().size(), h.pe_stack_commit);
        for (const auto& s : lx->image().sections()) {
          std::printf("  sec %-8s va=0x%06X vsz=0x%06X raw=0x%06X fl=%X%s\n",
                      s.name.c_str(), s.virtual_addr, s.virtual_size,
                      s.sizeof_raw, s.flags,
                      (s.flags & xbe::kSectionWriteable) ? " rw" : " ro");
        }
        // Guest TLS: write the index into the image (what guest code
        // reads back), feed the template to the fiber runtime.
        if (lx->has_tls()) {
          FiberTls::Init init;
          init.raw = lx->tls_template();
          init.zero_fill = lx->image().tls().zero_fill;
          init.index = 0;
          init.callbacks = lx->tls_callbacks();
          lx->write_tls_index(init.index);

          GuestScheduler sched;
          FiberTls tls(sched);
          tls.set_init(std::move(init));
          int total_writes = 0;
          uint64_t fm = 0, fw = 0;
          fm = tls.spawn_with_tls("game.main", [&](GuestThread& th) {
            auto* v = static_cast<uint32_t*>(tls.current_data(0, 4));
            if (v) { *v = 0x11111111u; ++total_writes; }
            th.yield();  // real context switch; block must follow the fiber
            if (v && *v == 0x11111111u) ++total_writes;
          });
          fw = tls.spawn_with_tls("game.work", [&](GuestThread& th) {
            auto* v = static_cast<uint32_t*>(tls.current_data(4, 4));
            if (v) { *v = 0x22222222u; ++total_writes; }
            th.yield();
            if (v && *v == 0x22222222u) ++total_writes;
          });
          sched.run_until_complete();
          std::printf("  fiber tls: main=0x%llx work=0x%llx writes=%d "
                      "events=%zu (attach+detach records)\n",
                      (unsigned long long)fm, (unsigned long long)fw,
                      total_writes, tls.events().size());
        }

        // Kernel HLE: resolve + patch LithiumX's real kernel thunk table,
        // then drive implemented exports through the dispatch boundary
        // (call() is the same boundary the Unicorn CPU core crosses).
        {
          GuestScheduler ksched;
          krnl::XboxKrnl krnl(lx->ram(), ksched);
          const auto slots = krnl.install_thunks(*lx);
          uint32_t impl = 0;
          for (const auto& s : slots) impl += s.implemented ? 1u : 0u;
          std::printf("  kernel hle: %zu thunks resolved, %u implemented "
                      "(table: %zu exports)\n",
                      slots.size(), impl, krnl::export_count());
          krnl.set_dbg_sink([](const std::string& s) {
            std::printf("    [DbgPrint] %s\n", s.c_str());
          });
          auto callk = [&krnl](uint32_t ord,
                               std::initializer_list<uint32_t> a) {
            std::vector<uint32_t> args(a);
            return krnl.call(ord, args.data(),
                             static_cast<uint32_t>(args.size()));
          };
          // Guest memory from the guest's own allocator (ordinal 165).
          const uint32_t cs_va = callk(165, {64});
          callk(291, {cs_va});  // RtlInitializeCriticalSection
          ksched.spawn("game.main", [&](GuestThread&) {
            callk(277, {cs_va});  // RtlEnterCriticalSection
            const uint32_t fmt = callk(165, {48});
            lx->ram().write_bytes(fmt, "main: holding cs, delaying", 29);
            callk(8, {fmt});  // DbgPrint (cdecl, guest string)
            const uint64_t one_ms = static_cast<uint64_t>(-10000);
            const uint32_t tv = callk(165, {8});
            lx->ram().write_bytes(tv, &one_ms, 8);
            callk(99, {0, 0, tv});  // KeDelayExecutionThread: 1 real tick
            callk(294, {cs_va});    // RtlLeaveCriticalSection -> wakes worker
          });
          ksched.spawn("game.work", [&](GuestThread&) {
            callk(277, {cs_va});  // blocks for real until main leaves
            const uint32_t fmt = callk(165, {48});
            lx->ram().write_bytes(fmt, "work: acquired cs", 18);
            callk(8, {fmt});
            callk(294, {cs_va});
          });
          ksched.run_until_complete();
          std::printf("  kernel hle: cs handoff done, ticks=%llu\n",
                      (unsigned long long)ksched.current_tick());
        }

#ifdef UNICORN_ENABLED
        // Guest execution: a REAL x86-32 CPU core (Unicorn TCG) executes
        // LithiumX's entry point. Kernel calls cross the same HLE as
        // above; the run stops honestly at the first unimplemented
        // import (XcSHAInit — the nxdk CRT mutex via NtCreateMutant
        // is served).
        {
          GuestScheduler gsched;
          krnl::XboxKrnl krnl(lx->ram(), gsched);
          krnl.install_thunks(*lx);
          ucore::UnicornRuntime rt(lx->ram(), krnl);
          rt.set_image(*lx);
          ucore::GuestCpu cpu(rt);
          cpu.set_teb(rt.build_teb());
          std::string err;
          if (cpu.attach(&err) && rt.has_tls_template()) {
            const uint32_t stack = rt.alloc_stack(0x40000);
            const bool ok = cpu.run(lx->image().entry_va(), stack,
                                    2000000, &err);
            std::printf("  guest cpu: executed %llu REAL instructions "
                        "of LithiumX -> %s\n",
                        (unsigned long long)cpu.instructions(),
                        ok ? "clean exit"
                           : cpu.fault().detail.c_str());
          } else {
            std::printf("  guest cpu: attach failed: %s\n",
                        err.c_str());
          }
        }
#endif
      }
    }
  }

  GpuStub gpu;
  // GpuBatch proof is inside the Vulkan block so it can use the device.
  // (Runs before CPU traps so trap-driven submit stays last.)
  int traps = 0;
  cpu_run_native([&](uint32_t id, uint64_t arg) {
    ++traps;
    std::printf("trap id=%u arg=%llu\n", id, (unsigned long long)arg);
    if (id == 2) gpu.submit(GpuPacket{.type = 1, .addr = 0x1000, .size = 64});
  });
  std::printf("traps=%d gpu_submitted=%llu last=%s\n", traps,
              (unsigned long long)gpu.submitted(), gpu.last_log().c_str());
  std::puts("MVP ok: native cpu + virtual mem + gpu stub");
  return 0;
}
