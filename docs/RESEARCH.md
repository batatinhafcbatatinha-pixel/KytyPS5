State described: c944a398 (2026-11-23), read 2026-11-25.

# Research notes

Three parts: [lessons from other emulators](#1-lessons-from-other-emulators), [ideas noted but not tried](#2-ideas-noted-but-not-tried), [architectural options visible in the code](#3-architectural-options-visible-in-the-code). No ranking here. Numbers come from [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md) (rows cited by date) and keep their conditions: RTX 3050 Laptop GPU, Ryzen 5 3600, Windows 11, title A / title B, 720p..4K as stated there. Structures: [GPU.md](GPU.md), [GRAPHICS_PIPELINE.md](GRAPHICS_PIPELINE.md), [MEMORY.md](MEMORY.md).

Source of part 1: a read-only survey of two other emulators (same problems; hypotheses, not measured here) and a scan of about 50 open and 100 closed pull requests on 2026-11-19. The gains quoted for pull requests are the authors' claims.

## 1. Lessons from other emulators

| Technique | Where it applies here | Done or not |
|---|---|---|
| Fault-window read-back (512 KiB window per CPU read of GPU-written memory, batched dirty islands, host-cached staging, async publication) | CPU reads of GPU data (guest faults, SRT walker) | Done: `ReadMemory`, 512 KiB window, eager downloads recorded per submission. Little new gain; the survey's top idea is to audit which producer submission each demanded range waits for; partly done by the wait probes (all true dependencies) |
| Read-back disabling / partial-content hashes | descriptors | Rejected: unsafe for changing descriptors; the speculation experiments confirmed ([log](OPTIMIZATION_LOG.md) 2026-11-12, 2026-11-14) |
| Compiled CPU walker of the descriptor tree with a cached traversal; descriptors refreshed per program lookup | per-draw resource resolution | Done (walker code generation from upstream). Memos added (description memo, ObtainBuffer memo, table-walk reuse) but never a whole-draw binding cache |
| Dependency-checked shader reuse (recorded memory dependencies and required SGPR values) | shader reuse | Similar: specialisation keys from walked descriptors; no persistent resolved-binding cache |
| Sparse arenas + page table + fault-bit buffer for shader memory access | address reads, GPU-SRT | Page table + fault buffer exist; sparse arenas not tried (survey ranks them low: CPU/cache structure, not the dominant wait) |
| Stream buffer for small clean buffers (16 KiB limit, 128 MiB) | small uploads | Done (64 MiB stream plus an in-submission reuse memo) |
| Reuse the GPU buffer of an image if GPU-modified instead of download + upload | image/buffer aliasing | Partly: image-to-image copy and compute fill-as-clear (title A) are this idea for two kernels |
| Texture overlap/depth reuse, shared backing, small-image dirty heuristic | image churn | Image backing recycle done (+8%, 2026-11-17); depth<->colour surface flips still recreate images when the pool misses |
| Pipeline/shader disk cache, warm-up | start-up and stutter | Done (shader store, serial warm-up on NVIDIA/Linux, cache stamp); a warm-store layout bug had to be fixed (2026-11-21) |
| Graphics pipeline libraries + background relink (another fork) | compile stutter | Not in this back end |
| Shader objects (no pipeline combinations) | compile cost | Not applicable: compile on a miss is synchronous |
| Dynamic rendering with the pass kept open while state is equal; image barriers accumulated and flushed once | pass splits, barriers | Pass reuse done in the context; a test with no dispatch barriers broke ordering (14.8 -> 6.2 fps, 2026-11-20); transfer/read-back-induced pass splits not audited |
| Upstream pull requests: timeline-poll skip (#1491, claimed 1.4 ms/frame), strided clear check (#2094, ~10% of a GPU thread), tracker upload memo (#1886), unmap drain only if the range is touched (#1263), evicted-image copy without lock (#1947), no texture lock for pages with no image (#1284), vertex push-data layout (#2128) | GPU-thread CPU | Ported 2026-11-19; combined effect in title B quality+RT without RT 18.6-19.4 -> 19.4-19.8 fps |
| Not taken: parallel eboot patching (#2260: 7-9 s -> 1-2 s load), parallel intro video decode (#1449), unmap/remap cache keep (#2064/#2066), batched sync read-backs (#2093, opt-in), walker reads beside pending writes (#1828, stacked, +1773 lines) | load time, video, memory churn | Not done; #1828 is the same family as the fork's own walker work |
| Rejected: async shader compile that skips draws (#2119), scheduler priority hacks (#2106/#1050/#1125), denormal-flush shortcut (#1565), bundled label batching (#506) | | Rejected on correctness |
| Another KytyPS5-based fork with a staged pipeline, same PC, same scene (2026-11-19 comparison) | whole renderer | Quality+RT 1080p: software RT 6.4 fps, native 12.6; performance mode 1080p 40.3 fps; this fork 14.8-15.4 and 58.6 at that date. The staged pipeline was not ported; this fork was faster in every mode |

## 2. Ideas noted but not tried

From the work log; none was implemented. "Evidence" cites [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md).

| Idea | What it would replace or add | Evidence | Unknown |
|---|---|---|---|
| GPU-side resolution of the remaining chain (scalar SRT values, written-buffer descriptors, indirect arguments) as a default path cheaper than the ~7% of the host view; GPU stores through run-time descriptors with write tracking; a CPU-known candidate set of descriptors for P11/P12 (bind and mark all) | the CPU walk and the wait per frame of "Known remaining waits" item 1 | each removed link moved the wait to the next reader; masks/hash-map variants were slower (dead ends); one wait per frame (~11-14 ms) remains in title B quality+RT | whether write tracking of run-time records is feasible at acceptable cost |
| "Mark only what the shader writes" through GPU write tracking of run-time records | conservative marking (13.4 MB / ~4000 ranges of eager downloads per frame for 16 KB actually written) | inexact ceiling 18 -> 27 fps (2026-11-20); marking only cached spans was slower | exact tracking design |
| Move guest-fault servicing off the GPU thread | `SendCommandSync` path (`SendCommandSync`, `graphicsRun.cpp:105-117`) | ceiling 5.4 ms/frame (~10%); sync command 5.3-5.9 ms/frame, 30-33 calls/frame (2026-11-20) | correctness of servicing on another thread |
| Detile straight into the image instead of detile + buffer-to-image copy | `TileManager::Detile` + `Image::Upload` ([MEMORY.md](MEMORY.md)) | unpriced | gain |
| Recognise the two-dword 16-bit-float RGBA clear | `TryConsumeComputeFill` (`renderCompute.cpp:142`) | ceiling 0.1-0.2 ms frame gain (noise); GPU 0.48 ms at 720p, 0.71 ms at 1080/1080 (2026-11-15) | |
| Skip render-target resolves when the target registers did not change | `ResolveRenderColorTarget` / `ResolveCache` (colour and depth) | 12% of the GPU thread in a 2026-11-16 profile (title B, 720p, 1330 draws/frame) | how much is already skipped by `ResolveCache::Reuse` |
| Present off the GPU thread | presentation | the "4%" was mis-symbolised: presentation already has its own thread | |
| Fewer Vulkan calls per draw | `renderDraw.cpp:1153-1213` | Vulkan driver ~28% of the GPU thread at ~50 fps (2026-11-16); pipeline-bind skip gave no gain (2026-11-17); recorder +5% (2026-11-19) | |
| Hash the unwatched set once per frame instead of per submission | `ScanUnwatchedPages` | partly done (set rebuilt every 64 frames; hashing at submit) | |
| Skip repeated BLAS updates within a frame | `RayTracing::Prepare` BLAS builds (`rayTracing.cpp:394-669`) | exact only if no GPU write reached the BLAS memory; BDA stores are known only after the GPU runs | |
| Reduce candidates reaching per-candidate guest code in the RT traversal; instruction-cache pressure | emitter of the traversal rewrite | alpha-test code ~7.5 ms of P5's 16 ms in probe splits; modules 276-595 KB of SPIR-V (2026-11-21/22) | large rewrite |
| Make the GPU timestamp probe reliable on title B | measurement | it under-reports on that title, so emulator-made GPU work there is unmeasured | cause of under-reporting |
| Whether the translated compute programs cost more than needed | recompiler output | no reference measured | |
| Sparse buffer arenas once merge/allocation cost is profiled; audit of transfer-induced pass splits | `BufferCache::CreateBuffer`; pass breaks | no numbers | |
| Remaining upstream PRs: #2260 (load time), #1449, #2064/#2066, #2093, #1828, a redesign of #2095 for this tree's page table | | claims only | |
| Fork candidates not tried: ObtainBuffer write memo (valid within one tick, judged to almost never hit here); BDA pre-sync once per submission (`SynchronizeBda` ~2/30 of samples, low value) | | | |
| Native-RT matcher coverage for more traversals; per-candidate any-hit hoisting | `TraceTraversalsWithRayQueries` | native RT rewrites 3 programs, +3% (2026-11-21) | |
| Reproducible A/B for the dim-surface defect (dump the shadow map in dim vs bright runs, compare with a 60 fps reference) | defect, not speed | cause open | |
| Title C: reserve/mapping calls the probe did not cover (batch map, memory-pool batch, mtype protect), and an export for memory mapping the kernel layer lacks | compatibility | title C stops at ~35 s | |
| Nsight GPU Trace of title A or of emulator-made passes | measurement | needs counters unlocked and a working attach (earlier attach failed) | |
| Maximal-reconvergence compute (tried in an interleaved batch for P4, result not recorded); wave64-aware split analysis for non-RT programs | emitter | non-RT wave64 programs cost ~3 ms/frame in total, judged not worth the risk (2026-11-20) | |

## 3. Architectural options visible in the code

Each option states what it would replace, what evidence exists in [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md), what is unknown. "Unmeasured" = no number in the notes.

### 3.1 Asynchronous pipeline and shader compilation
- Replaces: synchronous compile on a miss in `PipelineCache::GetProgram` (`src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp:309-501`, `CompileModule` `:253-307`), `GetGraphicsPipeline` (`:930`, `:1117`), `GetComputePipeline` (`:1125`, `:1144`), `CreatePipelineInternal` (`shaders.cpp:202-424`). Today there is no worker pool, no placeholder pipeline; only `WarmUp` runs creation in parallel.
- For: pipeline compile hitches of 6.6-8.3 s peak at scene start on cold stores (2026-11-19/21, "Known remaining waits" item 11); first run after every build is cold because the store is keyed by build time.
- Against: an async version that skips draws was rejected on correctness in the pull-request scan (#2119); a placeholder or deferred draw changes what the guest sees.
- Unknown: how many modules and pipelines a title reaches; what fraction of frame time falls in steady state after warm-up (`ShaderCompileNs` excludes translation, so the compile cost is under-counted).

### 3.2 Pipeline libraries (`VK_EXT_graphics_pipeline_library`) and shader objects
- Replaces: whole-pipeline creation per `GraphicsPipelineKey` (`pipelineCache.h:52-109`: stage ids + formats + vertex layout + blend/topology all in one key).
- For: key explosion is a candidate cause of many `vkCreateGraphicsPipelines` calls (candidate in [GPU.md section 4](GPU.md#4-pipeline-cache-and-shader-store)); another fork uses a staged pipeline.
- Against: that fork was slower in every mode in the 2026-11-19 comparison; a 2026-11-14 review of "instruction pointer invalid" device losses in another code base ranked fast-linked pipeline libraries on this driver as the most likely cause (unconfirmed). Shader objects need a different dynamic-state model than the current pipeline cache.
- Unknown: the number of distinct keys per shader hash; driver support list for the device.

### 3.3 Descriptor buffers instead of push descriptors / pool sets
- Replaces: `CommitBindings` (`renderer/descriptors.cpp:1252-1435`), `DescriptorHeap::Commit` (`descriptorHeap.cpp:34-84`), per-pipeline set layouts (`shaders.cpp:80-142`).
- For: descriptors are rewritten per stage per draw (push) or updated per draw on the GPU thread (pool, `vkUpdateDescriptorSets` not through the recorder); no descriptor-buffer extension is used today.
- Evidence: none specific. Vulkan driver time was ~28% of the GPU thread at 720p title B (2026-11-16 profile), not split by call. A skipped repeated pipeline bind gained nothing.
- Unknown: share of `CommitBindings` in the GPU thread; value of `max_push_descriptors` and how often the pool path runs.

### 3.4 Moving resource resolution off the GPU thread
- Replaces: `PrepareDrawRenderState`, `PrepareBindings`, `PrepareGraphicsBindings`, `FindBuffers`, `RebindBuffers` (`renderDraw.cpp:829`, `descriptors.cpp:779-1252`) done under the render mutex on one thread; includes the SRT walk (`MaterializeResources`).
- Evidence: "pure resource resolution ~7 ms (moving to workers is worth at most ~13%)" (2026-11-20, 18.1 fps probe build, 55 ms frame). Parallel page hashing gave ms gains but no fps in three tries; ObtainBuffer read-range memo made it slower; while a wait remains on the critical path CPU cuts do not show in fps (2026-11-23).
- Constraints seen in code: `BufferCache` maps have no locks (GPU-thread only, `bufferCache.h:101`); `TextureCache::m_lock` is a spin lock held through uploads; the SRT walk needs the GPU thread's fault handling ([MEMORY.md](MEMORY.md)).
- Unknown: how much of the resolution depends on state that changes between draws of one frame.

### 3.5 Dirty-range texture upload
- Replaces: whole-image re-upload in `TextureCache::RefreshImage`/`UploadImage` (`cache/textureCache.cpp:1236-1258`, `:950-1010`); there is no dirty rectangle, all mips and layers are uploaded each time.
- Evidence: title A at 4K output uploaded ~2.8 GB/s (57 MB/frame) of render targets re-uploaded through the detiler after a GPU buffer write; skipping those re-uploads gave 18.99 -> 17.71 ms (ceiling, 2026-11-15), and the two exact fixes (fill-as-clear, image copy) recovered most of it (-0.70 ms, then 17.96 -> 16.82 ms). Title B default size: skipping a 1440p image written whole by a compute program every frame gave no gain (2026-11-17, GPU-bound).
- Unknown: whether any title rewrites only part of a large texture per frame; the detile scratch reallocation cost (candidate, [MEMORY.md](MEMORY.md)).

### 3.6 Whole-draw binding cache / skipping unchanged draws
- Replaces: per-draw `GetProgram` (SRT walk, vector rebuilds), `FindBuffers`/`ObtainBuffer`, descriptor writes, per-draw bind calls ([GRAPHICS_PIPELINE.md section 9](GRAPHICS_PIPELINE.md#9-work-paid-per-draw--dispatch--batch--frame)).
- Evidence: the program lookup was 463 of 784 ms/s in the draw zone at the start (2026-11-12, title A, 43.3 fps); the fork's memos gave -1.7 ms (2026-11-14) and +3..8% steps later; GPU-written descriptors defeat reuse (stale reads break specialisation reuse, 24 fps).
- Needs: exact table generations (the survey's note); the table-walk reuse already added is the partial form.
- Unknown: fraction of draws whose descriptor words and bound ranges are unchanged between consecutive draws.

### 3.7 Barrier model: replace the full memory barrier after every dispatch
- Replaces: `ShaderAccessBarrier` after each dispatch (`renderCompute.cpp:566`, `shaderResourceBarrier.cpp:95-101`) and the all-commands hazard barrier before writing dispatches (`:469-473`).
- Evidence: removing barriers around dispatches (ceiling) dropped 14.8 -> 6.2 fps (GPU 47.8 -> 137 ms) because the BVH refit chain needs the order (2026-11-20). The code comment says the barrier replaced a host fence that serialised every dispatch.
- Unknown: how many dispatches are independent; no per-resource dependency tracking exists to drive narrower barriers.

### 3.8 Pass structure: load/store ops and in-pass resolve
- Replaces: colour `eLoad/eStore` and depth `eStore` always (`context.cpp:54-72`), resolves as separate commands (`image.cpp:399-448`), `EndRendering` at each upload and hazard.
- Evidence: none (no tile-based GPU was measured; the test GPU is an immediate-mode desktop part).
- Unknown: the number of pass breaks per frame (`beginRendering` vs `draw` counters).

### 3.9 Retaining the translated program for new variants
- Replaces: re-translation from guest memory when a known program needs a new module (`pipelineCache.cpp:477-479`, `:499`).
- Evidence: none measured; `ShaderCompileNs` does not cover `TranslateProgram`, so the cost is invisible in the overlay.
- Unknown: count of variants per hash; memory cost (the program cache is never evicted, walker code size unknown).

### 3.10 Recorder alternatives
- Replaces / refines: the 32 MiB ring recorder (`recorder.cpp`).
- Evidence: +5% in title B quality+RT (18.1-18.8 -> 19.1-19.6 fps, RT 12-13 -> 14-15 fps, 2026-11-19); cuts `b`, `c`, `f` reduce its per-command cost. Each batch end and each destroy forces a drain.
- Unknown: drain count per frame (no counter exists).

### 3.11 Presentation copies
- Replaces: copy into a frame (`Frame::CopyFrom`) followed by blit/draw into the swapchain image (`swapchain.cpp:208-222`, `:756-890`).
- Evidence: title A at 4K output ran at 51.3 fps but at 59.6-60 (cap) with 1280x720 output, i.e. the load is in output-resolution passes (2026-11-15); the split between game passes and these two operations is not stated (the two 3840x2160 passes cost 2.10 ms, the batch total was 14.98 ms).
- Unknown: the cost of the copy and the blit separately (`PresentNs` is a CPU-side timer).

### 3.12 Buffer cache structure
- Replaces: union-merge growth that never splits (`BufferCache::CreateBuffer/ResolveOverlaps`), upload barriers (`UploadCopies` ends rendering and uses global barriers), staging by CPU memcpy per dirty run.
- Evidence: in [MEMORY.md](MEMORY.md); numbers: eager read-back batching 4000 -> 11 calls/frame (2026-11-20); deferred page-protection application (cut `g`) crashed a guest and is off.
- Unknown: buffer sizes and creations per frame.

## Not covered yet

- Area 6 (diagnostics) study was not available; probes and diagnostics options are not summarised here.
- The survey of the two other emulators is quoted through the work log only; the survey text itself was not read by this edit.
- No evidence exists in the notes for options 3.2, 3.3 and 3.8; they come from reading the code (the studies of the host renderer and the memory area), not from measurement.
- Whether any of the listed upstream pull requests conflicts with fork changes (only #1828 was noted as the same family).
