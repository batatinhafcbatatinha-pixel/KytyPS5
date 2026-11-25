State described: c944a398 (2026-11-23), read 2026-11-25.

# Life of a frame

Order of work from the title's submit call to the presented image. Structures behind each step are in [GPU.md](GPU.md) (renderer, recorder, scheduler, pipeline cache, descriptors, shaders) and [MEMORY.md](MEMORY.md) (guest memory, write detection, buffer and texture caches, read-backs). Measured numbers: [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md). Paths are repository-relative; `graphicsRun.cpp` = `src/graphics/guest_gpu/graphicsRun.cpp`, `renderDraw.cpp`/`renderCompute.cpp`/`commandScheduler.cpp` = `src/graphics/host_gpu/renderer/...`.

Suspend point: the working tree holds an uncommitted change in `src/graphics/guest_gpu/graphicsRun.cpp/.h` (the switch `KYTY_SUSPEND_POINT_WAIT`). The text marks the places that depend on it "(uncommitted at the time of writing)".

```mermaid
sequenceDiagram
  participant T as guest thread
  participant Q as submission queues
  participant C as GPU thread (CP + renderer)
  participant R as recorder / submit threads
  participant H as host GPU
  participant P as present thread
  T->>Q: submit DCB (enqueue + signal)
  Q->>C: Process(submission)
  C->>C: PM4 decode, state, draws, dispatches
  C->>R: vkCmd* closures; batch tick N
  R->>H: vkQueueSubmit
  T->>Q: SuspendPoint (once per frame)
  C->>C: flip packet: PrepareVideoOutFlip
  H-->>C: tick done -> priority callbacks (labels, flip ready)
  P->>H: vblank: FlipQueue::Flip -> Present
```

## 1. Title submit call to the GPU thread

Facts:
- `Agc::Initialize` creates the window and the video-out driver and calls `g_renderer->InitializeGpu(&video_out)`, which creates the guest GPU worker (`src/libs/agc.cpp:67-83`, `graphicsRun.cpp:44`).
- `AgcDriverSubmitDcb` calls `submit_dcb(packet->addr, packet->dw_num)` (`agc.cpp:4179-4192`); `submit_dcb` dumps the buffer when enabled and calls `g_renderer->GetGpu().Submit(span)` (`:4174-4177`). `AgcDriverSubmitMultiDcbs` submits each non-null DCB in order (`:4201-4228`). `submit_command_buffer` routes queue numbers in `[0x20, 0x58)` to `submit_acb` (`SubmitCompute(queue, span)`, `:4230-4237`) and the rest to `submit_dcb` (`:4245-4255`). `AgcDriverAgrSubmit` is unimplemented and always fails (`:4194-4199`).
- `Submission` (`graphicsRun.h:55-66`): `type` in {`Commands`, `FlipPreparation`, `SuspendPoint`}, `queue_id`, a span of PM4 dwords, `Pm4Execution`, `blocked`/`resumed` flags, a `RangeSet` of unwatched BDA pages found at submit, a flip request id. Queues: `ComputePipeCount = 7`, `QueuesPerComputePipe = 8`, `ComputeQueueCount = 56`, `ComputeQueueBase = 0x20`, `QueueCount = 57`, one deque each (`graphicsRun.h:49-53`, `:79`).
- `GuestGpu::Submit` builds a `Commands` submission for queue 0 (`graphicsRun.cpp:119-128`); `SubmitCompute` maps a queue in range to queue id `1 + (queue - 0x20)` (`:130-141`); `SubmitFlipPreparation` builds a `FlipPreparation` submission for queue 0 (`:143-149`).
- `Enqueue` scans unwatched pages for `Commands` submissions on the submitting thread (`:425-429`), takes `m_queue_mutex`, refuses submissions after shutdown starts, pushes into the queue's deque, increments `m_submission_count` and signals `m_work_available` (`:430-435`). A separate deque `m_commands` holds deferred host callbacks (`m_pending_commands`, `graphicsRun.h:80-81`).
- The title's submit calls only enqueue and signal. The title blocks at a suspend point, at `WaitForIdle` (`:437-443`), at flip-event/vblank waits (`src/graphics/presentation/videoOut.cpp:1693-1708`) or on WAIT_REG_MEM inside PM4.

## 2. Submission queues to the command processor

`GuestGpu::ThreadRun` (`graphicsRun.cpp:445-549`):
- Labels the thread `Thread_Gpu` and tries to reserve a logical core (`:445-453`). Waits on `m_work_available` while both the deferred-command deque and the submission count are empty (`:460-465`); stops after draining when `m_stopping` (`:466-469`, `:502-506`); deferred commands are preferred to submissions (`:470-474`).
- Scans the 57 queues round-robin from `m_next_queue`, skipping a queue whose front is `blocked` (`:476-483`); if every front is blocked it waits on `m_work_available` with `WaitFor(..., 100)`, which takes microseconds (`src/common/threads.h:64`), so up to 100 µs (Windows rounds a wait under 1000 µs to 1 ms, `src/common/threads.cpp:292-296`), and clears the blocked flags (`:484-492`). Pops the submission, decrements the count, advances `m_next_queue` (`:494-499`).
- Times deferred commands and submissions with `Counter::GpuThreadBusyNs` (`:510-511`, `:526-529`). An incomplete `Process` marks the submission `blocked` and `resumed` and pushes it back to the front of its queue (`:531-537`); otherwise all fronts are unblocked (`:538-542`). `m_idle` is signalled when both deques drain (`:544-547`).

`GuestGpu::Process` (`:551-609`): gets the queue's `CommandProcessor` (`:166-175`), calls `BufferInit()` (`:228-231`: resets the draw counter, begins a scheduler command with the current register banks). Per type:
- `Commands`: advances the tracker epoch, rescans unwatched pages (if previously blocked) or queues the pages scanned at submit, runs `cp.Process`, flushes the scheduler when the execution made progress (`:557-572`).
- `FlipPreparation`: `cp.PrepareCpuFlip(request_id)` (`:573-575`).
- `SuspendPoint`: runs the render garbage collector, flushes and resets the processor, releases the suspend semaphore (`:576-584`). Committed state (c944a398): the semaphore is released at once after the flush, "like completion labels", when the CP has submitted every packet before it. (uncommitted at the time of writing) With `KYTY_SUSPEND_POINT_WAIT` unset or not starting with `0`, the release is a priority operation run when the host GPU has finished the work before it (`DeferPriorityOperation` inside the `SuspendPoint` case of `GuestGpu::Process`; this code is not in the committed tree); `0` keeps the committed behaviour. The comment states the default makes the guest prepare at most one frame ahead of the host GPU. `GuestGpu::SuspendPoint` (guest side) refuses to run on the GPU thread or inside a deferred operation, acquires the binary semaphore `m_suspend_point_ready` (initial 1, so the first point returns at once), enqueues a `SuspendPoint` submission and increments the completed-frame counter (`:151-160`).
- An incomplete execution returns `Pm4ProcessResult::Blocked` when its buffer stack is not empty (`:590-609`).

Hypothesis (not measured): a suspended WAIT_REG_MEM keeps its submission at a queue front and is retried only when the GPU thread revisits queues (`:531-543`, `:484-492`); if the guest writes the awaited address from another thread, wake-up depends on that short poll (100 µs, 1 ms on Windows), not on the write; while every front is blocked the GPU thread therefore wakes up about 1000-10000 times per second. No other notifier was checked. Test: count the poll wake-ups against the useful retries and the CPU time they use.

## 3. The command processor

Decode and dispatch (`CommandProcessor::ProcessPm4`, `graphicsRun.cpp:661-740`):
1. Walks a stack of command-buffer cursors (`:661-671`). Reads the header, `opcode = (header >> 8) & 0xff` (`:676-677`); skips a bare `0x80000000` header and length-1 packets (`:679-683`); dumps the packet when debug dumps are enabled (`:687-691`, gate `src/graphics/host_gpu/renderer/debug.h:26-29`).
2. Predicated packets are skipped when the predicate bit is set and `ShouldSkipPredicatedPackets()` (`:693-699`).
3. `PacketKeepsGuestMemory` classifies packets that cannot write guest memory from the CPU (`:623-659`); for all others, deferred buffer-cache tracking is flushed before the handler (`:701-703`).
4. Dispatch through the 256-entry table `g_cp_op_func` (`src/graphics/guest_gpu/command_processor/pm4Dispatch.cpp:42-86`, `:121`) at `graphicsRun.cpp:704`; an unhandled opcode logs a dump and exits (`:706-719`). A second table `g_cp_op_keeps_draw_window` decides whether the draw window ends before the packet (`:721-723`, table `pm4Dispatch.cpp:88-104`). The handler receives `packet + 1` and returns the dwords consumed; the dispatcher bounds-checks, returns early if suspended, advances the cursor and applies chain/branch buffers (`graphicsRun.cpp:724-740`).
5. Custom codes travel in NOP packets: `CpOpNop` looks up `g_cp_op_custom_func` by the NOP's R field (`pm4Handlers.cpp:1145-1163`; tables `pm4Dispatch.cpp:106-115`, `:123-124`; codes `pm4.h:72-80`). Register writes go through `SetRegisters` and per-bank tables (`pm4Handlers.cpp:1250-1271`; `g_hw_sh_func` `pm4Dispatch.cpp:119-120`).

State kept (`commandProcessor.h:129-148`): `HW::Context m_ctx` (render-target base address, array/mip view, format and compression flags, sample/fragment attributes, dimensions, DCC control, CMASK/FMASK addresses, clear words, DCC address; `hardwareContext.h:12-101`), an optional saved context, `HW::UserConfig m_ucfg`, `HW::Shader m_sh_ctx`, index type/size, index-buffer size and base, draw and dispatch indirect argument bases, persistent instance count and address, interrupt event id, `m_flush_draws`, a draw counter, a synthetic occlusion counter, the predicate-skip flag. `Reset` clears all (`graphicsRun.cpp:177-190`); `ResetQueue` clears the banks named and optionally pops a saved context (`:192-206`); `ApplyContextStateOperation` = clear/push/pop/push-clear (`:208-226`). `BufferFlush` flushes and publishes occlusion queries (`:233-237`); `BufferWait` begins a command and finishes the scheduler (`:239-242`).

Indirect buffers: `CpOpIndirectBuffer` treats a 14-dword length as a conditional branch (`pm4Handlers.cpp:945-950`), otherwise reads target and length and calls `ProcessIndirectBuffer(span, chain)` (`:952-974`); `CpOpBranch` evaluates a 64-bit compare against guest memory and chains the taken buffer (`:581-611`); the fetcher replaces the cursor for a chain and pushes a stack entry for a call (`graphicsRun.cpp:611-616`, `:732-740`).

Packet handlers that touch memory:
- WRITE_DATA: destination selector; fills GDS, writes in GPU order (`BufferCache::WriteInGpuOrder` on pages the CPU should not wait on), or writes the guest address with volatile stores (`graphicsRun.cpp:292-329`, parser `pm4Handlers.cpp:1392-1401`).
- RELEASE_MEM: normalises interrupt selectors, optionally writes back GPU-modified images, publishes occlusion queries, writes a 32/64-bit label (or reference clock / GDS value) honouring the GPU-order and backing-store paths, defers an interrupt event (`graphicsRun.cpp:1025-1105`, parser `pm4Handlers.cpp:1191-1206`).
- WAIT_REG_MEM32/64: reads guest memory once and calls `SuspendPm4()` when the condition is false (`graphicsRun.cpp:265-280`, parser `pm4Handlers.cpp:1355-1390`). MEM_SEMAPHORE increments/decrements a guest counter and suspends on zero (`:291-299`).
- Compute queues use the same `CommandProcessor` class with a different event id and an async flag (`:166-175`, `:980`, `:1044`).

Per-packet work before the renderer: a deferred buffer-cache flush for non-state packets; ending the draw window; a register-bank write; or a call into the render scheduler. `CountDraw` publishes read-back writes and, when `m_flush_draws != 0`, flushes the scheduler every N draws (`:832-843`; default `0` = off, `src/common/emulatorConfig.h:103`).

Hypotheses (not measured): the deferred buffer-cache flush before non-state packets serialises memory work into PM4 decode (`:701-703`, needs a count of `FlushDeferredTracking` calls against the packet mix); with `flush_draws != 0` extra queue submissions appear.

## 4. A draw, step by step

Entry from the packet: `IT_DRAW_INDEX_2` -> `CpOpDrawIndex` (`pm4Dispatch.cpp:56`) validates fields and calls `cp.DrawIndex(...)` (`pm4Handlers.cpp:698-715`); multi-instance `CpOpDrawIndexMultiInst` (`:719-742`); offset draws `CpOpDrawIndexOffset` (`:806-815`). `CommandProcessor::DrawIndex` calls `CountDraw`, fills index type/size and a missing instance count from `NumInstances()`, then `m_renderer.GetRenderExecutor().DrawIndex(CurrentBuffer(), args)` (`graphicsRun.cpp:845-852`).

Indirect: `CpOpDrawIndirect` coalesces consecutive matching packets into one multi-draw (`pm4Handlers.cpp:744-774`); `CpOpDrawIndirectMulti` decodes a GPU-side count (`:776-804`). Both call `DrawIndirectMulti` (`graphicsRun.cpp:907-973`): check the stride against Vulkan's indirect layouts, read a GPU count if present, add to `Counter::IndirectDraws`, patch indirect offsets into user SGPRs when possible (`PatchIndirectDrawOffsets`, `:869-905`), else loop over records, one renderer call per record (`:948-972`); then `RenderExecutor::DrawAuto` or `DrawIndex`.

Renderer side: `RenderExecutor::DrawIndex` (`renderDraw.cpp:1273`) or `DrawAuto` (`:1364`). Both hold `m_context.GetMutex()` for the whole draw (`:1281`, `:1372`). Legend: **[draw]** paid on every draw; **[cached]** reused when its key is unchanged.

1. **[draw]** `CommandScheduler::PopPendingOperations()` (`:1277`): runs deferred normal callbacks whose tick finished (polls the timeline at most once, `commandScheduler.cpp:316-344`); can block in `WaitPriorityOperations(tick)` (`:341`).
2. Early outs: zero counts; `ConsumeMetadataColorOperation` (colour-control mode EliminateFastClear/FmaskDecompress/DccDecompress, `:572-585`; FCE resolves the target to materialise clear metadata); `DepthStencilCopy` (`depthRenderTarget.cpp:358`: a draw replaced by `vkCmdCopyImage` when the guest forces depth/stencil dirty with different read/write bases); `ResolveColorTargets` (colour-control mode 3 -> `Image::Resolve`: copy or `resolveImage`, `renderDraw.cpp:1442-1472`, `image/image.cpp:399-448`). A consumed draw does `ResetBindings()` and returns.
3. Debug dump / `uc_check` / `hw_check` (`:1296-1315`); the checks are called unconditionally, their cost is in `debug.cpp` (not read in detail).
4. **[draw]** `GetDrawTopology` (`:744`): tri-fan/polygon/quad-list-legacy -> `eTriangleFan`; rect lists -> `ePatchList`; patch only if `Config::TessellationEnabled()` (default false). `ResolvePrimitiveRestart` (`:790`): may scan the whole index buffer in guest memory on the CPU when a custom reset value is set (`:819-825`). 8-bit indices are expanded to 16-bit in a `std::vector` per draw (`:1334-1345`).
5. `PrepareDrawRenderState` (`:829`): `ImageHeap::BeginDraw()`; `PipelineCache::GetGraphicsPrograms` (`pipelineCache.cpp:683-914`) -> per stage `GetProgram`. **[draw]** per stage: mapped-data lookup, vertex attribute table read (dynamic fetch), static-key rebuild, program map lookup, `MaterializeResources` (the SRT walk), module scan ([GPU.md section 12.3](GPU.md#123-per-draw-work-for-an-already-compiled-shader)); **[cached]** program, module and SPIR-V. A miss translates and compiles synchronously ([GPU.md section 4](GPU.md#lookup-and-miss-paths)). Then `ResolveRenderColorTarget` per slot (`colorRenderTarget.cpp:42`) and `ResolveRenderDepthTarget` (`depthRenderTarget.cpp:~255`): **[cached]** `ResolveCache` hit when the 8-slot register struct is byte-identical to the last for that slot and `Reuse` passes (depth: one cached entry); a miss builds an `ImageDesc` and calls `TextureCache::FindImage` ([MEMORY.md](MEMORY.md)). Attachments not matching the raster sample count are dropped with a one-time warning (`:857-879`). A draw with no colour, no depth and no active PS is skipped (`:881`).
6. `ResolveDrawEmit` (`:1242`): vertex offset / first instance; an indirect draw that cannot run natively (quad lists, tessellation, mesh without payload room) returns nullopt, the draw is not executed and `DrawIndex` returns false (the caller's reaction is not covered, see [Not covered yet](#not-covered-yet)).
7. `ExecutePreparedDraw` (`:1040`):
   a. `CopyDepthToColor` (`:986`): depth-to-colour copy for the rect-list "copy depth to colour" state; needs maintenance8 (else `EXIT_NOT_IMPLEMENTED`).
   b. Mesh limit checks (`:1048-1083`); mesh shaders get indices by device address (`:1085-1094`).
   c. **[draw]** `PrepareBindings` per stage (`descriptors.cpp:779-833`): each non-heap image through `ResolveTexture` unless `source.module` and `snapshot.images[i]` equal the last draw's and `ResolveCache::Reuse` passes (**[cached]**, `:809-811`); samplers through `SamplerCache::GetSampler`; user data copied into `shader_data`. `desc_memo` (8192 entries) memoises image descriptions (**[cached]**).
   d. **[draw]** `PrepareGraphicsBindings` (`descriptors.cpp:1191`): `FindBuffers` (per buffer resource: decode V#, clamp size, `BufferCache::FindBuffer`; `MarkWrittenTable` hashes up to 1 MiB of a bound table, `:887-995`); `BackDrawAddressBases` (3 cache pages after each scalar-loaded base, `:1176`); `PrepareBda` when the stage uses DMA; `ImageHeap::Rebind` when it uses the heap; `RebindImages` (`FindTexture` view per image, `:1136`); re-find targets whose binding went stale; `RebindBuffers` (`:1086`): `NativeStorageBuffer` -> `BufferCache::ObtainBuffer(address, size, written, sync_image, id)` per buffer; upload of `flattened_srt` and `shader_data` to the stream buffer (`NativeUpload`, `:743-749`).
   e. **[draw]** `AcquireVertexBuffers` (`:664`): collects non-empty vertex V# ranges, sorts and merges, `ObtainBuffer` per merged range. `PrepareIndexBuffer` (`:889`): guest range through `ObtainBuffer`, or a host-expanded copy through the Stream utility buffer.
   f. Indirect arguments: `ObtainBuffer` for the record array (`:1119-1126`).
   g. `PipelineCache::GetGraphicsPipeline` (`pipelineCache.cpp:930`): **[draw]** builds a `GraphicsPipelineKey` and does one `try_emplace` (XXH3 + memcmp); **[cached]** the pipeline; a miss creates it synchronously.
   h. `AcquireRenderTargets` (`:442`): `FindRenderTarget/FindDepthTarget` view; `Image::Transit` to attachment layout (GENERAL if also bound as texture, else COLOR_ATTACHMENT_OPTIMAL); depth load-clear decision including HTile "meta clear" consumption; attachment-feedback-loop detection for depth sampled in the same draw (`:499-522`). Note (`:1149`): preparation "may synchronously finish and restart the scheduler": `ObtainBuffer`/`FindImage` can submit the batch and wait.
   i. Record: `bindVertexBuffers2` (`:914`, every draw, no redundancy check); `CommitBindings` (`descriptors.cpp:1252`: push descriptors or a pool set, image `Transit`s, GDS barrier); `bindIndexBuffer`; `pushConstants` for draw dwords and user clip planes (`:1177-1185`); `UpdateGraphicsDynamicState` (`:311`) + `DynamicState::Commit` (`dynamicState.cpp:7`: only dirty states, **[cached]**; all marked dirty at the start of every command buffer, `commandScheduler.cpp:487`); a shader-write hazard barrier when a stage writes buffers (`EndRendering` + memory barrier, `:1193-1206`); `CommandBuffer::BeginRendering` (`context.cpp:43`: reuses the open pass if `RenderState` is equal (**[cached]**), else ends it and begins a new pass); optional `setCheckpointNV`; `bindPipeline` (every draw, `:1213`); occlusion query begin/end; then `draw`/`drawIndexed`/indirect/`drawMeshTasksEXT` (`:1216-1228`). Quad-list-legacy emits one 4-vertex draw per quad (`:969-981`). After a draw with buffer writes: `EndRendering` + `ShaderWriteBarrier` (`:1233-1236`). All these `vkCmd*` calls go to the recorder ring ([GPU.md section 3](GPU.md#recorder-srcgraphicshost_gpurendererrecordercpp)).
8. `ResetBindings()` clears the per-image binding flags of every bound image (`descriptors.cpp:772`).

Cached across draws: program/module lookup (by key), pipeline, image-description memo, `ResolveCache` colour/depth, per-stage resolved image bindings while descriptor values are unchanged, descriptor pool sets (batches of 32), dynamic state (dirty bits per command buffer), render-pass state (equality test).

Image barriers: `Image::GetBarriers` (`image/image.cpp:123-222`) emits none if layout and access are unchanged and the previous access had no write bits, so a storage image written and re-bound as storage gets a barrier per `Transit` ([GPU.md section 8](GPU.md#8-render-targets)).

## 5. A compute dispatch, step by step

Entry: `PM4 DISPATCH_DIRECT` -> `CpOpDispatchDirect` -> `CommandProcessor::DispatchDirect` (sets the compute wave size, passes the async-compute flag, `graphicsRun.cpp:975-980`) -> `RenderExecutor::DispatchDirect` (`renderCompute.cpp:308`); indirect: `DispatchIndirect` (`:660`).

Direct dispatch:
1. `PopPendingOperations`; `SubmitIfIdle()` unless async compute ([section 6](#6-flush-points-and-batch-submission)); take `m_context.GetMutex()` (`:328`); skip zero-group or null-CS dispatches.
2. Build `ShaderComputeInputInfo` (workgroup counts; thread-dimension mode divides by the CS group size); `ImageHeap::BeginDraw`; `GetComputeProgram` (`pipelineCache.cpp:916`): same per-stage program work as a draw stage.
3. Fast paths that replace the dispatch:
   - `TryConsumeComputeMetaClear` (`:57`): the written buffer is a CMASK/DCC/HTILE metadata address (`TextureCache::IsMeta/ClearMeta`); replaced by a metadata clear flag; skipped if the metadata is also read or the program has XOR.
   - `TryConsumeComputeFill` (`:142`): programs the recompiler classified as uniform fills (`resources.uniform_fill`, recogniser `RecognizePatternFill`, [GPU.md section 12.1](GPU.md#121-pipeline-guest-bytes-to-vkshadermodule)). Image fill of a stencil plane -> `TextureCache::ClearImage`; buffer fill -> `ClearImageFromBuffer` or CPU `BufferCache::FillBuffer` (range CPU-owned and with no registered image); buffer copy -> `CopyImageFromBuffer` only when both are whole images of one layout. Preconditions: group-size stride equals fill stride, whole range covered, sources disjoint, `specialization_reads.empty()` (`:382`). Otherwise the program runs.
   - Group counts above device limits are skipped with a log (`:397-409`).
4. `EndRendering`; `GetComputePipeline` (module id only, `pipelineCache.cpp:1125`); `PrepareBindings`; `CacheDmaBases` (`:239`, 3 cache pages after each scalar/user-data base); `FindBuffers`.
5. Early-compute decision ([GPU.md section 3](GPU.md#early-compute-side-queue)): if qualified, `BeginSide()` switches recording to the side command buffer.
6. `PrepareBda` (DMA); `PrepareRayQuery` (`:256`, `RayTracing::Prepare` per dispatch with a recognised TLAS traversal, [GPU.md section 7](GPU.md#7-ray-tracing-mirror)); `ImageHeap::Rebind`; `RebindImages`; `BindSharedMemory` (LDS in a utility buffer when `lds_storage`: size = LDS dwords x total groups, `:268-296`); `RebindBuffers`; `CommitBindings`.
7. `ShaderWriteHazardBarrier` (all-commands -> compute) only if the dispatch writes (`:469-473`); `dispatch`; then always `ShaderAccessBarrier` (shader R/W -> all-commands memory R/W, `:566`, `shaderResourceBarrier.cpp:95-101`): every dispatch ends with a full memory barrier from compute to all stages. Comment (`:470`, `:565`): it replaced a host fence that "used to serialize every dispatch".
8. Ordered-append GDS dispatches with at most one wave per group are split into one `dispatchBase` per group with a barrier between (`:542-559`).
9. With early compute: `SubmitSide()` ends the side buffer and submits it to the side queue; the next main-queue submit waits on it.

Indirect dispatch: `HasGpuDirtyBytes` check; the host reads the arguments only if CPU-owned (zero check `:669-673`); counts pass through a one-thread clamp job (`ClampIndirectCounts`, `:603-657`, shader `gpu_dispatch_clamp.comp`) writing a 4096-slot x 32-byte ring, with barriers before and after, then `dispatchIndirect`: one extra dispatch and two barriers per indirect dispatch. Indirect dispatches never use early compute.

Compute-side helper jobs in the same command buffer: dispatch clamp, BVH walk/mirror, fault-buffer processing (`FaultManager`), tilers, colour clear/transform, D16 promote/demote, MS-depth tests, PSSR linear upscale ([GPU.md section 11](GPU.md#11-helper-shaders)).

## 6. Flush points and batch submission

Flush and finish callers (`graphicsRun.cpp`):

| Caller | Where | Effect |
|---|---|---|
| `BufferFlush` | `:233-237` | flush scheduler, publish occlusion queries |
| `BufferWait` -> `Finish` | `:239-242` | submit and wait |
| `CountDraw` after N draws | `:833-839` | only if `m_flush_draws != 0` (default off) |
| `SubmitReadBackWrites` | `:826-829` | when the buffer cache holds eager read-back writes: batch submitted before the next draw/dispatch |
| RELEASE_MEM selector 5 (GDS read) | `:1087` | `Finish` |
| `WriteBackImages` | `:1107-1112` | only if `m_readback_storage_images`, then `Flush` |
| Video-out flip preparation | `:1193-1214` | `Flush` after `PrepareVideoOutFlip` |
| Suspend point | `:576-584` | `BufferFlush` + `Reset` |
| `SubmitIfIdle` (before each non-async dispatch) | `commandScheduler.cpp:170-179` | submit when the open batch holds >= 8 dispatches and the GPU finished every previous batch |
| `Wait(CurrentTick())` | `commandScheduler.cpp:300-309` | submits the open batch, waits, `BeginNext()` (used by buffer cache, texture cache, stream wraps: [GPU.md waits table](GPU.md#where-the-gpu-thread-or-the-thread-calling-it-waits-for-the-host-gpu)) |

Batch submit path (`commandScheduler.cpp`):
1. `Flush(submit)`: `BufferCache::RecordEagerDownloads()` (one batch of downloads for pages that were once CPU-read while GPU-dirty), `Submit`, `BeginNext` (`:164-168`).
2. `Submit` (`:535-568`): add the pending side wait (`m_side >= N`); `FlushDeferredTracking()` (page protections and upload bytes the batch depends on); `vkEndCommandBuffer` (drains the recorder ring); take the next tick; add the master timeline signal.
3. If async submit is on and the caller is the GPU thread, queue the batch to the submit thread (`SubmitThread`, `:230-262`); else `DrainSubmits()` and `vkQueueSubmit` on the calling thread. Each submit holds `graphics.queue_mutex`. A thread waiting on a queued batch submits it itself (`SubmitQueuedUpTo`, `:265-287`).
4. When the GPU finishes tick N: the priority thread waits the tick and runs priority callbacks (read-back publication into guest memory, flip completion, interrupts); the GPU thread runs normal callbacks (resource release) in `PopPendingOperations`.

## 7. Read-backs (host GPU to guest memory)

Read-backs are the points where the GPU thread, or a guest thread, waits for GPU results. Mechanisms and numbers live in [MEMORY.md](MEMORY.md); the sequence is:
- Buffers written by the GPU are access-watched; a guest read or write of such a page runs `SendCommandSync` on the GPU thread (the guest thread blocks until the GPU thread reaches a packet boundary), then a 512 KiB-window download with a GPU wait, then publication into guest memory.
- Eager downloads: pages once CPU-read while GPU-dirty are downloaded in one batch at every `Flush` and released when their tick finishes (`CompleteEagerDownloads`).
- Images are not written back by default (`readback_*` flags off); exceptions: GC under memory pressure, block-compressed alias retirement, texel-buffer reads of GPU-written images.
- The shader-recompiler's SRT walker reads GPU-written pages through the fault path or the cache buffer ([GPU.md section 12.3](GPU.md#123-per-draw-work-for-an-already-compiled-shader)).
- CP packets with known values (`WriteInGpuOrder`, `m_cp_known`) avoid some read-backs.

## 8. Flip and present

Facts:
1. Packet construction (`src/libs/agc.cpp:3986-4036`): `set_flip` obtains the port's display-buffer labels, allocates `Pm4::FLIP_PACKET_DW` dwords, encodes flip mode and buffer index in a user-data tag, writes the flip argument into user data with WRITE_DATA, and emits RELEASE_MEM with GL2 writeback that writes `1` to a shown buffer's label and interrupts with the port as context id. DCB and ACB variants differ only in user-data R value and release event (`:3985`, `:4038-4052`). `AgcDcbWaitUntilSafeForRendering` emits WAIT_REG_MEM_64 against the buffer label for shown buffers, else a NOP (`:4054-4097`).
2. The CP recognises the tags in `CpOpSetUconfigReg` and calls `CpFlip` or `CpWaitFlipDone` (`pm4Handlers.cpp:1300-1313`). `CpFlip` writes back images and calls `CommandProcessor::Flip` (`:1273-1287`); `CpWaitFlipDone` calls `WaitFlipDone` on the label (`:1289-1298`, `graphicsRun.cpp:1022`).
3. `CommandProcessor::Flip` publishes occlusion queries, calls `Sync::PrepareVideoOutFlip`, defers `CompleteFlip`, flushes the scheduler (`graphicsRun.cpp:1193-1206`). `PrepareVideoOutFlip` retries `SubmitFlipFromGpu` while the flip queue is full, calling `WaitForSubmitSlot` between attempts (`src/graphics/host_gpu/renderer/sync.cpp:52-70`; blocks the GPU thread).
4. `VideoOutDriver::SubmitFlipFromGpu` reserves a request with source `GpuEop` and calls `FlipQueue::Prepare` (`videoOut.cpp:1535-1547`). The CPU path `VideoOutSubmitFlip` reserves with source `Cpu` and enqueues a `FlipPreparation` submission (`:1517-1533`; `graphicsRun.cpp:143-149`, `:1208-1214`). `FlipQueue::Reserve` replaces superseded slave requests, rejects when `flipPendingNum >= 16`, appends a request, records `FlipsRequested`, `FrameClock::NoteFlip` and the tracker frame (`videoOut.cpp:854-919`, capacity `:66`). `FlipQueue::Prepare` moves a CPU request into the GPU queue when needed, checks the port generation, prepares a blank frame for special indices, else calls `Presenter::PrepareFrame` (`:967-1044`). `FlipQueue::Complete` marks the request `Ready` or discards a cancelled one (`:1046-1064`).
5. `Presenter::PrepareFrame`: acquire a frame from the pool, resolve the registered surface to an image, `Frame::CopyFrom` = end rendering, transition both images, `copyImage` with the smaller extent (`swapchain.cpp:937-965`, `:208-222`). Blank frames are cleared (`:967-987`).
6. Present thread (`std::jthread` `PresentThread`, `videoOut.cpp:266`, loop `:809-852`): period `1 / Config::GetVblankFrequency()`; sleeps to catch up, `VblankBegin` (pre-vblank events), tries `m_flip_queue.Flip()`, may re-present the last frame or a blank frame for the system overlay, `VblankEnd` (increments the per-port vblank count and timestamp, fires vblank events, signals `vblank_cond`, `:775-807`); counts `LateVblanks` when behind.
7. `FlipQueue::Flip` (`:1092-1199`): waits for all members of a group to be `Ready`, collects and sorts by bus, checks generation and `IsFlipDueLocked`, calls `Presenter::Present`, records a frame for the overlay on bus 0, updates flip status, fires flip events. A flip arriving before all group members are ready is deferred without presenting (`:1103-1110`).
8. Flip rate: a flip is due after `flip_rate + 1` vblanks, or immediately when the interval is at most 1 or `Config::IgnoreFlipRate()` is set (`videoOut.cpp:490-497`); `VideoOutSetFlipRate` accepts rates 0..2 (`:1260-1282`). `GetVblankFrequency` defaults to 60, clamped to 30..360 (`emulatorConfig.cpp:148-150`); a frequency of at least 119 Hz is reported to the title as a 120 Hz mode ([OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md), switches section).
9. `Presenter::Impl::Present` (`swapchain.cpp:1042-1097`): acquire swapchain image, system overlay, `RecordPresentCommands` (linear blit or shader draw, aspect fit unless stretch, letterbox clear, overlay layers, `ePresentSrcKHR`, `:756-890`), submit, `Swapchain::Present` (`presentKHR` under the graphics queue mutex, `:901-931`). Present mode from config (`Mailbox` default), Fifo fallback (`:436-450`). Frame ids are not a pacing primitive; pacing is the present thread period plus the present mode.
10. Completion the title sees: the label written by RELEASE_MEM (`agc.cpp:4023-4032`, `graphicsRun.cpp:1062-1075`) and a flip event with the flip argument (`videoOut.cpp:1182`); safe-to-render packets poll the same label (`agc.cpp:4080-4089`, `graphicsRun.cpp:265-275`); vblank waits use `vblank_cond` (`videoOut.cpp:1693-1708`).
11. PSSR upscale requests (`CpOpPssr` -> `ProcessPssr` -> `Upscale`) run on the command stream before the flip ([GPU.md section 10](GPU.md#10-upscalers)).

Frame statistics (`Counter::` values): `GpuThreadBusyNs` (`graphicsRun.cpp:510-511`, `:526-529`), `IndirectDraws` (`:925`), `FlipsRequested`/`FlipsRejected` (`videoOut.cpp:879-910`), `LateVblanks` (`:847-850`), `PresentNs`, `PresentAcquireNs`, `OverlayNs`, `DroppedPresents` (`swapchain.cpp:1042-1096`); all CPU-side scoped timers, no GPU timestamp query is read in these files. `PerformanceOverlayRecordFlip` is called from `FlipQueue::Flip` for bus 0 (`videoOut.cpp:1154-1155`, `performanceOverlay.cpp:748-756`); a sampler thread starts once, waits for `DebugCounters::Enabled()`, samples host statistics off the present path (`performanceOverlay.cpp:179-211`; comment `:176-178`: host statistics can block for milliseconds). FPS and frame time derive from recorded flips; the panel's GPU-wait value includes present pacing waits (comment `:378`).

Hypotheses (not measured): two full-surface operations per presented layer (copy into the frame, then blit/draw into the swapchain image) paid per frame, possibly significant at high output resolution; a late period increments `LateVblanks`; a full flip queue stalls the GPU thread in `WaitForSubmitSlot`; the overlay adds a pass when active. Measured effects of vblank frequency, flip-rate and output resolution: [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md) (2026-11-12, 2026-11-15, 2026-11-19).

## 9. Work paid per draw / dispatch / batch / frame

Facts from the code above (what is executed, not what it costs).

| Unit | Work paid |
|---|---|
| Per packet | opcode decode; deferred buffer-cache flush for non-state packets; draw-window end check; register-bank write for state packets |
| Per draw | `PopPendingOperations`; render-mutex lock; topology; optional index-buffer scan (custom restart) or 8-bit index expansion; per stage `GetProgram` (mapped-data lookup, vertex attribute table read for dynamic fetch, static-key build, map lookup, SRT walk + descriptor decode, module scan, `bind`); colour/depth `ResolveCache` check (miss: `FindImage`); `PrepareBindings` (image resolve unless reused, sampler lookup, user-data copy); `FindBuffers`/`ObtainBuffer` for every buffer incl. vertex, index, indirect; `MarkWrittenTable` hash for written tables; stream-buffer upload of `flattened_srt` and `shader_data`; descriptor write (push or pool set + `vkUpdateDescriptorSets`); `Image::Transit` per bound image; pipeline key build + lookup; vertex-buffer bind, pipeline bind, push constants (every draw); dynamic-state commit (dirty bits only); `ResetBindings`; recorder copy of each `vkCmd*` |
| Per draw, conditional | hazard barrier + `EndRendering` for a writing draw; `BeginRendering` when `RenderState` differs; image-heap `Acquire`/`Rebind`/`Commit` when the heap is used; `PrepareBda`/`SynchronizeBda` for DMA shaders; quad-list-legacy one draw per quad; `ConsumeMetadataColorOperation`/`DepthStencilCopy`/resolve replacing the draw |
| Per dispatch | per-draw items for one stage; `SubmitIfIdle`; fast-path checks (meta clear, fill); `CacheDmaBases`; `PrepareRayQuery` (`RayTracing::Prepare`: 2-3 dispatches, BLAS/TLAS builds, 5 barriers, readback copy, recorder drain); LDS buffer; hazard barrier when writing; full compute-to-all-stages memory barrier after every dispatch; indirect: clamp job (1 dispatch, 2 barriers) |
| Per upload batch | `UploadCopies`: `EndRendering`, two global memory barriers, staging memcpy and `vkCmdCopyBuffer` ([MEMORY.md](MEMORY.md)) |
| Per batch (flush) | `RecordEagerDownloads`; `FlushDeferredTracking`; `vkEndCommandBuffer` + recorder drain; tick + timeline signal; submit-thread hand-off and `vkQueueSubmit` (~28 us per the code comment); side-queue wait; command-pool buffer acquisition; dynamic state marked dirty again at the next batch start |
| Per new program / pipeline | translation, SPIR-V, module creation; layouts + `vkCreateGraphicsPipelines`; rect-list modules; `std::printf` per module |
| Per frame (suspend point) | render garbage collection (`RunGarbageCollector` for buffers, textures, ray tracing every 64 points); `FaultManager::ProcessFaultBuffer` scan when a BDA draw ran; (uncommitted at the time of writing) wait for the host GPU before the guest may continue unless `KYTY_SUSPEND_POINT_WAIT=0` |
| Per frame (flip) | `PrepareVideoOutFlip`; flush; frame copy; swapchain acquire; present blit/draw; overlay; present; flip/label completion callbacks |
| Per fault (guest threads, any time) | exception round trip, protection change(s), tracker locks; read of GPU-dirty page: sync command to the GPU thread + download + wait ([MEMORY.md](MEMORY.md)) |

## Not covered yet

- What the caller does when `DrawIndex` returns false after `ResolveDrawEmit` (the guest-side handling).
- Cost of `uc_check`/`hw_check`/`hw_print` with debug dumps off (`debug.cpp`).
- Whether presentation or the swapchain interacts with the side-queue semaphore; which queue `presentKHR` uses beyond the graphics queue mutex.
- Wake-up of a suspended WAIT_REG_MEM from a guest CPU write (only the 100 µs / 1 ms poll was found).
- Semantics of `VideoOutWaitVblank` relative to a skipped flip under multiple ports.
- Remainder of `RenderExecutor::ProcessPssr` after `renderPssr.cpp:108`; which library serves a given request.
- Wiring from `perf_overlay_enabled` (`emulatorConfig.h:113`) to `DebugCounters::Enabled()`.
- The deferred-command scheduler worker threads other than those listed in GPU.md.
- Whether `m_flush_draws`, `ShaderCacheEnabled`, `OcclusionQueriesEnabled`, `TessellationEnabled` are enabled in normal runs.
- `tile.cpp` and `gpu_format.cpp` were inventoried by function name only (layout/size maths, no claim about cost).
- `ScanUnwatchedPages` at submit (`graphicsRun.cpp:428`) and `RequestUnwatchedRescan`/`QueueUnwatchedPages` (`:562-566`): in MEMORY.md; cost not measured here.
- GPU duration of any step: no timestamp-query path in the files read.
- Per-draw `ObtainBuffer` call counts and typical bound range sizes.
- Any area of the loader/kernel side that blocks guest threads at the suspend point (not part of this document).
