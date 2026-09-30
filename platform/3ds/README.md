# Starwing 3DS adapter

This directory contains the existing libctru/Citro3D adapter, modified Citro2D, RSF profiles, local packaging scripts, and diagnostic tools.

The 0.67 runtime and packager read `version.txt`; CIA title version 66 is stored separately in `cia-version.txt`. Title ID `0004000005f58b00` is retained.

[Build instructions and private input paths](../../docs/BUILDING.md) · [Verification limits](../../docs/STATUS.md) · [Source provenance](../../docs/UPSTREAM.md)

The legacy `build.sh` target is diagnostic 0.1. The current gameplay target is `build-game.sh`. The current build requires locally prepared BPS/symbol resources, UI artwork headers, and presentation inputs; none are shipped as game data by this repository.
