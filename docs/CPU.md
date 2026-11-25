State described: c944a398 (2026-11-23), read 2026-11-25.

# Host CPU side (everything except the GPU thread's rendering work)

Source-only description; nothing here was measured unless it cites [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md). Parts of files that were not read are listed in [Not covered yet](#not-covered-yet). The GPU thread, recorder, submit and present threads are in [GPU.md](GPU.md) and [THREADING.md](THREADING.md).

## No CPU emulation

Guest x86-64 code executes natively on the host CPU in the emulator's own process. The host must support `x86-64-v3` ([ARCHITECTURE.md](ARCHITECTURE.md#targets-and-build)). Three things stand between guest code and the host: (1) imports are redirected to host functions, (2) some instructions are rewritten at load time, (3) a few instructions are emulated when they fault. A guest "thread" is a host pthread running on a guest stack.

## Loader and start-up

Facts:
- Order of `Init()`: config, log, content parameters, optional splash, pthread, profiler, network, memory, filesystem, controller, audio, graphics (`src/emulator.cpp:91-115`).
- Mounts: the application directory at `/app0` (mounted with `read_only = true`, and `KernelOpen` also refuses writes under `/app0`) and again at `/hostapp` (writable), per-title temporary and download directories (`src/emulator.cpp:176-182`, `:80-89`; `src/kernel/fileSystem.cpp:455-461`).
- HLE modules register before the ELF loads (`src/emulator.cpp:183-186`, `src/libs/libs.cpp:115-149`).
- `LoadElf()` resolves the guest path through the mount layer and calls `RuntimeLinker::LoadProgram()` (`src/emulator.cpp:117-124`). `LoadProgram()` opens the ELF, maps program data into guest memory, parses dynamic info and builds symbol data under the linker mutex (`src/loader/runtimeLinker.cpp:1227-1266`). `Elf64::Open()` reads wrapped and ELF headers, program/section headers and dynamic tables (`src/loader/elf.cpp:532-564`); `LoadSegment()` copies file bytes to guest virtual addresses (`src/loader/elf.cpp:242-264`).
- `Execute()` starts a detached host thread for `RuntimeLinker::Execute(game_patch)`; the original thread runs `WindowRun()` and then `std::quick_exit(0)` (`src/emulator.cpp:134-140`). `at_quick_exit` is used because guest threads may still run (`src/emulator.cpp:172-174`).
- On the execute thread, `RuntimeLinker::Execute()` makes the host thread the main guest pthread, creates its guest stack, loads an optional runtime library, relocates all programs, applies an optional patch plan, creates TLS, sets the guest FP environment and calls the entry point with a guest-stack argument block (`src/loader/runtimeLinker.cpp:1299-1351`; stack helper `src/kernel/pthread.cpp:1009-1041`).
- Content parameters come from JSON (`src/loader/systemContent.cpp:114`, `:183-192`, `:609-611`); memory setup consumes them (`src/kernel/memory.cpp:1025-1040`).
- Game patches: `--game-patch` entries can be written once or held; the hold thread compares and rewrites selected guest locations until `StopHolding()` (`src/loader/gamePatch.cpp:57-79`, `:358`). A held entry is re-applied whenever the game overwrites it (see [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md) for how it was used).

## Import resolution and the call bridge

Facts:
- Registration is macro based: `LIB_DEFINE` declares a `void(Loader::SymbolDatabase*)` initialiser; `LIB_NAME`, `LIB_ADD`, `LIB_OBJECT`, `LIB_FUNC`, `LIB_FUNC_IN` register a host function pointer under a symbol key (`src/libs/libs.h:19-33`). `Libs::InitAll()` calls every module initialiser (`src/libs/libs.cpp:115-149`).
- `SymbolDatabase::Add()` stores a generated `name[library][module][type]` key and the host address; `Find()` looks up the same key (`src/loader/symbolDatabase.cpp:16-37`, `:54-64`).
- An import name has three `#`-separated parts; `RuntimeLinker::Resolve()` splits it into NID, library id and module id, then searches the global database and each program's exports under the linker mutex (`src/loader/runtimeLinker.cpp:1380-1419`). Relocation kinds `R_X86_64_GLOB_DAT`, `JUMP_SLOT` and `R_X86_64_64` write the resolved address (`src/loader/runtimeLinker.cpp:811-856`).
- A resolved import therefore calls the host function directly through the SysV ABI (`KYTY_SYSV_ABI`, `src/libs/libKernel.cpp:1029`, `:2131`; registration stores the function address itself, `src/libs/libs.h:26`). No syscall dispatcher was observed in the read path.
- Unresolved function imports get a generated thunk (`src/loader/runtimeLinker.cpp:877-895`, `:146-165`). Calling it increments `UnresolvedImportCalls` and logs at most the first 1024 calls (`:168-184`). Unresolved objects point at an invalid-memory page (`:891-894`).
- `PRINT_NAME()` logs only when the per-library `thread_local` flag is on (`src/libs/libs.h:13-38`).
- libkernel registers POSIX and libkernel variants of the same functions via `LIB_FUNC_POSIX_KERNEL` (`src/libs/libKernel.cpp:63-69`, `:2186-2239`).
- Guest exception handlers: `KernelInstallExceptionHandler()` validates the signal, rejects duplicates and stores the guest pointer (`src/libs/libKernel.cpp:1029-1044`). On Windows a special-APC path can run a non-guest-context handler on a detached host thread, carrying TCB and guest-self state (`:1001-1024`).
- `KernelSyncOnAddress*` and `UmtxOp` are thin bridges to the address-wait implementation; the 32/64-bit experimental variants log a one-time warning (`src/libs/libKernel.cpp:2131-2184`, `:2136-2148`).
- HLE module list (registered in `src/libs/`): kernel, video-out, AGC/AGC driver, pad/controller, audio (AudioOut, AudioOut2, AudioIn, AJM/ACM, AvPlayer, NGS2, Audio3d), save data, user/system service, network/HTTP, trophy, codecs (JPEG/PNG, video decode), fonts, and many stubs. Most audio propagation APIs return stub handles or success (`src/libs/libAudio.cpp:41-167`).

## Guest instruction patching and fault-driven emulation

Facts:
- Load-time patching is enabled by AMD mode, Windows red-zone protection or a nonzero guest TSC multiplier; it reserves an 8 MiB trampoline region per program (`src/loader/runtimeLinker.cpp:1637-1656`).
- `PatchGuestCode()` rewrites guest `fs:[0]` TCB reads to `gs:[TcbGsOffset()]` and NOPs or short-circuits a matched `fs:[0x28]` store pattern (`src/loader/runtimeLinker.cpp:1029-1060`).
- Function boundaries come from `PT_GNU_EH_FRAME`; `PatchGuestInstructions()` runs per executable segment with protection, AMD, host-feature and TSC flags; instruction caches are flushed after protection is applied (`src/loader/runtimeLinker.cpp:1682-1717`, `:1741-1769`). The patcher returns early without function starts and takes a per-module mutex (`src/loader/guestInstructionPatcher.cpp:1536-1569`).
- Host features queried: SSE4a, RDPID, CLWB (`src/loader/guestInstructionPatcher.cpp:1520-1534`).
- Fault-driven emulation: `X64InstructionEmulator::TryEmulate()` handles reciprocal square root, MONITORX/MWAITX, SSE4a, SHA-NI (not on Apple) and CPU-extension cases (`src/loader/x64InstructionEmulator.cpp:819-840`). A completed load writes the value into RAX and advances RIP (`:842-846`). The `EmulatedInstructions` counter exists for this (`src/loader/runtimeLinker.cpp:630`).

## Host exception path

Facts:
- Windows installs `AddVectoredExceptionHandler(0, ...)`; POSIX installs `SA_SIGINFO | SA_RESTART | SA_ONSTACK` handlers for `SIGSEGV`, `SIGBUS`, `SIGILL` on a per-thread alternate stack, to keep fault handling off guest stacks that GPU tracking can make read-only (`src/common/hostException.cpp:69-73`, `:235-257`; installed at load, `src/loader/runtimeLinker.cpp:1678-1680`).
- The Windows filter runs on the faulting guest thread, builds `ExceptionInfo` (type, address, registers) and returns `EXCEPTION_CONTINUE_EXECUTION` when handled, which retries the instruction (`src/common/hostException.cpp:97-147`).
- `KytyExceptionHandler()` order (`src/loader/runtimeLinker.cpp:625-684`):
  1. illegal instruction: try `X64InstructionEmulator::TryEmulate`;
  2. access violation: SRT-walker check (load satisfied from the buffer cache, deferred to a shader path, or `CompleteLoad` writes RAX and skips the load, `:640-681`);
  3. otherwise `Memory::HandleGpuFault` (`src/kernel/memory.cpp:942`) → `RenderContext::HandleFault`, counting `GpuFaults` (`:677`, `:684`);
  4. return false → fatal handling.
- What `HandleFault` does on a guest write to a watched page, step by step, is in [MEMORY.md](MEMORY.md#from-a-guest-write-fault-to-the-resumed-instruction).

## Kernel layer

### Threads

- Main guest thread: the execute thread, default affinity mask, name `MainThread` (`src/kernel/pthread.cpp:970-1006`).
- Other guest threads: host `pthread_create()`; the wrapper allocates a guest stack, copies attributes, inherits the creator's affinity when unset, then `RunThread()` (`src/kernel/pthread.cpp:3194-3249`). `RunThread()` writes the guest FP environment, installs signal stacks on POSIX, creates TLS, sets `g_pthread_self`, records host id/name and switches to the guest stack before the guest entry (`:3137-3186`). Cleanup deletes TLS and marks the thread almost done (`:3125-3135`).
- A detached reaper thread sweeps freed detached guest threads every 10 s (`:952-960`, started `:1070`).
- Priority: only Windows changes the host `sched_priority`; elsewhere the guest priority is stored without a host call (`:3449-3486`). Yield is one `SwitchToThread`/`Sleep(0)` on Windows, `std::this_thread::yield()` elsewhere (`:498-506`, `:3585-3587`).
- Thread names recorded for the profiler: `Thread_Main` (`src/loader/runtimeLinker.cpp:1300`), `Thread_Window` (`src/graphics/presentation/window/window.cpp:868`), `Thread_Gpu`, `Thread_Recorder`. The full thread table is in [THREADING.md](THREADING.md#host-threads).

### Sync primitives and timers

| Primitive | Implementation | File:line |
|---|---|---|
| Sleep APIs | `KernelUsleep`/`KernelSleep`/`KernelNanosleep` call `SleepNanoInSlices`: a loop of host sleeps of at most `SIGNAL_APC_POLL_MICROS` = 10000 µs each, until the requested time has elapsed by the steady clock (the code comment: a guest signal queued as a special user APC runs only when a host sleep returns, which is the reason for the slices); a sleep is never cut short | `src/kernel/pthread.cpp:92`, `:508-515`, `:3808-3835` |
| Mutex | each waiter blocks on its own condition variable (`wait_until` the earlier of the deadline and now + 10 ms, in a loop until it owns the mutex or the deadline passes); unlock transfers ownership FIFO to the first waiter, then `notify_one()`, so a contended unlock wakes the waiter at once and the 10 ms is only the re-check bound | `src/kernel/pthread.cpp:1231-1288`, `:1341-1347` |
| Semaphore | vector of per-waiter condition variables, FIFO or priority order; `Signal` gives counts to waiters whose need fits and signals exactly those; a timed wait blocks for the whole remaining time in one `WaitFor`, only an untimed wait re-checks every 10 ms | `src/kernel/semaphore.cpp:21`, `:46-70`, `:146-175`, `:207-233` |
| Event flag | priority-ordered waiter queue under one mutex; `Set()` releases satisfied waiters in queue order, applying each clear before the next test | `src/kernel/eventFlag.cpp:37-65`, `:126-186` |
| Event queue | event list + mutex + condition variable; waits combine the caller timeout with the nearest timer deadline; trigger calls the filter hook or marks the event, then signals all waiters | `src/kernel/eventQueue.cpp:28-60`, `:135-197`, `:228-252` |
| Address waits (futex-style) | Linux: `FUTEX_WAIT_PRIVATE` with a timeout of at most 10 ms, retried until the deadline; `FUTEX_WAKE_PRIVATE` wakes it immediately. Elsewhere: a registry of condition-variable waiters, each `WaitFor` of at most 10 ms, `WakePortable` signals up to `count` of them; wake honours the requested count | `src/kernel/syncOnAddress.cpp:26-280` |

Timers exist as event-queue deadlines (above); the timer source file `src/loader/timer.cpp` was not read.

### Memory calls

- Constants: 12.5 GiB budget, 64 MiB reserved cache, 64 MiB included table memory, 4 KiB page tables mapping 2 MiB regions, default flexible size 448 MiB (`src/kernel/memory.cpp:57-71`). Guest page granularity is 16 KiB for runtime allocation (`:2903-2924`).
- Detail of the address space, locks and mapping notifications to the renderer: [MEMORY.md](MEMORY.md#guest-address-space-and-kernel-memory-calls).

### Files and async I/O

- Mount points and descriptor tables share a `Common::Mutex`; each open file has its own mutex and atomic flags (`src/kernel/fileSystem.cpp:44-103`).
- `KernelOpen()` decodes guest flags, rejects writes under `/app0`, ignores `O_DIRECT`, maps to a host file mode (`:464-509`). `KernelRead()`/`KernelWrite()` lock the file, call host `Common::File`, update byte counters, and flush only when `sync_writes` is set (`:674-740`, `:742-806`); `KernelPread()` is the positional path (`:808-834`).
- Archive reads are asynchronous: `ArchiveIoThread` runs a detached loop waiting on requests and completing them through per-request condition variables (`src/common/archive.cpp:195-219`).
- APR path resolution caches host paths by guest path (failed resolutions are forgotten because mounts may appear later) and caches file ids (`src/libs/libAmpr.cpp:47-61`, `:134-174`). Submissions use a global mutex and condition variable; `CompleteSubmission()` waits for the worker (`:295-318`).
- APR and AMM commands run on two static `CommandEngine` instances, each backed by a `std::jthread`: scan priority queues, poll unsatisfied waits with a 100 µs timed wait, run one command at a time outside the queue lock (`:1223-1279`, `:1332-1336`). APR host reads use a `thread_local` buffer capped at 4 MiB per chunk and copy each chunk into the guest destination (`:568-571`, `:1356-1409`).
- `GuestFileReadBytes` / `GuestFileWriteBytes` counters are active only with the performance overlay (`src/common/debugCounters.h:44-49`, `src/common/emulatorConfig.cpp:24-33`).

## Audio

| Part | What the code does | File:line |
|---|---|---|
| Registration | AudioOut, AudioOut2, AudioIn, voice chat/AJM/ACM/AvPlayer, decode, Audio3d, NGS2 | `src/libs/libAudio.cpp:171-226`, `:557-632`, `:1179-1244` |
| AudioOut | ports hold SDL audio streams; `OpenSdlDevice()` opens a playback stream and resumes it; `QueueSdlAudio()` remaps volume/channels, optionally sleeps in 1000 µs steps while the SDL queue is above the target latency, counts `AudioUnderruns` when a primed queue falls below one buffer | `src/libs/audio.cpp:112-125`, `:256-285`, `:299-409` |
| AudioOut2 | per-context submission mutex; a blocking push holds only its context mutex because backend pacing can sleep; push loops on `SleepMicro(audioout2_grain_micros(...))` while the modelled queue is full | `src/libs/libAudio2.cpp:213-229`, `:626-664` |
| NGS2 | reentrant user lock (own mutex + condition variable; rendering and voice calls do not take it); `g_ngs_mutex`, `g_racks_mutex`; `Ngs2SystemRender()` takes the system mutex, collects voices under the rack mutex, renders each voice and the mastering output for one grain, zeroes unwritten buffers, publishes state; default grain 256 samples at 48 kHz, max 512; per voice work: decode, resampling, filters, reverb, mastering, int16/float mixing | `src/libs/ngs2.cpp:377-405`, `:1419-1424`, `:3280-3337`, `:87-97`, `:1445-1452`, `:2225-2325`, `:2740-3236` |
| AJM | instance = decoder + resampler + gapless state behind a global mutex held for a whole decoder call; batch start/wait/cancel are stubs returning success; codecs built from instance flags | `src/libs/ajm.cpp:191-249`, `:436-454`, `:741-793`; interface `src/libs/ajm/decoder.h:65-174` |
| Audio3d | port open creates a slot queue, a semaphore and a detached `playback_simulate` thread (semaphore wait, modelled sleep, slot free) | `src/libs/audio3d.cpp:112-167`, `:56-94` |
| Controller audio | one SDL sender thread, created lazily | `src/libs/dualSenseBluetooth.cpp:357-369` |
| SDL callbacks | the stream is created with a null callback in the call read; no explicit callback-thread creation was seen | `src/libs/audio.cpp:269-276` |

## Per-call overheads (facts that bound a guest call's cost)

- Resolved import: direct host call, no marshalling layer (`src/libs/libs.h:26`).
- Mutex, untimed semaphore and address waits sit in a real blocking primitive (condition variable or futex) that a signal or wake ends at once; they re-check their state at least every 10 ms, which bounds the delay of a missed or racing wake-up, not the normal wake latency. Sleeps run as 10 ms slices of host sleeps (`src/kernel/pthread.cpp:92`, `:508-515`, `src/kernel/semaphore.cpp:222`, `src/kernel/syncOnAddress.cpp:63-76`).
- Memory calls take a recursive global operation lock plus a shared state mutex ([MEMORY.md](MEMORY.md#guest-address-space-and-kernel-memory-calls)).
- File calls take the per-file mutex and a host file operation per call (`src/kernel/fileSystem.cpp:718-730`, `:783-796`).
- Guest page faults on watched memory cost an exception round trip on the faulting guest thread ([MEMORY.md](MEMORY.md#cost-model-per-event)).
- Graphics submit calls only enqueue and signal the GPU thread (`src/graphics/guest_gpu/graphicsRun.cpp:425-435`); the guest blocks at suspend points, vblank waits and label waits ([THREADING.md](THREADING.md#where-threads-wait)).

## Candidates for CPU cost (hypotheses, not measured)

| Candidate | Mechanism (fact) | Hypothesis |
|---|---|---|
| 10 ms re-check bound | guest mutex, untimed semaphore and address waits time out of their blocking primitive every 10 ms and re-test; sleeps are looped host sleeps of at most 10 ms | extra wakeups of long-parked threads (one per 10 ms per waiter); wake latency of a normal signal is not affected; a lost-wake race would cost up to 10 ms (no instance observed) |
| Global memory lock | one recursive operation lock across allocate/protect/free (`src/kernel/memory.cpp:177-181`, `:2903-3025`) | concurrent guest threads and tracker protection calls can serialise on memory work |
| NGS2 render | system mutex per grain, voices gathered under a global rack mutex, per-sample per-voice work | voice count and grain size are the CPU scaling factors of audio |
| Audio pacing | AudioOut and AudioOut2 blocking paths sleep on the guest thread | guest threads can sit in output pacing instead of working |
| Synchronous I/O | APR and file reads are synchronous host reads (4 MiB chunks) | storage latency is exposed to the calling guest thread |
| Unresolved-import thunk | counter collected only with the overlay | a missing-export call site could be expensive and invisible in normal runs |
| Fault-driven emulation | emulated instructions go through an exception round trip | unsupported-instruction traps could matter on some builds; frequency unknown |
| Guest priority | host priority changes only on Windows | scheduling assumptions may not carry to POSIX hosts |

Measured CPU-side results (page-protection cost on the GPU thread, guest fault time) are in [OPTIMIZATION_LOG.md](OPTIMIZATION_LOG.md) and [MEMORY.md](MEMORY.md).

## Not covered yet

- `src/libs/libKernel.cpp` registrations after line 2239 (which exports are real or stubs).
- `src/kernel/semaphore.cpp` (remaining 458 lines), `src/kernel/eventFlag.cpp` lines 221-320, `src/kernel/eventQueue.cpp` lines 268-669.
- Non-Windows guest signal dispatch internals (`src/libs/libKernel.cpp:1001-1027`); where guest signal handlers run relative to guest contexts outside the Windows APC path.
- Individual transforms of `src/loader/guestInstructionPatcher.cpp` beyond the entry function; `src/loader/timer.cpp`.
- Remaining ~2600 lines of `src/kernel/memory.cpp`.
- Body of `ArchiveIoThread::Loop()` after `src/common/archive.cpp:219`; NGS2 outside the cited ranges; the rest of `src/libs/libAudio.cpp` after line 240.
- Concrete AJM codec headers (`aac_decoder.h`, `atrac9_decoder.h`, `mp3_decoder.h`, `opus_decoder.h`, `resampler.h`): cost and which codecs a title uses.
- Number of SDL audio callback threads at run time and their cores.
- How many guest threads a title creates and the priorities/affinities it requests (no workload observed).
- Set and frequency of emulated instructions, unresolved thunk calls, GPU faults and audio underruns in a real run (counters exist, no run was made).
- OpenXR thread body (`src/graphics/presentation/openxrHeadset.cpp:685-694`).
