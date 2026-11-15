# Building Starwing 3DS

Use [the README](../README.md) for installation. This branch builds the native
Nintendo 3DS application and its engine verification tools. Independent Android,
Apple, Switch, UWP, Vita, Quest and desktop application shells have been retired.

## Nintendo 3DS application

Install devkitPro/devkitARM, libctru and the updater libraries below. Run:

```sh
./platform/3ds/build-game.sh
```

The build produces its shared engine under `build-3ds/core` and its frontend
under `build-3ds/game`. CIA packaging additionally requires `makerom`,
`bannertool`, `ffmpeg` and the explicitly supplied presentation resources.
Use `STARWING_BUILD_CIA=0` to build the 3DSX only.

The repository deliberately excludes ROM-derived inputs. Supply these through
the existing build environment variables, outside the source checkout:

| Variable | Input |
| --- | --- |
| `STARFOX_PRIVATE_INCLUDE_DIR` | Generated localization/game-data headers. |
| `STARWING_PATCH_DIR` | The nine version-specific BPS inputs expected by the frontend. |
| `STARWING_SYMBOL_DIR` | `ultrastarfox.txt` and `starfox-ex.txt`. |
| `STARWING_UI_INCLUDE_DIR` | Generated UI include files when supplied separately. |
| `STARWING_BANNER_CGFX`, `STARWING_BANNER_AUDIO` | Private CIA presentation inputs. |

Missing inputs must stop configuration; do not substitute another version's
ROM data or publish private inputs to make a source-only check succeed.

### Producing the private inputs

These tools create the inputs above from sources you supply. Their output stays
local and out of Git.

| Tool | Produces |
| --- | --- |
| `tools/build_upstream.ps1`, `tools/build_starfox_ex.ps1` | The assembled UltraStarFox and Star Fox EX ROMs and symbol tables, from the pinned source commits with the DOSBox-X toolchain those projects ship (Windows). |
| `tools/generate_title_logos.py` | `title_logos.hpp`, the regional title-logo tiles, from the UltraStarFox source data. |
| `tools/extract_ex_dialogue.py`, `tools/import_dialogue_catalog.py` | The EX and Original dialogue catalogs used for localization. |
| `tools/generate_dialogue_header.py`, `tools/generate_ex_dialogue_header.py` | The dialogue catalog headers, from those catalogs. |
| `platform/3ds/tools/make-entry-panel.py` | `entry_panel_art_data.hpp`, the menu entry-panel artwork. |
| `platform/3ds/tools/fix-makerom-title-indices.py` | A source fix for building `makerom` locally when its title-version names collide. |

`tools/generate_localization_font.py` and `tools/generate_menu_header.py`
regenerate the tracked font and menu headers. `tools/generate-lean-65816.py`
regenerates `src/simulation/wdc65816_lean_ops.inl`; its output must stay
byte-for-byte identical to the tracked file.

### Local test builds

`STARWING_CIA_TITLE` changes only the HOME Menu name of a local CIA. A test
build also changes `platform/3ds/version.txt` to its label (for example
`Prueba 10`, which shows the test badge) and raises
`platform/3ds/cia-version.txt` above the installed title version. Do not commit
those two edits.
See [credits](legal/CREDITS.md) and [third-party notices](legal/THIRD_PARTY_NOTICES.md).

## Native verification

```sh
python3 tools/check-publication.py
bash tools/test-host.sh
cmake -S . -B build-host -DSTARFOX_BUILD_TESTS=ON -DSTARFOX_BUILD_TOOLS=ON
cmake --build build-host --parallel 4
ctest --test-dir build-host --output-on-failure
```

The lightweight shell suite is ROM-free. The full CMake engine suite requires
the same private generated headers as the engine; tests requiring game data
are registered only when `STARFOX_ROM_FILE`, `STARFOX_SYMBOLS_FILE`,
`STARFOX_EX_ROM_FILE` and `STARFOX_EX_SYMBOLS_FILE` point at existing files.
Host results do not establish physical 3DS presentation, HOME/sleep or
performance acceptance.

## Network dependencies

The built-in updater requires static ARM curl, Jansson, and mbedTLS libraries.
Set `STARWING_UPDATE_DEPS_ROOT` to their prefix when using `build-game.sh`, or
pass `-DSTARWING_UPDATE_DEPS_ROOT=/path/to/prefix` to the frontend CMake step.
The default is devkitPro's `portlibs/3ds`. The release uses curl 8.4.0, Jansson
2.14, and mbedTLS 2.28.8 with the attributed devkitPro 3DS patches. Sources,
hashes, configuration, and licenses are in `platform/3ds/update-dependencies/`.
The source includes a public CA certificate bundle, not a private key.

On Ubuntu, the public host suite can use `libjansson-dev` and `pkg-config`.
For a separate native host prefix, set `STARWING_JANSSON_PREFIX` before running
`tools/test-host.sh`. This is independent of the ARM updater prefix.

The release normalizes compiler source paths with `-ffile-prefix-map`, so
installed artifacts do not expose workstation paths. This changes diagnostic
source-path strings and debug information, not game or serialization algorithms.
