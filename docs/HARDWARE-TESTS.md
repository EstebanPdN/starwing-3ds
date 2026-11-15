# Hardware test tour

The game contains an automatic test tour for physical consoles. It measures
timing and checks the GPU while it flies a fixed route, so results from
different builds and consoles can be compared. It is a diagnostic tool, not a
game mode; the results are evidence for the exact build that produced them.

## Run it

1. Install the build to test. Test builds show their label (for example
   `Prueba 10`) on the HOME Menu and as a badge at the bottom of the lower
   screen.
2. Press **START + SELECT** while the Options menu is closed. Hold **R** as
   well to select the long tour. With automatic checkpoints enabled, the
   current game is saved first.
3. Leave the console alone; the lower screen shows the progress. At the end it
   shows `PRUEBA FINALIZADA` (or `PRUEBA CANCELADA`) and waits for START.
   Pressing **START** during the tour cancels it; the files written so far
   are kept.
4. Copy the whole `sdmc:/3ds/Starwing/` folder to a computer.

## What the tour does

Before gameplay, a GPU self-test draws 60 ROM-free patterns through the
production display code (ordinary presentation, BG2, the world, wide and 4:3
views, checker scales and palette changes) and compares the picture read back
from the GPU with the expected one.

The tour then enters 22 scenarios in order: `BOOT` (intro, title and menus,
driven with START taps) and the direct entries of every level of the three
routes, the black hole and the special stage. In each scenario it waits for
the level to settle, then measures fixed windows with a scripted controller:

| Tour | Windows per level | Window length | Total windows |
| --- | --- | --- | --- |
| Short (default) | 3 | 15 s | 65 |
| Long (hold R) | up to 10 (ends early when the level leaves gameplay) | 30 s | up to 212 |

After each window the tour writes a quick capture. Capture I/O happens after
the measured window and is excluded from its timing.

## Output files

| File | Content |
| --- | --- |
| `prueba/NN/resultado.txt` | One tour (`NN` is the first free number, 01-99): tour kind, every scenario, every window's FPS and logic rate, entry and capture failures, and whether it completed or was cancelled. |
| `prueba/NN/gpu-pixels.jsonl` | One line per GPU self-test case: submission and fence stages and the exact number of differing pixels. |
| `prueba/NN/gpu-case-NN.rgb565`, `gpu-case-NN-expected.rgb565` | Written only for a case whose pixels differ: the picture read back and the expected one. |
| `dumps/<capture>/prueba.txt` | Window header: `schema=3`, `build=<label>`, input script, scenario, window number and tour kind. |
| `dumps/<capture>/cuadros.csv` | One row per presented frame: time, work time, ticks run and the scripted input. |
| `dumps/<capture>/pixel-comparison.json`, `reference.rgb565` | Gameplay captures: the submitted top screen compared with a CPU reference of the same tick, palette, viewport and interpolation. A skipped comparison records its reason. |
| `dumps/<capture>/presentation-state.json` | Presentation snapshot (`schema=2`, `build`), with the PPU state and native draw commands beside it. |

The reference picture is 400×240 little-endian RGB565 with contiguous columns
and Y reversed. Every capture also carries the files described in
[Diagnostics](DIAGNOSTICS.md), including `runtime.json` with the build
fingerprint and `game-state.bin`, which reproduces the console's logic ticks
when replayed with the same input script. `gpu-recovery.txt` in the Starwing
folder records any GPU recovery.

## Reading the results

Check the fingerprint in each capture before comparing builds. Compare FPS
and logic rate per scenario and window, not tour averages alone, and read the
GPU self-test before trusting gameplay numbers: a lost GPU moves presentation
to the CPU and changes every timing that follows.

Host replays, the ARM cost simulator and emulators predict or check parts of
this behaviour; only a tour on the console measures it. Sustained FPS, audio
continuity and HOME/sleep transitions remain console acceptance criteria.
