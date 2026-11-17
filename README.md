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

The fork follows upstream `main` (merged regularly; last merge at upstream `15c8c618`) and adds
work on frame rate, measured on title B and title A. All of it is on by default
and each part has a switch.

| Change | What it does | Switch |
| --- | --- | --- |
| Early compute on a second queue | A compute program whose output later shaders read back is learned at run time and its dispatches run on a second Vulkan queue, ahead of the work already queued, when a per-dispatch check finds no pending write to its inputs and no queued read of its outputs. This removes most of the GPU thread's read-back waits. | `KYTY_EARLY_COMPUTE=0` turns it off |
| GPU-side descriptor reads for learned programs | Disabled since the merge of upstream `3acafb35`: upstream reworked the image heap these variants looked their textures up in, and the fork's lookup table was not carried over (its measured ceiling in title B was below 1 ms per frame). The shader-side code is still in the tree. | none |
| Walker page faults avoided | The descriptor walker reads pages that faulted recently through a checked load from the unprotected backing memory, without raising an exception. | none |
| Texture descriptions reused | The size, tiling layout and view information of a texture binding are kept per descriptor instead of being recomputed for every binding. | none |
| Rewritten pages left unwatched | A buffer page that the game writes again after its upload is no longer write-protected. It is uploaded once per guest command submission instead, which saves a page fault and two protection changes per write. A GPU write ends this for the page, and the set is rebuilt every 64 frames, so a page the game stopped writing stops being uploaded. | `KYTY_REWRITTEN_FRAMES=n` sets the period |
| Unchanged rewritten pages not uploaded again | Shader address reads see CPU bytes only through uploads, so the unwatched pages above were uploaded for them on every submission. When that set is 4 MiB or more, each page is hashed first and uploaded only when its bytes changed since the last time. | `KYTY_BDA_HASH=0` uploads them always |
| Image backings reused | A deleted image's Vulkan image and memory are kept for up to 2 s (128 MiB at most) and handed to the next image created with the same parameters, once the GPU is done with them. A surface used alternately as depth and as colour (title A's shadow cube and atlas) no longer creates and frees four images per frame. | none |
| Submit worker | The GPU thread's `vkQueueSubmit` calls run on a worker thread in submission order. A wait on a queued batch submits it on the waiting thread. | `KYTY_ASYNC_SUBMIT=0` submits on the GPU thread |
| Compute fills as image clears, more shapes | Upstream turns a compute dispatch that stores one uniform value over the whole memory of an image into a clear of that image. The shape analysis now also accepts a fill that stops at a run-time element count, stores raw dwords, and picks its word from a few run-time words; the fill counts as uniform when those words are equal at dispatch time. Anything else runs as before. title B clears its render targets this way every frame, and each one was re-uploaded through the detiler afterwards. | none |
| Compute copies of whole images as image copies | The same shape analysis recognises a compute program that stores, at each element index up to a run-time count, the dword it loads from another buffer at that index. When a dispatch of it covers exactly the memory of one image, reads exactly the memory of another image of the same layout (format, size, levels, tiling), and the source's newest contents are in its image, the two images are copied on the GPU; the source is no longer read back, the program does not run, and the destination is not re-uploaded. Anything else runs as before. title B copies several of its render targets this way every frame. | none |
| Read-back fixes | GPU-written image data is kept across eviction and raw reads; read-back downloads start when their batch is submitted. | none |
| Held values in game patches | A `--game-patch` memory entry may carry `"hold": true`. Such an entry is not checked or written at load: a thread replaces its `off` bytes by its `on` bytes whenever the game writes them, for values a game computes into its own data after start (a render size, for instance). | per entry |
| Device-loss report | Every failed Vulkan wait or submit prints one `[device-loss]` line with free RAM, commit charge and video memory usage and budget. | none |

Measured on 2026-11-14 in the first gameplay scene of title B (character standing, 60 s window),
with the game's render resolution patched to 1280x720, on an RTX 3050 laptop GPU and a Ryzen 5
3600: about 39 fps before this work, 46.6 fps with early compute, 49.8 fps after merging upstream's
changes of that day. The last batch (walker and texture descriptions) lowers the frame time by
1.7 ms (21.91 to 20.21 ms) in an in-run comparison of alternating 5 s windows. Separate runs differ
by about 1.5 fps on this machine, and other scenes, resolutions and games were not measured.
The next batch (unwatched rewritten pages and the submit worker) lowers the frame time by 0.34 ms
in the same kind of in-run comparison; its clean run showed 50.3 fps. Most of its effect is CPU
time on the game threads (about 140 ms of page-fault handling per second less), not frame rate.

Measured on 2026-11-17 in the test scene of the *title A* prologue at 1280x720 (same machine, 400 s runs):
image-backing reuse raises it from 51.1 to 55.3 fps in an in-run comparison, and together with the hashing of
rewritten pages the scene holds the 60 fps cap (59.8 fps in two runs). At the game's default size it stays at
27.7 fps, where the GPU is the limit.

Known open point in *title A*: in about half of the runs of every build, including builds without any of this
work, the "Security Notice" sign in the bank stays dim for the whole run. The cause is not found yet.

Known open point: 3 of 20 runs with the second queue lost the GPU device, all within one hour in
which the machine was short of memory and disk; none since. If it happens, the `[device-loss]`
line in the log says whether memory was the cause, and `KYTY_EARLY_COMPUTE=0` returns to one queue.

The render-resolution patches used for these measurements are in
[recomp-patches](https://github.com/batatinhafcbatatinha-pixel/recomp-patches) (`ps5/TITLE_ID/`)
and are loaded with upstream's `--game-patch` option.

Parked code: the shader-side part of an earlier attempt to move descriptor reads to the GPU is still in
the tree and cannot be switched on (`KYTY_GPU_SRT` and `KYTY_GPU_SRT_LEARN` no longer select it).

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
