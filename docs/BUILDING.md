# Building Starwing 3DS

The original native CMake build and current engine source are preserved. The
base revisions and 3DS modifications are recorded in [UPSTREAM.md](UPSTREAM.md).
Cartridge data, private UI artwork, BPS payloads, and HOME Menu resources must
be prepared locally; they are not part of the public source checkout.

## ROM-free checks from a clean checkout

Requirements: Python 3, a C++20 compiler, and Bash.

```sh
git clone https://github.com/EstebanPdN/starwing-3ds.git
cd starwing-3ds
python3 tools/check-publication.py
bash tools/test-host.sh
```

These checks exercise existing frame-pacing, launch-selection, screen-transfer,
and diagnostic-analyzer tests. They do not compile the native engine or create
a 3DSX or CIA. GitHub Actions runs these checks only; it does not publish software.

## Engine and private resource preparation

The current engine code is included with republication permission confirmed
by the maintainer. Its original GitHub endpoint returned 404 during preparation;
the source build does not depend on fetching that endpoint. The original
engine's history is retained locally and is not imported into this repository.

The UltraStarFox submodule remains pinned as a source reference. It is not
required for the ROM-free host checks. Its reconstruction outputs, BPS payloads,
and runtime data are private build inputs; initializing a submodule does not
grant permission to redistribute the game data it contains.

## Nintendo 3DS dependencies

- devkitPro with devkitARM and the Nintendo 3DS CMake toolchain.
- libctru and Citro3D; the modified Citro2D v1.7.0 copy is included with its
  license and exact local-change provenance.
- CMake 3.22 or later, Make, Python 3, and Bash.
- RetroCPU, snes_spc, and dr_libs at the commits pinned by the engine CMake file
  and preserved in the third-party notices. CMake fetches them; an existing
  local cache can be selected with `STARWING_HOST_DEPS`.
- `makerom`, `bannertool`, and `ffmpeg` for the optional CIA stage.

The 3DS build disables the desktop SDL3 runtime, optional xBRZ, and optional
MSU-1 music packaging. Do not change those scopes merely to match another port.

## Private inputs

The original local preparation includes nine BPS patch resources, the Original
and EX symbol maps, generated UI headers, and presentation files. They are not
public assets. Their exact paths can be supplied through:

| Variable | Input |
|---|---|
| `STARWING_PATCH_DIR` | Nine files named by `platform/3ds/CMakeLists.txt` |
| `STARWING_SYMBOL_DIR` | `ultrastarfox.txt` and `starfox-ex.txt` |
| `STARWING_PRESENTATION_DIR` | `top-background.rgb` and `icon-48.png` |
| `STARWING_UI_INCLUDE_DIR` | The five `*_art_data.hpp` UI headers |
| `STARFOX_PRIVATE_INCLUDE_DIR` | Root containing `starfox/localization/` data headers |
| `STARWING_BANNER_CGFX` | Private sculpted 3D HOME Menu model |
| `STARWING_BANNER_AUDIO` | Private banner audio input |

Use the exact locally prepared inputs from the existing working project or
regenerate permitted inputs with the source tools. The private localization
include root must contain `starfox/localization/title_logos.hpp`,
`dialogue_catalog.hpp`, and `ex_dialogue_catalog.hpp`; generators are in
`tools/generate_title_logos.py`, `generate_dialogue_header.py`, and
`generate_ex_dialogue_header.py`. Their inputs also stay local.

Missing private inputs prevent a complete build; a clean public checkout alone
cannot generate the current game candidate. Do not replace missing resources
with a ROM download or an invented asset pack.

## Complete 3DSX

```sh
DEVKITPRO=/opt/devkitpro \
STARFOX_PRIVATE_INCLUDE_DIR=/path/to/private/include \
STARWING_PATCH_DIR=/path/to/private/patches \
STARWING_SYMBOL_DIR=/path/to/private/symbols \
STARWING_PRESENTATION_DIR=/path/to/private/presentation \
STARWING_UI_INCLUDE_DIR=/path/to/private/ui-headers \
STARWING_BUILD_CIA=0 \
  bash platform/3ds/build-game.sh
```

Default outputs are `build-3ds/core/libstarfox_core.a` and
`build-3ds/game/Starwing-3DS-Game.3dsx`. The latter includes its generated RomFS
and SMDH. `STARWING_CORE_BUILD_DIR` and `STARWING_GAME_BUILD_DIR` override output
directories. The source version comes from `platform/3ds/version.txt`.

## CIA candidate

```sh
MAKEROM=/path/to/makerom BANNERTOOL=/path/to/bannertool \
STARWING_BANNER_CGFX=/path/to/private/banner.cgfx \
STARWING_BANNER_AUDIO=/path/to/private/banner-audio.mp3 \
  bash platform/3ds/build-game.sh
```

The default filename is `Starwing-3DS-Gameplay-0.67.cia`.
`STARWING_DIAGNOSTIC=1` selects
`Starwing-3DS-Hardware-Diagnostic-0.67.cia`. Internal CIA title version **66**
comes from `platform/3ds/cia-version.txt`; Title ID `0004000005f58b00` is retained.

The packager verifies SMDH/CBMD/CWAV, the exact icon/banner bytes, CGFX dictionary
integrity, and the ticket/TMD title version. Some makerom sources have a
`VER_MINOR` collision with their title-version array. The included
`platform/3ds/tools/fix-makerom-title-indices.py` repairs a separate local source
copy; use the repaired executable without overwriting another installed tool.

A successful 3DSX step does not imply CIA success. Missing CIA tools or private
banner inputs cause the CIA stage to fail explicitly. These commands do not
create a GitHub release or upload any binary.
