# Changelog

Published releases are listed first; earlier entries describe development candidates.

## 1.2-E2 — 2026-10-21 (experimental)

> ⚠️ **Experimental release — known HOME Menu freeze**
>
> Pressing HOME during a game can freeze the console: the bottom screen turns black and the top screen remains frozen. A forced shutdown by holding POWER may be required. I am aware of this problem and am working on a fix. HOME/sleep/resume reliability has not been established; use this build only if you accept this known issue.

- Enable gameplay on Old Nintendo 3DS / 2DS (experimental) and remove the
  launch notice.
- Address the black-screen failure path with bounded frame waits, a GPU
  completion/swap fence and automatic recovery. A lost GPU context retains
  its resources and uses separate CPU scanout buffers, so late GPU work cannot
  overwrite recovery. Recoveries are logged to `gpu-recovery.txt`; physical
  console acceptance remains pending.
- Attempt to stop audio and drain GPU work before HOME, and reset timing on
  return. **The console freeze on HOME remains reproducible on hardware.**
  Host checks cover the callback contract only; this release is not a HOME fix.
- Old 3DS: run SPC audio on the second CPU core and reduce background,
  presentation and per-instruction CPU work.
- Keep eligible PICA world/BG2 rendering active in 4:3; apply one consistent
  crop and scale to all layers and keep the HUD within the visible image.
- Speed up the CPU background used by reference rendering and recovery.
- Place stereo BG2 scenery behind the world rather than at the HUD plane,
  with matching GPU/CPU eye shifts and clamped edge coverage.
- Retry recoverable world-renderer failures on a later scene; a permanently
  abandoned GPU context stays on CPU presentation until the application exits.
- Include the 1.2-E1 crash fix.

## 1.2-E1 — 2026-10-19 (experimental)

- Fix a null texture pointer crash when handing native PICA rendering back to Citro2D.

## 1.1 — 2026-10-19

- Improve PICA rendering, audio processing and memory use.
- Default to Original speed, interpolation ON and a 60 FPS cap.
- Add an automatic Old 3DS compatibility notice in six console languages,
  with English fallback.

## 1.0 — 2026-10-17

- Initial release.

See [Compatibility](COMPATIBILITY.md) and [Verification status](STATUS.md)
for the scope of this first public distribution.

## 0.67 — historical development candidate

- Experimental inward stereo during eligible wide Mode 2 gameplay and training,
  controlled by the console's 3D slider; flat scenes keep their original path.
- Stereo projections for models, particles, dust, world text, and the grid;
  HUD and BG2 stay at the screen plane.
- Bulk clipped polygon spans and a grouped uniform-character BG2 carry resolver.
- New 3DS BG2 scheduling behind the higher-priority audio worker.
- Modified Citro2D used-buffer flushing and explicit command-list flushing,
  with fallback counters in diagnostics.
- Ticket/TMD title-version validation in the CIA packager.
- Repository preparation: English project documentation, explicit private
  inputs, synchronized port/CIA version files, and source-only host CI.

Physical 0.67 performance and stereo validation remain pending. Repository
preparation does not establish a new gameplay or compatibility result.

## 0.66 — preceding local candidate

- Uniform 8x8 BG2 character caching, verified against the existing planner.
- Removed redundant collision-list object synchronization while preserving the
  consumer's final state import.
- Continued local host differential checks and private New 3DS diagnostics.

Full source logic, visual content, installed title identity, and user SD data
are preserved by the repository preparation.
