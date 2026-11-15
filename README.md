# KytyPS5 (performance fork)

This is a fork of [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5), a free and open-source
PlayStation 5 translation layer written in C++ for Windows and Linux, with experimental macOS
support. Everything about the project itself (status, compatibility list, building, running,
contributing) is in the [upstream README](https://github.com/KytyPS5/KytyPS5#readme) and applies
here unchanged. This page only says what this fork changes.

> [!IMPORTANT]
> KytyPS5 is not affiliated with Sony Interactive Entertainment or PlayStation. The project does
> not distribute games or copyrighted system software. Use only game files that you have obtained
> legally.

## What differs from upstream

The fork follows upstream `main` (merged regularly; last merge at upstream `3e226bff`) and adds
work on frame rate, measured on *Control Ultimate Edition* (PPSA01949). All of it is on by default
and each part has a switch.

| Change | What it does | Switch |
| --- | --- | --- |
| Early compute on a second queue | A compute program whose output later shaders read back is learned at run time and its dispatches run on a second Vulkan queue, ahead of the work already queued, when a per-dispatch check finds no pending write to its inputs and no queued read of its outputs. This removes most of the GPU thread's read-back waits. | `KYTY_EARLY_COMPUTE=0` turns it off |
| GPU-side descriptor reads for learned programs | Programs that keep the GPU thread waiting on GPU-written descriptors are learned and read those descriptors on the GPU. Trials of new candidates are off while early compute is active, because a trial compiles a shader variant and can stall the game. | `KYTY_GPU_SRT=0` turns it off, `KYTY_GPU_SRT_LEARN=1` turns the trials back on |
| Walker page faults avoided | The descriptor walker reads pages that faulted recently through a checked load from the unprotected backing memory, without raising an exception. | none |
| Texture descriptions reused | The size, tiling layout and view information of a texture binding are kept per descriptor instead of being recomputed for every binding. | none |
| Rewritten pages left unwatched | A buffer page that the game writes again after its upload is no longer write-protected. It is uploaded once per guest command submission instead, which saves a page fault and two protection changes per write. A GPU write ends this for the page, and the set expires after 4096 submissions. | none |
| Submit worker | The GPU thread's `vkQueueSubmit` calls run on a worker thread in submission order. A wait on a queued batch submits it on the waiting thread. | `KYTY_ASYNC_SUBMIT=0` submits on the GPU thread |
| Read-back fixes | GPU-written image data is kept across eviction and raw reads; read-back downloads start when their batch is submitted. | none |
| Device-loss report | Every failed Vulkan wait or submit prints one `[device-loss]` line with free RAM, commit charge and video memory usage and budget. | none |

Measured on 2026-11-14 in the first gameplay scene of *Control* (character standing, 60 s window),
with the game's render resolution patched to 1280x720, on an RTX 3050 laptop GPU and a Ryzen 5
3600: about 39 fps before this work, 46.6 fps with early compute, 49.8 fps after merging upstream's
changes of that day. The last batch (walker and texture descriptions) lowers the frame time by
1.7 ms (21.91 to 20.21 ms) in an in-run comparison of alternating 5 s windows. Separate runs differ
by about 1.5 fps on this machine, and other scenes, resolutions and games were not measured.
The next batch (unwatched rewritten pages and the submit worker) lowers the frame time by 0.34 ms
in the same kind of in-run comparison; its clean run showed 50.3 fps. Most of its effect is CPU
time on the game threads (about 140 ms of page-fault handling per second less), not frame rate.

Known open point: 3 of 20 runs with the second queue lost the GPU device, all within one hour in
which the machine was short of memory and disk; none since. If it happens, the `[device-loss]`
line in the log says whether memory was the cause, and `KYTY_EARLY_COMPUTE=0` returns to one queue.

The render-resolution patches used for these measurements are in
[recomp-patches](https://github.com/batatinhafcbatatinha-pixel/recomp-patches) (`ps5/PPSA01949/`)
and are loaded with upstream's `--game-patch` option.

Parked code: an earlier attempt to move every descriptor read to the GPU is still in the tree,
off by default and not maintained since the last upstream merge.

## Building and running

Exactly as upstream: see
[Building](https://github.com/KytyPS5/KytyPS5#building) and
[Running](https://github.com/KytyPS5/KytyPS5#running). Bugs that also happen in upstream's build
belong in upstream's issue tracker; bugs that only happen here belong in this repository's.

## AI use

The changes of this fork were written with AI assistance (Claude models, supervised and tested by
the fork's maintainer on the hardware named above); the commits say how each change was measured.
Upstream asks that pull requests disclose AI involvement and come from a human contributor:
nothing from this fork is sent upstream by an AI agent.

## License

KytyPS5 is licensed under the [GNU General Public License version 2](LICENSE)
(`GPL-2.0-only`), and so is this fork.

This project is based on the original [Kyty](https://github.com/InoriRus/Kyty), which was released
under the MIT License. Kyty's original copyright and license notice are preserved in
[`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt). Third-party components remain subject to the
licenses included with those components.

## Credits

- [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5) and its contributors — the emulator itself;
  this fork is a small set of changes on top of their work.
- [InoriRus/Kyty](https://github.com/InoriRus/Kyty) — KytyPS5 is based on a heavily modified version
  of the original Kyty project.
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) — reference for understanding PS4
  memory behavior, GPU resource aliasing, cache coherency,
  and the AVPlayer implementation.
