# Games: frame rate with this fork

Measured with this fork on an **RTX 3050 laptop GPU (8 GB) + Ryzen 5 3600**, 60 Hz display (60 fps is the limit).

![In-game fps of Control Ultimate Edition and Grand Theft Auto V](fps/fps-games.svg)

| Game | Scene | Setting | fps | Picture |
| --- | --- | --- | --- | --- |
| *Control Ultimate Edition* (PPSA01949) | first gameplay scene | 720p render + 720p output | **59.6** (six 10 s windows, 59.0-59.9) | right |
| | | 720p render, output left at 3840x2160 | 51.3 | right |
| *Grand Theft Auto V* (PPSA04264) | bank scene of the prologue | Performance mode + 720p patch | **59.8** (two 400 s runs; about 39 before this fork's work) | right |
| | | game's default size | 27.7 (the GPU is the limit) | right |

**720p:** put `render-720p-output-720p.json` from
[recomp-patches](https://github.com/batatinhafcbatatinha-pixel/recomp-patches) (`ps5/PPSA01949/` or `ps5/PPSA04264/`)
through that repository's `python tools/recomp_patch.py kyty <patch.json> <out.json>` and start the game with
`--game-patch <out.json>`. In *Grand Theft Auto V* choose **Performance** in the graphics settings first: the patch
replaces that mode's sizes. The patches hold addresses and values only (no game code). A 1080p variant exists for both.

Known issue: in *Grand Theft Auto V*, in about half of the runs of every build (also without this fork's work) the
"Security Notice" sign in the bank stays dim. Cause not found yet.

[Back to the README](../README.md)
