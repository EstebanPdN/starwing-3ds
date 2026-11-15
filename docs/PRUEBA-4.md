# Prueba 4 — local hardware candidate

Prueba 3 lost the GPU before its gameplay captures, then spent roughly 38 ms
per presentation in CPU rendering and 12 ms in framebuffer transfer. Its
gameplay windows averaged 11.1 FPS. These are measurements of the failed
fallback route, not measurements of a healthy GPU renderer. Prueba 4 is not
yet accepted as stable 60 FPS on physical Old 3DS.

## Changes

- World geometry now shares Citro2D's shader and buffers. Its two defined
  texture coordinates supply palette/checker sampling; even colour is carried
  by the vertices. All rectangles submit in one ordered batch. No texture unit
  2, custom shader handoff, separate world VBO or private Citro3D context edits.
- Every offscreen target sets its own scissor. The world draw explicitly
  overwrites transparent pixels and restores blending before composition.
- Collision instructions stay in the fast 65C816 loop instead of repeatedly
  exporting/importing CPU state for short legacy accelerator blocks. Instruction
  semantics, execution budget, recent-PC history and I/O fallback remain intact.
- The lower HUD is retained when its actual sprite characters, palette, meter,
  dialogue and visible FPS text are unchanged. It still checks every source
  update; there is no deliberate reduction of HUD refresh rate.
- CPU recovery presentation uses tiled reads and rotated framebuffer writes,
  with the same nearest-sample crop and scale as before.

## Hardware test

The HOME title and persistent bottom badge identify **Prueba 4**, CIA version
**93**, with the existing title identity. START+SELECT starts the same tour;
holding R selects the long tour. Before gameplay, 20 ROM-free patterns exercise
ordinary presentation, BG2, the world, wide/4:3, checker scales 1–4, palette
changes and transparent world overdraw through the production display code.

`prueba/NN/gpu-pixels.jsonl` records before/after submission and bounded fences,
then the exact number of differing pixels. A timeout names the last stage and
uses the existing independent CPU recovery buffers. A pixel mismatch records
the actual framebuffer as `gpu-case-NN.rgb565`; it does not silently count as a
pass or permanently disable a responsive GPU.

Gameplay/training captures also save `pixel-comparison.json` and
`reference.rgb565` when a current mono presentation can be fenced and copied.
The CPU reference uses the same tick, palette, viewport and interpolation as
the submitted picture. The reference is 400×240 little-endian RGB565, columns
contiguous and Y reversed. Comparisons and SD writes happen after the timing
window. Missing comparison files mean no comparison was performed, not zero
differences. Menus and stereo are excluded from this gameplay comparison.

Copy the complete `sdmc:/3ds/Starwing/` test output for analysis, including
`prueba/`, `dumps/`, `build-info.txt` and `gpu-recovery.txt`. Verify the build
fingerprint before comparing to older results. GPU recovery now also records
the last vertex count, draw count and command-buffer use.

## Validation limits

Host tests verify raster compilation, palette/vertex encoding, GPU resource
and submission contracts with stubs, unchanged screen sampling and HUD source
invalidation. Hardware snapshot replay compares the old and new CPU output
and final state with fixed inputs. Engine tours compare audio, pixels and state
over level entries. Those checks do not execute a PICA200 or measure console FPS.

Physical acceptance requires a completed tour with a surviving GPU, inspection
of pixel differences, sustained presentation timing, audio, and HOME/sleep
transitions. Compilation or a host speedup is not that acceptance result.
