# Measuring performance

State described: c944a398 (2026-11-23), read 2026-11-25.

What can be measured today, how earlier numbers were taken, what is missing, and the benchmark plan. The ranking
of what to optimize is in [BOTTLENECKS.md](BOTTLENECKS.md); every past result is in
[OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md).

Nothing in this file was measured while writing it: no game was run. Numbers are quoted from the log with their
date and conditions.

## What the emulator already provides

| Tool | How to turn it on | What it gives | Limits |
|---|---|---|---|
| Debug counters (`src/common/debugCounters.h`) | enabled together with the performance overlay; `Add` and `ScopedTimer` do nothing while disabled | per-interval totals: draws, draw instances, indirect draws, dispatches, queue submits, GPU waits and wait time, write-tracking faults, shaders compiled and compile time (with peak), pipelines created and time (with peak), texture and buffer upload/download bytes, evictions, colour-metadata read-backs, flips requested/rejected, late vblanks, swapchain recreations, dropped presents, emulated instructions, unresolved import calls, guest file bytes, audio underruns, GPU-thread busy time, present / acquire / overlay time | shown on screen only; nothing writes them to a file |
| Performance overlay (`src/graphics/presentation/performanceOverlay.cpp`) | toggle at run time; a second toggle opens the detail panel | fps, frame ms, 1% low, worst frame, hitches, present mode, virtual vblank, flip queue state; process and machine CPU, busiest threads, context switches, page faults; GPU name, utilisation, driver memory, the counter rates above, a frame-timing block (guest GPU / compile, GPU wait / present, acquire / overlay), Vulkan heap use against budget; guest memory statistics, process memory, disk and guest file I/O | read by screenshot; the "CPU waiting on GPU" row also contains present pacing; its own sampler thread wakes once a second |
| Host statistics sampler (`src/common/hostStats.cpp`) | runs with the overlay | process and system CPU, memory, I/O, thread count and the busiest threads, from the operating system | per-thread numbers have no thread names attached on every platform |
| Tracy zones (`src/common/profiler.h`, `KYTY_PROFILER_FUNCTION`, `KYTY_PROFILER_BLOCK`, `KYTY_PROFILER_THREAD`) | always compiled in (on-demand, started by the option `--profile`, `3rdparty/CMakeLists.txt:228-234`, `src/common/profiler.cpp:19-21`); connect the Tracy viewer | zone times per thread with call stacks | about 84 zones exist and half are in the command-processor handlers; the buffer cache has one, the texture cache none, the tracker none: the functions named in BOTTLENECKS.md are mostly not covered |
| Device-loss line (`src/graphics/host_gpu/deviceLossReport.cpp`) | always; more detail with `--gpu-crash-diagnostics` (device-fault extension, driver checkpoints carrying the program id) | one `[device-loss]` line with memory figures and, when enabled, the faulting program | once per process |
| Shader compile lines (`pipelineCache.cpp`, one `printf` per compiled module; "Shader cache: loaded ... in N ms" at start) | always | how many modules were compiled, when, and how long the store took to load | the compile timer does not include translation |
| RenderDoc capture (`src/graphics/presentation/renderDoc.cpp`) | option that enables the integration, then a capture request; captures two guest flips | every pass, draw and resource of a frame with GPU durations | changes timing; one frame |
| Vulkan validation, GPU-assisted validation, SPIR-V validation of every module | options | correctness of what a change emits | slow; never on while measuring |
| Unit and GPU tests (`ctest`) | see [ARCHITECTURE.md](ARCHITECTURE.md#targets-and-build); test executables are built only when named | correctness of the tracker, page manager, caches, command lanes, recompiler | no performance test among them |

Run-time switches that turn single optimizations off (for A/B in one build) are listed in OPTIMIZATION_LOG.md.
Pacing options that matter for any measurement: `--vblank-frequency N`, `--ignore-flip-rate`, `--present-mode`.

## What was used from outside

- GPU vendor command-line monitor sampled once a second (utilisation, clock, power).
- Per-thread CPU time of the process sampled every 10 s.
- A sampling profiler at 1 kHz on the GPU thread, and stack samples of the busiest threads taken with a
  non-invasive debugger.
- A GPU trace tool with a shader profiler (one successful 500 ms capture; it could not attach in an earlier try).
- Temporary probes compiled into the emulator and removed before each commit: per-function timers on the GPU
  thread, GPU timestamp pairs per batch / pass / dispatch, a wait probe with stacks, a fault probe, and switches
  that a thread flips every N seconds to alternate two code paths inside one run.
- Scripts outside the repository that start the emulator, press pad buttons at fixed times, take screenshots at
  fixed times and read the frame counter from the window title.

## How earlier numbers were taken, and how far to trust them

- Frame rate = frames counted between two screenshots, 160 s apart in the later rounds, one or two runs per build.
- Separate runs of the same build differ by about 1.5-2 fps at 40-60 fps; one unexplained slow run of a base build
  measured 16.65 against 18.78. A change below about 1 ms per frame was only believed when measured by alternating
  inside one run.
- The first run after a build loads slowly (the shader store is stamped with the executable's time), so cold and
  warm runs must not be compared with each other.
- GPU temperature, memory pressure from other processes, a frame-rate overlay layer of another program (it crashed
  device creation) and rare driver device losses all disturbed runs.
- No run recorded a frame-time series: averages come from frame counts, worst frames and 1% lows only from the
  overlay as seen in a screenshot.

## What is missing

| Gap | Why it matters | Proposed addition |
|---|---|---|
| No frame-time log | 1% low, stutter and the frame-time histogram cannot be compared between builds; pacing changes cannot be judged by an average | an option that appends one line per presented frame (time, frame ms, and the per-frame deltas of the existing counters) to a CSV; cost when off: none |
| Counters only on screen | every breakdown so far needed a screenshot or a temporary probe | the same CSV carries the counters; a summary line at exit |
| Few profiler zones where the time goes | each round re-applied hand-made timers | permanent zones on `ObtainBuffer`, `FindBuffers`, `SynchronizeBda`, the eager read-back record/complete pair, `MaterializeResources`, `GetProgram`, `CommitBindings`, texture upload, page protection, scheduler waits (zones cost nothing unless `--profile` is given) |
| Waits are one total | the largest cost is a wait, and its cause had to be found with a stack probe each time | count and time waits per call site (a small fixed table indexed by reason) |
| No GPU time per pass in the emulator | host GPU time per program came from an external trace tool that attached once | optional timestamp queries around batches, passes and dispatches, reported per program id in the log |
| No reproducible scene inside the repository | scripts and inputs live outside; a new contributor cannot repeat a number | a documented procedure (below) plus, later, a command-stream capture and replay so a frame can be re-run without the title (research item) |
| No automatic A/B | every comparison was arranged by hand | a script that alternates base and candidate (or a switch inside one build) and prints mean, spread and the difference |

## Benchmark plan

**Scenarios.** At least one per limiter, each a fixed scene reached by fixed inputs, with a fixed camera:

| ID | Configuration | Limiter today | Used for |
|---|---|---|---|
| S1 | title B, quality mode with ray tracing, 1080p, test scene | GPU thread + waits + host GPU, all close | the hard case; the reference frame of BOTTLENECKS.md |
| S2 | title B, performance mode, 720p render and output, frame-rate cap lifted | GPU-thread CPU | per-draw CPU work |
| S3 | title B, performance mode, default size | host GPU | guard: must not get slower |
| S4 | title A, test scene, 720p render | compute and read-backs | the early-compute and fill/copy paths |
| S5 | cold start of S1 or S2 up to the first 60 s in the scene | compile stalls | worst frame, 1% low, time to scene |
| S6 | a menu or loading screen | pacing | flip and vblank changes |

Titles are not named in this repository; which titles A and B are is kept in the maintainer's local notes. Another
contributor substitutes titles they own and reports against their own base.

**Per run, record:** build (commit and whether the tree is clean), options and environment switches, store state
(cold or warm), average fps, frame-time mean / median / 1% low / 0.1% low / worst, hitches over 50 ms, GPU-thread
busy ms per frame, GPU wait ms per frame and count, queue submits, draws, dispatches, upload and download bytes per
frame, shaders and pipelines compiled, process CPU and the busiest threads, GPU utilisation, process memory and
video memory, write-tracking faults per second.

**Protocol.**
1. One game at a time on the machine; nothing else heavy running; the same display and refresh rate; frame-rate
   overlay layers of other programs off.
2. Warm-up: discard everything before the scene is reached plus 30 s.
3. Window: 120-160 s.
4. Base and candidate alternated, at least three runs each (B C B C B C), or one build with the switch flipped
   inside a run for changes expected below 1 ms.
5. Both arms on the same store state. For S5 clear the store before every run.
6. Report mean and min-max of each arm. A change is a gain when the arms do not overlap; otherwise it is recorded
   as "no measurable change".
7. Take a screenshot at a fixed instant in both arms and compare.
8. Record the result in OPTIMIZATION_LOG.md with the template at its top, whatever the outcome.

**Guard rails.** S3 and one run of a second title are repeated before a change is pushed: a gain in one scene that
costs another is reported as a trade, not a gain.

## Last recorded numbers

Quoted from OPTIMIZATION_LOG.md; hardware: laptop RTX 3050 and Ryzen 5 3600.

| Scenario | Result | Date, build |
|---|---|---|
| S1 | 20.6-20.8 fps (frame about 48 ms); GPU thread busy about 38 ms + 11-12 ms waiting; host GPU about 35.6 ms | 2026-11-23, `0a1b544d` / `5384fd81` |
| S1 after upstream's suspend-point change | 18.98 (cold) / 19.21 (warm) fps | 2026-11-23, `c944a398` |
| S1 at the default size | about 8.0 fps | 2026-11-23, `0a1b544d` |
| S2 | 59.8 fps at the 60 Hz cap; 49.5 fps with the GPU thread at 97% of a core before the last two changes | 2026-11-16 |
| performance mode 1080p, cap lifted | 63.5-65.2 fps | 2026-11-23 |
| S3 | 29.3 fps with the 30 cap lifted; GPU 99% | 2026-11-23, `5384fd81` |
| S4 | 55.9 fps at 4K output and 720p render, GPU 92%; at the cap with 720p output | 2026-11-15 |

No number exists yet for the merge of upstream `93cce665` (2026-11-25): it has not been run.
