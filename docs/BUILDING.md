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
| `STARWING_PRESENTATION_DIR` | Approved icon and top-screen presentation resources. |
| `STARWING_UI_INCLUDE_DIR` | Generated UI include files when supplied separately. |
| `STARWING_BANNER_CGFX`, `STARWING_BANNER_AUDIO` | Private CIA presentation inputs. |

Missing inputs must stop configuration; do not substitute another version's
ROM data or publish private inputs to make a source-only check succeed.
See [credits](legal/CREDITS.md) and [third-party notices](legal/THIRD_PARTY_NOTICES.md).

The Old-model launch screen is generated with Python and Pillow on the game's
native pixel grid. Supply your own ROM and matching symbol map:

```sh
python3 platform/3ds/tools/make-old-3ds-notice.py \
  --rom /path/to/original.sfc --symbols /path/to/original-symbols.txt \
  --output /path/to/private-ui --version 1.1
```

Place `old_3ds_notice_art_data.hpp` alongside the other generated UI headers
in `STARWING_UI_INCLUDE_DIR`. The generator produces all six languages and checks text bounds and exact
pixel encoding. This screen needs no user ROM or SD data at
runtime. The generator source is public; its ROM-derived output follows the
same private-input boundary as the other game artwork.

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
retain their explicit input checks. Host results do not establish physical
3DS presentation, HOME/sleep or performance acceptance.

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
