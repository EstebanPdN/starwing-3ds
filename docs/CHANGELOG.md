# Changelog

Published releases are listed first; earlier entries describe development candidates.

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
