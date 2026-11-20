# Games: frame rate with this fork

Measured with this fork on an **RTX 3050 laptop GPU (8 GB) + Ryzen 5 3600**. The *Control* rows were measured earlier
with a 60 fps limit. The *Grand Theft Auto V* matrix below was measured without any display or flip-rate cap
(`--ignore-flip-rate --present-mode Immediate`, and `--vblank-frequency 360` for the Performance modes).

![In-game fps of Grand Theft Auto V](fps/fps-games.svg)

| Game | Scene | Setting | fps | Picture |
| --- | --- | --- | --- | --- |
| *Control Ultimate Edition* (PPSA01949) | first gameplay scene | 720p render + 720p output | **59.6** (six 10 s windows, 59.0-59.9; 60 fps limit) | right |
| | | 720p render, output left at 3840x2160 | 51.3 | right |

### *Grand Theft Auto V* (PPSA04264): 3 graphics modes x 3 sizes, no fps cap

Prologue bank scene, fps = frames presented over 30 s, taken 300-330 s after start (about 60 s of gameplay), one run
per cell, build `ab7149c5` (clean build of that commit, defaults, software ray tracing), 2026-11-20. Cell = fps and the
size of the main render passes seen in the bank scene (Vulkan colour targets of the emulator).

| Graphics mode | 720p | 1080p | 4K |
| --- | --- | --- | --- |
| **Performance** | **61.3** fps (1280x720) | **58.6** fps (1280x720 + 1920x1080) | **27.4** fps (2560x1440 + 3840x2160) |
| **Performance RT** | **61.1** fps (1280x720) | **59.0** fps (1920x1080) | **27.3** fps (2560x1440 + 3840x2160) |
| **Fidelity** | **16.4** fps (1280x720) | **13.2** fps (1920x1080) | **5.6** fps (3840x2160) |

- Performance and Performance RT at 720p and 1080p sit at the game's own 60 fps limit (61.3 and 58.6-61.1 with
  `--vblank-frequency 360`, 58.9 for Performance 720p at the default 60); the Fidelity cells are far below any limit,
  so no cap is involved in them. At 4K the GPU is the limit.
- 720p and 1080p use the resolution patches (addresses and values only, see below); 4K uses the game's own sizes: the
  Performance modes then render 2560x1440 passes plus 3840x2160 output-sized passes, Fidelity renders 3840x2160.
  The Performance 1080p patch moves only some of the passes to 1080p (the rest stay 1280x720).
- Performance RT came out the same as Performance in this emulator within noise (graphics mode confirmed as
  *Performance RT* in the in-game settings); Fidelity is limited by the emulator's GPU thread (software ray tracing,
  one thread at about 90 %), not by the GPU alone.
- Window sizes 1280x720, 1920x1080 and 3840x2160 (client area) for the 720p, 1080p and 4K columns.
- The Fidelity and Performance RT 720p/1080p cells used a patch of the same kind (display and render size, plus the
  upscaler scale table set to 1.0).

**720p:** put `render-720p-output-720p.json` from
[recomp-patches](https://github.com/batatinhafcbatatinha-pixel/recomp-patches) (`ps5/PPSA01949/` or `ps5/PPSA04264/`)
through that repository's `python tools/recomp_patch.py kyty <patch.json> <out.json>` and start the game with
`--game-patch <out.json>`. In *Grand Theft Auto V* choose **Performance** in the graphics settings first: the patch
replaces that mode's sizes. The patches hold addresses and values only (no game code). A 1080p variant exists for both.

Known issue: in *Grand Theft Auto V*, in about half of the runs of every build (also without this fork's work) the
"Security Notice" sign in the bank stays dim. Cause not found yet.

[Back to the README](../README.md)
