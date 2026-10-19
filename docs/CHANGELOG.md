# Changelog

Published releases are listed first; earlier entries describe development candidates.

## 1.1.1 — 2026-10-19

- Select the Old 3DS notice language automatically from console settings:
  English, Spanish, Portuguese, Italian, German and French.
- Use English for unsupported console languages or failed system-language reads.
- Localize the message, planned-compatibility panel, version label and exit
  button while retaining the Python-drawn pixel interface and game lettering.

## 1.1 — 2026-10-19

- Show a game-style, Python-rasterized compatibility notice automatically on
  Old 3DS / 2DS, before reading ROMs or starting gameplay. Future support is planned.
- Default to Original game speed, interpolation ON and a 60 FPS limit.
  Preserve previously saved user preferences.
- Include the ordered PICA world backend and exact native SPC optimizations
  from 1.1-E1, with reference fallback for stereo and unsupported scenes.
- Retain the timing menu, updater, diagnostics, saves and title identity.

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
