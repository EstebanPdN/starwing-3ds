# Source provenance

Starwing 3DS adapts the existing native Starfox Enhanced C++ engine. Simulation
uses the native runtime and its remaining 65C816 boundary; rendering is not
based on an N64 engine. The 3DS adapter uses libctru, Citro3D, and a modified
Citro2D v1.7.0. Existing engine responsibilities and CMake targets are retained.

The exact engine commit, upstream URL, UltraStarFox revision, and port version
are recorded in `config/upstream-3ds.json`. The repository contains the current
engine sources, the 3DS adaptation, and their existing tests. The original
working tree, Git history, and private build inputs are preserved locally.

The engine checkout has no general root LICENSE or COPYING. Its third-party
notices and optional xBRZ GPL license do not grant a blanket license for the
native engine. The original engine GitHub endpoint returned 404 during
preparation on September 30, 2026. The maintainer confirmed permission to
republish the engine for this repository. No blanket license has been invented;
the permission does not extend to Nintendo's cartridge data or other assets.

## Material kept local

- Retail and reconstructed ROMs, runtime asset bundles, BPS payloads, and MSU
  music packs.
- Cartridge-derived title-logo and dialogue headers, their imported catalogs,
  and generated game data.
- Private HOME Menu model/audio, UI artwork payloads, and presentation binaries.
- Saves, autosaves, settings, dumps, memory pages, credentials, caches, builds,
  installable packages, and the original engine's Git history.
- Local operational instructions and private investigation records.

The showcase image documents the project's existing visual presentation. It
is separate from playable game data and is not a measured physical-console
result. Original third-party copyright notices and all upstream credits are
retained; the 3DS work is credited separately.
