# Diagnostics

Use a small number of deliberate captures at different states. A capture
interrupts work for SD I/O; capture time is excluded from ordinary performance
intervals. Repeated capture files do not by themselves provide independent
performance samples.

## Shortcuts

| Shortcut | Result |
| --- | --- |
| `L + R + A` | Quick capture with screens and diagnostic metadata. |
| `L + R + B` | Full capture with additional private state and memory pages. |
| `L + R + SELECT` | Toggle the diagnostic overlay. |
| `L + R + X` | Delete generated capture folders, including unfinished captures. |

The overlay also exposes touch cards. If the `overlay` preference is enabled,
SELECT can toggle it directly. Full capture takes priority when both action
buttons accompany the shoulder-button chord.

Captures live in `sdmc:/3ds/Starwing/dumps/`. An interrupted capture remains
staged as `.partial`; a complete capture has `manifest.json`, checksums, and a
`COMPLETE` marker. Completion means the writer finished, not that every game
path has been qualified.

## Inspect locally

```sh
python3 platform/3ds/tools/analyze-hardware-dump.py /path/to/Starwing
```

The analyzer accepts a Starwing directory or a structured capture directory.
Keep hashes enabled when checking integrity. Use the exact runtime build and
fingerprint to associate a dump with its executable; an old dump is evidence
for its own build, not a new version's performance.

| Evidence | Meaning |
| --- | --- |
| `runtime.json`, `manifest.json` | Version, fingerprint, console model, flow, and completion. |
| `top.png`, `bottom.png` | Submitted visual state, when available. |
| `performance.csv` | Bounded interval telemetry, including heap/linear usage, timing stages, and cache counters. |
| `presentation.csv` | Recent submission gaps, input masks, phases, and work. |
| `memory.txt` | Selected owner capacities and separate allocator counters. |
| `renderer.txt`, `audio.txt` | Rendering route, reuse, audio backend, worker, and packet information. |
| `flow-profile.txt` | Scoped native-boundary timing, not exclusive CPU time. |
| `game-state.bin`, `ppu-state.bin`, `spc-state.bin` | Private focused state for replay and inspection. |
| `memory-map.txt`, `mem-*.bin` | Private current-process mappings and memory pages. |

Some files depend on capture kind and build. Do not fabricate missing fields
when comparing older captures. The performance CSV uses 104 columns; the
presentation CSV uses 18. History holds 180 interval rows and the recent 512
submissions. Overlapping snapshots must be deduplicated before aggregation.

## Interpret memory correctly

Ordinary heap and linear memory are independent pools. Free linear memory does
not satisfy a failed C++ heap allocation. The current ordinary-heap display is
capacity minus allocated bytes; older builds displayed only free bytes inside
the committed allocator arena. Compare `heap_used`, `heap_arena`, capacity,
and the exact build before drawing a conclusion about fragmentation.

The allocation handler captures allocator metadata before game objects unwind.
The outer error log includes build, fingerprint, operation, flow, and available
memory context. Writing occurs after unwinding; the handler itself does not
allocate or perform file I/O. Preserve this file before restarting.

## Interpret time correctly

Logic frequency, renderer work, presentation service, GPU work, and actual
screen refresh are different measures. Several stage timings are nested.
Do not add them indiscriminately. `vblank_wait_us` retains a legacy column name
but records deadline sleep under the current rational pacing policy.

Emulator timings describe that emulator/profile. They establish neither Old
nor New 3DS physical FPS. Source-state replays establish only their named
fixtures. A higher average can still contain long frame gaps; inspect the gap
percentiles, maxima, missed/coalesced phases, and audio counters together.

## Reporting

Include version, console model, Original / EX, ROM revision, stage, settings,
and exact steps. A quick capture is the normal first report. If a full folder
is requested, share it privately with the maintainer. Review screens and paths
before sharing. Never attach a ROM, runtime companion, save, complete memory
archive, or unreviewed process pages to a public issue.
