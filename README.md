# seriesx-emu

<img src="icon.png" alt="seriesx-emu icon" width="128">

An original-Xbox (x86-32) emulator *foundation*: a real XBE binary loader, a
real cooperative fiber scheduler, a reverse-engineered `xboxkrnl.exe` kernel
HLE, an NV2A vertex-shader + register-combiner HLE, and an optional
Unicorn-based x86-32 CPU core that actually executes guest machine code.

> **Name warning:** the repository is called `seriesx-emu` for historical
> reasons (it started as a Series-X memory-model sketch). Every Xbox-specific
> line of code in it today targets the **original Xbox**: x86-32 Pentium III,
> 64 MiB RAM, XBE executables, `xboxkrnl.exe`. There is no Series X/S code,
> and there cannot be without per-console keys and dumped flash (see
> "Deliberately absent" below).

## Status (honest, no overclaiming)

What works today, each backed by tests:

| Piece | State |
|---|---|
| XBE loader (`xbe_loader`) | Parses and maps real homebrew XBEs (validated against LithiumX v0.9.7 / nxdk); retail/debug entry + kernel-thunk decode, TLS directory, per-section protection |
| Scheduler (`cpu_sched`) | Real ucontext-based fiber switching, virtual-time sleeps, blocking primitives, deadlock detection |
| Kernel HLE (`xbox_krnl`) | 371-export table generated from nxdk's `xboxkrnl.exe.def`; **58 exports implemented** with NT-faithful semantics (events, mutants, semaphores, system threads, multi-wait, critical sections, contiguous memory, interlocked, time, DbgPrint); 42/103 of LithiumX's real imports served |
| Guest CPU (`xbox_cpu`, optional) | Unicorn 2 TCG executes real XBE code; kernel calls cross the same HLE; per-fiber TEB/TLS on real instructions; every stop is a named fault record |
| NV2A shaders (`xbox_vs`, `xbox_rc`) | DX8-class vertex-shader microcode decoded, validated and interpreted; the register-combiner (pixel) machine evaluated on the CPU. No rasterizer, no command-stream front end |
| GPU / Vulkan remap (`gpu_stub`, `gpu_batch`) | Xbox-semantic GPU packets, heap-class checks and root-signature batches; Vulkan tests skip honestly when no device exists |
| Display (`xbox_display`, `vulkan_present`, optional) | Guest-RAM frontbuffer registry + Flip snapshots (checksum/PPM, headless); a real GLFW+Vulkan swapchain presents with FIFO vsync. The presenter is a dependency-free stub without GLFW; tests skip honestly without a device or display server |

Running the LithiumX entry point executes real instructions through the
nxdk CRT startup — it creates the CRT mutex via `NtCreateMutant` and only
stops at the next unimplemented import (`XcSHAInit`). That is the
designed, honestly-reported failure mode of an HLE that implements 58 of
371 exports, not a crash.

## Build

```sh
apt install cmake g++ libvulkan-dev      # or use your distro's equivalents
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Unicorn is **optional**. With it, the `xbox_cpu` guest-execution tests are
built and run; without it, everything else is identical:

```sh
pip install --user unicorn    # provides headers (vendored) + libunicorn.so.2
cmake -B build ...            # CMake prints "Unicorn CPU core: ..." when found
```

GLFW is also **optional** (`libglfw3-dev` / `find_package(glfw3)`). With it,
`--window` opens a real Vulkan swapchain on the guest frontbuffer; without it
the presenter compiles to a stub whose `init()` returns false and the rest of
the emulator is unchanged.

Expected test result: `100% tests passed` — GPU-dependent tests exit with
code 77 (ctest "Skipped") on machines without a Vulkan device.

## Research provenance and reproducibility

Everything Xbox-specific is derived from public sources, saved under
`scripts/research/`:

- `nxdk_xboxkrnl.exe.def`, `nxdk_xboxkrnl.h`, `nxdk_xboxdef.h`,
  `nxdk_ntstatus.h` — nxdk (def: CC0-1.0, headers: MIT). The MS decoration
  grammar of the def file gives the real calling conventions and argument
  sizes.
- `xboxdevwiki_kernel.html` + per-function pages — xboxdevwiki.net (CC BY-SA),
  cross-check only.
- `kernel_exports.tsv`, `parse_kernel_exports.py`, `dump_lithiumx_thunks.py`,
  `cxbx_cxbxkrnl.cpp` — the parsed wiki table, and Cxbx-Reloaded's HLE table
  for reference.

`src/xbox_krnl_exports.inc` is generated:

```sh
python3 scripts/gen_exports_inc.py
```

The regeneration is byte-for-byte reproducible from the vendored inputs.
Where the wiki and the import library disagree (ordinal 32), the import
library wins and a warning is printed. The kernel exports **no** heap
functions (`RtlAllocateHeap` does not exist in the table — CRT heaps live in
each XBE); this negative result is why the HLE has no fabricated heap API.

## Deliberately absent (do not ask)

- **No retail-game path**: retail titles need no decryption to *load*, but a
  full emulator needs BIOS content (µcode/keys) this project will never ship
  or fabricate.
- **No Series X/S emulation**: encrypted FS, per-console keys, no public
  dumps. The name is history.
- **No dumped kernel**: `XboxKrnlVersion` is zero-initialized rather than
  invented.

## Known limitations (documented, not hidden)

- ~58/371 kernel exports implemented; unknown imports stop the guest with a
  named fault record.
- Fibers use POSIX ucontext (`makecontext`/`swapcontext`), which POSIX marks
  obsolescent; glibc still supports it and it is the pragmatic choice for a
  stackful cooperative scheduler. A hand-rolled ASM switch is the future
  work if that ever matters.
- Cooperative scheduling only: contention on a critical section manifests
  when the owner voluntarily blocks while holding it.
- KeInitializeEvent-registered events, allocator bookkeeping and the handle
  table use linear scans — fine at emulator scale, listed here for honesty.
- TLS callbacks are recorded, not dispatched (dispatching them is guest-code
  execution, future CPU-core work).
- Alertable waits behave as non-alertable (no APCs).
- The NV2A shader HLE covers vertex microcode and register combiners only:
  texture shaders (samplers) are supplied by the caller, transcendentals are
  computed exactly where the hardware approximates, and there is no
  rasterizer or command-stream (pushbuffer) front end. The register-combiner
  MUX LSB mode is rejected rather than guessed (the wiki marks it FIXME).

## License

This project's code: GNU GPL v2 or later (see `LICENSE`) — chosen so the
optional Unicorn CPU core (GPL-2.0, QEMU-derived) is under the same license
as everything else; no more MIT/GPL combined-work split. The vendored
`vendor/unicorn/include` headers carry Unicorn's upstream licensing. The test
fixture `tests/lithiumx.xbe` is LithiumX v0.9.7 by Voxel9 (MIT), built with
nxdk. nxdk-derived research files in `scripts/research/` keep their upstream
licenses (CC0-1.0 / MIT); xboxdevwiki snapshots are CC BY-SA.
