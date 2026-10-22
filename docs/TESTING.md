# Testing

Verification is scoped to the environment and workload actually executed.
Host tests, ARM compilation, emulator execution, and physical-console tests
are separate evidence. Current status is in [STATUS.md](STATUS.md).

## ROM-free public suite

```sh
python3 tools/check-publication.py
bash tools/test-host.sh
```

The publication checker inspects the tracked selection, private-data boundaries,
possible secrets, submodule pin, version metadata, and local Markdown links.
The host script compiles standalone C++20 tests for frame pacing, launch
selection, screen transfer, presentation metrics, performance history, and
SHA-256; it also runs synthetic diagnostic-analyzer fixtures.

GPU lifetime stubs exercise queue-idle versus callback completion, a concurrent
swap/abandonment, system-tick timeout units, no synthetic completion, and late
GPU writes that cannot touch independent CPU recovery buffers. They model those
ownership contracts; they do not emulate the physical GPU.

The audio suspend regression invokes the real APT audio hook and Audio3ds code
with CSND completion deliberately withheld. It verifies that the hook only
updates memory flags and issues no CSND commands under both model profiles.
A pending HOME request stops audio and invokes GPU quiescence before entering
aptMainLoop; resume starts a fresh playback ring. Service stubs do not model
the APT/GSP/system-menu handshake. The owner still reproduced a full hardware
freeze, so this passing test is not HOME acceptance.

The performance-history regression covers an empty export, the 104-column
schema, chronological retention after more than 180 appends, and truncation
reporting. It protects the extracted storage contract without requiring a ROM
or console services.

GitHub Actions uses read-only contents permission and the pinned checkout action.
It runs this public suite. It does not contain private resources or build a CIA,
and a green run is not a physical gameplay result.

## Full native and target checks

The engine CMake suite contains additional raster, simulation, audio, BG2,
collision, strategy, shape, and stereo tests. Some require prepared ROMs,
symbol maps, or private generated headers. Do not mark an unavailable
input-dependent test as passed. Follow [Building](BUILDING.md).

Compile and link the actual ARM frontend after module moves. Verify that the
source version reaches the runtime definition, fingerprint includes linked
archives, ticket and TMD agree on the title version, and packaged content hashes
match the extracted content. Check the existing title identity and icon/banner
bytes. Source archives and installable binaries are distinct artifacts.

## Focused console route

1. Launch with the intended model and existing SD configuration.
2. Exercise title, controls, training, and entry into the first main level.
3. Move in both directions, use the selected scheme's firing control, pause,
   and resume. Review both screens and audio.
4. Take a few deliberate captures at meaningful state boundaries; analyze them
   before collecting more and remove only owned temporary captures afterward.
5. Exercise settings, restart, save/checkpoint restoration, HOME, and sleep on
   physical hardware when available.

Performance runs use **Wide Mode**, the primary play configuration. Keep 4:3
in visual-correctness checks only. Collect timing, state and deliberate image
captures in a planned run, analyze the saved evidence outside the emulator,
and repeat only when a change or an unresolved failure requires it. A capture
must occur after the timing window or in a separate correctness run.

Keep original user-provided SD evidence intact. Full diagnostic memory remains
local/private. After analysis, retain checked metadata, reviewed images,
aggregate results, and cleanup receipts rather than accumulating raw RAM copies.

## Preservation review

The organization pass separates save I/O, allocation diagnostics, and the CSV
ring. Its storage functions and ring algorithm are moved intact. Input mapping,
render and simulation algorithms, sound resources, menu assets, SD paths, and
serialization schemas are preserved. Version identifiers intentionally change.
The preceding allocation-pool correction is an intentional runtime change,
not a claim of behavior-preserving refactoring.

## Updater fixtures

The host suite checks numeric version ordering, equal-version normalization,
normal-release selection, exact asset names, repository/tag binding, size and
digest requirements, duplicate metadata, and input limits. Synthetic fixtures
never install a package or alter a real game. Live release verification checks
the same parser against the published GitHub response after publication.

The updater host adapter also exercises the actual installer with mocked AM/FS
services: overwrite/commit, wrong-title rejection, insufficient-space rejection,
partial-write cancellation, 3DSX backups, and failed-replacement rollback. These
are service-contract fixtures, not physical installation evidence.

The update fixtures also cover numeric `-E` ordering, channel isolation and
persistence, release-list selection, bounded Markdown conversion, proportional
font wrapping, and refusal of a non-increasing CIA title version. A local
rendering probe exercises the actual Options renderer with private ROM/font
inputs for native-style screens and touch/confirmation/cancellation behavior;
those inputs are not part of the public test suite.

## Broad native scene regression

`tests/native_scene_replay_test.cpp` accepts a private assembled ROM, matching
symbols, an authored entry symbol, an update count and `old` or `new`. Compare
its image/coverage results and audio/state SHA-256 with the retained audio
implementation. A sixth optional path exports a private world fixture after
the halfway point; it is not a portable save or public release asset.

The GPU fixture runner builds separately from `platform/3ds/tests` with the
3DS toolchain, `STARWING_REPLAY_FIXTURES` pointing at a private directory with
`cases.txt`, and `STARWING_CITRO2D_LIBRARY` pointing at the matching game archive.
Its production PICA renderer is read back as RGBA8 and compared with the
expected indexed composition. Results go to `sdmc:/native-gpu-results.json`.
Never publish the scene fixtures. See [Status](STATUS.md) for measured coverage
and the remaining physical-console acceptance boundaries.

The same target-test project also builds `display-gpu-replay.3dsx` without ROMs
or private fixtures. It requires the matching Citro2D archive and exercises sixteen
96,000-pixel cases: patterned upload, native palette/checker spans, BG2/HUD
composition, checker scales 1 through 4 and permanent CPU recovery, each in
panoramic and 4:3 views. These are correctness checks, not benchmarks. The
report is `sdmc:/display-gpu-results.json`; START exits after the report is saved.
Run both supported emulator backends, recording any diagnostic emulator change
such as the one-worker Software restriction documented in
[the 1.2 investigation](PERFORMANCE-1.2.md).

## PICA handoff crash regression

With `unicorn` and `pyelftools` installed, run:

```sh
python3 platform/3ds/tools/test-pica-handoff-arm.py path/to/1.1.elf path/to/1.2-E1.elf
```

The test executes the actual ARM handoff and linked Citro3D with page zero
unmapped. It requires the baseline to fault at address 12 and the candidate
to reach Citro2D. It does not simulate the GPU or replace console validation.
