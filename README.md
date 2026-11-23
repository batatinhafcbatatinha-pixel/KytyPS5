# KytyPS5 (performance fork)

## Frame rate: this fork vs original KytyPS5

| Configuration | Resolution | Original KytyPS5 | This fork | Gain |
|---|---|---|---|---|
| **Quality mode with ray tracing** | default | **closes when the scene loads** (emulator error) | **8.0 fps** | runs vs. does not run |
| **Performance mode, 30 fps limit unlocked** | default | **27.2 fps** | **29.3 fps** | **+8%** |
| **Performance mode, 60 fps limit unlocked** | 1080p (resolution patch) | **closes when the scene loads** (emulator error) | **65.2 fps** | runs vs. does not run |

RTX 3050 laptop GPU + Ryzen 5 3600, 2026-11-23, one demanding title, same test scene, average over 160 s, one run per
build and row; this fork `5384fd81` (quality row `0a1b544d`: 7.98 / 8.03 fps), original `578f2d43` (quality row
`a0b083c8`, two runs). Unlocked = flip rate ignored and 360 Hz vblank; the original has no flip-rate switch and ran with
the 360 Hz vblank only. The original closes with the same shader resource tracking error in both rows, with the
resolution patch held or written once.

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

The fork follows upstream `main` (merged regularly; last merge at upstream `578f2d43`). Everything it adds is about
frame rate, is on by default, and can be turned off where a switch is named.

- **Compute work runs ahead on a second GPU queue.** Programs whose results are read back later run early, so the
  GPU thread rarely waits for them. Off: `KYTY_EARLY_COMPUTE=0`.
- **Pages the game rewrites all the time stop being watched.** They are uploaded once per frame batch instead of
  faulting on every write, and only when their bytes really changed. Switches: `KYTY_REWRITTEN_FRAMES=n` (how often
  the list is rebuilt), `KYTY_BDA_HASH=0` (always upload).
- **GPU images are reused.** A deleted image is kept for up to 2 s and handed to the next one of the same shape, so
  shadow maps no longer create and free four images per frame.
- **Texture descriptions are cached** instead of recomputed for every binding, and the descriptor reader avoids page
  faults on pages it just touched.
- **`vkQueueSubmit` runs on its own thread.** Off: `KYTY_ASYNC_SUBMIT=0`.
- **More compute work becomes plain GPU clears and copies.** Fills and whole-image copies that some games run every
  frame are recognised and done as image clears and copies, without reading the data back.
- **Read-back fixes:** GPU-written image data is kept, and downloads start as soon as their batch is submitted.
- **Held values in game patches:** a `--game-patch` entry with `"hold": true` is written again whenever the game
  writes its own value (used by the resolution patches in
  [recomp-patches](https://github.com/batatinhafcbatatinha-pixel/recomp-patches)).
- **Device-loss report:** every failed Vulkan wait or submit prints one `[device-loss]` line with memory usage.

Open points:

- 3 of 20 runs with the second queue lost the GPU device, all in one hour when the PC was short of memory; none
  since. The `[device-loss]` line tells whether memory was the cause; `KYTY_EARLY_COMPUTE=0` goes back to one queue.
- An older attempt at reading descriptors on the GPU is still in the tree but switched off (it no longer fits
  upstream's reworked image heap).

## Building and running

Exactly as upstream: see
[Building](https://github.com/KytyPS5/KytyPS5#building) and
[Running](https://github.com/KytyPS5/KytyPS5#running). Bugs that also happen in upstream's build
belong in upstream's issue tracker; bugs that only happen here belong in this repository's.

## AI use

The changes of this fork were written with AI assistance (Claude models, supervised and tested by
the fork's maintainer); the commits say how each change was measured.
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
