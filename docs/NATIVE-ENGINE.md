# Experimental native rendering and audio

Included in stable 1.1, internal CIA version 87. Product: Starwing 3DS.
Physical Old/New 3DS acceptance remains pending. Host measurements below are
workload-specific and cannot be converted into console FPS.

## Responsibilities and model policy

| Module | Responsibility |
| --- | --- |
| `hardware_profile.hpp` | Conservative Old/unknown selection; independent New policy |
| `frame_3ds.cpp` | Existing game geometry, layer extraction, recording, fallback |
| `native_engine/scene.*` | Portable, bounded compiler to ordered rectangles |
| `native_engine/pica.*` | GPU resources, compact vertices, palette/checker draw |
| `native_engine/span.v.pica` | Position and palette/checker coordinates |
| `display_3ds.cpp` | GPU fence, background/world/foreground order and presentation |
| `tools/cmake/SpcNativeBlocks.cmake` | Guarded native execution of one profiled SPC loop |
| `tools/cmake/SpcDspOptimizations.cmake` | Exact ARM saturation in the pinned DSP |

Both model profiles use the new world backend for eligible wide Mode 2
2D gameplay/training. Old/unknown models retain a single cartridge SPC and no
additional CPU worker. New retains split SPC, its worker policy, and the
reference renderer when inward stereo is active. Configuration/checkpoint isolation,
Original speed/interpolation choices and approved Options/updater layout
remain as in E1.

Mode 1 native composition can be enabled explicitly by a development harness
with `set_native_engine(true, true)`. It is not selected by the product profile:
a 900-frame host preparation experiment cost 87.07 ms versus 79.24 ms for the
reference. That test proved image equivalence, not a performance benefit.
Unsupported scenes and stereo world rendering retain the reference. The 1.2
candidate also supports native spans in 4:3 via a shared final crop/scale;
stereo can use the GPU BG2 compositor. See [the 1.2 investigation](PERFORMANCE-1.2.md).

This backend replaces world pixel filling and world composition in eligible
scenes. Simulation, asset decoding, projection, clipping and span generation
remain shared. It is not yet a new engine for every scene or a triangle-level
GPU geometry implementation.

## Exact drawing contract

The production indexed CPU raster is the correctness oracle. Solid and
checkerboard spans become rectangles without per-pixel sampling. Textures,
edge masks and coverage masks retain exact integer sampling and emit solid
runs. The compiler clips bounds, preserves painter order and merges only
adjacent equal rectangles. It removes immediately preceding rectangles only
when the new rectangle covers them completely. No global material sorting
is permitted: it would change painter order.

Palette index zero must overwrite earlier world pixels while exposing the
background. Therefore world draws first overwrite a transparent RGBA8 target;
that target is then alpha-composited between background and foreground.
Blending directly into the background gives the wrong semantics. Palette
and checker texture V coordinates are inverted relative to screen coordinates;
the initial prototype's wrong colors were corrected at that boundary.

The scene is bounded to 16,384 rectangles. Capacity or unsupported-size
failure requests whole-frame CPU replay; no partially compiled scene is shown.
GPU allocation, frame acquisition or submission failure also routes through
a fresh reference frame. Switching routes invalidates the world cache.

## Buffers, cache and ownership

Each rectangle emits six 8-byte vertices: signed-short position, two byte
palette indices and signed-short dither scale. The shader derives UVs. The
reserved linear VBO is 786,432 bytes, compared with 3,538,944 bytes in the
first float-vertex prototype: a 2,752,512-byte reduction (77.8%). This comparison
is against the prototype, not E1, which had no native world VBO.

The world target occupies 512 x 256 x 4 = 524,288 bytes in VRAM. The palette
and checker are 1,024 and 256 bytes. Scene storage is pre-reserved once.
Only the populated vertex range and small palette are cleaned for GPU use.
There is no whole-heap cache flush.

Old 3DS no longer allocates the right-eye transfer staging buffer (262,144
linear bytes), two right-eye textures (524,288 VRAM bytes combined), or its
240 x 400 RGBA8/depth16 target (576,000 VRAM bytes). These are payload sizes;
allocator metadata/alignment and libctru framebuffer storage are separate.
New 3DS retains these resources. A failed model query selects mono resources.

`Display3ds` acquires a completed GPU frame before writing the persistent VBO.
The single GPU queue orders background rendering, the offscreen world draw,
then the final three-layer composite. The custom shader restores alpha blend
and texture state before returning to Citro2D. Shutdown drains the preceding
queue through FrameBegin before freeing native resources. VBlank waiting
alone is not a completion fence.

## Audio work and exactness

Profiling 1,200 gameplay packets found 4,825,782 of 16,246,814 SPC instructions
(29.70%) at the loop entry used by the native block. This is an instruction
share, not a CPU-time share. The loop generates data and cannot be discarded
as an idle wait.

The native operation is enabled for cartridge routing and both split stems. It verifies the
complete opcode pattern on every entry, reads operands live and rejects I/O,
ROM writes and writes into its own code. It executes a full iteration only
when at least 53 SPC clocks remain; short event boundaries use the pinned
interpreter. The terminating iteration retains the original 51-clock cost.
Registers, flags, RAM, DSP timing and output are compared against the original
interpreter. The enable flags and diagnostic counters are outside serialized
semantic state; existing checkpoint schemas remain unchanged.

The DSP remains the accurate pinned implementation. No sample-rate reduction,
voice removal, echo removal or approximate resampling was introduced. A
faster alternate DSP was investigated but not adopted because its state and
timing contracts do not match the existing checkpoints.

### Community Patches for A Link to the Past

Reviewed [Snipy44300's issue #49](https://github.com/EstebanPdN/zelda-alttp-3ds/issues/49),
series v10.0.11, specifically patches 0028 and 0056 from the attached source
archive (SHA-256 `8b4023d72aa204d33e8ef7cfa4bd7bb6e669286e60b6fe5c8df4f18facffcdae`).
Its author reports instruction-count reductions, not physical-console timings.
The patch sources were inspected; their build scripts were not run.

| Idea | Starwing assessment |
| --- | --- |
| Skip Gaussian interpolation at zero envelope/noise override | Already implemented in the accurate DSP before E2 |
| ARM SSAT instead of clamp comparisons | Implemented as an independently written exact helper; one ARMv6K instruction |
| Reuse each voice's stereo product for main and echo mix | The pinned DSP already reuses its product per channel |
| Unroll echo FIR | The pinned DSP already schedules explicit taps across DSP phases |
| Skip inaudible FIR/echo work | Not copied: intermediate fields are serialized and SPC writes can occur between phases |
| Skip released voice envelope work | Requires a separate proof against this core's envelope, key-on and phase state |
| Retain unaffected background planes | Existing revision-token background/foreground caches are retained; cache granularity remains a future profiling target |

The 16-18% figure in that proposal cannot be attributed to this application.
The successful transfer here is exact saturation, plus the principle of
proving that an eliminated operation cannot affect sound or future state.
Credit: Snipy's Community Patches for identifying this optimization pattern;
the source helper is independently written for this project's pinned core.

## Reproducible validation

ROM-free checks:

```sh
bash tools/test-host.sh
bash tools/test-native-audio.sh
```

The native audio script builds only the pinned SPC library and synthetic test;
FetchContent paths can be supplied as CMake arguments for offline dependencies.
The host suite includes the portable scene oracle and 231,075 saturation
values. The native audio suite compares PCM and complete core state 12,288
times and requires both acceleration and safe fallback cases.

Private developer fixtures additionally compare Original and EX for 512
controlled and 256 gameplay packets per experience, including full serialized
state samples every 17 packets, state restoration and corrupt-state rejection.
The 900-frame Corneria replay exercises 722 eligible native frames, with zero
pixel/coverage differences and 484,349 rectangles from 823,733 source commands.
Its total host renderer preparation was 70.35 ms versus 73.72 ms for reference;
it excludes PICA execution, cache maintenance and display submission.

ARM SSAT was compiled with the actual devkitARM ARMv6K configuration and run
under ARM1176 instruction emulation for 231,075 inputs. All matched, including
the int32 limits. This tests the ARM code branch; a host-only test would not.

Azahar validation uses isolated Old-model settings and copied local SD data.
Prototype inspection verifies colors, dither, layer ordering and scene
transitions. A fixed maximum of five diagnostic captures is used across the
investigation. Invalid-color prototype timings are not accepted as a gain.
The fifth and final capture used fingerprint
`f78962f33b2febb955a7ebcd9449c3daa27d22bada34d9dae24d0f113621828f`.
All 96,000 pixels of its actual PICA top-screen output match a CPU reference
reconstructed from the same saved game state at RGB565 display precision.
The capture confirms Old detection, cartridge routing, core 0, zero CPU quota,
no BG2 worker, no stereo, 1,435 rectangles, 8,610 vertices and one native draw.
Its 590 audio packets had no late or dropped packets. Snapshot vertex build
and clean times were 216 and 123 microseconds; these are emulator readings.

Across 29 one-second intervals, median renderer preparation was 5,515 us,
presentation work 2,968 us and SPC wait/compute on the main thread 16.1 ms.
These intervals contain different game scenes and are not a matched A/B
benchmark. They do not establish an overall FPS improvement. The emulator's
app FPS/status timing is not physical Old 3DS performance.

## Presentation limits and expanded coverage

FPS LIMIT is independent of GAME SPEED and INTERPOLATION. OFF / 30 / 60
are persisted per model and default to OFF. The rational presentation gate
retains sub-period credit, drops overdue whole periods and never advances game
or audio state. OFF removes that gate, while the source scheduler and physical
60 Hz display still bound output. It does not promise visible output over 60 Hz.

The first public experimental expands the comparison to all 75 discovered
level/scene entries in Original/EX, under both model/audio routes. The 150-case
matrix covers 187,200 candidate frames, 128,500 native frames and 18,450 stereo
frames. Every active image/alpha comparison agrees; coverage metadata outside
layered Mode 2 is deliberately ignored because that route never consumes it.
All 150 audio/full-state transcript digests match the pre-expansion routing.
Replay input alternates steering/firing and alpha 0 / 0.5 / 1, with periodic
stereo on New. Direct-entry coverage is not a complete stage playthrough.

The standalone `tests/native_gpu_replay.cpp` harness uses bounded private
world fixtures generated by `tests/native_scene_replay_test.cpp`. In Azahar,
the production packed-vertex shader and TEV pipeline pass 62 snapshots and
5,952,000 RGBA8 pixels with zero differences. This checks actual GPU drawing,
not only the CPU compiler. Backgrounds, HUD, input and audio are qualified by
the separate frontend/host/game tests; this isolated harness does not run them.

The unused Mode 1 experiment's two 400x240 indexed buffers now allocate lazily,
saving 192,000 bytes of idle pixel payload in the default profiles.

## Remaining engineering work

- Compare complete renderer + presentation + audio costs on matched physical
  Old/New routes, including menu, training and Corneria.
- Inspect sustained audio lead, late packets and allocation peaks rather than
  reading the frame counter alone.
- Move eligible polygon geometry directly to GPU triangles only after proving
  clipping, painter order, edge coverage and checkerboard behavior.
- Reduce CPU preparation and texture/mask expansion where real traces justify it.
- Qualify more stages, EX, checkpoint continuation, HOME/sleep and updater
  installation on hardware before promoting the experimental branch.

See [Performance research](PERFORMANCE-RESEARCH.md), [Status](STATUS.md) and
[Diagnostics](DIAGNOSTICS.md).
