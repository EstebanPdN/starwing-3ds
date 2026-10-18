# Verification status

Current development version: **1.0**; CIA internal title version: **81**.
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
