# Verification status

Current development version: **1.1-E1**; CIA internal title version: **82**.
This is an experimental candidate awaiting physical validation.
The latest stable public release remains **1.0**.
The product and HOME Menu title remain **Starwing 3DS**.

## Scope

This publication synchronizes the latest local engine/adapter work, separates
save storage, bounded CSV history, and allocation diagnostics into named
modules, and adds the requested built-in stable/experimental updater. It preserves
existing rendering/simulation/audio algorithms, controls, approved resources,
SD layout, and explicit checkpoint schema. The memory-pool correction preceding
the organization pass is an intentional functional change.

## Completed checks

- ROM-free pacing, launch-selection, screen-transfer, presentation-gap, bounded
  history, SHA-256, and synthetic diagnostic-analyzer checks.
- Byte-preserving extraction checks for checkpoint/SRAM function bodies and
  the performance-history implementation.
- ASan/UBSan updater fixtures for version ordering, canonical metadata, fixed
  asset names, size/digest bounds, duplicate rejection, AM overwrite behavior,
  wrong-title/space refusal, partial-write cancellation, and 3DSX rollback.
- Actual Options rendering probe with private ROM font inputs: native panel,
  wrapped/paged changelog, touch actions, default-No confirmation, cancellation,
  and blocked exits during installation.
- Final package boot in Azahar's New 3DS profile, training, and transition into
  Corneria. The training capture restores with an exact PPU match and replays
  128 native frames plus 31 logic/audio ticks with retained/fresh renderer
  agreement. These are emulator/host checks, not physical-device acceptance.
- ARMv6K compile/link of the actual frontend, including the updater service
  adapter; package verification is recorded with the resulting local artifacts.

## Evidence boundaries

Earlier physical captures identify their own versions, including 0.71. The
reported 0.80 device allocation failure is retained as a prior defect report.
Neither is physical acceptance of v1.0. Old/New 3DS sustained 60 FPS, complete
stage progression, EX coverage, stereo comfort, HOME/sleep lifecycle, and the
new updater's device installation remain pending physical checks.

Public CI exercises synthetic/native host contracts. Its fake install services
cannot qualify AM or filesystem behavior on a console. Emulator runs qualify
only the recorded emulator/profile and scenario. Download verification and
runtime acceptance are separate results.

The source repository excludes user ROMs, companions, saves, settings, private
process pages, and build caches. Release assets distribute the application
separately. See [Compatibility](COMPATIBILITY.md), [Testing](TESTING.md), and
[Updating](UPDATING.md) for current boundaries and procedures.

## 1.1-E1 candidate checks

- Automatic Old/unknown-model single-SPC selection, with no core-1 quota or
  worker request; New-model split/core-2 selection retained (host service fakes).
- Original and EX stage-bank fixtures: 512 controlled plus 256 gameplay packets each agree byte-for-byte
  with the retained unfiltered-cartridge SPC reference for PCM and all APU ports.
- Single-SPC restore/continuation and corrupt-state rejection.
- Existing split-route PCM/stems/ports/complete-state and 4,096-step CSND ring
  differentials pass; direct-audio regressions pass.
- Geometry pacing agrees at 30/60/120 presentation FPS across three stages;
  intro cadence and timing regression tests pass.
- Actual font/art Options probe exercises Original+interpolation, touch/D-pad,
  back navigation, profile defaults, preference import and isolated persistence.
- RGB565 transfer compares every output pixel at 320/400 widths and all 16
  brightness levels. Build uses ARMv6K hard-float, Release O3 and LTO.

These checks establish their individual contracts. They do not establish Old
3DS physical FPS or elimination of the intermittent New 3DS system error.

## Menu and title-screen follow-up

All Options pages, ROM pagination, three confirmations and all eight updater
states were rendered with the actual cartridge font/art. Touch targets and
D-pad actions were exercised. Narrow status text no longer point-samples away
font strokes; punctuation and accented release metadata have defined readable
forms. Changelog lines have a 16 px pitch and eight rows per page. Previous,
Back and Next have independent footer buttons. Errors expose their full text
on the upper screen. Completed updates show 100 percent.

The Old title's lower screen is retained between changes in the 500 ms prompt
phase or brightness. Title phase pixel comparisons pass. In an isolated macOS
ARM64 CPU probe (three 1,024-packet runs per route), single-SPC median cost was
111.33 microseconds/packet versus 210.153 for split audio, about 47% less.
This is a host algorithm-cost comparison, not Old 3DS timing or FPS evidence.
