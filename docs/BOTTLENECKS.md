# Bottlenecks and optimization plan

State described: c944a398 (2026-11-23), read 2026-11-25.

This file ranks where the time goes and what to try next. It is the only document that ranks; the facts behind it
live in [GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md), [GPU.md](GPU.md), [MEMORY.md](MEMORY.md),
[THREADING.md](THREADING.md) and [CPU.md](CPU.md), the measurements in
[OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md) (which also holds the measurement method used so far), the tools and
the benchmark plan in [PERFORMANCE.md](PERFORMANCE.md).

No game was run to write this file. Every number below is quoted from earlier measurements recorded in
OPTIMIZATION_LOG.md (date and conditions given there); everything marked *hypothesis* comes from reading the
code only and has no measurement behind it yet.

## How to read the evidence

Three kinds of evidence, in falling order of weight:

- **M (measured)**: a profile, counter or A/B from a recorded run.
- **C (ceiling)**: an experiment or calculation that bounds the possible gain ("even with no waits ...").
- **H (hypothesis)**: the code shows a cost that is paid often; how much it costs is unknown.

The reference frame for the percentages is the most demanding configuration measured so far: title B, quality mode
with ray tracing, 1080p, test scene, about 20.7 fps (frame about 48 ms) at fork commit `0a1b544d` / `5384fd81`
(2026-11-23). In that frame the GPU thread is busy about 38 ms and waits about 11-12 ms. The host GPU time of 35.6 ms per
frame was measured earlier (2026-11-23, round at about 18 fps, frame about 55 ms, GPU idle about 35%), so for the
48 ms frame the idle share is smaller. Two bounds follow (C): with no waits at all the GPU thread alone
allows about 26 fps, and the GPU work alone about 28 fps. So no single change reaches 30 fps there: the waits, the
GPU-thread CPU time and the GPU time all have to shrink.

Lighter configurations have a different limiter. Performance mode at 720p ran at the 60 Hz cap with the GPU thread
at 97% of a core just below it (M, 2026-11-16); performance mode at the default size is bound by the host GPU doing
the title's own work (M, GPU 99%).

## The ten largest candidates

| # | Candidate | Evidence | Size in the reference frame | Where |
|---|---|---|---|---|
| 1 | The GPU thread needs, on the CPU, values that a GPU program wrote earlier in the same frame (descriptor walks, record counts, condition words). It blocks until the host GPU has produced them. | M: waits 15.5 -> 8.5 -> 11-12 ms/frame over the last rounds; each removed link moved the wait to the next reader. One wait of 11-14 ms per frame is left. | about 11-12 ms of 48 (largest single item) | `SrtWalker.cpp:LoadGuest`, `ResourceMaterialization.cpp`, `bufferCache.cpp` read-back paths, `commandScheduler.cpp:Wait` |
| 2 | Page-protection calls for write watching (re-arming watchers at flush and in the BDA sync). | M: `NtProtectVirtualMemory` 14-19% of GPU-thread busy time (1 kHz sampler, 2026-11-23); 13 points of it under `PrepareBda -> SynchronizeBda`. | about 5-7 ms of GPU-thread time | `pageManager.cpp:Protect`, `regionManager.h`, `bufferCache.cpp:SynchronizeBda` |
| 3 | Per-draw and per-dispatch buffer resolution. | M: `ObtainBuffer` / `FindBuffers` 20-25% of busy time, inclusive. | about 8-9 ms inclusive | `descriptors.cpp:FindBuffers/RebindBuffers`, `bufferCache.cpp:ObtainBuffer` |
| 4 | Eager read-back bookkeeping and the flushes it causes. | M: record 12% + complete 16% inclusive; about 137 flushes per frame from `SubmitReadBackWrites`; a rate limit was bounded at +3% (C). | about 4-10 ms inclusive | `bufferCache.cpp:RecordEagerDownloads/CompleteEagerDownloads`, `graphicsRun.cpp:826-829` |
| 5 | Per-draw program lookup with the CPU walk of the shader resource table, for every stage of every draw, also when nothing changed. | M: 463 ms/s in title A at 43 fps (2026-11-12); "material resolve / program lookup" about 20% of the thread in performance mode 720p (2026-11-16). | 20% of the thread where draws dominate | `pipelineCache.cpp:GetProgram/FindModule`, `ResourceMaterialization.cpp:MaterializeResources` |
| 6 | Host GPU time of the heaviest translated compute programs (ray traversal emulated per lane pair, device-address page-table lookups, code size beyond the GPU's instruction cache). | M: one program 11.4 ms of 35.6 ms per frame (per-program GPU time, 2026-11-23, at about 18 fps); address lookup helper about 9% of all GPU samples; instruction-fetch stalls 51-70% in the top three programs (one GPU trace, 2026-11-22). | about 11 ms of GPU time for the largest program | `backend/spirv/spirvEmitterMemory.cpp:DefineGetBdaPointer`, `SpirvEmitter.cpp:EmitProgram`, `rayTracing.cpp` |
| 7 | Vulkan command recording per draw with no redundancy check (vertex buffers, pipeline, push constants and descriptors are issued for every draw). | M: driver about 28% of the GPU thread in performance mode 720p (2026-11-16), before the recorder thread existed. H for today: the recorder moved it to another thread at the price of a copy per command and a drain at every batch end. | unknown today | `renderDraw.cpp:1153-1213`, `descriptors.cpp:CommitBindings`, `recorder.cpp` |
| 8 | Guest threads faulting on GPU-tracked memory. Reads of GPU-written pages wait for a whole GPU frame through a synchronous command on the GPU thread; writes cost a fault and two protection changes per page per rewrite. | M: read faults about 150/s at 2.6 ms, write faults about 21.5k/s at 7.4 us (2026-11-21). C: moving fault servicing off the GPU thread is worth at most 5.4 ms/frame. | up to about 5 ms on the GPU thread, more on game threads | `regionManager.h`, `bufferCache.cpp:ReadMemory`, `graphicsRun.cpp:SendCommandSync` |
| 9 | First-use stalls: shader translation, SPIR-V build and driver pipeline compile all run on the GPU thread; the on-disk shader store is off in the emulator unless `--shader-cache` is given (the launcher's own default is on and passes it) and is dropped whenever the executable changes (stamp = executable time, or `KYTY_SHADER_CACHE_STAMP`). | M: pipeline compile peaks of 6.6-8.3 s at scene start on a cold store. This hurts worst frames and 1% lows, not the average. | hitches, not average fps | `pipelineCache.cpp:CompileModule/GetGraphicsPipeline`, `shaders.cpp:CreatePipelineInternal`, `pipelineSerialization.cpp` |
| 10 | Frame pacing: flips are quantized to a virtual vblank clock; the title's flip-rate request caps the rate; the moment the frame suspend point is released decides how far the title runs ahead of the host GPU. | M: 60 -> 120 Hz virtual vblank gave +7% in one scene at 720p; an upstream change of the suspend-point release cost 7.5% (20.76 -> 18.98 / 19.21 fps). | 7% each way where it applies | `videoOut.cpp:PresentThread/IsFlipDueLocked`, `graphicsRun.cpp:SuspendPoint/Process` |

Candidates that the code shows but nothing has priced yet (all H):

- Whole-image upload when a CPU-written texture changes (no dirty range): `textureCache.cpp:RefreshImage/UploadImage`.
- Render pass ended and begun again around uploads, layout changes and buffer-writing draws; colour always loaded
  and stored: `context.cpp:BeginRendering`, `bufferCache.cpp:UploadCopies`.
- A full memory barrier after every dispatch: `renderCompute.cpp:566`. (Removing the barriers outright was tried and
  made things much worse: see Dead ends in OPTIMIZATION_LOG.md. A narrower barrier has not been tried.)
- Presentation copies the surface into a frame and then blits or draws it into the swapchain image:
  `swapchain.cpp:Frame::CopyFrom`, `RecordPresentCommands`.
- A full re-translation of a known program for each new specialization, and no cap on variants:
  `pipelineCache.cpp:GetProgram`.
- A new descriptor-set layout and pipeline layout for every pipeline: `shaders.cpp:CreateLayouts`.
- Guest waits: read line by line (`pthread.cpp`, `semaphore.cpp`, `syncOnAddress.cpp`), mutex, semaphore and address
  waits block in a condition variable or futex and are woken by the signal; they only re-check every 10 ms, so the
  claim of "fixed time slices" does not hold for wake latency. Sleeps are looped host sleeps of at most 10 ms
  (details: [CPU.md](CPU.md#sync-primitives-and-timers)). No cost is known; low confidence.
- The global kernel memory lock held across map / protect / free work: `memory.cpp`.

## Opportunities, classified

Impact is for the reference frame unless stated. Confidence says how sure the gain is, not how sure the cost is.
Difficulty counts the risk to correctness as well as the amount of code.

| ID | Opportunity | Attacks | Potential impact | Confidence | Difficulty | Evidence |
|---|---|---|---|---|---|---|
| O1 | Keep the frame suspend point released when the host GPU has finished the work before it (the uncommitted switch), confirmed by an A/B against upstream's behaviour. | 10 | +7% (recovers a measured loss) | high | low | M |
| O2 | Present a flip when it is due instead of on the next 60 Hz tick, or raise the default virtual vblank. | 10 | up to +7% near a tick boundary; none when the GPU is the limit | medium | low-medium (the title sees vblank timing) | M (one scene) |
| O3 | Shader store on by default, and stamped by what really invalidates it (emitter version + profile) rather than the executable's time. | 9 | removes multi-second hitches from every run after the first | high | low-medium (a stale store must never be used) | M (hitch size), H (fix) |
| O4 | Keep the translated program so a new specialization only re-runs the last stages. | 9 | shorter first-use stalls | medium | medium | H |
| O5 | Skip render-target and depth-target resolution when the registers did not change since the last draw; extend the same test to vertex buffers, push constants and descriptor writes. | 5, 7 | a profile once showed target resolution at 12% of the thread at 720p; a pipeline-bind skip alone gave nothing | medium | medium | M (old profile), one negative result |
| O6 | Cut protection calls: coalesce re-arming over contiguous ranges, re-arm lazily at the next real use instead of at flush, or replace protection faults with the operating system's dirty-page tracking where the host has it. | 2, 8 | up to about 5-7 ms of GPU-thread time; shows in fps only once the waits of #1 are off the critical path | medium | high (two earlier variants corrupted data or crashed the guest) | M (cost), two failures |
| O7 | Make `ObtainBuffer` cheaper for unchanged buffers: finish the per-draw memo work (cuts a, i, j exist), bind one range per table instead of per record. | 3 | a few ms; same caveat as O6 | medium | medium | M (cost); earlier memo gave partial gain |
| O8 | Fewer flushes while eager read-backs are pending; batch the bookkeeping per frame. | 4 | bounded at +3% for the rate limit alone | low-medium | medium | C |
| O9 | Remove the last GPU-written dependency of the GPU thread: let shaders store through run-time descriptors with write tracking on the GPU, or give the CPU a known candidate set to bind. | 1 | up to about 11-14 ms of 48 (then bounded by the 38 ms of thread time) | medium for the size, low for a clean solution | very high | M (cost), many partial attempts |
| O10 | Move resource resolution (program lookup, table walk, buffer and image lookup) for the next draws to worker threads while the GPU thread records. | 3, 5 | bounded at about 13% | medium | very high (shared caches, one render mutex) | C |
| O11 | Service guest faults without the GPU thread (a guest read of GPU data takes its bytes from a finished download instead of a synchronous command). | 8 | at most 5.4 ms/frame on the GPU thread; smoother game threads | medium | high | C |
| O12 | Dirty-range texture upload and detile. | uploads | scene dependent: title A uploaded 45-170 MB per frame before the fill/copy recognitions; small in title B | low | medium | M (old), H |
| O13 | Translated-code size and cost on the host GPU: avoid the lane-pair copy for more program shapes, resolve device addresses that are provably inside one bound buffer without the page table, drop per-access bounds checks where the device's robust access already gives the same result. | 6 | several ms of GPU time in ray-traced scenes | low-medium | high (five earlier variants gave no gain or were slower) | M (cost), failures |
| O14 | Narrower barriers and fewer render-pass breaks (barrier only the written ranges' stages; keep a pass open across uploads by uploading before the pass). | pass breaks | unknown | low | medium-high | H |
| O15 | Pipeline layout reuse and pipeline compile off the draw path without skipping draws (compile ahead from the store at load; a blocking compile only for pipelines never seen). | 9 | hitches | medium | medium (asynchronous compile that skips draws was rejected) | H |
| O16 | Remove the 10 ms re-check wake-ups of parked guest waits (they already wake on signal). | CPU | unknown; at most a few wake-ups per second per parked thread | low | low-medium | H |

## Plan, in order

The order is: first be able to measure, then take the cheap measured wins, then spend effort where a ceiling says
it can pay. One change per experiment, each with a switch while it is under test, each recorded in
OPTIMIZATION_LOG.md whether it wins or not.

**Step 0 - repository state (before any experiment).**
- `main` now contains two upstream merges that have not been measured as a whole: the one at c944a398, which
  brought a measured 7.5% loss (upstream's earlier release of the frame suspend point), and the one of
  2026-11-25 (upstream `93cce665`, 32 commits), which has not been run at all. The switch meant to undo the loss
  (O1) is still uncommitted.
- Settle the base first: build, run the tests, then one comparison of the last measured commit (`5384fd81`),
  today's `main`, and `main` with the switch, on scenario S1 of PERFORMANCE.md, picture included. Nothing else
  starts on a base whose frame rate is unknown.

**Step 1 - measurement that does not depend on hand-made probes.** The list of gaps and the benchmark plan are in
[PERFORMANCE.md](PERFORMANCE.md).
- A frame-time log written by the emulator itself (one line per presented frame with the counters that already
  exist), so average, 1% low and worst frame come from the same run and the same file for every build.
- Profiler zones on the functions named in the table above. The Tracy client is already compiled into every build
  and starts with `--profile` ([ARCHITECTURE.md](ARCHITECTURE.md#targets-and-build)); only some functions carry
  zones (`KYTY_PROFILER_BLOCK`), so the ad-hoc probes of earlier rounds do not have to be re-applied by hand
  once the missing zones exist.
- A script that runs base and candidate in alternation and prints the difference with its spread.

**Step 2 - cheap, measured.** O1, O2, O3. Each is small, each has a measurement that says it should pay.

**Step 3 - GPU-thread CPU time, guided by step 1's zones.** O5, O7, O8, then O6. These only show in fps where the
GPU thread is the limiter (720p performance mode today; the reference frame once the waits are shorter), so they
are measured there first.

**Step 4 - first-use stalls.** O4, O15. Measured by worst frame and 1% low over a cold run and a warm run.

**Step 5 - the dependency chain.** O9, with O11 as its smaller relative. This is the item with the largest measured
size and the longest list of failed attempts; it starts with a written design that names, for the one wait left,
which program writes which words and what the CPU would need instead, and with a ceiling experiment before code.

**Step 6 - host GPU time.** O13, O14, O12. Needs a GPU trace per candidate before and after; the earlier failures
show that intuition about generated code does not predict the result on this hardware.

**Deferred until a ceiling says otherwise:** O10, O16.

## Tests each step needs

| Step | Correctness | Performance |
|---|---|---|
| any | build; `ctest`; the GPU test modes that passed before the change still pass; a picture of the test scene at a fixed instant equal to the base | base and candidate alternated, same scene, same settings, same store state (both cold or both warm) |
| 2 (pacing) | the title's vblank and flip events keep their order and counts (`pm4_flip`, `video_out_flip_rate` tests) | frame-time histogram, not only the average |
| 2 (store) | a store written by another emitter version is rejected (unit test on the header) | cold run versus second run: compile counters and worst frame |
| 3 | `memory_tracker`, `page_manager`, `buffer_cache_*`, `texture_cache_*`, `stream_buffer_ring`, `gpu_command_lane` | GPU-thread busy time per frame and per-zone time, then fps |
| 5 | long runs (the rare device loss and the runaway compute program are open defects: a change here must not raise their rate, which needs tens of runs to see) | wait time per frame, then fps |
| 6 | `shader_recompiler_*` tests, SPIR-V validation on every module, picture | GPU time per program from a GPU trace |

## What this ranking does not know

- The share of each item in titles other than the two measured.
- Today's cost of command recording (#7): the only profile is older than the recorder thread.
- Anything about audio, file I/O and the kernel layer under load: no profile has ever pointed there, and nobody has
  looked on purpose.
- The 1% low of any configuration: earlier runs recorded averages and worst frames from an overlay, not a
  frame-time series.
