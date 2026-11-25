State described: c944a398 (2026-11-23), read 2026-11-25.

# Architecture

Map of the emulator. Details live in the documents linked from each section; this file does not repeat them.

## What it is and is not

- A translation layer for PlayStation 5 software, written in C++20, with Vulkan 1.3 as the only graphics backend (`src/graphics/host_gpu/graphicContext.h:20`; devices below 1.3 are rejected).
- **No CPU emulation.** Guest x86-64 code runs natively on the host CPU. The build requires `x86-64-v3` (AVX2, BMI2, FMA) and disables FMA contraction (`src/utils.cmake:68-69`, `:84`, `:108-109`). Only a few instructions are emulated on fault, and some guest code is rewritten at load time: see [CPU.md](CPU.md#guest-instruction-patching-and-fault-driven-emulation).
- **HLE, not a kernel.** Guest imports resolve to host functions registered at start-up; a resolved call is a direct SysV-ABI call, no syscall dispatcher was observed ([CPU.md](CPU.md#import-resolution-and-the-call-bridge)).
- **A command processor, not a GPU driver.** The guest builds PM4 command buffers; a host thread decodes them and drives a Vulkan renderer ([GPU.md](GPU.md)).
- **A shader recompiler.** Guest GPU machine code is decoded, lifted to an IR, specialised on the descriptor values read at draw time and emitted as SPIR-V ([GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md)). There is no SPIRV-Tools optimiser in the path; `spvtools` is used only for optional validation (`src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:135-164`).
- **Guest memory is real host memory.** The title reads and writes it natively; the GPU side reaches it through copies and write detection by page protection ([MEMORY.md](MEMORY.md)).
- Platforms: Windows, Linux, macOS (macOS rides the POSIX/Linux code paths, `CMakeLists.txt:92-98`; experimental per the repository README). 64-bit only (`CMakeLists.txt:82-84`).

## Languages, libraries, dependencies

C++20 (`CMakeLists.txt:48`, `CMAKE_CXX_STANDARD 20`, extensions off). Clang is built with `-fno-rtti -fno-exceptions`; GCC with `-fno-exceptions` (`src/utils.cmake:91-111`). GLSL compute/fragment helper shaders under `src/graphics/host_gpu/shaders/` are compiled to SPIR-V at build time with `glslangValidator` (required, `CMakeLists.txt:245`) and embedded (`src/embed_spirv.cmake`). The launcher is Qt (`KYTY_BUILD_LAUNCHER`, default ON, `CMakeLists.txt:188`).

Linked into `kyty_emulator` (`CMakeLists.txt:339`): `common`, Vulkan-Headers, SPIRV-Tools (static), SDL3 (static), imgui, xxHash, FFmpeg (`ffmpeg-core`), fmt, nlohmann_json, LibAtrac9, Zydis, xbyak, opus, OpenXR loader, zstd (static). Also in the tree: VulkanMemoryAllocator, magic_enum, spdlog, tracy, cpuinfo, stb, ffx-api, renderdoc header, winpthread (Windows clang-cl).

| Dependency | How it enters the tree | Used for |
|---|---|---|
| SDL3 | submodule `3rdparty/SDL3` (branch `release-3.4.x`) | window, input, audio streams ([CPU.md](CPU.md#audio)) |
| Vulkan-Headers, SPIRV-Headers, SPIRV-Tools | submodules | Vulkan API; SPIR-V validation only |
| VulkanMemoryAllocator | submodule | image/buffer allocation ([MEMORY.md](MEMORY.md#allocator)) |
| xxHash | submodule | XXH3 for shader hashes, pipeline keys, page hashing |
| fmt, spdlog, magic_enum, nlohmann_json, imgui, tracy | submodules | formatting, logging, enum names, JSON (content parameters), overlay UI, profiler zones |
| ffmpeg-core | submodule (`KytyPS5/ext-ffmpeg-core`, shallow) | video/audio decode |
| LibAtrac9 | submodule | ATRAC9 audio decode |
| cpuinfo, stb, ffx-api, renderdoc, winpthread, patches | plain directories in `3rdparty/` (not in `.gitmodules`) | host CPU query, image loading, FSR headers, RenderDoc hook, pthread on clang-cl |
| opus, OpenXR-SDK, xbyak, zydis, zstd, ZArchive | `FetchContent` at configure time (`3rdparty/CMakeLists.txt`) | controller audio, headset loader, run-time x86 code generation (SRT walker JIT), x86 decoding for patching, compressed archives |
| googletest | the test library `kyty_test_gtest` compiles `3rdparty/fmt/test/gtest/gmock-gtest-all.cc` (`CMakeLists.txt:673-675`); that file is not present in the checked-out tree, and the include paths `3rdparty/gtest` (`CMakeLists.txt:144-145`) point to a directory that does not exist | tests |

Submodule list: `.gitmodules` (14 entries).

## Directory map

Sizes are file count and kilobytes on disk at read time (all file types).

| Folder | Files / KB | Content |
|---|---|---|
| `src/common/` | 56 / 322 | logging, config (`emulatorConfig`), debug counters, threads, hostException, archive, profiler, `parallelFor.h`, platform virtual memory |
| `src/loader/` | 19 / 296 | ELF loader, runtime linker, symbol database, guest instruction patcher, x64 instruction emulator, game patches, system content |
| `src/kernel/` | 19 / 478 | guest memory, pthreads, semaphores, event flags/queues, address waits, file system |
| `src/libs/` | 103 / 2459 | HLE library modules (libkernel, audio, video-out, AGC, pad, save data, network, codecs, ...); `libs.cpp` registers them |
| `src/graphics/guest_gpu/` | 11 + `command_processor/` 4 | PM4 command processor, hardware context, tile layout maths, PSSR request |
| `src/graphics/host_gpu/` | 17 + `renderer/` 31 + `shaders/` 30 | Vulkan context, VMA, page manager, trackers, renderer (draw, compute, recorder, scheduler, caches, images, pipelines, upscalers), helper GLSL |
| `src/graphics/presentation/` | 16 + `window/` 6 | video-out driver, flip queue, present thread, swapchain, overlay, OpenXR |
| `src/graphics/shader/` | 9 + `recompiler/` | shader recompiler: decode, CFG, IR, passes, SPIR-V backend (45,464 lines in the folder) |
| `src/launcher/` | 58 / 509 | Qt launcher (separate target) |
| `src/updater/` | 4 / 29 | update installer |
| `src/graphics` total | 259 / 4559 | |
| `tests/` | 66 / 5793 | unit and GPU tests, registered with CTest (`CMakeLists.txt:736`) |
| `3rdparty/` | see table above | |
| `assets/` | 34 / 418 | sounds, cursors, fonts, icons |
| `docs/` | | this knowledge base; `docs/_recon/` is untracked scratch |
| `nix/`, `flake.nix`, `shell.nix` | | Nix development environment |

## Targets and build

- Configure and build with CMake (minimum 3.19, `CMakeLists.txt:1`). Single-config generators default to `Release` (`CMakeLists.txt:7-11`).
- Targets: `kyty_emulator` (`main.cpp` + the globbed source lists of `libs`, `graphics`, `kernel`, `loader`; `CMakeLists.txt:935-944`), the Qt launcher, the updater, and test executables (`kyty_tests` aggregates them, `CMakeLists.txt:873-879`). The source glob uses `CONFIGURE_DEPENDS`.
- CI (`.github/workflows/build.yml`): three jobs (Windows with clang-cl and glslang from vcpkg, macOS x86_64, Linux with clang and lld), all `-DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`, Ninja, sccache, Qt 6.10.3. Each builds the `launcher` target (which pulls in `kyty_emulator`) plus a named list of test targets (a longer list on Windows than on macOS and Linux), runs `ctest -R` on the matching names, then `cmake --install`; a release job packages the three installs.
- Options and cache variables: `KYTY_BUILD_LAUNCHER` (ON), `KYTY_ENABLE_CLANG_TIDY` (OFF), `KYTY_BUILD_ORIGIN` (`Source`, `Official`, `Fork`; `Official` needs `KYTY_RELEASE_TAG`, `Fork` needs `KYTY_BUILD_REPOSITORY`), `KYTY_PROJECT_NAME`. `sccache` or `ccache` is used as compiler launcher when found (`CMakeLists.txt:14-28`). A git-version header is generated at build time.
- Build types: `Debug` selects `KYTY_BUILD_DEBUG`; every other type selects `KYTY_BUILD_RELEASE` (`CMakeLists.txt:87-91`). Some behaviour is Release-only, for example the driver pipeline cache and the shader store ([GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md)).
- Flags that matter for speed: `-march=x86-64-v3 -ffp-contract=off` (clang/gcc) or `/clang:-march=x86-64-v3 /clang:-ffp-contract=off` (clang-cl); clang-cl Release and RelWithDebInfo add `/O2 /DNDEBUG`; clang adds `-g -fno-omit-frame-pointer -fno-strict-aliasing`; static runtime on Windows (`/MT`, `-static`); `lld` linker with clang (`src/utils.cmake:68-111`, `CMakeLists.txt:133-138`). `CMakeLists.txt` and `src/utils.cmake` set no LTO; the CI jobs of all three platforms pass `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` (`.github/workflows/build.yml`), so release artefacts are LTO builds and a plain local configure is not. Measured effect of LTO: see [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md).
- Checks active in release: `EXIT_IF` is compiled out only when `KYTY_FINAL` is defined (`src/common/assert.h:17-24`). `KYTY_FINAL` comes from `#cmakedefine` in `src/cmake_config.h.in:10`; no `CMakeLists.txt` or `src/utils.cmake` line sets it, and neither does `.github/workflows/build.yml`, so by default (and in CI builds) the `EXIT_IF` checks stay active in Release as well. `EXIT` and `EXIT_NOT_IMPLEMENTED` are always calls to fatal handlers.
- Tracy is compiled into every build, not behind an option: `3rdparty/CMakeLists.txt:228-234` forces `TRACY_ENABLE`, `TRACY_ON_DEMAND`, `TRACY_DELAYED_INIT` and `TRACY_MANUAL_LIFETIME` ON, and `common` links `Tracy::TracyClient` publicly (`src/common/CMakeLists.txt:38`). The client is started at run time only when the emulator gets `--profile` (`src/main.cpp:225`; the launcher's profiler checkbox passes it, `src/launcher/src/mainDialog.cpp:301`; `Profiler::Initialize`, `src/common/profiler.cpp:18-26`). Zones are the `KYTY_PROFILER_BLOCK` / `KYTY_PROFILER_FUNCTION` macros (`src/common/profiler.h`), used in 19 files under `graphics/`, `loader/runtimeLinker.cpp` and `shader.cpp`; without `--profile` they cost only a `ProfilerAvailable()` check. A capture needs a Tracy viewer connected to a process started with `--profile` (the start-up message prints `127.0.0.1:8086`).
- Switches (environment, config, CLI) are documented in the document that owns the code they steer: [MEMORY.md](MEMORY.md#environment-switches-of-this-area), [THREADING.md](THREADING.md), [GPU.md](GPU.md), [GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md).

## Entry points

| Entry | Where | Role |
|---|---|---|
| process entry | `src/main.cpp:503-519` (Windows `wmain`), `:526` (`main` elsewhere) | normalises wide arguments on Windows |
| `Main()` | `src/main.cpp:475-499` | records the main host thread (`src/common/threads.cpp:193-198`), parses arguments, calls `Run()` |
| `Run()` | `src/emulator.cpp:142-174` | builds `Subsystems`, `Init()`, registers `atexit` (`KytyClose`) and `at_quick_exit` |
| `Init()` | `src/emulator.cpp:91-115` | order: config, log, content parameters, splash, pthread, profiler, network, memory, filesystem, controller, audio, graphics |
| `LoadElf()` / `Execute()` | `src/emulator.cpp:117-140` | load the program, start the execute thread, run the window loop on the original thread |
| `--game`, `--guest-exec` | `src/main.cpp:267-303` | directory (`/app0/eboot.bin`), archive, or explicit ELF plus guest arguments |

## How a title runs

1. **Start.** `Main()` parses `--game`/`--guest-exec`; `Run()` initialises subsystems in the order above and mounts the application directory at `/app0` (read-only for writes) and `/hostapp` ([CPU.md](CPU.md#loader-and-start-up)).
2. **Load.** HLE modules register their exports, then `RuntimeLinker::LoadProgram()` maps the ELF into guest memory and builds symbol data ([CPU.md](CPU.md#loader-and-start-up), [MEMORY.md](MEMORY.md#guest-address-space-and-kernel-memory-calls)).
3. **Guest threads.** `Execute()` starts a detached host thread that becomes the main guest pthread; the original thread runs the window loop. Relocation, optional guest-code patching and TLS creation precede the call to the entry point ([CPU.md](CPU.md#loader-and-start-up), [THREADING.md](THREADING.md#host-threads)).
4. **Library calls.** Guest imports call registered host functions directly; kernel objects (threads, mutexes, semaphores, queues, files, memory) are implemented in `src/kernel/` and `src/libs/` ([CPU.md](CPU.md#kernel-layer)).
5. **Graphics submit.** The title's graphics-library entry (`AgcDriverSubmitDcb` and the multi-buffer variant) enqueues a PM4 span on a per-queue deque and signals the GPU thread; at each frame boundary it calls a suspend point ([GPU.md](GPU.md), [THREADING.md](THREADING.md#frame-pacing-chain)).
6. **GPU thread.** `Thread_Gpu` picks submissions round-robin over 57 queues, runs the command processor per packet, and calls the renderer per draw/dispatch ([GPU.md](GPU.md)).
7. **Host renderer.** Per draw: shader program lookup (recompile on a miss), buffer/texture cache lookups with guest-memory upload, descriptors, Vulkan recording through the recorder thread, batch submit through the submit thread ([GPU.md](GPU.md), [GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md), [MEMORY.md](MEMORY.md)).
8. **Present.** A flip request goes through the flip queue; the present thread fires on a virtual vblank clock, copies the surface into a frame and blits/draws it into a swapchain image ([THREADING.md](THREADING.md#frame-pacing-chain), [GPU.md](GPU.md)).

## Subsystems and owner documents

| Subsystem | Source | Owner document |
|---|---|---|
| Loader, runtime linker, symbol database | `src/loader/` | [CPU.md](CPU.md) |
| Instruction patching and emulation, host exception path | `src/loader/guestInstructionPatcher.cpp`, `x64InstructionEmulator.cpp`, `src/common/hostException.cpp` | [CPU.md](CPU.md) |
| Kernel objects, files, async I/O | `src/kernel/`, `src/libs/libKernel.cpp`, `libAmpr.cpp` | [CPU.md](CPU.md) |
| Audio | `src/libs/libAudio*.cpp`, `ngs2.cpp`, `ajm.cpp`, `audio*.cpp` | [CPU.md](CPU.md) |
| Threads, locks, waits, frame pacing | all | [THREADING.md](THREADING.md) |
| Guest address space, write detection, buffer/texture caches, BDA | `src/kernel/memory.cpp`, `src/graphics/host_gpu/` | [MEMORY.md](MEMORY.md) |
| Command processor, GPU thread, host renderer, scheduler, presentation | `src/graphics/guest_gpu/`, `host_gpu/renderer/`, `presentation/` | [GPU.md](GPU.md) |
| Shader recompiler, pipeline and shader caches, descriptors | `src/graphics/shader/`, `host_gpu/renderer/pipeline/` | [GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md) |
| Debug counters, overlay, dumps, device-loss report | `src/common/debugCounters.h`, `presentation/performanceOverlay.cpp`, `host_gpu/deviceLossReport.cpp` | no document yet |
| Measured performance history | | [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md) |

## Not covered yet

- Whether `KYTY_FINAL` is set by any other release script: `.github/workflows/build.yml` does not set it.
- The launcher and updater targets were inventoried by folder only.
- Per-platform build differences (macOS entitlements, Nix) were not read.
- There is no document yet for the debug counters, overlay, dumps and device-loss report.
