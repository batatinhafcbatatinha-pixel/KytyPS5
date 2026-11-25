# Working on this repository

This is a performance fork of KytyPS5, a PlayStation 5 translation layer (C++20, Vulkan, SDL3, Qt launcher). The
title's x86-64 code runs natively; the emulator supplies the kernel and system libraries, the graphics command
processor, a shader recompiler and the mirroring of guest memory into Vulkan buffers and images. The fork exists to
raise frame rate without losing compatibility, accuracy or stability.

## Read first

| Question | Document |
|---|---|
| What is where, how a title runs end to end, how to build | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Loader, library call bridge, kernel layer, audio, files | [docs/CPU.md](docs/CPU.md) |
| Threads, locks, who waits for whom | [docs/THREADING.md](docs/THREADING.md) |
| Guest memory to Vulkan and back, write detection, caches | [docs/MEMORY.md](docs/MEMORY.md) |
| Host renderer, pipelines, descriptors, shader recompiler | [docs/GPU.md](docs/GPU.md) |
| A frame step by step, what is paid per draw | [docs/GRAPHICS_PIPELINE.md](docs/GRAPHICS_PIPELINE.md) |
| Ranked bottlenecks and the plan | [docs/BOTTLENECKS.md](docs/BOTTLENECKS.md) |
| What can be measured, what is missing, the benchmark plan, last numbers | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| How measurements were made, every experiment so far, dead ends, open defects, switches | [docs/OPTIMIZATION_LOG.md](docs/OPTIMIZATION_LOG.md) |
| Techniques from other emulators, ideas not tried | [docs/RESEARCH.md](docs/RESEARCH.md) |

Before proposing an optimization, search OPTIMIZATION_LOG.md for it: most obvious ideas were tried, and several
were slower or wrong. A dead end is reopened only with new evidence.

## Build and test

```
cmake -S . -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON       -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH=<Qt 6 msvc2022_64>
cmake --build <build> --target launcher <test targets>
cmake --install <build> --prefix <install>
ctest --test-dir <build> --output-on-failure
```

This is the Windows command of `.github/workflows/build.yml` (the Linux and macOS jobs use `clang`/`clang++`).
`launcher` is a real target (`src/launcher/CMakeLists.txt:17`, only when `KYTY_BUILD_LAUNCHER` is ON, the default); it
depends on `kyty_emulator` and `stage_updater` (`:73`), so it builds the emulator too. `cmake --install` installs
`kyty_emulator` and, with the launcher, the launcher and updater.

- Submodules under `3rdparty/` must be checked out (CI: `submodules: recursive`); `glslangValidator` must be on
  `PATH` (required at configure time, `CMakeLists.txt:245`). A full build takes tens of minutes; build outside the
  conversation and never run a game while a build is running.
- Every test executable is `EXCLUDE_FROM_ALL`: it exists only if named in `--target` (CI names a subset) or
  built through the aggregate target `kyty_tests` (`CMakeLists.txt:873`, which does not list every test
  executable). `ctest` runs the tests registered under `BUILD_TESTING` (`CMakeLists.txt:736` on); a registered test
  whose executable was not built fails to start. CI runs only a subset by name (`ctest -R "^(...)$"`), not the whole
  list. Some GPU test modes are not registered and some fail on the base; OPTIMIZATION_LOG.md lists which. A test that
  failed before your change is reported, not hidden; a test that fails because of your change is fixed or the change
  is dropped.
- Formatting: `src/.clang-format` and `.pre-commit-config.yaml` (clang-format v22.1.3 on `src/**/*.{cpp,h,inc}`).
  `.clang-tidy` is in `src/` and runs only with `-DKYTY_ENABLE_CLANG_TIDY=ON` (default OFF).

## Rules

Performance work:
- Measure before and after. A change counts as an optimization only when a recorded measurement shows the gain;
  the protocol is in PERFORMANCE.md ("Benchmark plan"), the method used so far in OPTIMIZATION_LOG.md. Never
  write a number that was not measured.
- One change per experiment. A change under test has a switch (environment variable read once) so base and
  candidate can be compared in one build; the switch goes away, or is documented, when the change is kept.
- Check the picture as well as the frame rate: same scene, same instant, against the base.
- Do not buy frame rate with wrong data. Stale reads, skipped uploads and skipped waits are ceiling experiments
  only: they are marked as such and never committed as the default path.
- Do not remove emulation checks (`EXIT_IF`, bounds and loop guards, robust access) to gain speed.
- Temporary probes and test switches are removed before a commit. Keep them as a local diff if they may be needed.
- Record every experiment in OPTIMIZATION_LOG.md, including the neutral ones and the failures.
- When a document here turns out wrong or stale, fix it in the same change.

Repository:
- This is a fork. Fetch upstream and merge new commits before starting work, keep the fork's changes, note what
  came in, and re-measure after a merge: upstream changes have cost frame rate before.
- Text written by upstream is left as upstream wrote it. Nothing from this fork is sent upstream by an AI agent.
- Publish source and configuration only: no game files, dumps, captures, generated shaders or third-party binaries.
- Never name a game anywhere in the repository: not in code, comments, documents, tests, commit messages, branch
  or file names. No titles, abbreviations, title ids, disc serials, or scene names. Write "one title", "title A",
  "the test scene"; test fixtures use made-up names and ids.
- Do not paste console SDK code, system-software code, or disassembly of them or of any game. Reimplement from
  behaviour, public documentation and tests, and say where the knowledge came from.
- A commit is pushed only when it is finished and stable: it builds, what it changes was run and behaves, nothing
  half-done is on by default. Work in progress and commits with a known regression stay local.
- Do not change the user's settings, saved configuration or game data.

## State of the tree

`docs/` describes commit c944a398 (2026-11-23). When you change behaviour that a document describes, update the
document and its "State described" line.
